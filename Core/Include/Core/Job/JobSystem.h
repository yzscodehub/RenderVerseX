#pragma once

/**
 * @file JobSystem.h
 * @brief High-level job/task system for parallel execution
 */

#include "Core/Job/ThreadPool.h"
#include <memory>
#include <atomic>

namespace RVX
{
    /**
     * @brief Job priority levels
     */
    enum class JobPriority
    {
        Low,
        Normal,
        High,
        Critical
    };

    /**
     * @brief Handle to a submitted job
     */
    class JobHandle
    {
    public:
        JobHandle() = default;

        /// Check if the job has completed
        bool IsComplete() const 
        { 
            return m_completed ? m_completed->load() : true; 
        }

        /// Wait for the job to complete
        void Wait() const
        {
            if (m_future.valid())
            {
                m_future.wait();
            }
        }

    private:
        friend class JobSystem;
        
        std::shared_future<void> m_future;
        std::shared_ptr<std::atomic<bool>> m_completed;
    };

    /**
     * @brief High-level job system for parallel task execution
     * 
     * The JobSystem provides a convenient interface for submitting
     * parallel work. It wraps the ThreadPool with additional features
     * like job handles and priority scheduling.
     * 
     * Usage:
     * @code
     * // Get the global job system
     * JobSystem& jobs = JobSystem::Get();
     * 
     * // Submit a job
     * JobHandle handle = jobs.Submit([]() {
     *     DoExpensiveWork();
     * });
     * 
     * // Continue with other work...
     * 
     * // Wait for completion
     * handle.Wait();
     * 
     * // Or submit many jobs and wait for all
     * std::vector<JobHandle> handles;
     * for (int i = 0; i < 100; ++i) {
     *     handles.push_back(jobs.Submit([i]() {
     *         ProcessItem(i);
     *     }));
     * }
     * jobs.WaitAll(handles);
     * @endcode
     */
    class JobSystem
    {
    public:
        /// Get the global JobSystem instance
        static JobSystem& Get();

        /**
         * @brief Initialize the job system
         * @param numWorkers Number of worker threads (0 = auto)
         */
        void Initialize(size_t numWorkers = 0);

        /// Shutdown the job system
        void Shutdown();

        /// Check if initialized
        bool IsInitialized() const { return m_threadPool != nullptr; }

        /**
         * @brief Submit a job for execution
         * @param func Function to execute
         * @return Handle to track job completion
         */
        template<typename F>
        JobHandle Submit(F&& func)
        {
            if (!m_threadPool)
            {
                // Execute synchronously if not initialized
                func();
                return JobHandle{};
            }

            auto completed = std::make_shared<std::atomic<bool>>(false);
            
            auto future = m_threadPool->Submit([func = std::forward<F>(func), completed]() {
                func();
                completed->store(true);
            });

            JobHandle handle;
            handle.m_future = future.share();
            handle.m_completed = completed;
            return handle;
        }

        /**
         * @brief Submit a job that returns a value
         */
        template<typename F>
        auto SubmitWithResult(F&& func) -> std::future<std::invoke_result_t<F>>
        {
            if (!m_threadPool)
            {
                std::promise<std::invoke_result_t<F>> promise;
                promise.set_value(func());
                return promise.get_future();
            }

            return m_threadPool->Submit(std::forward<F>(func));
        }

        /**
         * @brief Run a parallel for loop
         * @param start Start index (inclusive)
         * @param end End index (exclusive)
         * @param func Function(index) to call for each item
         * @param batchSize Number of items per job (0 = auto)
         */
        template<typename F>
        void ParallelFor(size_t start, size_t end, F&& func, size_t batchSize = 0)
        {
            if (start >= end)
                return;

            if (!m_threadPool || end - start == 1)
            {
                for (size_t i = start; i < end; ++i)
                {
                    func(i);
                }
                return;
            }

            if (batchSize == 0)
            {
                batchSize = std::max(size_t(1), (end - start) / (m_threadPool->GetThreadCount() * 4));
            }

            std::vector<std::future<void>> futures;
            
            for (size_t batchStart = start; batchStart < end; batchStart += batchSize)
            {
                size_t batchEnd = std::min(batchStart + batchSize, end);
                
                futures.push_back(m_threadPool->Submit([&func, batchStart, batchEnd]() {
                    for (size_t i = batchStart; i < batchEnd; ++i)
                    {
                        func(i);
                    }
                }));
            }

            for (auto& future : futures)
            {
                future.wait();
            }
        }

        /**
         * @brief Wait for multiple jobs to complete
         */
        void WaitAll(const std::vector<JobHandle>& handles)
        {
            for (const auto& handle : handles)
            {
                handle.Wait();
            }
        }

        /**
         * @brief Wait for all pending jobs to complete
         */
        void WaitAllPending()
        {
            if (m_threadPool)
            {
                m_threadPool->WaitAll();
            }
        }

        /**
         * @brief Get the number of worker threads
         */
        size_t GetWorkerCount() const
        {
            return m_threadPool ? m_threadPool->GetThreadCount() : 0;
        }

    private:
        JobSystem() = default;
        ~JobSystem() { Shutdown(); }

        JobSystem(const JobSystem&) = delete;
        JobSystem& operator=(const JobSystem&) = delete;

        std::unique_ptr<ThreadPool> m_threadPool;
    };

} // namespace RVX
