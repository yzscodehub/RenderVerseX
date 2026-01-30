#pragma once

/**
 * @file Caustics.h
 * @brief Underwater light caustics rendering
 * 
 * Simulates the light patterns created by water surface
 * focusing light onto underwater surfaces.
 */

#include "Core/Types.h"
#include "Core/MathTypes.h"
#include "RHI/RHITexture.h"
#include "RHI/RHIBuffer.h"
#include "RHI/RHIPipeline.h"

#include <memory>

namespace RVX
{
    class IRHIDevice;
    class RHICommandContext;
    class WaterSimulation;

    /**
     * @brief Caustics rendering quality
     */
    enum class CausticsQuality : uint8
    {
        Off,        ///< Disabled
        Low,        ///< Simple projected texture
        Medium,     ///< Animated caustics
        High        ///< Ray-traced caustics
    };

    /**
     * @brief Caustics configuration
     */
    struct CausticsDesc
    {
        CausticsQuality quality = CausticsQuality::Medium;
        uint32 textureSize = 512;           ///< Caustics texture resolution
        float intensity = 1.0f;             ///< Caustics brightness
        float scale = 5.0f;                 ///< UV scale for caustics pattern
        float speed = 1.0f;                 ///< Animation speed
        float maxDepth = 20.0f;             ///< Maximum depth for caustics
        float focusFalloff = 0.5f;          ///< How fast caustics fade with depth
    };

    /**
     * @brief Underwater caustics renderer
     * 
     * Generates and renders caustic light patterns that appear on
     * underwater surfaces when light is refracted through water.
     * 
     * Features:
     * - Dynamic caustics from wave simulation
     * - Animated caustics texture
     * - Depth-based intensity falloff
     * - Multiple quality levels
     * 
     * Usage:
     * @code
     * CausticsDesc desc;
     * desc.quality = CausticsQuality::High;
     * desc.intensity = 1.5f;
     * 
     * auto caustics = std::make_unique<Caustics>();
     * caustics->Initialize(desc);
     * caustics->InitializeGPU(device);
     * 
     * // Per frame
     * caustics->Update(deltaTime, waterSimulation);
     * caustics->GenerateCaustics(commandContext);
     * @endcode
     */
    class Caustics
    {
    public:
        using Ptr = std::unique_ptr<Caustics>;

        Caustics() = default;
        ~Caustics() = default;

        // Non-copyable
        Caustics(const Caustics&) = delete;
        Caustics& operator=(const Caustics&) = delete;

        // =====================================================================
        // Initialization
        // =====================================================================

        /**
         * @brief Initialize caustics renderer
         * @param desc Caustics descriptor
         * @return true if initialization succeeded
         */
        bool Initialize(const CausticsDesc& desc);

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
         * @brief Update caustics animation
         * @param deltaTime Frame delta time
         * @param simulation Water simulation for wave data
         */
        void Update(float deltaTime, const WaterSimulation* simulation);

        /**
         * @brief Generate caustics texture
         * @param ctx Command context
         */
        void GenerateCaustics(RHICommandContext& ctx);

        /**
         * @brief Apply caustics to scene
         * @param ctx Command context
         * @param depthTexture Scene depth texture
         * @param lightDir Light direction
         * @param waterHeight Water surface height
         */
        void ApplyCaustics(RHICommandContext& ctx, RHITexture* depthTexture,
                           const Vec3& lightDir, float waterHeight);

        // =====================================================================
        // Properties
        // =====================================================================

        void SetIntensity(float intensity) { m_intensity = intensity; }
        float GetIntensity() const { return m_intensity; }

        void SetScale(float scale) { m_scale = scale; }
        float GetScale() const { return m_scale; }

        void SetSpeed(float speed) { m_speed = speed; }
        float GetSpeed() const { return m_speed; }

        void SetMaxDepth(float depth) { m_maxDepth = depth; }
        float GetMaxDepth() const { return m_maxDepth; }

        CausticsQuality GetQuality() const { return m_quality; }

        // =====================================================================
        // GPU Resources
        // =====================================================================

        /**
         * @brief Get the caustics texture
         */
        RHITexture* GetCausticsTexture() const { return m_causticsTexture.Get(); }

        /**
         * @brief Check if GPU initialized
         */
        bool IsGPUInitialized() const { return m_gpuInitialized; }

    private:
        void GenerateAnimatedCaustics(RHICommandContext& ctx);
        void GenerateRaytracedCaustics(RHICommandContext& ctx);

        CausticsQuality m_quality = CausticsQuality::Medium;
        uint32 m_textureSize = 512;
        float m_intensity = 1.0f;
        float m_scale = 5.0f;
        float m_speed = 1.0f;
        float m_maxDepth = 20.0f;
        float m_focusFalloff = 0.5f;
        float m_time = 0.0f;

        // GPU resources
        RHITextureRef m_causticsTexture;
        RHITextureRef m_tempTexture;
        RHIBufferRef m_paramBuffer;
        RHIPipelineRef m_generatePipeline;
        RHIPipelineRef m_applyPipeline;

        bool m_gpuInitialized = false;
    };

} // namespace RVX
