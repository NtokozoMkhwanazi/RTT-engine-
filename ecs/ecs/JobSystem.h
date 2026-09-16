#pragma once

#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include <vector>
#include <atomic>
#include <memory>
#include <future>

namespace ecs {

/**
 * Job System - Multi-threaded task execution for parallel system updates
 * 
 * Provides a thread pool with work stealing for efficient parallel execution
 * of ECS system updates.
 */
class JobSystem {
public:
    /**
     * Job handle for tracking completion
     */
    struct Job;  // forward-decl: lets JobHandle hold a shared_ptr<Job>

    struct JobHandle {
        JobHandle() = default;

        JobHandle(JobHandle&&) noexcept = default;
        JobHandle& operator=(JobHandle&&) noexcept = default;
        JobHandle(const JobHandle&) = default;
        JobHandle& operator=(const JobHandle&) = default;

        /**
         * Wait for the job to complete
         */
        void wait() {
            if (m_future.valid()) {
                m_future.wait();
            }
        }

        /**
         * Check if the job is complete
         */
        bool isComplete() const {
            return !m_future.valid() ||
                   m_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        }

    private:
        friend class JobSystem;
        std::shared_future<void> m_future;         // observed by wait()/isComplete()
        std::shared_ptr<Job> m_job;         // live Job node for dep-graph edges
    };

    /**
     * Job with dependencies
     */
    struct Job {
        std::function<void()> func;
        uint32_t priority = 0;  // Higher = more urgent
        std::shared_ptr<std::promise<void>> promise;          // fulfilled by the worker on completion
        std::atomic<int32_t> pendingDeps{0};                   // unsatisfied prerequisites; 0 => ready to run
        std::vector<std::weak_ptr<Job>> dependents;           // reverse edges (jobs waiting on this one)
    };

    /**
     * Constructor - creates the thread pool
     * @param numThreads Number of worker threads (default: hardware concurrency)
     */
    explicit JobSystem(size_t numThreads = 0) 
        : m_stop(false)
        , m_activeJobs(0)
    {
        if (numThreads == 0) {
            // Ryzen 7 3700U: 4 cores, 8 threads
            // Use 3 worker threads (main + 3 workers = 4 threads total)
            // Leave 4 threads for OS, background tasks, and GPU driver
            size_t hwThreads = std::thread::hardware_concurrency();
            if (hwThreads >= 8) {
                numThreads = 3;  // 3 workers for 8-thread CPU
            } else if (hwThreads >= 4) {
                numThreads = 2;  // 2 workers for 4-thread CPU
            } else {
                numThreads = 1;  // Single worker for dual-core
            }
        }
        
        m_workers.reserve(numThreads);
        for (size_t i = 0; i < numThreads; ++i) {
            m_workers.emplace_back([this] { workerThread(); });
        }
    }

    /**
     * Destructor - waits for all jobs to complete
     */
    ~JobSystem() {
        shutdown();
    }

    // Prevent copying
    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    /**
     * Initialize the job system
     */
    void init() {
        m_initialized = true;
    }

    /**
     * Shutdown the job system
     */
    void shutdown() {
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_stop = true;
        }
        
        m_condition.notify_all();
        
        for (auto& worker : m_workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        
        m_initialized = false;
    }

    /**
     * Add a job to the queue
     * @param func The function to execute
     * @param priority Job priority (higher = more urgent)
     * @return JobHandle for tracking completion
     */
    JobHandle addJob(std::function<void()> func, uint32_t priority = 0) {
        auto job = std::make_shared<Job>();
        auto promise = std::make_shared<std::promise<void>>();
        job->func = std::move(func);
        job->priority = priority;
        job->promise = promise;
        job->pendingDeps.store(0, std::memory_order_release);

        JobHandle handle;
        handle.m_future = promise->get_future().share();
        handle.m_job = job;

        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_readyJobs.push(job);   // no deps -> immediately ready
        }
        m_condition.notify_one();
        return handle;
    }

    /**
     * Add a job with dependencies
     * @param func The function to execute
     * @param dependencies Jobs that must complete before this one runs
     * @param priority Job priority
     * @return JobHandle for tracking completion
     */
    JobHandle addJobWithDeps(std::function<void()> func,
                              std::vector<JobHandle> dependencies,
                              uint32_t priority = 0) {
        auto job = std::make_shared<Job>();
        auto promise = std::make_shared<std::promise<void>>();
        job->func = std::move(func);
        job->priority = priority;
        job->promise = promise;

        int32_t pending = 0;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            for (auto& dep : dependencies) {
                if (dep.m_job) {
                    // Reverse edge: when `dep` finishes it will decrement
                    // job->pendingDeps and enqueue `job` when it hits 0.
                    dep.m_job->dependents.push_back(job);
                    ++pending;
                }
            }
            job->pendingDeps.store(pending, std::memory_order_release);
            if (pending == 0) {
                // Dependencies already satisfied -> eligible to run immediately.
                m_readyJobs.push(job);
            }
            // otherwise the job is parked in the dep graph and pushed onto
            // m_readyJobs by its last finishing dependency (see workerThread).
        }
        if (pending == 0) {
            m_condition.notify_one();
        }

        JobHandle handle;
        handle.m_future = promise->get_future().share();
        handle.m_job = job;
        return handle;
    }

    /**
     * Add multiple jobs and wait for all to complete
     * @param jobs Vector of functions to execute in parallel
     */
    void addJobsAndWait(std::vector<std::function<void()>> jobs) {
        std::vector<JobHandle> handles;
        handles.reserve(jobs.size());
        
        for (auto& job : jobs) {
            handles.push_back(addJob(std::move(job)));
        }
        
        // Wait for all jobs to complete
        for (auto& handle : handles) {
            handle.wait();
        }
    }

    /**
     * Add jobs in parallel for range-based processing
     * @param begin Start index
     * @param end End index
     * @param func Function to execute for each index
     */
    void parallelFor(size_t begin, size_t end, std::function<void(size_t)> func) {
        if (end <= begin) return;
        
        size_t totalJobs = end - begin;
        size_t numWorkers = m_workers.size();
        size_t jobsPerWorker = (totalJobs + numWorkers - 1) / numWorkers;
        
        std::vector<JobHandle> handles;
        handles.reserve(numWorkers);
        
        for (size_t i = 0; i < numWorkers; ++i) {
            size_t jobBegin = begin + i * jobsPerWorker;
            size_t jobEnd = std::min(jobBegin + jobsPerWorker, end);
            
            if (jobBegin >= end) break;
            
            handles.push_back(addJob([=]() {
                for (size_t j = jobBegin; j < jobEnd; ++j) {
                    func(j);
                }
            }));
        }
        
        // Wait for all parallel jobs to complete
        for (auto& handle : handles) {
            handle.wait();
        }
    }

    /**
     * Wait for all jobs to complete
     */
    void waitForAll() {
        std::unique_lock<std::mutex> lock(m_completionMutex);
        m_completionCondition.wait(lock, [this] {
            return m_activeJobs == 0;
        });
    }

    /**
     * Get the number of worker threads
     */
    size_t getWorkerCount() const {
        return m_workers.size();
    }

    /**
     * Get the number of active jobs
     */
    size_t getActiveJobCount() const {
        return m_activeJobs;
    }

    /**
     * Check if the job system is initialized
     */
    bool isInitialized() const { return m_initialized; }

private:
    /**
     * Worker thread function
     */
    void workerThread() {
        while (true) {
            std::shared_ptr<Job> job;

            {
                std::unique_lock<std::mutex> lock(m_queueMutex);

                // Wait for a ready job or shutdown. Only jobs whose dependency
                // counters reached 0 are enqueued, so the worker never blocks
                // on (or re-checks) a dependency -- this is what prevents the
                // addJobWithDeps starvation deadlock.
                m_condition.wait(lock, [this] {
                    return m_stop || !m_readyJobs.empty();
                });

                if (m_stop && m_readyJobs.empty()) {
                    return;
                }

                // Get the highest priority ready job
                job = m_readyJobs.top();
                m_readyJobs.pop();
            }

            // Execute the job (the worker is never blocked on a dependency)
            m_activeJobs.fetch_add(1, std::memory_order_relaxed);
            if (job->func) job->func();
            if (job->promise) job->promise->set_value();  // fulfill waiters
            m_activeJobs.fetch_sub(1, std::memory_order_relaxed);

            // Fan out completion: decrement each dependent's pending counter;
            // enqueue the dependent the instant its last dependency finishes.
            for (auto& weakChild : job->dependents) {
                if (auto child = weakChild.lock()) {
                    if (child->pendingDeps.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                        {
                            std::unique_lock<std::mutex> lock(m_queueMutex);
                            m_readyJobs.push(child);
                        }
                        m_condition.notify_one();
                    }
                }
            }

            m_completionCondition.notify_all();
        }
    }

    /**
     * Priority comparator for jobs (operates on shared Job nodes)
     */
    struct JobPriorityComparator {
        bool operator()(const std::shared_ptr<Job>& a, const std::shared_ptr<Job>& b) const {
            return a->priority < b->priority;  // Higher priority = processed first
        }
    };

    std::vector<std::thread> m_workers;
    std::priority_queue<std::shared_ptr<Job>, std::vector<std::shared_ptr<Job>>, JobPriorityComparator> m_readyJobs;

    std::mutex m_queueMutex;
    std::condition_variable m_condition;

    std::mutex m_completionMutex;
    std::condition_variable m_completionCondition;

    std::atomic<bool> m_stop;
    std::atomic<size_t> m_activeJobs;
    
    bool m_initialized = false;
};

/**
 * System Executor - Manages parallel execution of ECS systems
 */
class SystemExecutor {
public:
    /**
     * System execution group
     * Systems in the same group can run in parallel
     */
    struct ExecutionGroup {
        std::vector<class System*> systems;
        std::string name;
    };

    SystemExecutor(JobSystem& jobSystem)
        : m_jobSystem(jobSystem)
    {
    }

    /**
     * Add a system to an execution group
     * @param system The system to add
     * @param groupID The group ID (systems in same group run in parallel)
     */
    void addSystem(System* system, int groupID = 0) {
        if (groupID >= static_cast<int>(m_groups.size())) {
            m_groups.resize(groupID + 1);
        }
        m_groups[groupID].systems.push_back(system);
    }

    /**
     * Execute all systems in order
     * Systems within each group execute in parallel
     */
    void update(float deltaTime) {
        std::vector<JobSystem::JobHandle> groupHandles;
        groupHandles.reserve(m_groups.size());
        
        for (auto& group : m_groups) {
            // Create a job for this group
            // By-value capture of `this` + `group` + deltaTime: self-contained job.
            // `group` is copied (no dangling ref into the local m_groups vector); `this` is
            // needed to reach m_jobSystem. See World::updateParallel for the same rationale.
            auto handle = m_jobSystem.addJob([this, group, deltaTime]() {
                // Execute all systems in this group in parallel
                std::vector<std::function<void()>> systemJobs;
                systemJobs.reserve(group.systems.size());
                
                for (auto* system : group.systems) {
                    if (system->isEnabled()) {
                        systemJobs.push_back([system, deltaTime]() {
                            system->update(deltaTime);
                        });
                    }
                }
                
                // Execute system jobs in parallel
                if (!systemJobs.empty()) {
                    m_jobSystem.addJobsAndWait(std::move(systemJobs));
                }
            });
            
            groupHandles.push_back(std::move(handle));
        }
        
        // Wait for all groups to complete
        for (auto& handle : groupHandles) {
            handle.wait();
        }
    }

    /**
     * Render all systems (sequential, rendering is typically GPU-bound)
     */
    void render() {
        for (auto& group : m_groups) {
            for (auto* system : group.systems) {
                if (system->isEnabled()) {
                    system->render();
                }
            }
        }
    }

    /**
     * Clear all execution groups
     */
    void clear() {
        m_groups.clear();
    }

    /**
     * Get the number of execution groups
     */
    size_t getGroupCount() const {
        return m_groups.size();
    }

private:
    JobSystem& m_jobSystem;
    std::vector<ExecutionGroup> m_groups;
};

} // namespace ecs
