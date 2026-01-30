#pragma once

/**
 * @file AtmosphericScattering.h
 * @brief Atmospheric scattering for realistic sky rendering
 * 
 * Implements physically-based atmospheric scattering using
 * Rayleigh and Mie scattering models.
 */

#include "Core/Types.h"
#include "Core/MathTypes.h"
#include "RHI/RHI.h"
#include <memory>

namespace RVX
{
    class IRHIDevice;
    class RHICommandContext;
    class RenderGraph;
    struct RGTextureHandle;

    /**
     * @brief Atmospheric scattering configuration
     */
    struct AtmosphericScatteringConfig
    {
        // Planet parameters
        float planetRadius = 6371000.0f;        ///< Planet radius in meters (Earth = 6371km)
        float atmosphereHeight = 100000.0f;     ///< Atmosphere height in meters (100km)
        
        // Scattering coefficients
        Vec3 rayleighScattering = Vec3(5.8e-6f, 13.5e-6f, 33.1e-6f);  ///< Rayleigh at sea level
        Vec3 mieScattering = Vec3(21e-6f);      ///< Mie scattering coefficient
        Vec3 mieAbsorption = Vec3(4.4e-6f);     ///< Mie absorption
        float mieAnisotropy = 0.8f;             ///< Mie phase function anisotropy
        
        // Scale heights
        float rayleighScaleHeight = 8500.0f;    ///< Rayleigh scale height in meters
        float mieScaleHeight = 1200.0f;         ///< Mie scale height in meters
        
        // Ozone absorption (for more accurate blue)
        Vec3 ozoneAbsorption = Vec3(0.65e-6f, 1.88e-6f, 0.085e-6f);
        float ozoneHeight = 25000.0f;           ///< Ozone layer center height
        float ozoneWidth = 15000.0f;            ///< Ozone layer width
        
        // Sun parameters
        Vec3 sunDirection = Vec3(0.0f, 1.0f, 0.0f);
        Vec3 sunColor = Vec3(1.0f, 0.98f, 0.95f);
        float sunIntensity = 20.0f;             ///< Sun illuminance in lux (simplified)
        float sunDiskSize = 0.0046f;            ///< Angular radius (0.0046 = actual sun)
        
        // Quality
        uint32 transmittanceLutSize = 256;      ///< Transmittance LUT resolution
        uint32 scatteringLutSize = 32;          ///< Multi-scattering LUT resolution
        uint32 skyViewLutSize = 192;            ///< Sky view LUT resolution
        uint32 numScatteringSamples = 32;       ///< Ray march samples
        
        // Multi-scattering approximation
        bool enableMultiScattering = true;
        uint32 multiScatteringOrder = 3;
        
        // Aerial perspective
        bool enableAerialPerspective = true;
        float aerialPerspectiveDistance = 50000.0f;
    };

    /**
     * @brief Atmospheric scattering renderer
     * 
     * Implements a physically-based atmospheric scattering model based on
     * Bruneton's improved model.
     * 
     * Features:
     * - Rayleigh scattering (blue sky)
     * - Mie scattering (sun halo, haze)
     * - Ozone absorption (deep blue at high altitudes)
     * - Multi-scattering approximation
     * - Precomputed transmittance LUT
     * - Sky-view LUT for fast rendering
     * - Aerial perspective
     * - Sunrise/sunset color gradients
     * 
     * LUT Precomputation:
     * 1. Transmittance LUT: optical depth along view rays
     * 2. Multi-scattering LUT: infinite bounces approximation
     * 3. Sky-View LUT: full sky rendering from ground
     */
    class AtmosphericScattering
    {
    public:
        AtmosphericScattering() = default;
        ~AtmosphericScattering();

        // Non-copyable
        AtmosphericScattering(const AtmosphericScattering&) = delete;
        AtmosphericScattering& operator=(const AtmosphericScattering&) = delete;

        // =========================================================================
        // Lifecycle
        // =========================================================================

        void Initialize(IRHIDevice* device, const AtmosphericScatteringConfig& config = {});
        void Shutdown();
        bool IsInitialized() const { return m_device != nullptr; }

        // =========================================================================
        // Configuration
        // =========================================================================

        void SetConfig(const AtmosphericScatteringConfig& config);
        const AtmosphericScatteringConfig& GetConfig() const { return m_config; }

        /**
         * @brief Set sun direction (normalized)
         */
        void SetSunDirection(const Vec3& direction);
        const Vec3& GetSunDirection() const { return m_config.sunDirection; }

        /**
         * @brief Set sun color and intensity
         */
        void SetSunColor(const Vec3& color, float intensity);

        /**
         * @brief Set viewer height above planet surface
         */
        void SetViewerHeight(float height) { m_viewerHeight = height; }
        float GetViewerHeight() const { return m_viewerHeight; }

        void SetEnabled(bool enabled) { m_enabled = enabled; }
        bool IsEnabled() const { return m_enabled; }

        // =========================================================================
        // LUT Management
        // =========================================================================

        /**
         * @brief Check if LUTs need to be recomputed
         */
        bool NeedsLUTUpdate() const { return m_lutsNeedUpdate; }

        /**
         * @brief Force LUT recomputation
         */
        void InvalidateLUTs() { m_lutsNeedUpdate = true; }

        /**
         * @brief Precompute all LUTs (call when parameters change)
         */
        void PrecomputeLUTs(RHICommandContext& ctx);

        /**
         * @brief Get the transmittance LUT
         */
        RHITexture* GetTransmittanceLUT() const { return m_transmittanceLut.Get(); }

        /**
         * @brief Get the multi-scattering LUT
         */
        RHITexture* GetMultiScatteringLUT() const { return m_multiScatteringLut.Get(); }

        /**
         * @brief Get the sky-view LUT
         */
        RHITexture* GetSkyViewLUT() const { return m_skyViewLut.Get(); }

        // =========================================================================
        // Rendering
        // =========================================================================

        /**
         * @brief Render the sky
         * @param ctx Command context
         * @param outputTarget Render target
         * @param depthBuffer Scene depth (for aerial perspective)
         * @param viewMatrix Camera view matrix
         * @param projMatrix Camera projection matrix
         */
        void RenderSky(RHICommandContext& ctx,
                       RHITexture* outputTarget,
                       RHITexture* depthBuffer,
                       const Mat4& viewMatrix,
                       const Mat4& projMatrix);

        /**
         * @brief Add sky rendering to render graph
         */
        void AddToGraph(RenderGraph& graph,
                        RGTextureHandle outputTarget,
                        RGTextureHandle depthBuffer,
                        const Mat4& viewMatrix,
                        const Mat4& projMatrix);

        /**
         * @brief Apply aerial perspective to scene color
         */
        void ApplyAerialPerspective(RenderGraph& graph,
                                     RGTextureHandle sceneColor,
                                     RGTextureHandle depth,
                                     RGTextureHandle output,
                                     const Mat4& viewMatrix,
                                     const Mat4& projMatrix);

        // =========================================================================
        // Utility
        // =========================================================================

        /**
         * @brief Get sky color at a given direction (for ambient lighting)
         */
        Vec3 GetSkyColor(const Vec3& direction) const;

        /**
         * @brief Get sun disk color at the current sun position
         */
        Vec3 GetSunDiskColor() const;

        /**
         * @brief Calculate transmittance along a ray
         */
        Vec3 GetTransmittance(const Vec3& origin, const Vec3& direction, float distance) const;

    private:
        void CreateLUTs();
        void ComputeTransmittanceLUT(RHICommandContext& ctx);
        void ComputeMultiScatteringLUT(RHICommandContext& ctx);
        void ComputeSkyViewLUT(RHICommandContext& ctx);

        IRHIDevice* m_device = nullptr;
        AtmosphericScatteringConfig m_config;
        bool m_enabled = true;

        float m_viewerHeight = 1.0f;  // Above planet surface
        bool m_lutsNeedUpdate = true;

        // Precomputed LUTs
        RHITextureRef m_transmittanceLut;       // 2D: (cos zenith, altitude)
        RHITextureRef m_multiScatteringLut;     // 2D: (cos sun zenith, altitude)
        RHITextureRef m_skyViewLut;             // 2D: (azimuth, zenith)
        RHITextureRef m_aerialPerspectiveLut;   // 3D: (x, y, depth)

        // Pipelines
        RHIPipelineRef m_transmittancePipeline;
        RHIPipelineRef m_multiScatteringPipeline;
        RHIPipelineRef m_skyViewPipeline;
        RHIPipelineRef m_skyRenderPipeline;
        RHIPipelineRef m_aerialPerspectivePipeline;

        // Constant buffer
        RHIBufferRef m_constantBuffer;
    };

} // namespace RVX
