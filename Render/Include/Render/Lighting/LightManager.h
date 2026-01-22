#pragma once

/**
 * @file LightManager.h
 * @brief Manages scene lights and GPU light buffers
 */

#include "Core/Types.h"
#include "Core/MathTypes.h"
#include "RHI/RHI.h"
#include <vector>

namespace RVX
{
    class RenderScene;

    /**
     * @brief GPU light data for directional lights
     */
    struct GPUDirectionalLight
    {
        Vec3 direction{0.0f, -1.0f, 0.0f};
        float intensity = 1.0f;
        Vec3 color{1.0f, 1.0f, 1.0f};
        int32 shadowMapIndex = -1;
        Mat4 lightSpaceMatrix = Mat4Identity();
    };

    /**
     * @brief GPU light data for point lights
     */
    struct GPUPointLight
    {
        Vec3 position{0.0f, 0.0f, 0.0f};
        float range = 10.0f;
        Vec3 color{1.0f, 1.0f, 1.0f};
        float intensity = 1.0f;
    };

    /**
     * @brief GPU light data for spot lights
     */
    struct GPUSpotLight
    {
        Vec3 position{0.0f, 0.0f, 0.0f};
        float range = 10.0f;
        Vec3 direction{0.0f, -1.0f, 0.0f};
        float innerConeAngle = 0.9f;  // cos(inner)
        Vec3 color{1.0f, 1.0f, 1.0f};
        float outerConeAngle = 0.8f;  // cos(outer)
        float intensity = 1.0f;
        int32 shadowMapIndex = -1;
        Vec2 padding{0.0f, 0.0f};
    };

    /**
     * @brief Light constants for GPU
     */
    struct LightConstants
    {
        GPUDirectionalLight mainLight;
        uint32 numPointLights = 0;
        uint32 numSpotLights = 0;
        Vec2 padding{0.0f, 0.0f};
    };

    /**
     * @brief Manages scene lighting and GPU buffers
     */
    class LightManager
    {
    public:
        LightManager() = default;
        ~LightManager();

        // Non-copyable
        LightManager(const LightManager&) = delete;
        LightManager& operator=(const LightManager&) = delete;

        // =========================================================================
        // Lifecycle
        // =========================================================================

        void Initialize(IRHIDevice* device);
        void Shutdown();
        bool IsInitialized() const { return m_device != nullptr; }

        // =========================================================================
        // Light Collection
        // =========================================================================

        /**
         * @brief Collect lights from render scene
         */
        void CollectLights(const RenderScene& scene);

        /**
         * @brief Set the main directional light
         */
        void SetMainLight(const Vec3& direction, const Vec3& color, float intensity);

        /**
         * @brief Add a point light
         */
        void AddPointLight(const Vec3& position, const Vec3& color, float intensity, float range);

        /**
         * @brief Add a spot light
         */
        void AddSpotLight(const Vec3& position, const Vec3& direction, const Vec3& color,
                          float intensity, float range, float innerAngle, float outerAngle);

        /**
         * @brief Clear all lights for new frame
         */
        void Clear();

        // =========================================================================
        // GPU Buffer Management
        // =========================================================================

        /**
         * @brief Update GPU buffers with current light data
         */
        void UpdateGPUBuffers();

        /**
         * @brief Get the light constants buffer
         */
        RHIBuffer* GetLightConstantsBuffer() const { return m_lightConstantsBuffer.Get(); }

        /**
         * @brief Get the point lights structured buffer
         */
        RHIBuffer* GetPointLightsBuffer() const { return m_pointLightsBuffer.Get(); }

        /**
         * @brief Get the spot lights structured buffer
         */
        RHIBuffer* GetSpotLightsBuffer() const { return m_spotLightsBuffer.Get(); }

        // =========================================================================
        // Accessors
        // =========================================================================

        const GPUDirectionalLight& GetMainLight() const { return m_mainLight; }
        const std::vector<GPUPointLight>& GetPointLights() const { return m_pointLights; }
        const std::vector<GPUSpotLight>& GetSpotLights() const { return m_spotLights; }

        uint32 GetPointLightCount() const { return static_cast<uint32>(m_pointLights.size()); }
        uint32 GetSpotLightCount() const { return static_cast<uint32>(m_spotLights.size()); }

        static constexpr uint32 MaxPointLights = 256;
        static constexpr uint32 MaxSpotLights = 128;

    private:
        void EnsureBuffers();

        IRHIDevice* m_device = nullptr;

        // CPU-side light data
        GPUDirectionalLight m_mainLight;
        std::vector<GPUPointLight> m_pointLights;
        std::vector<GPUSpotLight> m_spotLights;

        // GPU buffers
        RHIBufferRef m_lightConstantsBuffer;
        RHIBufferRef m_pointLightsBuffer;
        RHIBufferRef m_spotLightsBuffer;
    };

} // namespace RVX
