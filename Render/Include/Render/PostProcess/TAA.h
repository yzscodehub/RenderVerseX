/**
 * @file TAA.h
 * @brief Temporal Anti-Aliasing
 * 
 * High-quality temporal anti-aliasing with motion vector support.
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
     * @brief TAA configuration
     */
    struct TAAConfig
    {
        float jitterScale = 1.0f;           ///< Jitter scale for sub-pixel sampling
        int jitterPhase = 8;                 ///< Number of jitter samples in sequence
        
        float feedbackMin = 0.88f;          ///< Minimum history blend factor
        float feedbackMax = 0.97f;          ///< Maximum history blend factor
        
        float motionScale = 1.0f;           ///< Motion vector scale
        float velocityWeight = 1000.0f;     ///< Weight for velocity-based rejection
        
        bool useMotionVectors = true;       ///< Use motion vectors for reprojection
        bool sharpen = true;                ///< Apply sharpening after TAA
        float sharpness = 0.25f;            ///< Sharpening amount
        
        bool antiFlicker = true;            ///< Reduce flickering on thin features
        bool clampHistory = true;           ///< Clamp history to neighborhood
    };

    /**
     * @brief Temporal Anti-Aliasing
     * 
     * Implements high-quality TAA with:
     * - Sub-pixel jittering for temporal supersampling
     * - Motion vector reprojection
     * - Neighborhood clamping for ghosting reduction
     * - Velocity weighting for disocclusion handling
     * 
     * Usage:
     * 1. Apply jitter offset to projection matrix each frame
     * 2. Render scene with motion vectors
     * 3. Call Resolve() with current frame and motion vectors
     */
    class TAA
    {
    public:
        TAA() = default;
        ~TAA();

        // Non-copyable
        TAA(const TAA&) = delete;
        TAA& operator=(const TAA&) = delete;

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

        const TAAConfig& GetConfig() const { return m_config; }
        void SetConfig(const TAAConfig& config);

        void SetEnabled(bool enabled) { m_enabled = enabled; }
        bool IsEnabled() const { return m_enabled; }

        // =========================================================================
        // Jitter
        // =========================================================================

        /**
         * @brief Get jitter offset for current frame
         * @param frameIndex Current frame number
         * @return Jitter offset in clip space (-1 to 1)
         */
        Vec2 GetJitterOffset(uint64 frameIndex) const;

        /**
         * @brief Get jitter offset in pixels
         * @param frameIndex Current frame number
         * @return Jitter offset in pixels
         */
        Vec2 GetJitterOffsetPixels(uint64 frameIndex) const;

        /**
         * @brief Apply jitter to projection matrix
         * @param projMatrix Original projection matrix
         * @param frameIndex Current frame number
         * @return Jittered projection matrix
         */
        Mat4 JitterProjectionMatrix(const Mat4& projMatrix, uint64 frameIndex) const;

        // =========================================================================
        // Rendering
        // =========================================================================

        /**
         * @brief Resolve TAA for current frame
         * @param ctx Command context
         * @param currentColor Current frame color
         * @param depthTexture Scene depth buffer
         * @param motionVectors Motion vector buffer (optional)
         * @param frameIndex Current frame number
         */
        void Resolve(RHICommandContext& ctx,
                     RHITexture* currentColor,
                     RHITexture* depthTexture,
                     RHITexture* motionVectors,
                     uint64 frameIndex);

        /**
         * @brief Get the TAA result texture
         */
        RHITexture* GetResult() const { return m_result.Get(); }

        /**
         * @brief Reset history (call after camera cut)
         */
        void ResetHistory();

    private:
        void CreateResources(uint32 width, uint32 height);
        void SwapHistory();

        // Halton sequence for jitter
        static float Halton(int index, int base);
        static Vec2 HaltonSequence(int index);

        IRHIDevice* m_device = nullptr;
        TAAConfig m_config;
        bool m_enabled = true;

        uint32 m_width = 0;
        uint32 m_height = 0;

        // Double-buffered history
        RHITextureRef m_history[2];
        int m_currentHistory = 0;

        // Output
        RHITextureRef m_result;

        // Pipelines
        RHIPipelineRef m_taaPipeline;
        RHIPipelineRef m_sharpenPipeline;
        RHIPipelineRef m_copyPipeline;

        // Constant buffer
        RHIBufferRef m_constantBuffer;

        // State
        bool m_historyValid = false;
        Mat4 m_prevViewProj;
    };

} // namespace RVX
