#pragma once

/**
 * @file LightingFramePlan.h
 * @brief Immutable, backend-neutral lighting selection for one render frame/view.
 */

#include "Core/Types.h"
#include "Render/Renderer/ShadowConstants.h"
#include "RenderContracts/RenderFrameTypes.h"
#include "RenderContracts/RenderPipelineProfile.h"

#include <string>
#include <vector>

namespace RVX
{
    inline constexpr uint32 RVX_LIGHTING_FRAME_PLAN_SCHEMA_VERSION = 2;

    /**
     * @brief Convert a validated [1000, 40000] Kelvin value to linear RGB.
     *
     * Invalid inputs return black. Authoring tint is multiplied separately so
     * the plan always hashes the final renderer color actually sent to the GPU.
     */
    [[nodiscard]] Vec3 EvaluateBlackbodyColorTemperature(
        float32 kelvin) noexcept;

    /** @brief Fail-closed outcome for a LightingFramePlanBuilder request. */
    enum class LightingFramePlanCode : uint8
    {
        Ready = 0,
        InvalidSchema,
        InvalidFrameIdentity,
        InvalidViewIdentity,
        InvalidSceneLightStateHash,
        InvalidConfigurationRevision,
        InvalidQuality,
        InvalidCapacity,
        InvalidLightCandidate,
        DuplicateLightCandidate,
        InvalidCSMRequest,
        InvalidIBLRequest,
        CapacityExceeded,
        IdentityMismatch,
    };

    /**
     * @brief One renderer-normalized light candidate.
     *
     * Intensity and color are canonical renderer values. RenderContracts has
     * already converted directional lights to lux and local lights to candela;
     * the plan hashes only those canonical values, never authoring ambiguity.
     */
    struct LightingLightCandidate
    {
        uint64 lightId = 0;
        RenderLightType type = RenderLightType::Directional;
        uint32 priority = 0;
        Vec3 position{0.0f};
        Vec3 direction{0.0f, -1.0f, 0.0f};
        Vec3 color{1.0f};
        float32 normalizedIntensity = 1.0f;
        float32 range = 0.0f;
        float32 innerConeRadians = 0.0f;
        float32 outerConeRadians = 0.0f;
        uint32 layerMask = ~0U;
        bool castsShadows = false;
    };

    /** @brief CSM requirements already resolved for the current plan request. */
    struct LightingCSMRequest
    {
        bool requested = false;
        bool inputsReady = false;
        uint32 cascadeCount = 0;
        uint64 configurationRevision = 0;
    };

    /** @brief Texture IBL requirements already resolved for the current plan request. */
    struct LightingIBLRequest
    {
        bool requested = false;
        bool inputsReady = false;
        uint64 resourceRevision = 0;
        uint32 prefilteredMipLevels = 0;
        float32 normalizedIntensity = 1.0f;
    };

    /** @brief Explicit local-light capacity and overflow outcome. */
    struct LightingFramePlanCapacityEvidence
    {
        uint32 localLightCapacity = 0;
        uint32 requestedLocalLightCount = 0;
        uint32 selectedLocalLightCount = 0;
        uint32 overflowLocalLightCount = 0;
        uint32 pointLightCapacity = 0;
        uint32 requestedPointLightCount = 0;
        uint32 selectedPointLightCount = 0;
        uint32 overflowPointLightCount = 0;
        uint32 spotLightCapacity = 0;
        uint32 requestedSpotLightCount = 0;
        uint32 selectedSpotLightCount = 0;
        uint32 overflowSpotLightCount = 0;
        bool truncationAllowed = false;
        bool overflowed = false;
    };

    /** @brief Complete value-only input to LightingFramePlanBuilder. */
    struct LightingFramePlanRequest
    {
        uint32 schemaVersion = RVX_LIGHTING_FRAME_PLAN_SCHEMA_VERSION;
        RenderFrameIdentity frameIdentity{};
        RenderViewIdentity viewIdentity{};
        uint64 sceneLightStateHash = 0;
        uint64 lightingConfigurationRevision = 0;
        uint64 qualityRevision = 0;
        RenderQualityTier quality = RenderQualityTier::Medium;
        uint32 pointLightCapacity = 0;
        uint32 spotLightCapacity = 0;
        /** Only an explicitly declared overflow policy may truncate local lights. */
        bool allowLocalLightTruncation = false;
        std::vector<LightingLightCandidate> lightCandidates{};
        LightingCSMRequest csm{};
        LightingIBLRequest ibl{};
        /** When non-zero, any changed input must fail instead of publishing a new plan. */
        uint64 expectedIdentityHash = 0;
    };

    class LightingFramePlanBuilder;

    /**
     * @brief Immutable per-frame/per-view lighting selection.
     *
     * The plan owns every selected value. It intentionally exposes only const
     * accessors so later frame preparation cannot mutate a published selection.
     */
    class LightingFramePlan final
    {
    public:
        LightingFramePlan() = default;

        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] const RenderFrameIdentity& GetFrameIdentity() const noexcept;
        [[nodiscard]] const RenderViewIdentity& GetViewIdentity() const noexcept;
        [[nodiscard]] uint64 GetSceneLightStateHash() const noexcept;
        [[nodiscard]] uint64 GetLightingConfigurationRevision() const noexcept;
        [[nodiscard]] uint64 GetQualityRevision() const noexcept;
        [[nodiscard]] RenderQualityTier GetQuality() const noexcept;
        [[nodiscard]] const LightingCSMRequest& GetCSMRequest() const noexcept;
        [[nodiscard]] const LightingIBLRequest& GetIBLRequest() const noexcept;
        [[nodiscard]] const LightingLightCandidate* GetPrimaryDirectionalLight() const noexcept;
        [[nodiscard]] const std::vector<LightingLightCandidate>& GetLocalLights() const noexcept;
        [[nodiscard]] const LightingFramePlanCapacityEvidence& GetCapacityEvidence() const noexcept;
        [[nodiscard]] uint64 GetIdentityHash() const noexcept;

    private:
        friend class LightingFramePlanBuilder;

        RenderFrameIdentity m_frameIdentity{};
        RenderViewIdentity m_viewIdentity{};
        uint64 m_sceneLightStateHash = 0;
        uint64 m_lightingConfigurationRevision = 0;
        uint64 m_qualityRevision = 0;
        RenderQualityTier m_quality = RenderQualityTier::Medium;
        LightingCSMRequest m_csm{};
        LightingIBLRequest m_ibl{};
        bool m_hasPrimaryDirectionalLight = false;
        LightingLightCandidate m_primaryDirectionalLight{};
        std::vector<LightingLightCandidate> m_localLights{};
        LightingFramePlanCapacityEvidence m_capacityEvidence{};
        uint64 m_identityHash = 0;
    };

    struct LightingFramePlanBuildResult
    {
        LightingFramePlanCode code = LightingFramePlanCode::InvalidSchema;
        LightingFramePlan plan{};
        LightingFramePlanCapacityEvidence capacityEvidence{};
        std::string diagnostic{};

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return code == LightingFramePlanCode::Ready;
        }
    };

    /** @brief Stateless builder that validates, canonicalizes, and seals a lighting plan. */
    class LightingFramePlanBuilder final
    {
    public:
        [[nodiscard]] LightingFramePlanBuildResult Build(
            const LightingFramePlanRequest& request) const;
    };

    [[nodiscard]] const char* GetLightingFramePlanCodeName(
        LightingFramePlanCode code) noexcept;
} // namespace RVX
