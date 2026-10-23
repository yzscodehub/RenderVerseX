#include "Render/Lighting/LocalShadowFramePlan.h"

#include "Core/Diagnostics/ContentHash.h"

#include <algorithm>
#include <limits>
#include <unordered_set>
#include <utility>

namespace RVX
{
namespace
{
    [[nodiscard]] bool IsLocalLightType(RenderLightType type) noexcept
    {
        return type == RenderLightType::Point || type == RenderLightType::Spot;
    }

    [[nodiscard]] bool IsDeclaredOverflowPolicy(
        LocalShadowOverflowPolicy policy) noexcept
    {
        switch (policy)
        {
            case LocalShadowOverflowPolicy::Fail:
            case LocalShadowOverflowPolicy::StableUnshadowedFallback:
                return true;
            default:
                return false;
        }
    }

    [[nodiscard]] uint32 GetPageCount(RenderLightType type) noexcept
    {
        switch (type)
        {
            case RenderLightType::Point:
                return RVX_LOCAL_SHADOW_POINT_PAGE_COUNT;
            case RenderLightType::Spot:
                return RVX_LOCAL_SHADOW_SPOT_PAGE_COUNT;
            default:
                return 0;
        }
    }

    [[nodiscard]] bool CheckedAdd(uint32 left,
                                  uint32 right,
                                  uint32& outValue) noexcept
    {
        if (left > std::numeric_limits<uint32>::max() - right)
        {
            return false;
        }
        outValue = left + right;
        return true;
    }

    [[nodiscard]] bool CheckedMultiply(uint32 left,
                                       uint32 right,
                                       uint32& outValue) noexcept
    {
        if (left != 0 && right > std::numeric_limits<uint32>::max() / left)
        {
            return false;
        }
        outValue = left * right;
        return true;
    }

    [[nodiscard]] bool IsAtlasConfigStructurallyValid(
        const LocalShadowAtlasConfig& config,
        bool& arithmeticOverflow) noexcept
    {
        arithmeticOverflow = false;
        if (config.pageResolution == 0 ||
            !IsDeclaredOverflowPolicy(config.overflowPolicy))
        {
            return false;
        }

        uint32 pointPages = 0;
        uint32 maximumPages = 0;
        if (!CheckedMultiply(config.pointLightCapacity,
                             RVX_LOCAL_SHADOW_POINT_PAGE_COUNT,
                             pointPages) ||
            !CheckedAdd(pointPages, config.spotLightCapacity, maximumPages))
        {
            arithmeticOverflow = true;
            return false;
        }
        static_cast<void>(maximumPages);
        return true;
    }

    [[nodiscard]] bool EvidenceIsValid(
        const LocalShadowFramePlanEvidence& evidence,
        const LocalShadowAtlasConfig& config) noexcept
    {
        if (evidence.pageCapacity != config.pageCapacity ||
            evidence.pointLightCapacity != config.pointLightCapacity ||
            evidence.spotLightCapacity != config.spotLightCapacity ||
            evidence.overflowPolicy != config.overflowPolicy ||
            evidence.selectedPageCount > evidence.requestedPageCount ||
            evidence.selectedPageCount > evidence.pageCapacity ||
            evidence.overflowPageCount !=
                evidence.requestedPageCount - evidence.selectedPageCount ||
            evidence.selectedPointLightCount >
                evidence.requestedPointLightCount ||
            evidence.selectedPointLightCount > evidence.pointLightCapacity ||
            evidence.overflowPointLightCount !=
                evidence.requestedPointLightCount -
                    evidence.selectedPointLightCount ||
            evidence.selectedSpotLightCount >
                evidence.requestedSpotLightCount ||
            evidence.selectedSpotLightCount > evidence.spotLightCapacity ||
            evidence.overflowSpotLightCount !=
                evidence.requestedSpotLightCount - evidence.selectedSpotLightCount)
        {
            return false;
        }

        const bool overflowed = evidence.overflowPageCount != 0 ||
            evidence.overflowPointLightCount != 0 ||
            evidence.overflowSpotLightCount != 0;
        return evidence.overflowed == overflowed;
    }

    [[nodiscard]] uint64 ComputeIdentityHash(
        const RenderFrameIdentity& frameIdentity,
        const RenderViewIdentity& viewIdentity,
        uint64 lightingPlanIdentity,
        uint64 atlasGeneration,
        const LocalShadowAtlasConfig& config,
        const LocalShadowFramePlanEvidence& evidence,
        const std::vector<LocalShadowAllocation>& allocations) noexcept
    {
        uint64 hash = Diagnostics::RVX_DIAGNOSTICS_FNV1A64_OFFSET_BASIS;
        Diagnostics::MixContentHashValue(
            hash, RVX_LOCAL_SHADOW_FRAME_PLAN_SCHEMA_VERSION);
        Diagnostics::MixContentHashValue(hash, frameIdentity.frameSequence);
        Diagnostics::MixContentHashValue(hash, frameIdentity.sceneRevision);
        Diagnostics::MixContentHashValue(hash, frameIdentity.surfaceGeneration);
        Diagnostics::MixContentHashValue(
            hash, frameIdentity.configurationRevision);
        Diagnostics::MixContentHashValue(hash, viewIdentity.sceneRuntimeId);
        Diagnostics::MixContentHashValue(hash, viewIdentity.cameraEntityPacked);
        Diagnostics::MixContentHashValue(hash, viewIdentity.subViewIndex);
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(viewIdentity.role));
        Diagnostics::MixContentHashValue(hash, lightingPlanIdentity);
        Diagnostics::MixContentHashValue(hash, atlasGeneration);
        Diagnostics::MixContentHashValue(hash, config.pageResolution);
        Diagnostics::MixContentHashValue(hash, config.pageCapacity);
        Diagnostics::MixContentHashValue(hash, config.pointLightCapacity);
        Diagnostics::MixContentHashValue(hash, config.spotLightCapacity);
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(config.overflowPolicy));
        Diagnostics::MixContentHashValue(hash, evidence.requestedPageCount);
        Diagnostics::MixContentHashValue(hash, evidence.selectedPageCount);
        Diagnostics::MixContentHashValue(hash, evidence.overflowPageCount);
        Diagnostics::MixContentHashValue(
            hash, evidence.requestedPointLightCount);
        Diagnostics::MixContentHashValue(
            hash, evidence.selectedPointLightCount);
        Diagnostics::MixContentHashValue(
            hash, evidence.overflowPointLightCount);
        Diagnostics::MixContentHashValue(
            hash, evidence.requestedSpotLightCount);
        Diagnostics::MixContentHashValue(
            hash, evidence.selectedSpotLightCount);
        Diagnostics::MixContentHashValue(
            hash, evidence.overflowSpotLightCount);
        Diagnostics::MixContentHashValue(hash, allocations.size());
        for (const LocalShadowAllocation& allocation : allocations)
        {
            Diagnostics::MixContentHashValue(hash, allocation.lightId);
            Diagnostics::MixContentHashValue(
                hash, static_cast<uint64>(allocation.type));
            Diagnostics::MixContentHashValue(
                hash, allocation.lightBufferIndex);
            Diagnostics::MixContentHashValue(hash, allocation.handle.slot);
            Diagnostics::MixContentHashValue(
                hash, allocation.handle.generation);
            Diagnostics::MixContentHashValue(hash, allocation.firstPage);
            Diagnostics::MixContentHashValue(hash, allocation.pageCount);
        }
        return hash == 0 ? 1 : hash;
    }

    [[nodiscard]] LocalShadowFramePlanBuildResult Fail(
        LocalShadowFramePlanCode code,
        std::string diagnostic,
        LocalShadowFramePlanEvidence evidence = {})
    {
        LocalShadowFramePlanBuildResult result;
        result.code = code;
        result.evidence = evidence;
        result.diagnostic = std::move(diagnostic);
        return result;
    }
} // namespace

bool LocalShadowAtlasHandle::IsValid() const noexcept
{
    return slot != 0 && generation != 0 && planIdentityHash != 0 &&
        IsLocalLightType(type);
}

bool LocalShadowAtlasHandle::Matches(
    uint64 expectedPlanIdentityHash,
    uint64 expectedGeneration,
    RenderLightType expectedType) const noexcept
{
    return IsValid() && expectedPlanIdentityHash != 0 &&
        expectedGeneration != 0 &&
        planIdentityHash == expectedPlanIdentityHash &&
        generation == expectedGeneration && type == expectedType;
}

bool LocalShadowFramePlan::IsValid() const noexcept
{
    bool arithmeticOverflow = false;
    if (m_identityHash == 0 || !m_frameIdentity.IsValid() ||
        !m_viewIdentity.IsValid() || m_lightingPlanIdentity == 0 ||
        m_atlasGeneration == 0 ||
        !IsAtlasConfigStructurallyValid(
            m_atlasConfig, arithmeticOverflow) || arithmeticOverflow ||
        !EvidenceIsValid(m_evidence, m_atlasConfig) ||
        (m_evidence.overflowed &&
         m_atlasConfig.overflowPolicy == LocalShadowOverflowPolicy::Fail))
    {
        return false;
    }

    uint32 expectedFirstPage = 0;
    uint32 selectedPointLightCount = 0;
    uint32 selectedSpotLightCount = 0;
    std::unordered_set<uint64> lightIds;
    std::unordered_set<uint64> lightBufferIdentities;
    lightIds.reserve(m_allocations.size());
    lightBufferIdentities.reserve(m_allocations.size());
    for (size_t index = 0; index < m_allocations.size(); ++index)
    {
        const LocalShadowAllocation& allocation = m_allocations[index];
        const uint32 pageCount = GetPageCount(allocation.type);
        const uint64 lightBufferIdentity =
            (static_cast<uint64>(allocation.type) << 32U) |
            allocation.lightBufferIndex;
        if (allocation.lightId == 0 || pageCount == 0 ||
            allocation.pageCount != pageCount ||
            allocation.firstPage != expectedFirstPage ||
            index >= std::numeric_limits<uint32>::max() ||
            allocation.handle.slot != static_cast<uint32>(index) + 1U ||
            !allocation.handle.Matches(
                m_identityHash, m_atlasGeneration, allocation.type) ||
            !lightIds.insert(allocation.lightId).second ||
            !lightBufferIdentities.insert(lightBufferIdentity).second ||
            !CheckedAdd(expectedFirstPage, pageCount, expectedFirstPage) ||
            expectedFirstPage > m_evidence.pageCapacity)
        {
            return false;
        }

        if (allocation.type == RenderLightType::Point)
        {
            if (!CheckedAdd(
                    selectedPointLightCount, 1U, selectedPointLightCount))
            {
                return false;
            }
        }
        else if (!CheckedAdd(
                     selectedSpotLightCount, 1U, selectedSpotLightCount))
        {
            return false;
        }
    }

    return expectedFirstPage == m_evidence.selectedPageCount &&
        selectedPointLightCount == m_evidence.selectedPointLightCount &&
        selectedSpotLightCount == m_evidence.selectedSpotLightCount &&
        m_allocations.size() ==
            static_cast<size_t>(selectedPointLightCount) +
                selectedSpotLightCount &&
        m_identityHash == ComputeIdentityHash(
            m_frameIdentity, m_viewIdentity, m_lightingPlanIdentity,
            m_atlasGeneration, m_atlasConfig, m_evidence, m_allocations);
}

bool LocalShadowFramePlan::Matches(
    const RenderFrameIdentity& frameIdentity,
    const RenderViewIdentity& viewIdentity,
    uint64 lightingPlanIdentity,
    uint64 atlasGeneration) const noexcept
{
    return IsValid() && frameIdentity.IsValid() && viewIdentity.IsValid() &&
        lightingPlanIdentity != 0 && atlasGeneration != 0 &&
        m_frameIdentity == frameIdentity && m_viewIdentity == viewIdentity &&
        m_lightingPlanIdentity == lightingPlanIdentity &&
        m_atlasGeneration == atlasGeneration;
}

bool LocalShadowFramePlan::IsCompatibleWith(
    const LightingFramePlan& lightingPlan,
    uint64 atlasGeneration) const noexcept
{
    return lightingPlan.IsValid() &&
        Matches(lightingPlan.GetFrameIdentity(), lightingPlan.GetViewIdentity(),
                lightingPlan.GetIdentityHash(), atlasGeneration);
}

const LocalShadowAllocation* LocalShadowFramePlan::FindAllocation(
    uint64 lightId) const noexcept
{
    if (!IsValid() || lightId == 0)
    {
        return nullptr;
    }
    const auto found = std::find_if(
        m_allocations.begin(), m_allocations.end(),
        [lightId](const LocalShadowAllocation& allocation)
        {
            return allocation.lightId == lightId;
        });
    return found == m_allocations.end() ? nullptr : &*found;
}

const LocalShadowAllocation* LocalShadowFramePlan::ResolveAllocation(
    LocalShadowAtlasHandle handle,
    uint64 expectedLightingPlanIdentity,
    uint64 expectedAtlasGeneration) const noexcept
{
    if (!IsValid() || expectedLightingPlanIdentity == 0 ||
        expectedLightingPlanIdentity != m_lightingPlanIdentity ||
        expectedAtlasGeneration == 0 ||
        expectedAtlasGeneration != m_atlasGeneration ||
        !handle.Matches(
            m_identityHash, expectedAtlasGeneration, handle.type) ||
        handle.slot > m_allocations.size())
    {
        return nullptr;
    }

    const LocalShadowAllocation& allocation =
        m_allocations[handle.slot - 1U];
    return allocation.handle == handle ? &allocation : nullptr;
}

const RenderFrameIdentity& LocalShadowFramePlan::GetFrameIdentity() const noexcept
{
    return m_frameIdentity;
}

const RenderViewIdentity& LocalShadowFramePlan::GetViewIdentity() const noexcept
{
    return m_viewIdentity;
}

uint64 LocalShadowFramePlan::GetLightingPlanIdentity() const noexcept
{
    return m_lightingPlanIdentity;
}

uint64 LocalShadowFramePlan::GetAtlasGeneration() const noexcept
{
    return m_atlasGeneration;
}

const LocalShadowAtlasConfig&
LocalShadowFramePlan::GetAtlasConfig() const noexcept
{
    return m_atlasConfig;
}

const std::vector<LocalShadowAllocation>&
LocalShadowFramePlan::GetAllocations() const noexcept
{
    return m_allocations;
}

const LocalShadowFramePlanEvidence&
LocalShadowFramePlan::GetEvidence() const noexcept
{
    return m_evidence;
}

uint64 LocalShadowFramePlan::GetIdentityHash() const noexcept
{
    return m_identityHash;
}

LocalShadowFramePlanBuildResult LocalShadowFramePlanBuilder::Build(
    const LightingFramePlan& lightingPlan,
    const LocalShadowFramePlanRequest& request) const
{
    if (request.schemaVersion != RVX_LOCAL_SHADOW_FRAME_PLAN_SCHEMA_VERSION)
    {
        return Fail(LocalShadowFramePlanCode::InvalidSchema,
                    "Local shadow frame plan schema version is unsupported");
    }
    if (!lightingPlan.IsValid())
    {
        return Fail(LocalShadowFramePlanCode::InvalidLightingPlan,
                    "Local shadow planning requires a sealed LightingFramePlan");
    }
    if (request.atlasGeneration == 0)
    {
        return Fail(LocalShadowFramePlanCode::InvalidAtlasGeneration,
                    "Local shadow atlas generation must be non-zero");
    }
    bool arithmeticOverflow = false;
    if (!IsAtlasConfigStructurallyValid(request.atlas, arithmeticOverflow))
    {
        return Fail(
            arithmeticOverflow
                ? LocalShadowFramePlanCode::ArithmeticOverflow
                : LocalShadowFramePlanCode::InvalidAtlasConfiguration,
            arithmeticOverflow
                ? "Local shadow atlas capacity arithmetic overflowed"
                : "Local shadow atlas configuration is invalid");
    }

    const uint64 lightingPlanIdentity = lightingPlan.GetIdentityHash();
    if (request.expectedLightingPlanIdentity != 0 &&
        request.expectedLightingPlanIdentity != lightingPlanIdentity)
    {
        return Fail(
            LocalShadowFramePlanCode::LightingPlanIdentityMismatch,
            "Local shadow planning rejected a stale LightingFramePlan identity");
    }

    LocalShadowFramePlanEvidence evidence;
    evidence.pageCapacity = request.atlas.pageCapacity;
    evidence.pointLightCapacity = request.atlas.pointLightCapacity;
    evidence.spotLightCapacity = request.atlas.spotLightCapacity;
    evidence.overflowPolicy = request.atlas.overflowPolicy;

    std::vector<LocalShadowAllocation> allocations;
    const std::vector<LightingLightCandidate>& localLights =
        lightingPlan.GetLocalLights();
    allocations.reserve(std::min<size_t>(
        localLights.size(), request.atlas.pointLightCapacity +
                                static_cast<size_t>(request.atlas.spotLightCapacity)));

    uint32 pointLightBufferIndex = 0;
    uint32 spotLightBufferIndex = 0;
    uint32 nextPage = 0;
    for (const LightingLightCandidate& light : localLights)
    {
        if (!IsLocalLightType(light.type))
        {
            return Fail(LocalShadowFramePlanCode::InvalidLightingPlan,
                        "LightingFramePlan exposed a non-local light in its local-light order",
                        evidence);
        }

        uint32 lightBufferIndex = 0;
        if (light.type == RenderLightType::Point)
        {
            lightBufferIndex = pointLightBufferIndex;
            if (!CheckedAdd(pointLightBufferIndex, 1U,
                            pointLightBufferIndex))
            {
                return Fail(LocalShadowFramePlanCode::ArithmeticOverflow,
                            "Point-light buffer index overflowed uint32",
                            evidence);
            }
        }
        else
        {
            lightBufferIndex = spotLightBufferIndex;
            if (!CheckedAdd(spotLightBufferIndex, 1U,
                            spotLightBufferIndex))
            {
                return Fail(LocalShadowFramePlanCode::ArithmeticOverflow,
                            "Spot-light buffer index overflowed uint32",
                            evidence);
            }
        }

        if (!light.castsShadows)
        {
            continue;
        }

        const uint32 pageCount = GetPageCount(light.type);
        uint32 requestedPageCount = 0;
        if (!CheckedAdd(evidence.requestedPageCount, pageCount,
                        requestedPageCount))
        {
            return Fail(LocalShadowFramePlanCode::ArithmeticOverflow,
                        "Requested local-shadow page count overflowed uint32",
                        evidence);
        }
        evidence.requestedPageCount = requestedPageCount;

        uint32* requestedTypeCount = light.type == RenderLightType::Point
            ? &evidence.requestedPointLightCount
            : &evidence.requestedSpotLightCount;
        if (!CheckedAdd(*requestedTypeCount, 1U, *requestedTypeCount))
        {
            return Fail(LocalShadowFramePlanCode::ArithmeticOverflow,
                        "Requested local-shadow light count overflowed uint32",
                        evidence);
        }

        const uint32 selectedTypeCount = light.type == RenderLightType::Point
            ? evidence.selectedPointLightCount
            : evidence.selectedSpotLightCount;
        const uint32 typeCapacity = light.type == RenderLightType::Point
            ? evidence.pointLightCapacity
            : evidence.spotLightCapacity;
        const bool typeCapacityAvailable = selectedTypeCount < typeCapacity;
        const bool pageCapacityAvailable =
            nextPage <= evidence.pageCapacity &&
            pageCount <= evidence.pageCapacity - nextPage;
        if (!typeCapacityAvailable || !pageCapacityAvailable)
        {
            continue;
        }

        if (allocations.size() >= std::numeric_limits<uint32>::max())
        {
            return Fail(LocalShadowFramePlanCode::ArithmeticOverflow,
                        "Local-shadow allocation handle slot overflowed uint32",
                        evidence);
        }

        LocalShadowAllocation allocation;
        allocation.lightId = light.lightId;
        allocation.type = light.type;
        allocation.lightBufferIndex = lightBufferIndex;
        allocation.handle.slot = static_cast<uint32>(allocations.size()) + 1U;
        allocation.handle.generation = request.atlasGeneration;
        allocation.handle.type = light.type;
        allocation.firstPage = nextPage;
        allocation.pageCount = pageCount;
        allocations.push_back(allocation);

        if (!CheckedAdd(nextPage, pageCount, nextPage))
        {
            return Fail(LocalShadowFramePlanCode::ArithmeticOverflow,
                        "Selected local-shadow page count overflowed uint32",
                        evidence);
        }
        if (light.type == RenderLightType::Point)
        {
            if (!CheckedAdd(evidence.selectedPointLightCount, 1U,
                            evidence.selectedPointLightCount))
            {
                return Fail(LocalShadowFramePlanCode::ArithmeticOverflow,
                            "Selected point-shadow count overflowed uint32",
                            evidence);
            }
        }
        else if (!CheckedAdd(evidence.selectedSpotLightCount, 1U,
                             evidence.selectedSpotLightCount))
        {
            return Fail(LocalShadowFramePlanCode::ArithmeticOverflow,
                        "Selected spot-shadow count overflowed uint32",
                        evidence);
        }
    }

    evidence.selectedPageCount = nextPage;
    evidence.overflowPageCount =
        evidence.requestedPageCount - evidence.selectedPageCount;
    evidence.overflowPointLightCount =
        evidence.requestedPointLightCount - evidence.selectedPointLightCount;
    evidence.overflowSpotLightCount =
        evidence.requestedSpotLightCount - evidence.selectedSpotLightCount;
    evidence.overflowed = evidence.overflowPageCount != 0 ||
        evidence.overflowPointLightCount != 0 ||
        evidence.overflowSpotLightCount != 0;

    if (!EvidenceIsValid(evidence, request.atlas))
    {
        return Fail(LocalShadowFramePlanCode::ArithmeticOverflow,
                    "Local shadow capacity evidence is inconsistent",
                    evidence);
    }
    if (evidence.overflowed &&
        request.atlas.overflowPolicy == LocalShadowOverflowPolicy::Fail)
    {
        return Fail(
            LocalShadowFramePlanCode::CapacityExceeded,
            "Local shadow atlas capacity was exceeded without a declared fallback",
            evidence);
    }

    const uint64 identityHash = ComputeIdentityHash(
        lightingPlan.GetFrameIdentity(), lightingPlan.GetViewIdentity(),
        lightingPlanIdentity, request.atlasGeneration, request.atlas,
        evidence, allocations);
    if (request.expectedIdentityHash != 0 &&
        request.expectedIdentityHash != identityHash)
    {
        return Fail(LocalShadowFramePlanCode::IdentityMismatch,
                    "Local shadow plan no longer matches the expected identity",
                    evidence);
    }

    // The plan identity is a derived stale-handle guard. It is assigned only
    // after hashing and is intentionally excluded from ComputeIdentityHash to
    // avoid a self-referential hash contract.
    for (LocalShadowAllocation& allocation : allocations)
    {
        allocation.handle.planIdentityHash = identityHash;
    }

    LocalShadowFramePlanBuildResult result;
    result.code = LocalShadowFramePlanCode::Ready;
    result.evidence = evidence;
    result.plan.m_frameIdentity = lightingPlan.GetFrameIdentity();
    result.plan.m_viewIdentity = lightingPlan.GetViewIdentity();
    result.plan.m_lightingPlanIdentity = lightingPlanIdentity;
    result.plan.m_atlasGeneration = request.atlasGeneration;
    result.plan.m_atlasConfig = request.atlas;
    result.plan.m_allocations = std::move(allocations);
    result.plan.m_evidence = evidence;
    result.plan.m_identityHash = identityHash;
    return result;
}

const char* GetLocalShadowFramePlanCodeName(
    LocalShadowFramePlanCode code) noexcept
{
    switch (code)
    {
        case LocalShadowFramePlanCode::Ready: return "ready";
        case LocalShadowFramePlanCode::InvalidSchema: return "invalid-schema";
        case LocalShadowFramePlanCode::InvalidLightingPlan:
            return "invalid-lighting-plan";
        case LocalShadowFramePlanCode::InvalidAtlasGeneration:
            return "invalid-atlas-generation";
        case LocalShadowFramePlanCode::InvalidAtlasConfiguration:
            return "invalid-atlas-configuration";
        case LocalShadowFramePlanCode::ArithmeticOverflow:
            return "arithmetic-overflow";
        case LocalShadowFramePlanCode::LightingPlanIdentityMismatch:
            return "lighting-plan-identity-mismatch";
        case LocalShadowFramePlanCode::CapacityExceeded:
            return "capacity-exceeded";
        case LocalShadowFramePlanCode::IdentityMismatch:
            return "identity-mismatch";
        default:
            return "unknown";
    }
}
} // namespace RVX
