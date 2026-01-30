#pragma once

/**
 * @file VolumetricLighting.h
 * @brief Volumetric lighting post-process effect
 * 
 * Implements screen-space volumetric lighting (god rays, light shafts).
 */

#include "Core/MathTypes.h"
#include "Render/PostProcess/PostProcessStack.h"

namespace RVX
{
    class ClusteredLighting;

    /**
     * @brief Volumetric lighting quality
     */
    enum class VolumetricQuality : uint8
    {
        Low,            ///< 16 samples, full resolution
        Medium,         ///< 32 samples, half resolution
        High,           ///< 64 samples, half resolution
        Ultra           ///< 128 samples, full resolution
    };

    /**
     * @brief Volumetric lighting configuration
     */
    struct VolumetricLightingConfig
    {
        VolumetricQuality quality = VolumetricQuality::Medium;
        
        // Scattering parameters
        float intensity = 1.0f;             ///< Overall intensity
        float scattering = 0.1f;            ///< Scattering coefficient (fog density)
        float anisotropy = 0.7f;            ///< Mie scattering anisotropy (-1 to 1, 0 = isotropic)
        float absorption = 0.05f;           ///< Light absorption coefficient
        
        // Ray marching
        float maxDistance = 100.0f;         ///< Maximum ray march distance
        float jitterAmount = 0.5f;          ///< Temporal jitter for noise reduction
        
        // Height fog integration
        bool useHeightFog = true;
        float fogHeight = 10.0f;            ///< Height at which fog starts to fade
        float fogFalloff = 0.5f;            ///< How quickly fog fades with height
        
        // Noise (for volumetric clouds/fog variation)
        bool useNoise = true;
        float noiseScale = 0.02f;
        float noiseIntensity = 0.3f;
        
        // Temporal filtering
        bool temporalReprojection = true;
        float temporalWeight = 0.95f;       ///< History blend weight
        
        // Optimization
        bool halfResolution = true;
        bool useClusteredLighting = true;   ///< Integrate with clustered lighting
    };

    /**
     * @brief Volumetric lighting post-process pass
     * 
     * Implements ray-marched volumetric lighting:
     * 1. Ray march through volume from camera
     * 2. Accumulate in-scattered light at each step
     * 3. Apply shadow map for shadowed regions
     * 4. Temporal reprojection for stability
     * 5. Bilateral upscale (if half-res)
     * 
     * Features:
     * - Physically-based scattering (Henyey-Greenstein phase function)
     * - Integration with shadow maps
     * - Clustered lighting integration for local lights
     * - Height fog integration
     * - 3D noise for variation
     * - Temporal reprojection
     */
    class VolumetricLightingPass : public IPostProcessPass
    {
    public:
        VolumetricLightingPass();
        ~VolumetricLightingPass() override = default;

        const char* GetName() const override { return "VolumetricLighting"; }
        int32 GetPriority() const override { return 50; }  // Very early, before most effects

        void Configure(const PostProcessSettings& settings) override;
        void AddToGraph(RenderGraph& graph, RGTextureHandle input, RGTextureHandle output) override;

        // =========================================================================
        // Extended AddToGraph with lighting data
        // =========================================================================

        /**
         * @brief Add volumetric lighting with shadow maps and depth
         */
        void AddToGraph(RenderGraph& graph, 
                        RGTextureHandle input,
                        RGTextureHandle depth,
                        RGTextureHandle shadowMap,
                        RGTextureHandle output);

        // =========================================================================
        // Configuration
        // =========================================================================

        void SetConfig(const VolumetricLightingConfig& config) { m_config = config; }
        const VolumetricLightingConfig& GetConfig() const { return m_config; }

        void SetIntensity(float intensity) { m_config.intensity = intensity; }
        float GetIntensity() const { return m_config.intensity; }

        void SetScattering(float scattering) { m_config.scattering = scattering; }
        float GetScattering() const { return m_config.scattering; }

        void SetAnisotropy(float anisotropy) { m_config.anisotropy = anisotropy; }
        float GetAnisotropy() const { return m_config.anisotropy; }

        void SetMaxDistance(float dist) { m_config.maxDistance = dist; }
        float GetMaxDistance() const { return m_config.maxDistance; }

        void SetQuality(VolumetricQuality quality) { m_config.quality = quality; }
        VolumetricQuality GetQuality() const { return m_config.quality; }

        void SetHeightFog(bool enable, float height = 10.0f, float falloff = 0.5f);
        bool IsHeightFogEnabled() const { return m_config.useHeightFog; }

        void SetTemporalReprojection(bool enable) { m_config.temporalReprojection = enable; }
        bool IsTemporalReprojectionEnabled() const { return m_config.temporalReprojection; }

        // =========================================================================
        // Lighting integration
        // =========================================================================

        void SetClusteredLighting(ClusteredLighting* clustering) { m_clusteredLighting = clustering; }

        /**
         * @brief Set directional light for volumetric shadows
         */
        void SetDirectionalLight(const Vec3& direction, const Vec3& color, float intensity);

        /**
         * @brief Set camera matrices for temporal reprojection
         */
        void SetCameraMatrices(const Mat4& view, const Mat4& proj, 
                                const Mat4& prevView, const Mat4& prevProj);

    private:
        VolumetricLightingConfig m_config;
        ClusteredLighting* m_clusteredLighting = nullptr;
        
        // Light data
        Vec3 m_lightDirection = Vec3(0.0f, -1.0f, 0.0f);
        Vec3 m_lightColor = Vec3(1.0f);
        float m_lightIntensity = 1.0f;
        
        // Camera data
        Mat4 m_viewMatrix;
        Mat4 m_projMatrix;
        Mat4 m_prevViewMatrix;
        Mat4 m_prevProjMatrix;
        Mat4 m_lightViewProj;
        
        // Temporal data
        uint32 m_frameIndex = 0;
    };

} // namespace RVX
