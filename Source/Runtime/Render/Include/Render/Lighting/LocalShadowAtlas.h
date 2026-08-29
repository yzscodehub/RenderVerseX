#pragma once

/**
 * @file LocalShadowAtlas.h
 * @brief Immutable CPU texture-array and view-matrix plan for local shadows.
 */

#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "Render/Lighting/LightingFramePlan.h"
#include "Render/Lighting/LocalShadowFramePlan.h"
#include "RHI/RHITexture.h"

#include <optional>
#include <string>
#include <vector>

namespace RVX
{
    inline constexpr uint32 RVX_LOCAL_SHADOW_ATLAS_SCHEMA_VERSION = 1;
    inline constexpr float32 RVX_LOCAL_SHADOW_CLEAR_DEPTH = 1.0f;
    inline constexpr float32 RVX_LOCAL_SHADOW_NEAR_DEVICE_DEPTH = 0.0f;
    inline constexpr float32 RVX_LOCAL_SHADOW_FAR_DEVICE_DEPTH = 1.0f;
    inline constexpr uint32 RVX_LOCAL_SHADOW_DEPTH_BYTES_PER_TEXEL = 4;
    inline constexpr RHICompareOp RVX_LOCAL_SHADOW_DEPTH_COMPARE =
        RHICompareOp::LessEqual;

    /** @brief Canonical coordinate contract used by all local-shadow pages. */
    enum class LocalShadowCoordinateSystem : uint8
    {
        RightHandedYUp = 0,
    };

    /**
     * @brief Forward-Z [0,1]; clear/far are one and compare is LessEqual.
     */
    enum class LocalShadowDepthConvention : uint8
    {
        ForwardZeroToOne = 0,
    };

    /**
     * @brief Stable point-face order followed by the single spot projection.
     *
     * Point allocation page offsets are exactly the enum values [0,5]. The
     * order matches the conventional +X,-X,+Y,-Y,+Z,-Z cube-face order while
     * the backing resource remains a Texture2D array, not a cube texture.
     */
    enum class LocalShadowPageProjection : uint8
    {
        PointPositiveX = 0,
        PointNegativeX,
        PointPositiveY,
        PointNegativeY,
        PointPositiveZ,
        PointNegativeZ,
        Spot,
    };

    /** @brief Fail-closed result of producing the CPU atlas/view plan. */
    enum class LocalShadowAtlasPlanCode : uint8
    {
        Ready = 0,
        InvalidSchema,
        InvalidLightingPlan,
        InvalidLocalShadowPlan,
        AtlasGenerationMismatch,
        LocalShadowPlanIdentityMismatch,
        InvalidProjectionConfiguration,
        ArithmeticOverflow,
        MissingLight,
        LightContractMismatch,
        InvalidClipRange,
        UnsupportedSpotCone,
        InvalidViewMatrix,
        IdentityMismatch,
    };

    /**
     * @brief Full-capacity Render-owned D32 Texture2D-array allocation contract.
     *
     * RHI has no separate Texture2DArray dimension. A Texture2D descriptor
     * with arraySize > 1 is the canonical array representation. Each page is
     * addressed through a one-layer subresource view.
     */
    struct LocalShadowAtlasTextureContract
    {
        bool textureRequired = false;
        uint32 pageResolution = 0;
        uint32 pageCapacity = 0;
        uint64 estimatedAllocationBytes = 0;
        RHITextureDesc descriptor{};

        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] const RHITextureDesc* GetTextureDesc() const noexcept;
        [[nodiscard]] std::optional<RHITextureViewDesc>
            MakeDepthViewDesc(uint32 pageIndex) const noexcept;
        [[nodiscard]] std::optional<RHITextureViewDesc>
            MakeShaderResourceViewDesc() const noexcept;
    };

    /** @brief One exact depth-array page and its right-handed view projection. */
    struct LocalShadowAtlasPage
    {
        uint32 pageIndex = 0;
        uint64 lightId = 0;
        RenderLightType lightType = RenderLightType::Directional;
        uint32 lightBufferIndex = 0;
        LocalShadowAtlasHandle allocationHandle{};
        LocalShadowPageProjection projection =
            LocalShadowPageProjection::Spot;
        Vec3 lightPosition{0.0f};
        Vec3 forward{0.0f, 0.0f, -1.0f};
        Vec3 up{0.0f, 1.0f, 0.0f};
        float32 fieldOfViewRadians = 0.0f;
        float32 nearPlane = 0.0f;
        float32 farPlane = 0.0f;
        Mat4 view{1.0f};
        Mat4 projectionMatrix{1.0f};
        Mat4 viewProjection{1.0f};
    };

    /**
     * @brief Honest preparation evidence; this CPU contract records no GPU work.
     */
    struct LocalShadowAtlasExecutionEvidence
    {
        uint32 allocationCount = 0;
        uint32 pageCount = 0;
        uint32 pointLightCount = 0;
        uint32 pointPageCount = 0;
        uint32 spotLightCount = 0;
        uint32 spotPageCount = 0;
        uint32 pageCapacity = 0;
        uint64 estimatedAllocationBytes = 0;
        bool textureRequired = false;
        bool cpuPlanReady = false;
        bool gpuResourceCreated = false;
        bool gpuCommandsRecorded = false;
        bool gpuExecuted = false;
    };

    /** @brief Value-only request pinned to the Render-owned atlas generation. */
    struct LocalShadowAtlasPlanRequest
    {
        uint32 schemaVersion = RVX_LOCAL_SHADOW_ATLAS_SCHEMA_VERSION;
        /** Current non-zero generation supplied by the atlas resource owner. */
        uint64 atlasGeneration = 0;
        /** Exact near clip used for every selected local-light projection. */
        float32 nearPlane = 0.05f;
        /** Optional stale local-plan pin. */
        uint64 expectedLocalShadowPlanIdentity = 0;
        /** Optional deterministic output pin. */
        uint64 expectedIdentityHash = 0;
    };

    class LocalShadowAtlasPlanBuilder;

    /** @brief Immutable CPU atlas/view plan; it never claims GPU execution. */
    class LocalShadowAtlasPlan final
    {
    public:
        LocalShadowAtlasPlan() = default;

        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] bool IsCompatibleWith(
            const LightingFramePlan& lightingPlan,
            const LocalShadowFramePlan& localShadowPlan,
            uint64 atlasGeneration) const noexcept;
        [[nodiscard]] const LocalShadowAtlasPage* FindPage(
            uint32 pageIndex) const noexcept;
        [[nodiscard]] const LocalShadowAtlasPage* ResolvePage(
            LocalShadowAtlasHandle handle,
            uint32 pageOffset,
            uint64 expectedAtlasPlanIdentity,
            uint64 expectedAtlasGeneration) const noexcept;

        [[nodiscard]] const RenderFrameIdentity& GetFrameIdentity() const noexcept;
        [[nodiscard]] const RenderViewIdentity& GetViewIdentity() const noexcept;
        [[nodiscard]] uint64 GetLightingPlanIdentity() const noexcept;
        [[nodiscard]] uint64 GetLocalShadowPlanIdentity() const noexcept;
        [[nodiscard]] uint64 GetAtlasGeneration() const noexcept;
        [[nodiscard]] float32 GetNearPlane() const noexcept;
        [[nodiscard]] LocalShadowCoordinateSystem
            GetCoordinateSystem() const noexcept;
        [[nodiscard]] LocalShadowDepthConvention
            GetDepthConvention() const noexcept;
        [[nodiscard]] const LocalShadowAtlasTextureContract&
            GetTextureContract() const noexcept;
        [[nodiscard]] const std::vector<LocalShadowAtlasPage>&
            GetPages() const noexcept;
        [[nodiscard]] const LocalShadowAtlasExecutionEvidence&
            GetExecutionEvidence() const noexcept;
        [[nodiscard]] uint64 GetIdentityHash() const noexcept;

    private:
        friend class LocalShadowAtlasPlanBuilder;

        RenderFrameIdentity m_frameIdentity{};
        RenderViewIdentity m_viewIdentity{};
        uint64 m_lightingPlanIdentity = 0;
        uint64 m_localShadowPlanIdentity = 0;
        uint64 m_atlasGeneration = 0;
        float32 m_nearPlane = 0.0f;
        LocalShadowCoordinateSystem m_coordinateSystem =
            LocalShadowCoordinateSystem::RightHandedYUp;
        LocalShadowDepthConvention m_depthConvention =
            LocalShadowDepthConvention::ForwardZeroToOne;
        LocalShadowAtlasTextureContract m_textureContract{};
        std::vector<LocalShadowAtlasPage> m_pages{};
        LocalShadowAtlasExecutionEvidence m_executionEvidence{};
        uint64 m_identityHash = 0;
    };

    struct LocalShadowAtlasPlanBuildResult
    {
        LocalShadowAtlasPlanCode code =
            LocalShadowAtlasPlanCode::InvalidSchema;
        LocalShadowAtlasPlan plan{};
        LocalShadowAtlasExecutionEvidence evidence{};
        std::string diagnostic{};

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return code == LocalShadowAtlasPlanCode::Ready;
        }
    };

    /** @brief Stateless builder for the CPU-only atlas and matrix contract. */
    class LocalShadowAtlasPlanBuilder final
    {
    public:
        [[nodiscard]] LocalShadowAtlasPlanBuildResult Build(
            const LightingFramePlan& lightingPlan,
            const LocalShadowFramePlan& localShadowPlan,
            const LocalShadowAtlasPlanRequest& request) const;
    };

    [[nodiscard]] const char* GetLocalShadowAtlasPlanCodeName(
        LocalShadowAtlasPlanCode code) noexcept;
} // namespace RVX
