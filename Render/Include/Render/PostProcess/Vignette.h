#pragma once

/**
 * @file Vignette.h
 * @brief Vignette post-process effect
 * 
 * Darkens the edges of the screen for a cinematic look.
 */

#include "Core/MathTypes.h"
#include "Render/PostProcess/PostProcessStack.h"

namespace RVX
{
    /**
     * @brief Vignette shape modes
     */
    enum class VignetteMode : uint8
    {
        Classic,        ///< Standard radial vignette
        Rounded,        ///< Rounded rectangle vignette
        Natural         ///< Simulates lens vignette (cos^4 falloff)
    };

    /**
     * @brief Vignette configuration
     */
    struct VignetteConfig
    {
        VignetteMode mode = VignetteMode::Classic;
        
        float intensity = 0.4f;         ///< Vignette strength (0 to 1)
        float smoothness = 0.5f;        ///< Edge smoothness (0 to 1)
        float roundness = 1.0f;         ///< Shape roundness (0 = more square, 1 = circle)
        
        Vec2 center = Vec2(0.5f, 0.5f); ///< Vignette center (normalized)
        Vec3 color = Vec3(0.0f);        ///< Vignette color (default: black)
        
        bool rounded = true;            ///< Round to screen aspect ratio
    };

    /**
     * @brief Vignette post-process pass
     * 
     * Applies a vignette effect that darkens or tints the edges of the screen.
     * Supports multiple modes and customizable shapes.
     */
    class VignettePass : public IPostProcessPass
    {
    public:
        VignettePass();
        ~VignettePass() override = default;

        const char* GetName() const override { return "Vignette"; }
        int32 GetPriority() const override { return 850; }  // Late in pipeline

        void Configure(const PostProcessSettings& settings) override;
        void AddToGraph(RenderGraph& graph, RGTextureHandle input, RGTextureHandle output) override;

        // =========================================================================
        // Configuration
        // =========================================================================

        void SetConfig(const VignetteConfig& config) { m_config = config; }
        const VignetteConfig& GetConfig() const { return m_config; }

        void SetIntensity(float intensity) { m_config.intensity = intensity; }
        float GetIntensity() const { return m_config.intensity; }

        void SetSmoothness(float smoothness) { m_config.smoothness = smoothness; }
        float GetSmoothness() const { return m_config.smoothness; }

        void SetRoundness(float roundness) { m_config.roundness = roundness; }
        float GetRoundness() const { return m_config.roundness; }

        void SetCenter(const Vec2& center) { m_config.center = center; }
        const Vec2& GetCenter() const { return m_config.center; }

        void SetColor(const Vec3& color) { m_config.color = color; }
        const Vec3& GetColor() const { return m_config.color; }

        void SetMode(VignetteMode mode) { m_config.mode = mode; }
        VignetteMode GetMode() const { return m_config.mode; }

    private:
        VignetteConfig m_config;
    };

} // namespace RVX
