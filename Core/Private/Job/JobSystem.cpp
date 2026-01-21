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
        RVX_CORE_WARN("JobSystem already initialized");
        return;
    }

    m_threadPool = std::make_unique<ThreadPool>(numWorkers);
    RVX_CORE_INFO("JobSystem initialized with {} worker threads", m_threadPool->GetThreadCount());
}

void JobSystem::Shutdown()
{
    if (m_threadPool)
    {
        RVX_CORE_DEBUG("JobSystem shutting down...");
        m_threadPool->WaitAll();
        m_threadPool.reset();
        RVX_CORE_INFO("JobSystem shutdown complete");
    }
}

} // namespace RVX
