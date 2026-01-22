#pragma once

/**
 * @file Bloom.h
 * @brief Bloom post-process effect
 */

#include "Render/PostProcess/PostProcessStack.h"

namespace RVX
{
    /**
     * @brief Bloom post-process pass
     * 
     * Implements a multi-pass bloom effect:
     * 1. Threshold bright areas
     * 2. Downsample with blur
     * 3. Upsample and combine
     */
    class BloomPass : public IPostProcessPass
    {
    public:
        BloomPass();
        ~BloomPass() override = default;

        const char* GetName() const override { return "Bloom"; }
        int32 GetPriority() const override { return 500; }  // Before tone mapping

        void Configure(const PostProcessSettings& settings) override;
        void AddToGraph(RenderGraph& graph, RGTextureHandle input, RGTextureHandle output) override;

        // =========================================================================
        // Configuration
        // =========================================================================

        void SetThreshold(float threshold) { m_threshold = threshold; }
        float GetThreshold() const { return m_threshold; }

        void SetIntensity(float intensity) { m_intensity = intensity; }
        float GetIntensity() const { return m_intensity; }

        void SetRadius(float radius) { m_radius = radius; }
        float GetRadius() const { return m_radius; }

        void SetMipCount(uint32 mips) { m_mipCount = mips; }
        uint32 GetMipCount() const { return m_mipCount; }

        void SetSoftKnee(float knee) { m_softKnee = knee; }
        float GetSoftKnee() const { return m_softKnee; }

    private:
        float m_threshold = 1.0f;
        float m_intensity = 1.0f;
        float m_radius = 0.5f;
        float m_softKnee = 0.5f;
        uint32 m_mipCount = 5;
    };

} // namespace RVX
