#pragma once

/**
 * @file Bloom.h
 * @brief Bloom post-process effect
 */

#include "Render/PostProcess/PostProcessStack.h"

#include <array>

namespace RVX
{
    class PipelineCache;
    class ResourceViewCache;

    /**
     * @brief HDR Bloom post-process pass
     *
     * Builds a compact transient pyramid. When enabled, the final opaque
     * fullscreen pass reads the source scene plus every pyramid level and
     * writes the HDR destination exactly once. Full quality presets and
     * compute downsample remain future expansions.
     */
    class BloomPass : public IPostProcessPass
    {
    public:
        BloomPass();
        ~BloomPass() override = default;

        PostProcessPassDescriptor Describe() const override
        {
            return {PostProcessPassId::Bloom, "Bloom", PostProcessStage::HDRPost, 500,
                    PostProcessColorDomain::SceneLinearHDR,
                    PostProcessColorDomain::SceneLinearHDR,
                    PostProcessTransfer::None};
        }
        const char* GetName() const override { return "Bloom"; }
        int32 GetPriority() const override { return 500; }  // Before tone mapping

        void Configure(const PostProcessSettings& settings) override;
        void AddToGraph(RenderGraph& graph, RGTextureHandle input, RGTextureHandle output) override;
        void AddToGraph(RenderGraph& graph,
                        const PostProcessFrameInputs& frameInputs,
                        RGTextureHandle output) override;

        [[nodiscard]] uint64 GetLastExposureStateContentHash() const noexcept
        {
            return m_lastExposureStateContentHash;
        }

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
        enum class PassMode : uint32
        {
            CopyScene = 0,
            Extract = 1,
            Downsample = 2,
            CompositePyramid = 3,
        };

        void AddFullscreenPass(RenderGraph& graph,
                               const char* passName,
                               RGTextureHandle input,
                               RGBufferHandle exposureState,
                               uint64 exposureStateContentHash,
                               RGTextureHandle output,
                               PassMode mode,
                               float threshold,
                               float intensity,
                               float radius);
        void AddCompositePyramidPass(
            RenderGraph& graph,
            RGTextureHandle source,
            const std::array<RGTextureHandle, 3>& pyramid,
            RGBufferHandle exposureState,
            uint64 exposureStateContentHash,
            RGTextureHandle output);
        void AddToGraphWithExposure(RenderGraph& graph,
                                    RGTextureHandle input,
                                    RGBufferHandle exposureState,
                                    uint64 exposureStateContentHash,
                                    RGTextureHandle output);
        bool EnsureRuntimeResources();
        RHIBufferRef CreatePassConstants(uint32 width,
                                         uint32 height,
                                         float threshold,
                                         float intensity,
                                         float radius,
                                         float softKnee,
                                         PassMode mode) const;

        float m_threshold = 1.0f;
        float m_intensity = 1.0f;
        float m_radius = 0.5f;
        float m_softKnee = 0.5f;
        PipelineCache* m_pipelineCache = nullptr;
        ResourceViewCache* m_viewCache = nullptr;
        IRHIDevice* m_resourceDevice = nullptr;
        RHISamplerRef m_sampler;
        uint64 m_lastExposureStateContentHash = 0;
    };

} // namespace RVX
