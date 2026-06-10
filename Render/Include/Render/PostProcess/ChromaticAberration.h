#pragma once

/**
 * @file ChromaticAberration.h
 * @brief Chromatic aberration post-process effect
 * 
 * Simulates lens chromatic aberration (color fringing).
 */

#include "Core/MathTypes.h"
#include "Render/PostProcess/PostProcessStack.h"

#include <deque>

namespace RVX
{
    class PipelineCache;
    class ResourceViewCache;

    /**
     * @brief Chromatic aberration configuration
     */
    struct ChromaticAberrationConfig
    {
        float intensity = 0.1f;         ///< Overall strength (0 to 1)
        float startOffset = 0.0f;       ///< Offset from center to start effect
        
        // Per-channel shifts
        Vec2 redOffset = Vec2(-1.0f, 0.0f);    ///< Red channel shift direction
        Vec2 greenOffset = Vec2(0.0f, 0.0f);   ///< Green channel shift (usually 0)
        Vec2 blueOffset = Vec2(1.0f, 0.0f);    ///< Blue channel shift direction
        
        bool radialFalloff = true;      ///< More effect towards edges
        bool useSpectral = false;       ///< Use full spectral (7-tap) sampling
    };

    /**
     * @brief Chromatic aberration post-process pass
     * 
     * Simulates lens chromatic aberration by shifting color channels.
     * Can use simple RGB separation or spectral sampling for higher quality.
     */
    class ChromaticAberrationPass : public IPostProcessPass
    {
    public:
        ChromaticAberrationPass();
        ~ChromaticAberrationPass() override = default;

        const char* GetName() const override { return "ChromaticAberration"; }
        int32 GetPriority() const override { return 935; }  // LDR, after color grading and before vignette/FXAA

        void Configure(const PostProcessSettings& settings) override;
        void AddToGraph(RenderGraph& graph, RGTextureHandle input, RGTextureHandle output) override;

        /**
         * @brief Provide GPU resources required by the fullscreen ChromaticAberration path
         */
        void SetResources(PipelineCache* pipelineCache, ResourceViewCache* viewCache);

        // =========================================================================
        // Configuration
        // =========================================================================

        void SetConfig(const ChromaticAberrationConfig& config);
        const ChromaticAberrationConfig& GetConfig() const { return m_config; }

        void SetIntensity(float intensity) { m_config.intensity = intensity; }
        float GetIntensity() const { return m_config.intensity; }

        void SetSpectralSampling(bool enable);
        bool IsSpectralSampling() const { return m_config.useSpectral; }

    private:
        bool EnsureRuntimeResources();
        bool UpdateConstants(uint32 width, uint32 height, const ChromaticAberrationConfig& config);
        void RefreshSupportState();

        ChromaticAberrationConfig m_config;
        PipelineCache* m_pipelineCache = nullptr;
        ResourceViewCache* m_viewCache = nullptr;
        IRHIDevice* m_resourceDevice = nullptr;
        RHIBufferRef m_constantBuffer;
        RHISamplerRef m_sampler;
        std::deque<RHIDescriptorSetRef> m_retainedDescriptorSets;
    };

} // namespace RVX
