#pragma once

/**
 * @file MotionBlur.h
 * @brief Motion blur post-process effect
 * 
 * Implements per-object motion blur using velocity buffers.
 */

#include "Core/MathTypes.h"
#include "Render/PostProcess/PostProcessStack.h"

namespace RVX
{
    /**
     * @brief Motion blur quality presets
     */
    enum class MotionBlurQuality : uint8
    {
        Low,            ///< 4 samples
        Medium,         ///< 8 samples  
        High,           ///< 16 samples
        Ultra           ///< 32 samples
    };

    /**
     * @brief Motion blur configuration
     */
    struct MotionBlurConfig
    {
        // Blur settings
        float intensity = 1.0f;             ///< Overall blur intensity
        float maxVelocity = 32.0f;          ///< Maximum velocity in pixels
        float minVelocity = 0.5f;           ///< Minimum velocity to trigger blur
        
        // Quality
        MotionBlurQuality quality = MotionBlurQuality::Medium;
        
        // Shutter settings
        float shutterAngle = 180.0f;        ///< Shutter angle (degrees, 360 = full frame)
        float shutterPhase = 0.0f;          ///< Shutter phase offset
        
        // Options
        bool perObjectMotionBlur = true;    ///< Use per-object motion vectors
        bool cameraMotionBlur = true;       ///< Apply camera motion blur
        bool depthAwareBlur = true;         ///< Use depth to prevent background bleeding
        
        // Reconstruction
        float softZDistance = 0.1f;         ///< Soft depth comparison distance
        float jitterStrength = 0.5f;        ///< Sample jittering for noise reduction
        
        // Tile-based optimization
        bool useTileMax = true;             ///< Use tile-based max velocity for optimization
        uint32 tileSize = 20;               ///< Tile size in pixels
    };

    /**
     * @brief Motion blur post-process pass
     * 
     * Implements a velocity-based motion blur effect:
     * 1. Compute per-pixel velocity from motion vectors
     * 2. Tile-based velocity max for optimization
     * 3. Scatter/gather blur along velocity direction
     * 4. Depth-aware filtering to prevent artifacts
     * 
     * Features:
     * - Per-object motion blur using motion vectors
     * - Camera motion blur for fast camera movement
     * - Tile-based optimization for performance
     * - Depth-aware filtering to prevent bleeding
     * - Variable shutter angle simulation
     */
    class MotionBlurPass : public IPostProcessPass
    {
    public:
        MotionBlurPass();
        ~MotionBlurPass() override = default;

        const char* GetName() const override { return "MotionBlur"; }
        int32 GetPriority() const override { return 100; }  // Very early, before DOF

        void Configure(const PostProcessSettings& settings) override;
        void AddToGraph(RenderGraph& graph, RGTextureHandle input, RGTextureHandle output) override;

        // =========================================================================
        // Extended AddToGraph with velocity buffer
        // =========================================================================

        /**
         * @brief Add motion blur to render graph with velocity buffer
         * @param graph Render graph
         * @param input Scene color input
         * @param velocity Motion vector buffer (RG16F format)
         * @param depth Scene depth buffer
         * @param output Final output texture
         */
        void AddToGraph(RenderGraph& graph, RGTextureHandle input,
                        RGTextureHandle velocity, RGTextureHandle depth,
                        RGTextureHandle output);

        // =========================================================================
        // Configuration
        // =========================================================================

        void SetConfig(const MotionBlurConfig& config) { m_config = config; }
        const MotionBlurConfig& GetConfig() const { return m_config; }

        void SetIntensity(float intensity) { m_config.intensity = intensity; }
        float GetIntensity() const { return m_config.intensity; }

        void SetMaxVelocity(float maxVel) { m_config.maxVelocity = maxVel; }
        float GetMaxVelocity() const { return m_config.maxVelocity; }

        void SetShutterAngle(float degrees) { m_config.shutterAngle = degrees; }
        float GetShutterAngle() const { return m_config.shutterAngle; }

        void SetQuality(MotionBlurQuality quality) { m_config.quality = quality; }
        MotionBlurQuality GetQuality() const { return m_config.quality; }

        void SetDepthAware(bool enable) { m_config.depthAwareBlur = enable; }
        bool IsDepthAware() const { return m_config.depthAwareBlur; }

        void SetPerObjectBlur(bool enable) { m_config.perObjectMotionBlur = enable; }
        bool HasPerObjectBlur() const { return m_config.perObjectMotionBlur; }

        void SetCameraBlur(bool enable) { m_config.cameraMotionBlur = enable; }
        bool HasCameraBlur() const { return m_config.cameraMotionBlur; }

        // =========================================================================
        // Camera motion (when no velocity buffer available)
        // =========================================================================

        /**
         * @brief Set camera matrices for camera-based motion blur
         */
        void SetCameraMatrices(const Mat4& currentViewProj, const Mat4& prevViewProj);

    private:
        MotionBlurConfig m_config;
        Mat4 m_currentViewProj;
        Mat4 m_prevViewProj;
        bool m_hasCameraData = false;
    };

} // namespace RVX
