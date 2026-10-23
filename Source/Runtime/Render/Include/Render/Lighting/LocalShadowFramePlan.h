#pragma once

/**
 * @file LocalShadowFramePlan.h
 * @brief Immutable point/spot shadow-atlas allocation for one lighting plan.
 */

#include "Core/Types.h"
#include "Render/Lighting/LightingFramePlan.h"

#include <string>
#include <vector>

namespace RVX
{
    inline constexpr uint32 RVX_LOCAL_SHADOW_FRAME_PLAN_SCHEMA_VERSION = 1;
    inline constexpr uint32 RVX_LOCAL_SHADOW_POINT_PAGE_COUNT = 6;
    inline constexpr uint32 RVX_LOCAL_SHADOW_SPOT_PAGE_COUNT = 1;

    /** @brief Explicit outcome when the configured atlas cannot shadow every request. */
    enum class LocalShadowOverflowPolicy : uint8
    {
        Fail = 0,
        StableUnshadowedFallback,
    };

    /** @brief Fail-closed result of building an immutable local-shadow plan. */
    enum class LocalShadowFramePlanCode : uint8
    {
        Ready = 0,
        InvalidSchema,
        InvalidLightingPlan,
        InvalidAtlasGeneration,
        InvalidAtlasConfiguration,
        ArithmeticOverflow,
        LightingPlanIdentityMismatch,
        CapacityExceeded,
        IdentityMismatch,
    };

    /**
     * @brief Fixed page-array budget selected by renderer composition.
     *
     * One point light consumes six contiguous pages. One spot light consumes
     * one page. Type capacities and the shared page capacity are independent,
     * explicit limits.
     */
    struct LocalShadowAtlasConfig
    {
        uint32 pageResolution = 0;
        uint32 pageCapacity = 0;
        uint32 pointLightCapacity = 0;
        uint32 spotLightCapacity = 0;
        LocalShadowOverflowPolicy overflowPolicy =
            LocalShadowOverflowPolicy::Fail;
    };

    /** @brief Generation-qualified logical allocation; slot zero is invalid. */
    struct LocalShadowAtlasHandle
    {
        uint32 slot = 0;
        uint64 generation = 0;
        /** Derived identity of the exact immutable allocation plan. */
        uint64 planIdentityHash = 0;
        RenderLightType type = RenderLightType::Directional;

        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] bool Matches(uint64 expectedPlanIdentityHash,
                                   uint64 expectedGeneration,
                                   RenderLightType expectedType) const noexcept;

        friend bool operator==(const LocalShadowAtlasHandle&,
                               const LocalShadowAtlasHandle&) = default;
    };

    /** @brief One selected local light's stable GPU-buffer and atlas mapping. */
    struct LocalShadowAllocation
    {
        uint64 lightId = 0;
        RenderLightType type = RenderLightType::Directional;
        /** Point or Spot structured-buffer index used by clustered lighting. */
        uint32 lightBufferIndex = 0;
        LocalShadowAtlasHandle handle{};
        uint32 firstPage = 0;
        uint32 pageCount = 0;

        friend bool operator==(const LocalShadowAllocation&,
                               const LocalShadowAllocation&) = default;
    };

    /** @brief Complete requested/selected/fallback evidence for one build. */
    struct LocalShadowFramePlanEvidence
    {
        uint32 pageCapacity = 0;
        uint32 requestedPageCount = 0;
        uint32 selectedPageCount = 0;
        uint32 overflowPageCount = 0;
        uint32 pointLightCapacity = 0;
        uint32 requestedPointLightCount = 0;
        uint32 selectedPointLightCount = 0;
        uint32 overflowPointLightCount = 0;
        uint32 spotLightCapacity = 0;
        uint32 requestedSpotLightCount = 0;
        uint32 selectedSpotLightCount = 0;
        uint32 overflowSpotLightCount = 0;
        LocalShadowOverflowPolicy overflowPolicy =
            LocalShadowOverflowPolicy::Fail;
        bool overflowed = false;
    };

    /** @brief Value-only input paired with an already sealed LightingFramePlan. */
    struct LocalShadowFramePlanRequest
    {
        uint32 schemaVersion = RVX_LOCAL_SHADOW_FRAME_PLAN_SCHEMA_VERSION;
        LocalShadowAtlasConfig atlas{};
        /** Non-zero generation issued by the Render-owned atlas owner. */
        uint64 atlasGeneration = 0;
        /** Optional stale-plan pin. Zero accepts the supplied sealed plan. */
        uint64 expectedLightingPlanIdentity = 0;
        /** Optional deterministic-output pin. Zero accepts a newly computed identity. */
        uint64 expectedIdentityHash = 0;
    };

    class LocalShadowFramePlanBuilder;

    /** @brief Immutable local-shadow allocation for one exact frame/view. */
    class LocalShadowFramePlan final
    {
    public:
        LocalShadowFramePlan() = default;

        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] bool Matches(const RenderFrameIdentity& frameIdentity,
                                   const RenderViewIdentity& viewIdentity,
                                   uint64 lightingPlanIdentity,
                                   uint64 atlasGeneration) const noexcept;
        [[nodiscard]] bool IsCompatibleWith(
            const LightingFramePlan& lightingPlan,
            uint64 atlasGeneration) const noexcept;

        [[nodiscard]] const LocalShadowAllocation* FindAllocation(
            uint64 lightId) const noexcept;
        [[nodiscard]] const LocalShadowAllocation* ResolveAllocation(
            LocalShadowAtlasHandle handle,
            uint64 expectedLightingPlanIdentity,
            uint64 expectedAtlasGeneration) const noexcept;

        [[nodiscard]] const RenderFrameIdentity& GetFrameIdentity() const noexcept;
        [[nodiscard]] const RenderViewIdentity& GetViewIdentity() const noexcept;
        [[nodiscard]] uint64 GetLightingPlanIdentity() const noexcept;
        [[nodiscard]] uint64 GetAtlasGeneration() const noexcept;
        [[nodiscard]] const LocalShadowAtlasConfig& GetAtlasConfig() const noexcept;
        [[nodiscard]] const std::vector<LocalShadowAllocation>&
            GetAllocations() const noexcept;
        [[nodiscard]] const LocalShadowFramePlanEvidence& GetEvidence() const noexcept;
        [[nodiscard]] uint64 GetIdentityHash() const noexcept;

    private:
        friend class LocalShadowFramePlanBuilder;

        RenderFrameIdentity m_frameIdentity{};
        RenderViewIdentity m_viewIdentity{};
        uint64 m_lightingPlanIdentity = 0;
        uint64 m_atlasGeneration = 0;
        LocalShadowAtlasConfig m_atlasConfig{};
        std::vector<LocalShadowAllocation> m_allocations{};
        LocalShadowFramePlanEvidence m_evidence{};
        uint64 m_identityHash = 0;
    };

    struct LocalShadowFramePlanBuildResult
    {
        LocalShadowFramePlanCode code =
            LocalShadowFramePlanCode::InvalidSchema;
        LocalShadowFramePlan plan{};
        LocalShadowFramePlanEvidence evidence{};
        std::string diagnostic{};

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return code == LocalShadowFramePlanCode::Ready;
        }
    };

    /** @brief Stateless deterministic planner for point/spot atlas pages. */
    class LocalShadowFramePlanBuilder final
    {
    public:
        [[nodiscard]] LocalShadowFramePlanBuildResult Build(
            const LightingFramePlan& lightingPlan,
            const LocalShadowFramePlanRequest& request) const;
    };

    [[nodiscard]] const char* GetLocalShadowFramePlanCodeName(
        LocalShadowFramePlanCode code) noexcept;
} // namespace RVX
