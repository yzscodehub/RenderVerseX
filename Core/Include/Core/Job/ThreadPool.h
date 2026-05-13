#pragma once

/**
 * @file ThreadPool.h
 * @brief Simple thread pool implementation for parallel task execution
 */

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <atomic>

namespace RVX
{
    // Forward declaration
    enum class JobPriority;

    /**
     * @brief Task wrapper with priority
     */
    struct Task
    {
        std::function<void()> func;
        int32_t priority = 0;  // Higher = more priority

        // For priority_queue (higher priority first)
        bool operator<(const Task& other) const
        {
            return priority < other.priority;
        }
    };

    /**
     * @brief Simple thread pool for parallel task execution
     *
     * The thread pool manages a fixed number of worker threads
     * and executes tasks submitted to it.
     *
     * Usage:
     * @code
     * ThreadPool pool(4);  // 4 worker threads
     *
     * auto future = pool.Submit([]() {
     *     return ExpensiveComputation();
     * });
     *
     * auto result = future.get();  // Wait for result
     * @endcode
     */
    class ThreadPool
    {
    public:
        /**
         * @brief Create a thread pool
         * @param numThreads Number of worker threads (0 = hardware concurrency)
         */
        explicit ThreadPool(size_t numThreads = 0);

        /// Destructor waits for all tasks to complete
        ~ThreadPool();

        // Non-copyable
        ThreadPool(const ThreadPool&) = delete;
        ThreadPool& operator=(const ThreadPool&) = delete;

        /**
         * @brief Submit a task for execution (default priority)
         * @return Future for the task result
         */
        template<typename F, typename... Args>
        auto Submit(F&& func, Args&&... args)
            -> std::future<std::invoke_result_t<F, Args...>>
        {
            return SubmitImpl(std::forward<F>(func), 0, std::forward<Args>(args)...);
        }

        /**
         * @brief Submit a task with priority
         * @param priority JobPriority (Low=0, Normal=1, High=2, Critical=3)
         * @return Future for the task result
         */
        template<typename F, typename... Args>
        auto Submit(F&& func, JobPriority priority, Args&&... args)
            -> std::future<std::invoke_result_t<F, Args...>>
        {
            return SubmitImpl(std::forward<F>(func), static_cast<int32_t>(priority), std::forward<Args>(args)...);
        }

        /**
         * @brief Submit a task without caring about the result
         */
        void SubmitDetached(std::function<void()> task);

        /**
         * @brief Wait for all pending tasks to complete
         */
        void WaitAll();

        /**
         * @brief Get the number of worker threads
         */
        size_t GetThreadCount() const { return m_threads.size(); }

        /**
         * @brief Get the number of pending tasks
         */
        size_t GetPendingCount() const;

        /**
         * @brief Check if there are pending tasks
         */
        bool HasPendingTasks() const { return GetPendingCount() > 0; }

    private:
        // Implementation for Submit
        template<typename F>
        auto SubmitImpl(F&& func, int32_t priority)
            -> std::future<std::invoke_result_t<F>>
        {
            using ReturnType = std::invoke_result_t<F>;

            auto task = std::make_shared<std::packaged_task<ReturnType()>>(
                std::forward<F>(func)
            );

            std::future<ReturnType> future = task->get_future();

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_stopping)
                {
                    throw std::runtime_error("Submit called on stopped ThreadPool");
                }
                m_tasks.emplace(Task{[task]() { (*task)(); }, priority});
            }

            m_condition.notify_one();
            return future;
        }

        // Implementation for Submit with Args (variadic)
        template<typename F, typename... Args>
        auto SubmitImpl(F&& func, int32_t priority, Args&&... args)
            -> std::future<std::invoke_result_t<F, Args...>>
        {
            using ReturnType = std::invoke_result_t<F, Args...>;

            auto task = std::make_shared<std::packaged_task<ReturnType()>>(
                std::bind(std::forward<F>(func), std::forward<Args>(args)...)
            );

            std::future<ReturnType> future = task->get_future();

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_stopping)
                {
                    throw std::runtime_error("Submit called on stopped ThreadPool");
                }
                m_tasks.emplace(Task{[task]() { (*task)(); }, priority});
            }

            m_condition.notify_one();
            return future;
        }

        void WorkerLoop();

        std::vector<std::thread> m_threads;
        std::priority_queue<Task> m_tasks;

        mutable std::mutex m_mutex;
        std::condition_variable m_condition;
        std::condition_variable m_completionCondition;

        std::atomic<bool> m_stopping{false};
        std::atomic<size_t> m_activeTasks{0};
    };

} // namespace RVX
