#pragma once

/**
 * @file LightManager.h
 * @brief Manages scene lights and GPU light buffers
 */

#include "Core/Types.h"
#include "Core/MathTypes.h"
#include "RHI/RHI.h"

#include <array>
#include <cstddef>
#include <type_traits>
#include <vector>

namespace RVX
{
    class RenderScene;
    class LightingFramePlan;
    class LocalShadowFramePlan;
    class LocalShadowAtlasPlan;

    /**
     * @brief Fixed shader-visible local-shadow page budget.
     *
     * The CPU atlas may reserve a larger physical array, but the current
     * lighting ABI deliberately exposes at most this many matrices/pages.
     * Plans exceeding this visible budget are rejected before any GPU upload.
     */
    inline constexpr uint32 RVX_MAX_LOCAL_SHADOW_PAGES = 32;

    /** @brief Receiver bias values copied into the immutable light constants. */
    struct LocalShadowReceiverSettings
    {
        float32 depthBias = 0.001f;
        float32 normalBias = 0.01f;
    };

    /** Exact terminal result of attaching one local-shadow atlas to staged lights. */
    enum class LocalShadowBindingCode : uint8
    {
        NotApplied = 0,
        Ready,
        PlanNotStaged,
        InvalidLocalShadowPlan,
        InvalidAtlasPlan,
        LightingPlanIdentityMismatch,
        AtlasPlanIdentityMismatch,
        AtlasGenerationMismatch,
        VisiblePageCapacityExceeded,
        InvalidReceiverSettings,
        InvalidTextureContract,
        AllocationMismatch,
    };

    [[nodiscard]] const char* GetLocalShadowBindingCodeName(
        LocalShadowBindingCode code) noexcept;

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
        /** First Texture2DArray page for this point light, or -1 if unshadowed. */
        int32 shadowMapIndex = -1;
        // Scalar tail mirrors the HLSL SSBO contract. A vec3 at byte 36 is
        // not representable by GLSL std430 on the OpenGL compatibility path.
        float32 padding0 = 0.0f;
        float32 padding1 = 0.0f;
        float32 padding2 = 0.0f;
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
        /** Page-indexed right-handed local shadow view-projection table. */
        std::array<Mat4, RVX_MAX_LOCAL_SHADOW_PAGES>
            localShadowViewProjections{};
        /** x/y/z/w = visible page count, inverse page resolution, depth bias, normal bias. */
        uint32 localShadowPageCount = 0;
        float32 localShadowInvResolution = 0.0f;
        float32 localShadowDepthBias = 0.0f;
        float32 localShadowNormalBias = 0.0f;
    };

    static_assert(std::is_standard_layout_v<GPUDirectionalLight>);
    static_assert(std::is_standard_layout_v<GPUPointLight>);
    static_assert(std::is_standard_layout_v<GPUSpotLight>);
    static_assert(std::is_standard_layout_v<LightConstants>);
    static_assert(sizeof(GPUDirectionalLight) == 96);
    static_assert(sizeof(GPUPointLight) == 48);
    static_assert(offsetof(GPUPointLight, shadowMapIndex) == 32);
    static_assert(offsetof(GPUPointLight, padding0) == 36);
    static_assert(offsetof(GPUPointLight, padding1) == 40);
    static_assert(offsetof(GPUPointLight, padding2) == 44);
    static_assert(sizeof(GPUSpotLight) == 64);
    static_assert(offsetof(GPUSpotLight, shadowMapIndex) == 52);
    static_assert(sizeof(LightConstants) == 2176);
    static_assert(offsetof(LightConstants, numPointLights) == 96);
    static_assert(offsetof(LightConstants, localShadowViewProjections) == 112);
    static_assert(offsetof(LightConstants, localShadowPageCount) == 2160);

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
         * @brief Attach the exact local-shadow atlas plan to the currently
         * staged LightingFramePlan before GPU upload.
         *
         * This is deliberately a narrow one-way operation: it validates all
         * frame, plan, allocation and atlas identities, writes only the
         * selected point/spot page bases and page matrices, and invalidates
         * the staged upload on any mismatch.  Unallocated lights retain -1
         * and therefore remain explicitly lit.
         */
        [[nodiscard]] bool ApplyLocalShadowPlan(
            const LocalShadowFramePlan& localShadowPlan,
            const LocalShadowAtlasPlan& atlasPlan,
            LocalShadowReceiverSettings receiverSettings = {});

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
        [[nodiscard]] LocalShadowBindingCode
            GetLocalShadowBindingCode() const noexcept
        {
            return m_localShadowBindingCode;
        }
        [[nodiscard]] bool HasLocalShadowPlan() const noexcept
        {
            return m_localShadowBindingCode == LocalShadowBindingCode::Ready;
        }

        static constexpr uint32 MaxPointLights = 256;
        static constexpr uint32 MaxSpotLights = 128;

    private:
        void EnsureBuffers();
        LightingGpuUploadResult FailGpuUpload(LightingGpuUploadCode code);
        [[nodiscard]] bool FailLocalShadowBinding(
            LocalShadowBindingCode code,
            const char* diagnostic);
        void ClearLocalShadowState() noexcept;

        IRHIDevice* m_device = nullptr;

        // CPU-side light data
        GPUDirectionalLight m_mainLight;
        std::vector<GPUPointLight> m_pointLights;
        std::vector<GPUSpotLight> m_spotLights;
        /** Exact LightingFramePlan identities for the corresponding GPU rows. */
        std::vector<uint64> m_pointLightIds;
        std::vector<uint64> m_spotLightIds;
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
        LocalShadowBindingCode m_localShadowBindingCode =
            LocalShadowBindingCode::NotApplied;
        uint64 m_localShadowAtlasPlanIdentity = 0;
        uint64 m_localShadowAtlasGeneration = 0;
        std::array<Mat4, RVX_MAX_LOCAL_SHADOW_PAGES>
            m_localShadowViewProjections{};
        uint32 m_localShadowPageCount = 0;
        float32 m_localShadowInvResolution = 0.0f;
        float32 m_localShadowDepthBias = 0.0f;
        float32 m_localShadowNormalBias = 0.0f;

        // GPU buffers
        RHIBufferRef m_lightConstantsBuffer;
        RHIBufferRef m_pointLightsBuffer;
        RHIBufferRef m_spotLightsBuffer;
    };

} // namespace RVX
