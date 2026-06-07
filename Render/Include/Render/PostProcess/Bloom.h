#pragma once

/**
 * @file Bloom.h
 * @brief Bloom post-process effect
 */

#include "Render/PostProcess/PostProcessStack.h"

#include <deque>

namespace RVX
{
    class PipelineCache;
    class ResourceViewCache;

    /**
     * @brief Minimum fullscreen Bloom post-process pass
     *
     * R9e implements a single fullscreen approximation that thresholds bright
     * pixels, samples a small neighborhood, and composites the contribution
     * back into scene color. Full mip-chain bloom remains a future expansion.
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

        /**
         * @brief Provide GPU resources required by the fullscreen Bloom path
         */
        void SetResources(PipelineCache* pipelineCache, ResourceViewCache* viewCache);

        // =========================================================================
        // Configuration
        // =========================================================================

        void SetThreshold(float threshold) { m_threshold = threshold; }
        float GetThreshold() const { return m_threshold; }

        void SetIntensity(float intensity) { m_intensity = intensity; }
        float GetIntensity() const { return m_intensity; }

        void SetRadius(float radius) { m_radius = radius; }
        float GetRadius() const { return m_radius; }

        void SetSoftKnee(float knee) { m_softKnee = knee; }
        float GetSoftKnee() const { return m_softKnee; }

    private:
        bool EnsureRuntimeResources();
        bool UpdateConstants(uint32 width,
                             uint32 height,
                             float threshold,
                             float intensity,
                             float radius,
                             float softKnee);

        float m_threshold = 1.0f;
        float m_intensity = 1.0f;
        float m_radius = 0.5f;
        float m_softKnee = 0.5f;
        PipelineCache* m_pipelineCache = nullptr;
        ResourceViewCache* m_viewCache = nullptr;
        IRHIDevice* m_resourceDevice = nullptr;
        RHIBufferRef m_constantBuffer;
        RHISamplerRef m_sampler;
        std::deque<RHIDescriptorSetRef> m_retainedDescriptorSets;
    };

} // namespace RVX
