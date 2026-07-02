/**
 * @file GPUProfiler.cpp
 * @brief GPUProfiler implementation
 */

#include "Render/Debug/GPUProfiler.h"
#include "Core/Log.h"

namespace RVX
{

GPUProfiler::~GPUProfiler()
{
    Shutdown();
}

void GPUProfiler::Initialize(IRHIDevice* device)
{
    if (m_device)
    {
        RVX_CORE_WARN("GPUProfiler: Already initialized");
        return;
    }

    if (!device)
    {
        RVX_CORE_ERROR("GPUProfiler: Cannot initialize without an RHI device");
        return;
    }

    m_device = device;
    m_currentScopes.reserve(32);
    m_results.reserve(32);

    RHIQueryPoolDesc queryDesc;
    queryDesc.type = RHIQueryType::Timestamp;
    queryDesc.count = MaxQueries;
    queryDesc.debugName = "GPUProfilerTimestampQueries";
    m_queryPool = m_device->CreateQueryPool(queryDesc);
    m_timestampQueriesSupported = m_queryPool != nullptr;
    if (!m_timestampQueriesSupported)
    {
        RVX_CORE_WARN("GPUProfiler: Timestamp queries are unavailable; GPU profiling disabled");
    }

    RVX_CORE_DEBUG("GPUProfiler: Initialized");
}

void GPUProfiler::Shutdown()
{
    if (!m_device)
        return;

    m_currentScopes.clear();
    m_results.clear();
    m_scopeTimings.clear();
    m_queryPool.Reset();
    m_timestampQueriesSupported = false;
    m_device = nullptr;

    RVX_CORE_DEBUG("GPUProfiler: Shutdown");
}

void GPUProfiler::BeginFrame()
{
    if (!m_enabled || !m_timestampQueriesSupported)
        return;

    m_currentScopes.clear();
    m_currentDepth = 0;
    m_nextQueryIndex = 0;
}

void GPUProfiler::EndFrame()
{
    if (!m_enabled || !m_timestampQueriesSupported)
        return;

    m_results.clear();
    m_frameTimeMs = 0.0f;
}

void GPUProfiler::BeginScope(RHICommandContext& ctx, const char* name)
{
    if (!m_enabled || !m_timestampQueriesSupported)
        return;

    (void)ctx;  // Would write timestamp query

    ScopeData scope;
    scope.name = name;
    scope.startQueryIndex = m_nextQueryIndex++;
    scope.depth = m_currentDepth++;

    m_currentScopes.push_back(scope);
}

void GPUProfiler::EndScope(RHICommandContext& ctx)
{
    if (!m_enabled || !m_timestampQueriesSupported || m_currentScopes.empty())
        return;

    (void)ctx;  // Would write timestamp query

    // Find the last scope at current depth
    for (auto it = m_currentScopes.rbegin(); it != m_currentScopes.rend(); ++it)
    {
        if (it->depth == m_currentDepth - 1 && it->endQueryIndex == 0)
        {
            it->endQueryIndex = m_nextQueryIndex++;
            break;
        }
    }

    if (m_currentDepth > 0)
    {
        m_currentDepth--;
    }
}

float GPUProfiler::GetScopeTimeMs(const char* name) const
{
    auto it = m_scopeTimings.find(name);
    if (it != m_scopeTimings.end())
    {
        return it->second;
    }
    return 0.0f;
}

GPUProfiler::Stats GPUProfiler::GetStats() const
{
    Stats stats;
    stats.activeScopeCount = static_cast<uint32>(m_currentScopes.size());
    stats.queryCount = m_nextQueryIndex;
    stats.avgFrameTimeMs = m_frameTimeMs;  // Would be averaged over m_averageFrames
    return stats;
}

} // namespace RVX
