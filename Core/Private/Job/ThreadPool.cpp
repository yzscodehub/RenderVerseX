/**
 * @file ThreadPool.cpp
 * @brief ThreadPool implementation
 */

#include "Core/Job/ThreadPool.h"

namespace RVX
{

ThreadPool::ThreadPool(size_t numThreads)
{
    if (numThreads == 0)
    {
        numThreads = std::thread::hardware_concurrency();
        if (numThreads == 0)
        {
            numThreads = 4;  // Fallback
        }
    }

    m_threads.reserve(numThreads);
    for (size_t i = 0; i < numThreads; ++i)
    {
        m_threads.emplace_back(&ThreadPool::WorkerLoop, this);
    }
}

ThreadPool::~ThreadPool()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = true;
    }

    m_condition.notify_all();

    for (auto& thread : m_threads)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }
}

void ThreadPool::SubmitDetached(std::function<void()> task)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping)
        {
            return;
        }
        m_tasks.emplace(std::move(task));
    }
    m_condition.notify_one();
}

void ThreadPool::WaitAll()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_completionCondition.wait(lock, [this]() {
        return m_tasks.empty() && m_activeTasks == 0;
    });
}

size_t ThreadPool::GetPendingCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_tasks.size() + m_activeTasks;
}

void ThreadPool::WorkerLoop()
{
    while (true)
    {
        std::function<void()> task;

        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_condition.wait(lock, [this]() {
                return m_stopping || !m_tasks.empty();
            });

            if (m_stopping && m_tasks.empty())
            {
                return;
            }

            task = std::move(m_tasks.front());
            m_tasks.pop();
            ++m_activeTasks;
        }

        task();

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            --m_activeTasks;
        }
        m_completionCondition.notify_all();
    }
}

} // namespace RVX
