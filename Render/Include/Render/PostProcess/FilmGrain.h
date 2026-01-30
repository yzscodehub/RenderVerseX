#pragma once

/**
 * @file FilmGrain.h
 * @brief Film grain post-process effect
 * 
 * Simulates film grain for a cinematic look.
 */

#include "Render/PostProcess/PostProcessStack.h"

namespace RVX
{
    /**
     * @brief Film grain type
     */
    enum class FilmGrainType : uint8
    {
        Fast,           ///< Fast procedural noise
        FilmLike,       ///< More realistic film simulation
        Colored         ///< Color grain (for certain film stocks)
    };

    /**
     * @brief Film grain configuration
     */
    struct FilmGrainConfig
    {
        FilmGrainType type = FilmGrainType::FilmLike;
        
        float intensity = 0.2f;             ///< Grain strength (0 to 1)
        float response = 0.8f;              ///< Luminance response (how grain varies with brightness)
        
        float size = 1.5f;                  ///< Grain particle size
        float luminanceContribution = 1.0f; ///< Contribution to luminance
        float colorContribution = 0.0f;     ///< Contribution to color
        
        bool animated = true;               ///< Animate grain over time
        float animationSpeed = 1.0f;        ///< Animation speed multiplier
    };

    /**
     * @brief Film grain post-process pass
     * 
     * Adds realistic film grain noise to the image.
     * Supports different grain types and luminance-dependent intensity.
     */
    class FilmGrainPass : public IPostProcessPass
    {
    public:
        FilmGrainPass();
        ~FilmGrainPass() override = default;

        const char* GetName() const override { return "FilmGrain"; }
        int32 GetPriority() const override { return 950; }  // Very late (after most effects)

        void Configure(const PostProcessSettings& settings) override;
        void AddToGraph(RenderGraph& graph, RGTextureHandle input, RGTextureHandle output) override;

        // =========================================================================
        // Configuration
        // =========================================================================

        void SetConfig(const FilmGrainConfig& config) { m_config = config; }
        const FilmGrainConfig& GetConfig() const { return m_config; }

        void SetIntensity(float intensity) { m_config.intensity = intensity; }
        float GetIntensity() const { return m_config.intensity; }

        void SetType(FilmGrainType type) { m_config.type = type; }
        FilmGrainType GetType() const { return m_config.type; }

        void SetSize(float size) { m_config.size = size; }
        float GetSize() const { return m_config.size; }

        void SetAnimated(bool animated) { m_config.animated = animated; }
        bool IsAnimated() const { return m_config.animated; }

        /**
         * @brief Set frame time for animation
         */
        void SetFrameTime(float time) { m_frameTime = time; }

    private:
        FilmGrainConfig m_config;
        float m_frameTime = 0.0f;
    };

} // namespace RVX
