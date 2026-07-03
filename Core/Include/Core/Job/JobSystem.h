#pragma once

/**
 * @file JobSystem.h
 * @brief High-level job/task system for parallel execution
 */

#include "Core/Job/ThreadPool.h"
#include "Core/Types.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <type_traits>
#include <vector>

namespace RVX
{
    /**
     * @brief Job priority levels
     * Higher values = higher priority
     */
    enum class JobPriority
    {
        Low = 0,
        Normal = 1,
        High = 2,
        Critical = 3
    };

    /**
     * @brief Thread/context used to run a job completion callback.
     */
    enum class JobCompletionDispatch
    {
        WorkerThread = 0,
        MainThread
    };

    /**
     * @brief Submission metadata for runtime jobs.
     */
    struct JobSubmissionDesc
    {
        JobPriority priority = JobPriority::Normal;
        std::string category;
        std::function<void()> continuation;
        JobCompletionDispatch completionDispatch = JobCompletionDispatch::WorkerThread;
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

        /// Wait for completion with a timeout.
        bool WaitFor(uint32 timeoutMs) const
        {
            if (!m_future.valid())
            {
                return true;
            }

            return m_future.wait_for(std::chrono::milliseconds(timeoutMs)) == std::future_status::ready;
        }

        /// Get the named category used for scheduling/diagnostics.
        const std::string& GetCategory() const { return m_category; }

        /// Get the requested priority.
        JobPriority GetPriority() const { return m_priority; }

    private:
        friend class JobSystem;
        
        std::shared_future<void> m_future;
        std::shared_ptr<std::atomic<bool>> m_completed;
        std::string m_category;
        JobPriority m_priority = JobPriority::Normal;
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
            return Submit(std::forward<F>(func), JobPriority::Normal);
        }

        /**
         * @brief Submit a job with priority
         * @param func Function to execute
         * @param priority Job priority
         * @return Handle to track job completion
         */
        template<typename F>
        JobHandle Submit(F&& func, JobPriority priority)
        {
            JobSubmissionDesc desc;
            desc.priority = priority;
            return Submit(std::forward<F>(func), desc);
        }

        /**
         * @brief Submit a job with diagnostic metadata and optional continuation.
         */
        template<typename F>
        JobHandle Submit(F&& func, const JobSubmissionDesc& desc)
        {
            if (!m_threadPool)
            {
                // Execute synchronously if not initialized
                try
                {
                    func();
                }
                catch (...)
                {
                    QueueCompletion(desc.continuation, desc.completionDispatch);
                    throw;
                }

                QueueCompletion(desc.continuation, desc.completionDispatch);

                JobHandle handle;
                handle.m_category = desc.category;
                handle.m_priority = desc.priority;
                return handle;
            }

            auto completed = std::make_shared<std::atomic<bool>>(false);
            auto continuation = desc.continuation;
            const JobCompletionDispatch completionDispatch = desc.completionDispatch;

            auto future = m_threadPool->Submit([this,
                                                func = std::forward<F>(func),
                                                completed,
                                                continuation = std::move(continuation),
                                                completionDispatch]() mutable {
                try
                {
                    func();
                }
                catch (...)
                {
                    completed->store(true, std::memory_order_release);
                    QueueCompletion(std::move(continuation), completionDispatch);
                    throw;
                }

                completed->store(true, std::memory_order_release);
                QueueCompletion(std::move(continuation), completionDispatch);
            }, desc.priority);

            JobHandle handle;
            handle.m_future = future.share();
            handle.m_completed = completed;
            handle.m_category = desc.category;
            handle.m_priority = desc.priority;
            return handle;
        }

        /**
         * @brief Submit a job that returns a value
         */
        template<typename F>
        auto SubmitWithResult(F&& func) -> std::future<std::invoke_result_t<F>>
        {
            JobSubmissionDesc desc;
            return SubmitWithResult(std::forward<F>(func), desc);
        }

        /**
         * @brief Submit a value-returning job with metadata and optional continuation.
         */
        template<typename F>
        auto SubmitWithResult(F&& func, const JobSubmissionDesc& desc) -> std::future<std::invoke_result_t<F>>
        {
            using ReturnType = std::invoke_result_t<F>;

            if (!m_threadPool)
            {
                std::promise<ReturnType> promise;
                try
                {
                    if constexpr (std::is_void_v<ReturnType>)
                    {
                        func();
                        promise.set_value();
                    }
                    else
                    {
                        promise.set_value(func());
                    }
                }
                catch (...)
                {
                    promise.set_exception(std::current_exception());
                }

                QueueCompletion(desc.continuation, desc.completionDispatch);
                return promise.get_future();
            }

            auto continuation = desc.continuation;
            const JobCompletionDispatch completionDispatch = desc.completionDispatch;

            return m_threadPool->Submit([this,
                                         func = std::forward<F>(func),
                                         continuation = std::move(continuation),
                                         completionDispatch]() mutable -> ReturnType {
                try
                {
                    if constexpr (std::is_void_v<ReturnType>)
                    {
                        func();
                        QueueCompletion(std::move(continuation), completionDispatch);
                    }
                    else
                    {
                        ReturnType result = func();
                        QueueCompletion(std::move(continuation), completionDispatch);
                        return result;
                    }
                }
                catch (...)
                {
                    QueueCompletion(std::move(continuation), completionDispatch);
                    throw;
                }
            }, desc.priority);
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
         * @brief Queue a callback for the next main-thread completion dispatch point.
         */
        void DispatchToMainThread(std::function<void()> callback);

        /**
         * @brief Run queued main-thread completions.
         * @return Number of callbacks dispatched.
         */
        size_t ProcessMainThreadCompletions(size_t maxCallbacks = std::numeric_limits<size_t>::max());

        /**
         * @brief Get queued main-thread completion count.
         */
        size_t GetPendingMainThreadCompletionCount() const;

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

        void QueueCompletion(std::function<void()> continuation, JobCompletionDispatch dispatch);
        void RunCompletion(std::function<void()> continuation);

        std::unique_ptr<ThreadPool> m_threadPool;
        mutable std::mutex m_mainThreadCompletionMutex;
        std::queue<std::function<void()>> m_mainThreadCompletions;
    };

} // namespace RVX
