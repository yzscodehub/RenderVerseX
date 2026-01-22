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

    m_device = device;
    m_currentScopes.reserve(32);
    m_results.reserve(32);

    // TODO: Create timestamp query pool based on RHI capabilities
    // This would use device->CreateQueryPool() when available

    RVX_CORE_DEBUG("GPUProfiler: Initialized");
}

void GPUProfiler::Shutdown()
{
    if (!m_device)
        return;

    m_currentScopes.clear();
    m_results.clear();
    m_scopeTimings.clear();
    m_device = nullptr;

    RVX_CORE_DEBUG("GPUProfiler: Shutdown");
}

void GPUProfiler::BeginFrame()
{
    if (!m_enabled)
        return;

    m_currentScopes.clear();
    m_currentDepth = 0;
    m_nextQueryIndex = 0;
}

void GPUProfiler::EndFrame()
{
    if (!m_enabled)
        return;

    // Read back timestamp query results from previous frame
    // TODO: Actual implementation would:
    // 1. Wait for query results to be available
    // 2. Read timestamp values
    // 3. Convert to milliseconds using GPU timestamp frequency
    // 4. Store in m_results

    // For now, just clear and prepare for next frame
    m_results.clear();
    
    for (const auto& scope : m_currentScopes)
    {
        GPUTimingResult result;
        result.name = scope.name;
        result.depth = scope.depth;
        result.gpuTimeMs = 0.0f;  // Would be calculated from timestamps
        m_results.push_back(result);
    }

    // Calculate total frame time
    if (!m_results.empty())
    {
        m_frameTimeMs = 0.0f;
        for (const auto& result : m_results)
        {
            if (result.depth == 0)
            {
                m_frameTimeMs += result.gpuTimeMs;
            }
        }
    }
}

void GPUProfiler::BeginScope(RHICommandContext& ctx, const char* name)
{
    if (!m_enabled)
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
    if (!m_enabled || m_currentScopes.empty())
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
