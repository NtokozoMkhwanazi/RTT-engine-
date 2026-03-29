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
    struct JobHandle {
        JobHandle() : m_future(nullptr) {}

        // Move constructor and assignment
        JobHandle(JobHandle&& other) noexcept : m_future(std::move(other.m_future)) {}
        JobHandle& operator=(JobHandle&& other) noexcept {
            m_future = std::move(other.m_future);
            return *this;
        }

        // Copy constructor and assignment (shared state via shared_ptr)
        JobHandle(const JobHandle& other) = default;
        JobHandle& operator=(const JobHandle& other) = default;

        /**
         * Wait for the job to complete
         */
        void wait() {
            if (m_future && m_future->valid()) {
                m_future->wait();
            }
        }

        /**
         * Check if the job is complete
         */
        bool isComplete() const {
            if (m_future && m_future->valid()) {
                return m_future->wait_for(std::chrono::seconds(0)) == std::future_status::ready;
            }
            return true;
        }

    private:
        friend class JobSystem;
        std::shared_ptr<std::future<void>> m_future;
    };

    /**
     * Job with dependencies
     */
    struct Job {
        std::function<void()> func;
        uint32_t priority = 0;  // Higher = more urgent
        std::shared_ptr<std::promise<void>> promise;  // For completion tracking
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
            // Default to hardware concurrency minus 1 (leave one for main thread)
            numThreads = std::max(1u, std::thread::hardware_concurrency() - 1);
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
        std::promise<void> promise;
        JobHandle handle;
        handle.m_future = std::make_shared<std::future<void>>(promise.get_future());

        {
            std::unique_lock<std::mutex> lock(m_queueMutex);

            // Wrap function to set promise when done
            auto wrappedFunc = [func = std::move(func), promise = std::make_shared<std::promise<void>>(std::move(promise))]() mutable {
                func();
                promise->set_value();
            };

            Job job;
            job.func = std::move(wrappedFunc);
            job.priority = priority;
            job.promise = nullptr;  // Not used for simple jobs

            m_jobs.push(std::move(job));
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
        std::promise<void> promise;
        JobHandle handle;
        handle.m_future = std::make_shared<std::future<void>>(promise.get_future());

        {
            std::unique_lock<std::mutex> lock(m_queueMutex);

            // Store dependencies as shared futures (copy from handles)
            std::vector<std::shared_ptr<std::future<void>>> depFutures;
            depFutures.reserve(dependencies.size());
            for (auto& dep : dependencies) {
                if (dep.m_future && dep.m_future->valid()) {
                    depFutures.push_back(dep.m_future);
                }
            }

            // Wrap function to wait for dependencies first
            auto wrappedFunc = [func = std::move(func),
                                promise = std::make_shared<std::promise<void>>(std::move(promise)),
                                deps = std::move(depFutures)]() mutable {
                // Wait for all dependencies
                for (auto& dep : deps) {
                    if (dep && dep->valid()) {
                        dep->wait();
                    }
                }
                func();
                promise->set_value();
            };

            Job job;
            job.func = std::move(wrappedFunc);
            job.priority = priority;
            job.promise = nullptr;

            m_jobs.push(std::move(job));
        }

        m_condition.notify_one();
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
            Job job;
            
            {
                std::unique_lock<std::mutex> lock(m_queueMutex);
                
                // Wait for a job or shutdown
                m_condition.wait(lock, [this] {
                    return m_stop || !m_jobs.empty();
                });
                
                if (m_stop && m_jobs.empty()) {
                    return;
                }
                
                // Get the highest priority job
                job = std::move(m_jobs.top());
                m_jobs.pop();
            }
            
            // Execute the job
            m_activeJobs++;
            job.func();
            m_activeJobs--;
            
            // Notify completion waiters
            m_completionCondition.notify_all();
        }
    }

    /**
     * Priority comparator for jobs
     */
    struct JobPriorityComparator {
        bool operator()(const Job& a, const Job& b) const {
            return a.priority < b.priority;  // Higher priority = processed first
        }
    };

    std::vector<std::thread> m_workers;
    std::priority_queue<Job, std::vector<Job>, JobPriorityComparator> m_jobs;
    
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
            auto handle = m_jobSystem.addJob([=, &group]() {
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
