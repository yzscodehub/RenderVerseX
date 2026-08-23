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
    class LightingFramePlan;

    /** @brief Result of binding immutable lighting selection to GPU light buffers. */
    enum class LightingPlanCollectionCode : uint8
    {
        NotBound = 0,
        /** CPU selection passed all plan checks but has not reached GPU memory. */
        Staged,
        Ready,
        InvalidPlan,
        InvalidEvidence,
        PerTypeCapacityExceeded,
        UploadFailed,
    };

    /** Exact terminal status of one plan-to-GPU upload attempt. */
    enum class LightingGpuUploadCode : uint8
    {
        NotAttempted = 0,
        Ready,
        NotInitialized,
        PlanNotStaged,
        MissingLightConstantsBuffer,
        MissingPointLightsBuffer,
        MissingSpotLightsBuffer,
        InvalidLightConstantsBufferSize,
        InvalidPointLightsBufferSize,
        InvalidSpotLightsBufferSize,
        MapLightConstantsFailed,
        MapPointLightsFailed,
        MapSpotLightsFailed,
    };

    [[nodiscard]] const char* GetLightingGpuUploadCodeName(
        LightingGpuUploadCode code) noexcept;

    /**
     * @brief Result of atomically publishing one staged light plan to GPU buffers.
     *
     * A non-ready result never publishes a plan identity.  Callers must not
     * freeze or bind the buffers from that frame, even if an older frame's
     * bytes remain physically resident in an upload allocation.
     */
    struct LightingGpuUploadResult
    {
        LightingGpuUploadCode code = LightingGpuUploadCode::NotAttempted;
        uint64 publishedLightingPlanIdentity = 0;

        [[nodiscard]] constexpr bool Succeeded() const noexcept
        {
            return code == LightingGpuUploadCode::Ready &&
                   publishedLightingPlanIdentity != 0;
        }
    };

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
        void CollectLights(const RenderScene& scene,
                           uint32 cullingMask = ~0U);

        /**
         * @brief Collect exactly the immutable selection published by one lighting plan.
         *
         * The plan's aggregate and per-type capacity evidence are authoritative.
         * Per-type GPU capacities are an additional hard safety limit; a plan
         * that exceeds one fails closed rather than silently changing selection.
         * @return True only when every selected plan light was accepted unchanged.
         */
        [[nodiscard]] bool CollectLights(const LightingFramePlan& plan);

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
         * @brief Transactionally upload the staged light plan to GPU buffers.
         *
         * All three upload buffers are mapped before any bytes are written.
         * A failed map unmaps any earlier mapping, invalidates the staged plan,
         * and returns an explicit error so callers cannot publish stale bytes.
         */
        [[nodiscard]] LightingGpuUploadResult UpdateGPUBuffers();

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
        uint32 GetPointLightRequestedCount() const { return m_pointLightRequestedCount; }
        uint32 GetSpotLightRequestedCount() const { return m_spotLightRequestedCount; }
        uint32 GetPointLightAdmittedCount() const { return GetPointLightCount(); }
        uint32 GetSpotLightAdmittedCount() const { return GetSpotLightCount(); }
        uint32 GetPointLightCapacity() const { return MaxPointLights; }
        uint32 GetSpotLightCapacity() const { return MaxSpotLights; }
        uint32 GetPointLightOverflowCount() const { return m_pointLightOverflowCount; }
        uint32 GetSpotLightOverflowCount() const { return m_spotLightOverflowCount; }
        uint32 GetPointShadowRequestCount() const { return m_pointShadowRequestCount; }
        uint32 GetSpotShadowRequestCount() const { return m_spotShadowRequestCount; }
        uint32 GetLocalShadowRequestCount() const
        {
            return m_pointShadowRequestCount + m_spotShadowRequestCount;
        }
        [[nodiscard]] bool IsLightingPlanBound() const noexcept
        {
            return m_lightingPlanCollectionCode == LightingPlanCollectionCode::Ready &&
                   m_gpuUploadResult.Succeeded();
        }
        [[nodiscard]] LightingPlanCollectionCode
            GetLightingPlanCollectionCode() const noexcept
        {
            return m_lightingPlanCollectionCode;
        }
        [[nodiscard]] uint64 GetLightingPlanIdentity() const noexcept
        {
            return m_lightingPlanIdentity;
        }
        [[nodiscard]] const LightingGpuUploadResult&
            GetLastGpuUploadResult() const noexcept
        {
            return m_gpuUploadResult;
        }
        [[nodiscard]] uint64 GetPrimaryDirectionalLightId() const noexcept
        {
            return m_primaryDirectionalLightId;
        }
        [[nodiscard]] bool HasPrimaryDirectionalShadowRequest() const noexcept
        {
            return m_primaryDirectionalShadowRequested;
        }
        [[nodiscard]] uint32 GetPlanLocalLightRequestedCount() const noexcept
        {
            return m_planLocalLightRequestedCount;
        }
        [[nodiscard]] uint32 GetPlanLocalLightAdmittedCount() const noexcept
        {
            return m_planLocalLightAdmittedCount;
        }
        [[nodiscard]] uint32 GetPlanLocalLightOverflowCount() const noexcept
        {
            return m_planLocalLightOverflowCount;
        }

        static constexpr uint32 MaxPointLights = 256;
        static constexpr uint32 MaxSpotLights = 128;

    private:
        void EnsureBuffers();
        LightingGpuUploadResult FailGpuUpload(LightingGpuUploadCode code);

        IRHIDevice* m_device = nullptr;

        // CPU-side light data
        GPUDirectionalLight m_mainLight;
        std::vector<GPUPointLight> m_pointLights;
        std::vector<GPUSpotLight> m_spotLights;
        uint32 m_pointLightRequestedCount = 0;
        uint32 m_spotLightRequestedCount = 0;
        uint32 m_pointLightOverflowCount = 0;
        uint32 m_spotLightOverflowCount = 0;
        uint32 m_pointShadowRequestCount = 0;
        uint32 m_spotShadowRequestCount = 0;
        LightingPlanCollectionCode m_lightingPlanCollectionCode =
            LightingPlanCollectionCode::NotBound;
        LightingGpuUploadResult m_gpuUploadResult;
        /** Selection identity awaiting an all-or-nothing upload. */
        uint64 m_stagedLightingPlanIdentity = 0;
        /** Identity currently safe to bind for this frame. */
        uint64 m_lightingPlanIdentity = 0;
        uint64 m_primaryDirectionalLightId = 0;
        bool m_primaryDirectionalShadowRequested = false;
        uint32 m_planLocalLightRequestedCount = 0;
        uint32 m_planLocalLightAdmittedCount = 0;
        uint32 m_planLocalLightOverflowCount = 0;

        // GPU buffers
        RHIBufferRef m_lightConstantsBuffer;
        RHIBufferRef m_pointLightsBuffer;
        RHIBufferRef m_spotLightsBuffer;
    };

} // namespace RVX
