#pragma once

/**
 * @file DOF.h
 * @brief Depth of Field post-process effect
 * 
 * Implements physically-based depth of field with bokeh simulation.
 */

#include "Render/PostProcess/PostProcessStack.h"

namespace RVX
{
    /**
     * @brief Bokeh shape types
     */
    enum class BokehShape : uint8
    {
        Circle,         ///< Circular bokeh (default)
        Hexagon,        ///< 6-sided hexagonal bokeh
        Octagon,        ///< 8-sided octagonal bokeh
        Custom          ///< Custom bokeh texture
    };

    /**
     * @brief DOF quality presets
     */
    enum class DOFQuality : uint8
    {
        Low,            ///< Fast, minimal samples
        Medium,         ///< Balanced quality/performance
        High,           ///< High quality
        Ultra           ///< Maximum quality with bokeh sprites
    };

    /**
     * @brief Depth of Field configuration
     */
    struct DOFConfig
    {
        // Focus settings
        float focusDistance = 10.0f;        ///< Distance to focus plane (meters)
        float focusRange = 5.0f;            ///< Range around focus plane that's sharp
        
        // Aperture settings (physically based)
        float aperture = 5.6f;              ///< f-stop (lower = more blur)
        float focalLength = 50.0f;          ///< Lens focal length in mm
        float sensorSize = 36.0f;           ///< Sensor size in mm (35mm = 36)
        
        // Blur settings
        float maxBlurRadius = 8.0f;         ///< Maximum blur radius in pixels
        float nearBlurScale = 1.0f;         ///< Near field blur intensity
        float farBlurScale = 1.0f;          ///< Far field blur intensity
        
        // Quality
        DOFQuality quality = DOFQuality::Medium;
        BokehShape bokehShape = BokehShape::Circle;
        float bokehBrightness = 1.0f;       ///< Brightness threshold for bokeh sprites
        float bokehScale = 1.0f;            ///< Size multiplier for bokeh
        
        // Optimization
        bool halfResolution = true;         ///< Compute DOF at half resolution
        bool useTiledRendering = true;      ///< Use tiled approach for performance
        
        // Artistic controls
        float chromaticAberration = 0.0f;   ///< Chromatic aberration in bokeh (0-1)
        float anamorphicRatio = 1.0f;       ///< Anamorphic stretch (1 = circular)
    };

    /**
     * @brief Depth of Field post-process pass
     * 
     * Implements a multi-pass DOF effect:
     * 1. Compute Circle of Confusion (CoC) from depth
     * 2. Separate foreground and background
     * 3. Apply blur with proper alpha handling
     * 4. Composite with sharp in-focus region
     * 
     * Features:
     * - Physically-based CoC calculation
     * - Separate near/far field handling
     * - Bokeh shape simulation
     * - Chromatic aberration in blur
     * - Half-resolution optimization
     */
    class DOFPass : public IPostProcessPass
    {
    public:
        DOFPass();
        ~DOFPass() override = default;

        const char* GetName() const override { return "DepthOfField"; }
        int32 GetPriority() const override { return 200; }  // Early in pipeline (after SSAO)

        void Configure(const PostProcessSettings& settings) override;
        void AddToGraph(RenderGraph& graph, RGTextureHandle input, RGTextureHandle output) override;

        // =========================================================================
        // Extended AddToGraph with depth buffer
        // =========================================================================

        /**
         * @brief Add DOF to render graph with depth buffer
         * @param graph Render graph
         * @param input Scene color input
         * @param depth Scene depth buffer
         * @param output Final output texture
         */
        void AddToGraph(RenderGraph& graph, RGTextureHandle input, 
                        RGTextureHandle depth, RGTextureHandle output);

        // =========================================================================
        // Configuration
        // =========================================================================

        void SetConfig(const DOFConfig& config) { m_config = config; }
        const DOFConfig& GetConfig() const { return m_config; }

        void SetFocusDistance(float distance) { m_config.focusDistance = distance; }
        float GetFocusDistance() const { return m_config.focusDistance; }

        void SetAperture(float fstop) { m_config.aperture = fstop; }
        float GetAperture() const { return m_config.aperture; }

        void SetFocalLength(float mm) { m_config.focalLength = mm; }
        float GetFocalLength() const { return m_config.focalLength; }

        void SetFocusRange(float range) { m_config.focusRange = range; }
        float GetFocusRange() const { return m_config.focusRange; }

        void SetMaxBlurRadius(float radius) { m_config.maxBlurRadius = radius; }
        float GetMaxBlurRadius() const { return m_config.maxBlurRadius; }

        void SetQuality(DOFQuality quality) { m_config.quality = quality; }
        DOFQuality GetQuality() const { return m_config.quality; }

        void SetBokehShape(BokehShape shape) { m_config.bokehShape = shape; }
        BokehShape GetBokehShape() const { return m_config.bokehShape; }

        void SetHalfResolution(bool enable) { m_config.halfResolution = enable; }
        bool IsHalfResolution() const { return m_config.halfResolution; }

        // =========================================================================
        // Focus control
        // =========================================================================

        /**
         * @brief Set focus using autofocus from screen position
         * @param screenX Normalized screen X (0-1)
         * @param screenY Normalized screen Y (0-1)
         * @param depth Depth value at that position
         */
        void SetAutoFocus(float screenX, float screenY, float depth);

        /**
         * @brief Enable smooth focus transition
         */
        void SetFocusTransitionSpeed(float speed) { m_focusTransitionSpeed = speed; }

    private:
        /**
         * @brief Calculate Circle of Confusion size
         * @param depth Linear depth in meters
         * @return CoC radius in pixels (negative for foreground)
         */
        float CalculateCoC(float depth) const;

        DOFConfig m_config;
        float m_focusTransitionSpeed = 5.0f;
        float m_currentFocusDistance = 10.0f;
        RGTextureHandle m_depthTexture;
    };

} // namespace RVX
