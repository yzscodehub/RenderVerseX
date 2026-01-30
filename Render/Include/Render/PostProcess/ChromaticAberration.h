#pragma once

/**
 * @file ChromaticAberration.h
 * @brief Chromatic aberration post-process effect
 * 
 * Simulates lens chromatic aberration (color fringing).
 */

#include "Core/MathTypes.h"
#include "Render/PostProcess/PostProcessStack.h"

namespace RVX
{
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
        int32 GetPriority() const override { return 860; }  // Late in pipeline

        void Configure(const PostProcessSettings& settings) override;
        void AddToGraph(RenderGraph& graph, RGTextureHandle input, RGTextureHandle output) override;

        // =========================================================================
        // Configuration
        // =========================================================================

        void SetConfig(const ChromaticAberrationConfig& config) { m_config = config; }
        const ChromaticAberrationConfig& GetConfig() const { return m_config; }

        void SetIntensity(float intensity) { m_config.intensity = intensity; }
        float GetIntensity() const { return m_config.intensity; }

        void SetSpectralSampling(bool enable) { m_config.useSpectral = enable; }
        bool IsSpectralSampling() const { return m_config.useSpectral; }

    private:
        ChromaticAberrationConfig m_config;
    };

} // namespace RVX
