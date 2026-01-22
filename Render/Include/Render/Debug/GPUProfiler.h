#pragma once

/**
 * @file GPUProfiler.h
 * @brief GPU timing and profiling utilities
 */

#include "Core/Types.h"
#include "RHI/RHI.h"
#include <vector>
#include <string>
#include <unordered_map>

namespace RVX
{
    /**
     * @brief GPU timing result for a single scope
     */
    struct GPUTimingResult
    {
        std::string name;
        float gpuTimeMs = 0.0f;
        uint32 depth = 0;
    };

    /**
     * @brief GPU profiler using timestamp queries
     * 
     * Features:
     * - Timestamp queries for GPU pass timing
     * - Multi-frame averaging
     * - Hierarchical scope tracking
     * - Integration with RenderGraph pass stats
     */
    class GPUProfiler
    {
    public:
        GPUProfiler() = default;
        ~GPUProfiler();

        // Non-copyable
        GPUProfiler(const GPUProfiler&) = delete;
        GPUProfiler& operator=(const GPUProfiler&) = delete;

        // =========================================================================
        // Lifecycle
        // =========================================================================

        void Initialize(IRHIDevice* device);
        void Shutdown();
        bool IsInitialized() const { return m_device != nullptr; }

        // =========================================================================
        // Profiling API
        // =========================================================================

        /**
         * @brief Begin a new frame of profiling
         */
        void BeginFrame();

        /**
         * @brief End the current frame and read back results
         */
        void EndFrame();

        /**
         * @brief Begin a named profiling scope
         * @param ctx Command context for timestamp query
         * @param name Scope name
         */
        void BeginScope(RHICommandContext& ctx, const char* name);

        /**
         * @brief End the current profiling scope
         */
        void EndScope(RHICommandContext& ctx);

        // =========================================================================
        // Results
        // =========================================================================

        /**
         * @brief Get timing results from the previous frame
         */
        const std::vector<GPUTimingResult>& GetResults() const { return m_results; }

        /**
         * @brief Get the total GPU frame time in milliseconds
         */
        float GetFrameTimeMs() const { return m_frameTimeMs; }

        /**
         * @brief Get timing for a specific scope by name
         */
        float GetScopeTimeMs(const char* name) const;

        // =========================================================================
        // Configuration
        // =========================================================================

        /**
         * @brief Enable/disable profiling
         */
        void SetEnabled(bool enabled) { m_enabled = enabled; }
        bool IsEnabled() const { return m_enabled; }

        /**
         * @brief Set number of frames to average
         */
        void SetAverageFrames(uint32 frames) { m_averageFrames = frames; }

        // =========================================================================
        // Statistics
        // =========================================================================

        struct Stats
        {
            uint32 activeScopeCount = 0;
            uint32 queryCount = 0;
            float avgFrameTimeMs = 0.0f;
        };

        Stats GetStats() const;

    private:
        struct ScopeData
        {
            std::string name;
            uint32 startQueryIndex = 0;
            uint32 endQueryIndex = 0;
            uint32 depth = 0;
        };

        IRHIDevice* m_device = nullptr;
        bool m_enabled = true;
        uint32 m_averageFrames = 4;

        // Current frame scope stack
        std::vector<ScopeData> m_currentScopes;
        uint32 m_currentDepth = 0;
        uint32 m_nextQueryIndex = 0;

        // Results
        std::vector<GPUTimingResult> m_results;
        std::unordered_map<std::string, float> m_scopeTimings;
        float m_frameTimeMs = 0.0f;

        // Query resources
        static constexpr uint32 MaxQueries = 256;
        // Note: Actual query pool would be created based on RHI capabilities
    };

    /**
     * @brief RAII scope helper for GPU profiling
     */
    class GPUProfileScope
    {
    public:
        GPUProfileScope(GPUProfiler& profiler, RHICommandContext& ctx, const char* name)
            : m_profiler(profiler), m_ctx(ctx)
        {
            m_profiler.BeginScope(m_ctx, name);
        }

        ~GPUProfileScope()
        {
            m_profiler.EndScope(m_ctx);
        }

        // Non-copyable, non-movable
        GPUProfileScope(const GPUProfileScope&) = delete;
        GPUProfileScope& operator=(const GPUProfileScope&) = delete;

    private:
        GPUProfiler& m_profiler;
        RHICommandContext& m_ctx;
    };

    // Macro for easy GPU profiling
    #define RVX_GPU_PROFILE_SCOPE(profiler, ctx, name) \
        GPUProfileScope _gpu_scope_##__LINE__(profiler, ctx, name)

} // namespace RVX
