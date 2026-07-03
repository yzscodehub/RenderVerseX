/**
 * @file JobSystem.cpp
 * @brief JobSystem implementation
 */

#include "Core/Job/JobSystem.h"
#include "Core/Log.h"

namespace RVX
{

JobSystem& JobSystem::Get()
{
    static JobSystem instance;
    return instance;
}

void JobSystem::Initialize(size_t numWorkers)
{
    if (m_threadPool)
    {
        if (Log::GetCoreLogger())
        {
            RVX_CORE_WARN("JobSystem already initialized");
        }
        return;
    }

    m_threadPool = std::make_unique<ThreadPool>(numWorkers);
    if (Log::GetCoreLogger())
    {
        RVX_CORE_INFO("JobSystem initialized with {} worker threads", m_threadPool->GetThreadCount());
    }
}

void JobSystem::Shutdown()
{
    if (m_threadPool)
    {
        if (Log::GetCoreLogger())
        {
            RVX_CORE_DEBUG("JobSystem shutting down...");
        }
        m_threadPool->WaitAll();
        m_threadPool.reset();
        if (Log::GetCoreLogger())
        {
            RVX_CORE_INFO("JobSystem shutdown complete");
        }
    }
}

void JobSystem::DispatchToMainThread(std::function<void()> callback)
{
    QueueCompletion(std::move(callback), JobCompletionDispatch::MainThread);
}

size_t JobSystem::ProcessMainThreadCompletions(size_t maxCallbacks)
{
    size_t processed = 0;

    while (processed < maxCallbacks)
    {
        std::function<void()> completion;
        {
            std::lock_guard<std::mutex> lock(m_mainThreadCompletionMutex);
            if (m_mainThreadCompletions.empty())
            {
                break;
            }

            completion = std::move(m_mainThreadCompletions.front());
            m_mainThreadCompletions.pop();
        }

        RunCompletion(std::move(completion));
        ++processed;
    }

    return processed;
}

size_t JobSystem::GetPendingMainThreadCompletionCount() const
{
    std::lock_guard<std::mutex> lock(m_mainThreadCompletionMutex);
    return m_mainThreadCompletions.size();
}

void JobSystem::QueueCompletion(std::function<void()> continuation, JobCompletionDispatch dispatch)
{
    if (!continuation)
    {
        return;
    }

    if (dispatch == JobCompletionDispatch::MainThread)
    {
        {
            std::lock_guard<std::mutex> lock(m_mainThreadCompletionMutex);
            m_mainThreadCompletions.push(std::move(continuation));
        }
        return;
    }

    RunCompletion(std::move(continuation));
}

void JobSystem::RunCompletion(std::function<void()> continuation)
{
    if (!continuation)
    {
        return;
    }

    try
    {
        continuation();
    }
    catch (const std::exception& e)
    {
        if (Log::GetCoreLogger())
        {
            RVX_CORE_ERROR("Job completion callback failed: {}", e.what());
        }
    }
    catch (...)
    {
        if (Log::GetCoreLogger())
        {
            RVX_CORE_ERROR("Job completion callback failed with an unknown exception");
        }
    }
}

} // namespace RVX
