#pragma once

/**
 * @file CPUProfiler.h
 * @brief High-resolution CPU profiling and timing utilities
 * 
 * Features:
 * - Hierarchical scope tracking
 * - Multi-frame averaging
 * - Per-thread profiling support
 * - RAII scope helpers
 */

#include "Core/Types.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <mutex>
#include <thread>

namespace RVX
{
    // Forward declarations
    class CPUProfiler;

    /**
     * @brief Unique identifier for a profiling scope
     */
    using ScopeId = uint32;

    /**
     * @brief Invalid scope ID constant
     */
    constexpr ScopeId RVX_INVALID_SCOPE_ID = ~0u;

    /**
     * @brief Result data for a single profiling scope
     */
    struct CPUTimingResult
    {
        std::string name;
        float timeMs = 0.0f;           // Time in milliseconds
        float avgTimeMs = 0.0f;        // Averaged time over multiple frames
        float minTimeMs = 0.0f;        // Minimum recorded time
        float maxTimeMs = 0.0f;        // Maximum recorded time
        uint32 depth = 0;              // Hierarchy depth
        uint32 callCount = 0;          // Number of calls this frame
        std::thread::id threadId;      // Thread that recorded this
    };

    /**
     * @brief Frame-level profiling data
     */
    struct CPUFrameData
    {
        float frameTimeMs = 0.0f;
        float avgFrameTimeMs = 0.0f;
        uint64 frameIndex = 0;
        std::vector<CPUTimingResult> scopes;
    };

    /**
     * @brief High-resolution CPU profiler
     * 
     * Usage:
     * @code
     * // Initialize once
     * CPUProfiler::Get().Initialize();
     * 
     * // Each frame
     * CPUProfiler::Get().BeginFrame();
     * 
     * {
     *     RVX_CPU_PROFILE_SCOPE("Update");
     *     // ... update code ...
     * }
     * 
     * CPUProfiler::Get().EndFrame();
     * 
     * // Get results
     * const auto& frame = CPUProfiler::Get().GetLastFrame();
     * @endcode
     */
    class CPUProfiler
    {
    public:
        // =========================================================================
        // Singleton Access
        // =========================================================================

        /**
         * @brief Get the global profiler instance
         */
        static CPUProfiler& Get();

        // =========================================================================
        // Lifecycle
        // =========================================================================

        /**
         * @brief Initialize the profiler
         * @param averageFrames Number of frames for averaging (default: 60)
         */
        void Initialize(uint32 averageFrames = 60);

        /**
         * @brief Shutdown and release resources
         */
        void Shutdown();

        /**
         * @brief Check if profiler is initialized
         */
        bool IsInitialized() const { return m_initialized; }

        // =========================================================================
        // Frame Control
        // =========================================================================

        /**
         * @brief Begin a new profiling frame
         */
        void BeginFrame();

        /**
         * @brief End the current frame and process results
         */
        void EndFrame();

        // =========================================================================
        // Scope Profiling
        // =========================================================================

        /**
         * @brief Begin a named profiling scope
         * @param name Scope name (must be string literal or have static lifetime)
         * @return Scope ID for EndScope
         */
        ScopeId BeginScope(const char* name);

        /**
         * @brief End a profiling scope
         * @param id Scope ID returned from BeginScope
         */
        void EndScope(ScopeId id);

        // =========================================================================
        // Results
        // =========================================================================

        /**
         * @brief Get data from the last completed frame
         */
        const CPUFrameData& GetLastFrame() const { return m_lastFrameData; }

        /**
         * @brief Get timing for a specific scope by name
         * @param name Scope name
         * @return Time in milliseconds, or 0 if not found
         */
        float GetScopeTimeMs(const char* name) const;

        /**
         * @brief Get the averaged frame time
         */
        float GetAvgFrameTimeMs() const { return m_lastFrameData.avgFrameTimeMs; }

        /**
         * @brief Get current FPS based on frame time
         */
        float GetFPS() const;

        // =========================================================================
        // Configuration
        // =========================================================================

        /**
         * @brief Enable/disable profiling
         */
        void SetEnabled(bool enabled) { m_enabled = enabled; }
        bool IsEnabled() const { return m_enabled; }

        /**
         * @brief Set the number of frames for averaging
         */
        void SetAverageFrames(uint32 frames);

        /**
         * @brief Get the frame history for charting
         */
        const std::vector<float>& GetFrameTimeHistory() const { return m_frameTimeHistory; }

        // =========================================================================
        // Statistics
        // =========================================================================

        struct Stats
        {
            uint64 totalFrames = 0;
            float avgFrameTime = 0.0f;
            float minFrameTime = 0.0f;
            float maxFrameTime = 0.0f;
            uint32 activeScopeCount = 0;
        };

        Stats GetStats() const;

    private:
        CPUProfiler() = default;
        ~CPUProfiler() { Shutdown(); }

        // Non-copyable
        CPUProfiler(const CPUProfiler&) = delete;
        CPUProfiler& operator=(const CPUProfiler&) = delete;

        // =========================================================================
        // Internal Types
        // =========================================================================

        using Clock = std::chrono::high_resolution_clock;
        using TimePoint = Clock::time_point;

        struct ScopeInstance
        {
            const char* name = nullptr;
            TimePoint startTime;
            uint32 depth = 0;
            std::thread::id threadId;
        };

        struct ScopeStats
        {
            float totalTimeMs = 0.0f;
            float minTimeMs = FLT_MAX;
            float maxTimeMs = 0.0f;
            uint32 callCount = 0;
            std::vector<float> history;  // For averaging
        };

        // =========================================================================
        // Internal State
        // =========================================================================

        bool m_initialized = false;
        bool m_enabled = true;
        bool m_inFrame = false;

        // Frame timing
        TimePoint m_frameStartTime;
        uint64 m_frameIndex = 0;
        uint32 m_averageFrames = 60;

        // Active scopes (per-thread using thread_local indirection)
        std::vector<ScopeInstance> m_activeScopeStack;
        uint32 m_nextScopeId = 0;

        // Accumulated data for current frame
        std::vector<CPUTimingResult> m_currentFrameScopes;

        // Historical data
        std::unordered_map<std::string, ScopeStats> m_scopeStats;
        std::vector<float> m_frameTimeHistory;
        float m_totalFrameTime = 0.0f;

        // Last completed frame data
        CPUFrameData m_lastFrameData;

        // Thread safety
        mutable std::mutex m_mutex;
    };

    /**
     * @brief RAII helper for CPU profiling scopes
     */
    class CPUProfileScope
    {
    public:
        explicit CPUProfileScope(const char* name)
            : m_scopeId(CPUProfiler::Get().BeginScope(name))
        {
        }

        ~CPUProfileScope()
        {
            if (m_scopeId != RVX_INVALID_SCOPE_ID)
            {
                CPUProfiler::Get().EndScope(m_scopeId);
            }
        }

        // Non-copyable, non-movable
        CPUProfileScope(const CPUProfileScope&) = delete;
        CPUProfileScope& operator=(const CPUProfileScope&) = delete;
        CPUProfileScope(CPUProfileScope&&) = delete;
        CPUProfileScope& operator=(CPUProfileScope&&) = delete;

    private:
        ScopeId m_scopeId;
    };

    /**
     * @brief Conditional profiling scope (only active when profiling is enabled)
     */
    class ConditionalProfileScope
    {
    public:
        explicit ConditionalProfileScope(const char* name)
        {
            if (CPUProfiler::Get().IsEnabled())
            {
                m_scopeId = CPUProfiler::Get().BeginScope(name);
            }
        }

        ~ConditionalProfileScope()
        {
            if (m_scopeId != RVX_INVALID_SCOPE_ID)
            {
                CPUProfiler::Get().EndScope(m_scopeId);
            }
        }

        // Non-copyable, non-movable
        ConditionalProfileScope(const ConditionalProfileScope&) = delete;
        ConditionalProfileScope& operator=(const ConditionalProfileScope&) = delete;

    private:
        ScopeId m_scopeId = RVX_INVALID_SCOPE_ID;
    };

} // namespace RVX

// =============================================================================
// Profiling Macros
// =============================================================================

#ifdef RVX_ENABLE_PROFILING
    /**
     * @brief Profile a scope with the given name
     */
    #define RVX_CPU_PROFILE_SCOPE(name) \
        ::RVX::CPUProfileScope _rvx_cpu_scope_##__LINE__(name)

    /**
     * @brief Profile a function (uses __FUNCTION__ as name)
     */
    #define RVX_CPU_PROFILE_FUNCTION() \
        ::RVX::CPUProfileScope _rvx_cpu_func_scope(__FUNCTION__)

    /**
     * @brief Begin a named profile section manually
     */
    #define RVX_CPU_PROFILE_BEGIN(name) \
        auto _rvx_scope_id_##name = ::RVX::CPUProfiler::Get().BeginScope(#name)

    /**
     * @brief End a named profile section
     */
    #define RVX_CPU_PROFILE_END(name) \
        ::RVX::CPUProfiler::Get().EndScope(_rvx_scope_id_##name)
#else
    #define RVX_CPU_PROFILE_SCOPE(name) ((void)0)
    #define RVX_CPU_PROFILE_FUNCTION() ((void)0)
    #define RVX_CPU_PROFILE_BEGIN(name) ((void)0)
    #define RVX_CPU_PROFILE_END(name) ((void)0)
#endif
