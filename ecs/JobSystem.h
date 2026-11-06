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
#include <chrono>
#include "profiler.h"   // PROFILE_CPU_SCOPE / thread-safe Profiler (ecs -> editor utility)

namespace ecs {

/**
 * Job System - Multi-threaded task execution for parallel system updates
 * 
 * Provides a thread pool with work stealing for efficient parallel execution
 * of ECS system updates.
 */
class JobSystem {
public:
    // Tuning for JobHandle::wait()'s bounded backoff (see wait() below):
    //  - how many yield-spins to do before sleeping when there is no stealable work;
    //  - how long to nap for before re-checking the queue.
    static constexpr int kSpinBeforeSleep = 16;
    static constexpr int kSleepOnIdleUs  = 50;

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
         * Wait for the job to complete.
         *
         * Rather than blocking the calling thread (which starves the pool and
         * deadlocks nested dependencies), this drains other ready jobs from the
         * system queue inline via work-stealing until THIS job is finished. When
         * a parent job calls wait() on its children, the waiting thread becomes
         * a helper and runs those very children itself instead of sleeping.
         */
        void wait() {
            if (!m_future.valid()) {
                return;
            }

            // Loop until this specific job is ready. While waiting, help the pool
            // by executing other ready tasks inline. This breaks the nested-
            // starvation deadlock where a parent holds a worker thread while its
            // child jobs sit in m_readyJobs waiting for a free thread that will
            // never come.
            //
            // Bounded backoff: while there is stealable work, drain it inline
            // (cache-hot, resets the backoff so we stay aggressive). When the
            // queue is empty for a while, stop busy-yielding and take a short
            // nap so we don't pin a core on a long single-job wait.
            int idleSpins = 0;
            while (m_future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                // If a pointer to the system is available and there is work to
                // do, do it!
                if (m_system && m_system->executeNextJob()) {
                    idleSpins = 0;          // made progress -> keep stealing eagerly
                    continue;               // successfully helped run a job; re-check our status
                }

                // Queue is empty but our job isn't done yet. Spin-yield briefly
                // to stay cache-hot and respond instantly to newly-ready work,
                // then fall back to a short sleep to avoid burning 100% CPU on
                // a long wait with nothing to steal.
                if (idleSpins < kSpinBeforeSleep) {
                    std::this_thread::yield();
                    ++idleSpins;
                } else {
                    std::this_thread::sleep_for(std::chrono::microseconds(kSleepOnIdleUs));
                }
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
        JobSystem* m_system = nullptr;       // back-pointer enabling inline work-stealing
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
        
        // Pre-allocate the Job object pool (default 4096 slots; overflow falls
        // back to the heap). Initialized before workers so addJob is usable
        // immediately, and held by shared_ptr so it outlives any Job handle.
        m_pool = std::make_shared<JobPool>();
        m_pool->init(4096);

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
        auto job = allocateJob();
        auto promise = std::make_shared<std::promise<void>>();
        job->func = std::move(func);
        job->priority = priority;
        job->promise = promise;
        job->pendingDeps.store(0, std::memory_order_release);

        JobHandle handle;
        handle.m_future = promise->get_future().share();
        handle.m_job = job;
        handle.m_system = this;  // link the system context so wait() can help process jobs

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
        auto job = allocateJob();
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
        handle.m_system = this;  // link the system context so wait() can help process jobs
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
     * Internal fixed-size object pool for Job nodes.
     *
     * Why this matters at high frame rates: SystemExecutor::update / World
     * submit one Job per system (and per parallelFor chunk) every frame, often
     * hundreds/thousands of times per second. Each std::make_shared<Job>
     * allocates+constructs the Job payload on the global heap and frees it on
     * completion. By caching Job slots in a pre-reserved arena we turn the
     * payload allocation into an O(1) free-list pop (no malloc/lock on the
     * hot path) and pack hot Job structs contiguously for cache-friendly queue
     * iteration.
     *
     * Note: std::shared_ptr needs a separate control block per Job, so the
     * block is still heap-allocated; we only recycle the Job payload itself.
     * (To eliminate that last allocation too, see the recommended follow-up at
     * the bottom of this file: std::allocate_shared over a pooled allocator,
     * or an intrusive JobId-based lifetime.)
     *
     * Ownership: m_pool is held by shared_ptr and the custom deleter captures a
     * ref to it, so the pool stays alive as long as any pooled Job node exists
     * -- even if the JobSystem itself is destroyed first. No dangling-deleter
     * hazard.
     */
    struct JobPool {
        struct Node {
            // Raw storage sized/aligned for a Job; constructed via placement-new.
            // Kept as a standalone struct (no `next` field) so we never touch
            // uninitialized memory -- the free-list is tracked as indices instead.
            alignas(Job) unsigned char storage[sizeof(Job)];
        };

        std::unique_ptr<Node[]> nodes;        // owning arena
        std::vector<size_t> freeStack;        // LIFO free-list of node indices
        std::mutex mtx;
        size_t capacity = 0;

        void init(size_t cap) {
            capacity = cap;
            nodes = std::make_unique<Node[]>(cap);
            freeStack.reserve(cap);
            for (size_t i = cap; i-- > 0;) {
                freeStack.push_back(i);       // seed the free-list (high index first)
            }
        }

        Job* allocate() {
            std::lock_guard<std::mutex> lock(mtx);
            if (freeStack.empty()) {
                return nullptr;               // drained: caller falls back to the heap
            }
            const size_t idx = freeStack.back();
            freeStack.pop_back();
            Node* node = &nodes[idx];
            return new (node->storage) Job(); // construct a Job in-place
        }

        void deallocate(Job* ptr) {
            if (!ptr) return;
            std::lock_guard<std::mutex> lock(mtx);
            ptr->~Job();                       // destroy Job contents first
            Node* node = reinterpret_cast<Node*>(ptr); // Job lives at offset 0 of Node
            const size_t idx = static_cast<size_t>(node - nodes.get());
            freeStack.push_back(idx);
        }
    };

    /**
     * Allocate a Job node from the pool when possible, falling back to the heap
     * allocator if the pool is momentarily exhausted (so a job is NEVER
     * dropped). The returned shared_ptr keeps the pool alive via a captured
     * ref, so pooled nodes are always safe to return.
     */
    std::shared_ptr<Job> allocateJob() {
        if (m_pool && m_pool->capacity > 0) {
            if (Job* p = m_pool->allocate()) {
                return std::shared_ptr<Job>(p, [pool = m_pool](Job* q) {
                    if (q) pool->deallocate(q);
                });
            }
        }
        return std::make_shared<Job>();       // overflow / uninit -> ordinary heap
    }

    /**
     * Helper to pop and execute the highest-priority ready job.
     *
     * Encapsulated here so both the worker loop and JobHandle::wait() reuse the
     * exact same pop + execute + fan-out logic. Returns true if a job was
     * executed, false if the queue was empty.
     */
    bool executeNextJob() {
        std::shared_ptr<Job> job;

        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            if (m_readyJobs.empty()) {
                return false;
            }

            job = m_readyJobs.top();
            m_readyJobs.pop();
        }

        // Execute the job (the running thread never blocks on a dependency)
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
        return true;
    }

    /**
     * Worker thread function
     */
    void workerThread() {
        while (true) {
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
            }

            // Pull and run a job using our shared helper.
            executeNextJob();
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
    std::shared_ptr<JobPool> m_pool;       // object pool for Job payloads (ref-counted)
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
     * Execute all systems in order.
     *
     * Systems within each execution group run in parallel (one job per
     * system); groups are drained (barrier) sequentially to respect broad
     * inter-group dependencies.
     *
     * This is the flattened layout: instead of spawning one wrapper job per
     * group that *then* calls addJobsAndWait() internally (an extra
     * allocation + enqueue/dequeue + a worker picking up the wrapper before
     * any real work starts), the caller dispatches each system job directly
     * and waits inline. That removes one task per group from the pipeline and
     * lets the calling thread immediately help drain the pool via the
     * work-stealing wait(), which is what keeps nested parallel regions
     * deadlock-free under load.
     */
    void update(float deltaTime) {
        // The Group Layer is sequential: each group fully completes before the
        // next starts (preserves, e.g., PhysicsSystem before the system that
        // reads its results).
        for (auto& group : m_groups) {
            if (group.systems.empty()) continue;

            // forProfiler Phase 3: instrument the per-group dispatch. The
            // Profiler is now thread-safe, so this scope (running on the
            // calling thread) and the per-system scopes below (running on
            // worker threads via work-stealing) safely coexist.
            PROFILE_CPU_SCOPE(("SystemExecutor.Group." + group.name).c_str());

            std::vector<JobSystem::JobHandle> systemHandles;
            systemHandles.reserve(group.systems.size());

            // 1. Kick off all enabled systems within this group in parallel.
            //    By-value capture of `system` + `deltaTime` yields a fully
            //    self-contained task (no dangling refs into m_groups).
            for (auto* system : group.systems) {
                if (system->isEnabled()) {
                    auto handle = m_jobSystem.addJob([system, deltaTime]() {
                        PROFILE_CPU_SCOPE(("System." + std::string(system->getName())).c_str());
                        system->update(deltaTime);
                    });
                    systemHandles.push_back(std::move(handle));
                }
            }

            // 2. Inline work-stealing wait. The calling thread does NOT sleep
            //    here — it drains other ready jobs from the pool until every
            //    system in this group is finished, then advances.
            for (auto& handle : systemHandles) {
                handle.wait();
            }
            // Barrier: all systems at this group are guaranteed complete
            // before the next group begins.
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
