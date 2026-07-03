#pragma once

/**
 * @file Underwater.h
 * @brief Underwater post-processing effects
 * 
 * Provides visual effects for when the camera is below the water surface.
 */

#include "Water/WaterTypes.h"
#include "RHI/RHITexture.h"
#include "RHI/RHIBuffer.h"
#include "RHI/RHIPipeline.h"

#include <memory>

namespace RVX
{
    class IRHIDevice;
    class RHICommandContext;

    /**
     * @brief Underwater post-processing effects
     * 
     * Applies visual effects when the camera is submerged, including:
     * - Color tinting and fog
     * - Light absorption based on depth
     * - Screen distortion/refraction
     * - Motion blur
     * - God rays from the surface
     * - Floating particle effects
     * 
     * Usage:
     * @code
     * UnderwaterDesc desc;
     * desc.quality = UnderwaterQuality::High;
     * desc.properties.fogColor = Vec3(0.0f, 0.2f, 0.3f);
     * 
     * auto underwater = std::make_unique<Underwater>();
     * underwater->Initialize(desc);
     * underwater->InitializeGPU(device);
     * 
     * // During rendering
     * if (camera.position.y < waterHeight) {
     *     float depth = waterHeight - camera.position.y;
     *     underwater->Apply(ctx, colorTarget, depthTarget, depth, lightDir);
     * }
     * @endcode
     */
    class Underwater
    {
    public:
        using Ptr = std::unique_ptr<Underwater>;

        Underwater() = default;
        ~Underwater() = default;

        // Non-copyable
        Underwater(const Underwater&) = delete;
        Underwater& operator=(const Underwater&) = delete;

        // =====================================================================
        // Initialization
        // =====================================================================

        /**
         * @brief Initialize underwater effects
         * @param desc Underwater descriptor
         * @return true if initialization succeeded
         */
        bool Initialize(const UnderwaterDesc& desc);

        /**
         * @brief Initialize GPU resources
         * @param device RHI device
         * @return true if initialization succeeded
         */
        bool InitializeGPU(IRHIDevice* device);

        // =====================================================================
        // Rendering
        // =====================================================================

        /**
         * @brief Update effects animation
         * @param deltaTime Frame delta time
         */
        void Update(float deltaTime);

        /**
         * @brief Apply underwater effects
         * @param ctx Command context
         * @param colorTarget Scene color target
         * @param depthTarget Scene depth target
         * @param cameraDepth Camera depth below water
         * @param lightDirection Light direction for god rays
         */
        void Apply(RHICommandContext& ctx, RHITexture* colorTarget, RHITexture* depthTarget,
                   float cameraDepth, const Vec3& lightDirection);

        // =====================================================================
        // Properties
        // =====================================================================

        UnderwaterProperties& GetProperties() { return m_properties; }
        const UnderwaterProperties& GetProperties() const { return m_properties; }
        void SetProperties(const UnderwaterProperties& props);

        UnderwaterQuality GetQuality() const { return m_quality; }
        void SetQuality(UnderwaterQuality quality);

        // =====================================================================
        // State
        // =====================================================================

        /**
         * @brief Set whether camera is underwater
         */
        void SetUnderwater(bool underwater) { m_isUnderwater = underwater; }
        bool IsUnderwater() const { return m_isUnderwater; }

        // =====================================================================
        // GPU Resources
        // =====================================================================

        /**
         * @brief Check if GPU initialized
         */
        bool IsGPUInitialized() const { return m_gpuInitialized; }

    private:
        void ApplyFog(RHICommandContext& ctx, RHITexture* colorTarget, float depth);
        void ApplyDistortion(RHICommandContext& ctx, RHITexture* colorTarget);
        void ApplyGodRays(RHICommandContext& ctx, RHITexture* colorTarget, 
                          const Vec3& lightDir);
        void RenderParticles(RHICommandContext& ctx);

        UnderwaterQuality m_quality = UnderwaterQuality::High;
        UnderwaterProperties m_properties;
        bool m_isUnderwater = false;
        float m_time = 0.0f;

        // GPU resources
        RHITextureRef m_tempTexture;
        RHIBufferRef m_paramBuffer;
        RHIPipelineRef m_fogPipeline;
        RHIPipelineRef m_distortionPipeline;
        RHIPipelineRef m_godRayPipeline;
        RHIPipelineRef m_particlePipeline;

        // Particle data
        RHIBufferRef m_particleBuffer;
        uint32 m_particleCount = 0;

        bool m_gpuInitialized = false;
    };

} // namespace RVX
