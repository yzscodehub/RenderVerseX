#pragma once

/**
 * @file FXAA.h
 * @brief Fast Approximate Anti-Aliasing post-process effect
 */

#include "Render/PostProcess/PostProcessStack.h"

namespace RVX
{
    /**
     * @brief FXAA quality presets
     */
    enum class FXAAQuality : uint8
    {
        Low,        // Fastest, visible artifacts
        Medium,     // Balanced
        High,       // Best quality, slower
        Ultra       // Maximum quality
    };

    /**
     * @brief FXAA post-process pass
     * 
     * Implements NVIDIA's Fast Approximate Anti-Aliasing algorithm.
     * This is applied after tone mapping (in LDR) for best results.
     */
    class FXAAPass : public IPostProcessPass
    {
    public:
        FXAAPass();
        ~FXAAPass() override = default;

        const char* GetName() const override { return "FXAA"; }
        int32 GetPriority() const override { return 1000; }  // After tone mapping

        void Configure(const PostProcessSettings& settings) override;
        void AddToGraph(RenderGraph& graph, RGTextureHandle input, RGTextureHandle output) override;

        // =========================================================================
        // Configuration
        // =========================================================================

        void SetQuality(FXAAQuality quality) { m_quality = quality; }
        FXAAQuality GetQuality() const { return m_quality; }

        /**
         * @brief Set edge threshold
         * @param threshold Minimum local contrast to detect edge (default 0.166)
         */
        void SetEdgeThreshold(float threshold) { m_edgeThreshold = threshold; }
        float GetEdgeThreshold() const { return m_edgeThreshold; }

        /**
         * @brief Set edge threshold minimum
         * @param min Minimum edge threshold (default 0.0833)
         */
        void SetEdgeThresholdMin(float min) { m_edgeThresholdMin = min; }
        float GetEdgeThresholdMin() const { return m_edgeThresholdMin; }

        /**
         * @brief Set subpixel quality
         * @param quality Subpixel AA quality (0.0 = off, 1.0 = full)
         */
        void SetSubpixelQuality(float quality) { m_subpixelQuality = quality; }
        float GetSubpixelQuality() const { return m_subpixelQuality; }

    private:
        FXAAQuality m_quality = FXAAQuality::Medium;
        float m_edgeThreshold = 0.166f;
        float m_edgeThresholdMin = 0.0833f;
        float m_subpixelQuality = 0.75f;
    };

} // namespace RVX
