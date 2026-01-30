/**
 * @file CPUProfiler.cpp
 * @brief CPU profiling implementation
 */

#include "Debug/CPUProfiler.h"
#include "Core/Log.h"
#include <algorithm>
#include <numeric>

namespace RVX
{
    // =========================================================================
    // Singleton
    // =========================================================================

    CPUProfiler& CPUProfiler::Get()
    {
        static CPUProfiler instance;
        return instance;
    }

    // =========================================================================
    // Lifecycle
    // =========================================================================

    void CPUProfiler::Initialize(uint32 averageFrames)
    {
        if (m_initialized)
        {
            return;
        }

        m_averageFrames = averageFrames;
        m_frameTimeHistory.reserve(averageFrames);
        m_activeScopeStack.reserve(64);
        m_currentFrameScopes.reserve(64);

        m_initialized = true;
        RVX_CORE_INFO("CPUProfiler initialized with {} frame averaging", averageFrames);
    }

    void CPUProfiler::Shutdown()
    {
        if (!m_initialized)
        {
            return;
        }

        m_activeScopeStack.clear();
        m_currentFrameScopes.clear();
        m_scopeStats.clear();
        m_frameTimeHistory.clear();
        m_lastFrameData = CPUFrameData{};

        m_initialized = false;
        RVX_CORE_INFO("CPUProfiler shutdown");
    }

    // =========================================================================
    // Frame Control
    // =========================================================================

    void CPUProfiler::BeginFrame()
    {
        if (!m_initialized || !m_enabled)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(m_mutex);

        m_inFrame = true;
        m_frameStartTime = Clock::now();
        m_currentFrameScopes.clear();
        m_activeScopeStack.clear();
        m_nextScopeId = 0;
    }

    void CPUProfiler::EndFrame()
    {
        if (!m_initialized || !m_enabled || !m_inFrame)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(m_mutex);

        // Calculate frame time
        auto frameEndTime = Clock::now();
        auto frameDuration = std::chrono::duration_cast<std::chrono::microseconds>(
            frameEndTime - m_frameStartTime
        );
        float frameTimeMs = frameDuration.count() / 1000.0f;

        // Update frame time history
        m_frameTimeHistory.push_back(frameTimeMs);
        if (m_frameTimeHistory.size() > m_averageFrames)
        {
            m_frameTimeHistory.erase(m_frameTimeHistory.begin());
        }

        // Calculate average frame time
        float avgFrameTime = 0.0f;
        if (!m_frameTimeHistory.empty())
        {
            avgFrameTime = std::accumulate(m_frameTimeHistory.begin(), m_frameTimeHistory.end(), 0.0f)
                / static_cast<float>(m_frameTimeHistory.size());
        }

        // Update scope statistics
        for (auto& scope : m_currentFrameScopes)
        {
            auto& stats = m_scopeStats[scope.name];
            stats.totalTimeMs += scope.timeMs;
            stats.minTimeMs = std::min(stats.minTimeMs, scope.timeMs);
            stats.maxTimeMs = std::max(stats.maxTimeMs, scope.timeMs);
            stats.callCount++;

            stats.history.push_back(scope.timeMs);
            if (stats.history.size() > m_averageFrames)
            {
                stats.history.erase(stats.history.begin());
            }

            // Calculate averaged time for this scope
            if (!stats.history.empty())
            {
                scope.avgTimeMs = std::accumulate(stats.history.begin(), stats.history.end(), 0.0f)
                    / static_cast<float>(stats.history.size());
            }
            scope.minTimeMs = stats.minTimeMs;
            scope.maxTimeMs = stats.maxTimeMs;
        }

        // Store completed frame data
        m_lastFrameData.frameTimeMs = frameTimeMs;
        m_lastFrameData.avgFrameTimeMs = avgFrameTime;
        m_lastFrameData.frameIndex = m_frameIndex;
        m_lastFrameData.scopes = m_currentFrameScopes;

        m_totalFrameTime += frameTimeMs;
        m_frameIndex++;
        m_inFrame = false;
    }

    // =========================================================================
    // Scope Profiling
    // =========================================================================

    ScopeId CPUProfiler::BeginScope(const char* name)
    {
        if (!m_initialized || !m_enabled || !m_inFrame)
        {
            return RVX_INVALID_SCOPE_ID;
        }

        std::lock_guard<std::mutex> lock(m_mutex);

        ScopeId id = m_nextScopeId++;

        ScopeInstance scope;
        scope.name = name;
        scope.startTime = Clock::now();
        scope.depth = static_cast<uint32>(m_activeScopeStack.size());
        scope.threadId = std::this_thread::get_id();

        m_activeScopeStack.push_back(scope);

        return id;
    }

    void CPUProfiler::EndScope(ScopeId id)
    {
        if (!m_initialized || !m_enabled || !m_inFrame || id == RVX_INVALID_SCOPE_ID)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_activeScopeStack.empty())
        {
            RVX_CORE_WARN("CPUProfiler::EndScope called with empty stack");
            return;
        }

        // Pop the scope from the stack
        ScopeInstance scope = m_activeScopeStack.back();
        m_activeScopeStack.pop_back();

        // Calculate elapsed time
        auto endTime = Clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
            endTime - scope.startTime
        );
        float timeMs = duration.count() / 1000.0f;

        // Store the result
        CPUTimingResult result;
        result.name = scope.name;
        result.timeMs = timeMs;
        result.depth = scope.depth;
        result.callCount = 1;
        result.threadId = scope.threadId;

        m_currentFrameScopes.push_back(result);
    }

    // =========================================================================
    // Results
    // =========================================================================

    float CPUProfiler::GetScopeTimeMs(const char* name) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        for (const auto& scope : m_lastFrameData.scopes)
        {
            if (scope.name == name)
            {
                return scope.timeMs;
            }
        }

        return 0.0f;
    }

    float CPUProfiler::GetFPS() const
    {
        float avgFrameTime = m_lastFrameData.avgFrameTimeMs;
        if (avgFrameTime > 0.0f)
        {
            return 1000.0f / avgFrameTime;
        }
        return 0.0f;
    }

    // =========================================================================
    // Configuration
    // =========================================================================

    void CPUProfiler::SetAverageFrames(uint32 frames)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        m_averageFrames = frames;

        // Trim history if needed
        if (m_frameTimeHistory.size() > frames)
        {
            m_frameTimeHistory.erase(
                m_frameTimeHistory.begin(),
                m_frameTimeHistory.begin() + (m_frameTimeHistory.size() - frames)
            );
        }

        for (auto& [name, stats] : m_scopeStats)
        {
            if (stats.history.size() > frames)
            {
                stats.history.erase(
                    stats.history.begin(),
                    stats.history.begin() + (stats.history.size() - frames)
                );
            }
        }
    }

    // =========================================================================
    // Statistics
    // =========================================================================

    CPUProfiler::Stats CPUProfiler::GetStats() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        Stats stats;
        stats.totalFrames = m_frameIndex;
        stats.avgFrameTime = m_lastFrameData.avgFrameTimeMs;
        stats.activeScopeCount = static_cast<uint32>(m_scopeStats.size());

        if (!m_frameTimeHistory.empty())
        {
            stats.minFrameTime = *std::min_element(m_frameTimeHistory.begin(), m_frameTimeHistory.end());
            stats.maxFrameTime = *std::max_element(m_frameTimeHistory.begin(), m_frameTimeHistory.end());
        }

        return stats;
    }

} // namespace RVX
