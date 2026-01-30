/**
 * @file SSR.h
 * @brief Screen-Space Reflections effect
 * 
 * Implements hierarchical ray marching for efficient SSR.
 */

#pragma once

#include "Core/Types.h"
#include "Core/MathTypes.h"
#include "RHI/RHI.h"
#include <memory>

namespace RVX
{
    class IRHIDevice;
    class RHICommandContext;
    class RHITexture;

    /**
     * @brief SSR quality preset
     */
    enum class SSRQuality : uint8
    {
        Low,        ///< Fast (linear ray march)
        Medium,     ///< Balanced (HiZ ray march, low resolution)
        High,       ///< High quality (HiZ ray march)
        Ultra       ///< Maximum quality (stochastic ray march)
    };

    /**
     * @brief SSR configuration
     */
    struct SSRConfig
    {
        SSRQuality quality = SSRQuality::Medium;
        
        float maxDistance = 100.0f;         ///< Maximum reflection ray distance
        float thickness = 0.1f;             ///< Depth buffer thickness for ray-surface test
        int maxSteps = 64;                  ///< Maximum ray march steps
        int binarySearchSteps = 8;          ///< Binary search refinement steps
        
        float roughnessThreshold = 0.5f;    ///< Don't compute SSR above this roughness
        float edgeFade = 0.1f;              ///< Fade reflections near screen edges
        
        bool halfResolution = true;         ///< Compute at half resolution
        bool temporalFilter = true;         ///< Enable temporal filtering
        bool stochastic = false;            ///< Enable stochastic sampling for rough surfaces
    };

    /**
     * @brief Screen-Space Reflections
     * 
     * Implements high-quality SSR using hierarchical ray marching.
     * 
     * Features:
     * - HiZ acceleration for fast ray marching
     * - Stochastic sampling for rough reflections
     * - Temporal accumulation for stability
     * - Edge fade and sky fallback
     */
    class SSR
    {
    public:
        SSR() = default;
        ~SSR();

        // Non-copyable
        SSR(const SSR&) = delete;
        SSR& operator=(const SSR&) = delete;

        // =========================================================================
        // Lifecycle
        // =========================================================================

        void Initialize(IRHIDevice* device, uint32 width, uint32 height);
        void Shutdown();
        void Resize(uint32 width, uint32 height);
        bool IsInitialized() const { return m_device != nullptr; }

        // =========================================================================
        // Configuration
        // =========================================================================

        const SSRConfig& GetConfig() const { return m_config; }
        void SetConfig(const SSRConfig& config);

        void SetEnabled(bool enabled) { m_enabled = enabled; }
        bool IsEnabled() const { return m_enabled; }

        // =========================================================================
        // Rendering
        // =========================================================================

        /**
         * @brief Compute SSR
         * @param ctx Command context
         * @param colorTexture Scene color buffer
         * @param depthTexture Scene depth buffer
         * @param normalTexture Scene normal buffer
         * @param roughnessTexture Roughness buffer (can be packed)
         * @param viewMatrix Camera view matrix
         * @param projMatrix Camera projection matrix
         */
        void Compute(RHICommandContext& ctx,
                     RHITexture* colorTexture,
                     RHITexture* depthTexture,
                     RHITexture* normalTexture,
                     RHITexture* roughnessTexture,
                     const Mat4& viewMatrix,
                     const Mat4& projMatrix);

        /**
         * @brief Get the reflection result texture
         */
        RHITexture* GetResult() const { return m_reflectionResult.Get(); }

        /**
         * @brief Get the hit mask texture (for debugging)
         */
        RHITexture* GetHitMask() const { return m_hitMask.Get(); }

    private:
        void CreateResources(uint32 width, uint32 height);
        void BuildHiZPyramid(RHICommandContext& ctx, RHITexture* depth);
        void RayMarch(RHICommandContext& ctx);
        void Resolve(RHICommandContext& ctx, RHITexture* color);
        void TemporalFilter(RHICommandContext& ctx);

        IRHIDevice* m_device = nullptr;
        SSRConfig m_config;
        bool m_enabled = true;

        uint32 m_width = 0;
        uint32 m_height = 0;

        // Render targets
        RHITextureRef m_reflectionResult;
        RHITextureRef m_hitMask;
        RHITextureRef m_rayHitUV;
        RHITextureRef m_history;

        // HiZ pyramid
        RHITextureRef m_hiZPyramid;
        std::vector<RHITextureViewRef> m_hiZMips;

        // Pipelines
        RHIPipelineRef m_hiZPipeline;
        RHIPipelineRef m_rayMarchPipeline;
        RHIPipelineRef m_resolvePipeline;
        RHIPipelineRef m_temporalPipeline;

        // Constant buffer
        RHIBufferRef m_constantBuffer;
    };

} // namespace RVX
