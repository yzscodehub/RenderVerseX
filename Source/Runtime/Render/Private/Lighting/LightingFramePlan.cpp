#include "Render/Lighting/LightingFramePlan.h"

#include "Core/Diagnostics/ContentHash.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>

namespace RVX
{
namespace
{
    [[nodiscard]] float32 SRGBChannelToLinear(float32 value) noexcept
    {
        value = std::clamp(value, 0.0f, 1.0f);
        return value <= 0.04045f
            ? value / 12.92f
            : std::pow((value + 0.055f) / 1.055f, 2.4f);
    }

    [[nodiscard]] bool IsDeclaredRenderLightType(RenderLightType type) noexcept
    {
        switch (type)
        {
            case RenderLightType::Directional:
            case RenderLightType::Point:
            case RenderLightType::Spot: return true;
            default: return false;
        }
    }

    [[nodiscard]] bool IsFiniteFloat(float32 value) noexcept
    {
        return std::isfinite(value);
    }

    [[nodiscard]] bool IsFiniteVector(const Vec3& value) noexcept
    {
        return IsFiniteFloat(value.x) && IsFiniteFloat(value.y) &&
               IsFiniteFloat(value.z);
    }

    [[nodiscard]] float32 CanonicalizeSignedZero(float32 value) noexcept
    {
        return value == 0.0f ? 0.0f : value;
    }

    [[nodiscard]] float32 QuantizeStableFloat(
        float32 value,
        float32 scale) noexcept
    {
        return CanonicalizeSignedZero(std::round(value * scale) / scale);
    }

    [[nodiscard]] Vec3 CanonicalizeDirection(const Vec3& value) noexcept
    {
        const float32 lengthSquared =
            value.x * value.x + value.y * value.y + value.z * value.z;
        const float32 inverseLength = 1.0f / std::sqrt(lengthSquared);
        constexpr float32 directionQuantization = 1.0e6f;
        return Vec3(
            QuantizeStableFloat(
                value.x * inverseLength, directionQuantization),
            QuantizeStableFloat(
                value.y * inverseLength, directionQuantization),
            QuantizeStableFloat(
                value.z * inverseLength, directionQuantization));
    }

    void CanonicalizeCandidate(LightingLightCandidate& candidate) noexcept
    {
        candidate.position = Vec3(
            CanonicalizeSignedZero(candidate.position.x),
            CanonicalizeSignedZero(candidate.position.y),
            CanonicalizeSignedZero(candidate.position.z));
        candidate.color = Vec3(
            CanonicalizeSignedZero(candidate.color.x),
            CanonicalizeSignedZero(candidate.color.y),
            CanonicalizeSignedZero(candidate.color.z));
        candidate.normalizedIntensity =
            CanonicalizeSignedZero(candidate.normalizedIntensity);
        candidate.range = CanonicalizeSignedZero(candidate.range);
        candidate.innerConeRadians =
            CanonicalizeSignedZero(candidate.innerConeRadians);
        candidate.outerConeRadians =
            CanonicalizeSignedZero(candidate.outerConeRadians);

        switch (candidate.type)
        {
            case RenderLightType::Directional:
                candidate.position = Vec3(0.0f);
                candidate.direction =
                    CanonicalizeDirection(candidate.direction);
                candidate.range = 0.0f;
                candidate.innerConeRadians = 0.0f;
                candidate.outerConeRadians = 0.0f;
                break;
            case RenderLightType::Point:
                candidate.direction = Vec3(0.0f);
                candidate.innerConeRadians = 0.0f;
                candidate.outerConeRadians = 0.0f;
                break;
            case RenderLightType::Spot:
                candidate.direction =
                    CanonicalizeDirection(candidate.direction);
                break;
            default:
                break;
        }
    }

    [[nodiscard]] bool IsValidCandidate(
        const LightingLightCandidate& candidate) noexcept
    {
        if (candidate.lightId == 0 || !IsDeclaredRenderLightType(candidate.type) ||
            !IsFiniteVector(candidate.position) ||
            !IsFiniteVector(candidate.direction) || !IsFiniteVector(candidate.color) ||
            !IsFiniteFloat(candidate.normalizedIntensity) ||
            !IsFiniteFloat(candidate.range) ||
            !IsFiniteFloat(candidate.innerConeRadians) ||
            !IsFiniteFloat(candidate.outerConeRadians))
        {
            return false;
        }

        if (candidate.color.x < 0.0f || candidate.color.y < 0.0f ||
            candidate.color.z < 0.0f || candidate.normalizedIntensity < 0.0f ||
            candidate.range < 0.0f || candidate.innerConeRadians < 0.0f ||
            candidate.outerConeRadians < 0.0f)
        {
            return false;
        }

        if (candidate.type == RenderLightType::Point)
        {
            return candidate.range > 0.0f;
        }

        const float32 directionLengthSquared =
            candidate.direction.x * candidate.direction.x +
            candidate.direction.y * candidate.direction.y +
            candidate.direction.z * candidate.direction.z;
        if (directionLengthSquared <= 1.0e-8f)
        {
            return false;
        }

        if (candidate.type == RenderLightType::Spot)
        {
            constexpr float32 RVX_PI = 3.14159265358979323846f;
            return candidate.range > 0.0f &&
                   candidate.innerConeRadians < candidate.outerConeRadians &&
                   candidate.outerConeRadians <= RVX_PI;
        }

        return true;
    }

    [[nodiscard]] bool IsValidCSMRequest(const LightingCSMRequest& request) noexcept
    {
        if (request.cascadeCount > RVX_MAX_DIRECTIONAL_SHADOW_CASCADES)
        {
            return false;
        }

        if (!request.requested)
        {
            return !request.inputsReady;
        }

        if (request.configurationRevision == 0)
        {
            return false;
        }
        // A request remains part of the immutable plan even when its inputs
        // are unavailable. Optional composition can then take an explicit
        // unshadowed fallback while receipts still report requested=true.
        return !request.inputsReady || request.cascadeCount != 0;
    }

    [[nodiscard]] bool IsValidIBLRequest(const LightingIBLRequest& request) noexcept
    {
        if (!IsFiniteFloat(request.normalizedIntensity) ||
            request.normalizedIntensity < 0.0f)
        {
            return false;
        }

        if (!request.requested)
        {
            return !request.inputsReady;
        }

        // Preserve author intent when assets are absent. Resource identity and
        // mip evidence are mandatory only for a ready texture-IBL request.
        return !request.inputsReady ||
               (request.resourceRevision != 0 &&
                request.prefilteredMipLevels != 0);
    }

    void CanonicalizeLightingRequests(
        LightingFramePlanRequest& request) noexcept
    {
        if (!request.csm.requested)
        {
            // Disabled CSM has no configuration contribution. Validation has
            // already checked the complete request before this cleanup.
            request.csm = {};
        }

        request.ibl.normalizedIntensity =
            CanonicalizeSignedZero(request.ibl.normalizedIntensity);
        if (!request.ibl.requested)
        {
            // The default IBL intensity is authoring-oriented (1.0f), but a
            // disabled plan must publish the all-zero canonical state.
            request.ibl = {};
            request.ibl.normalizedIntensity = 0.0f;
            return;
        }

        if (!request.ibl.inputsReady)
        {
            // Requested-but-unready IBL preserves the requested bit and
            // normalized intensity so optional composition can report intent,
            // while unavailable asset evidence cannot perturb plan identity.
            request.ibl.resourceRevision = 0;
            request.ibl.prefilteredMipLevels = 0;
        }
    }

    void MixBool(uint64& hash, bool value)
    {
        Diagnostics::MixContentHashValue(hash, value ? 1U : 0U);
    }

    void MixFloat(uint64& hash, float32 value)
    {
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(std::bit_cast<uint32>(value)));
    }

    void MixVector(uint64& hash, const Vec3& value)
    {
        MixFloat(hash, value.x);
        MixFloat(hash, value.y);
        MixFloat(hash, value.z);
    }

    void MixCandidate(uint64& hash, const LightingLightCandidate& candidate)
    {
        Diagnostics::MixContentHashValue(hash, candidate.lightId);
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(candidate.type));
        Diagnostics::MixContentHashValue(hash, candidate.priority);
        MixVector(hash, candidate.position);
        MixVector(hash, candidate.direction);
        MixVector(hash, candidate.color);
        MixFloat(hash, candidate.normalizedIntensity);
        MixFloat(hash, candidate.range);
        MixFloat(hash, candidate.innerConeRadians);
        MixFloat(hash, candidate.outerConeRadians);
        Diagnostics::MixContentHashValue(hash, candidate.layerMask);
        MixBool(hash, candidate.castsShadows);
    }

    [[nodiscard]] uint64 ComputeIdentityHash(
        const LightingFramePlanRequest& request,
        const std::vector<LightingLightCandidate>& candidates)
    {
        uint64 hash = Diagnostics::RVX_DIAGNOSTICS_FNV1A64_OFFSET_BASIS;
        Diagnostics::MixContentHashValue(hash, RVX_LIGHTING_FRAME_PLAN_SCHEMA_VERSION);
        Diagnostics::MixContentHashValue(hash, request.frameIdentity.frameSequence);
        Diagnostics::MixContentHashValue(hash, request.frameIdentity.sceneRevision);
        Diagnostics::MixContentHashValue(hash, request.frameIdentity.surfaceGeneration);
        Diagnostics::MixContentHashValue(
            hash, request.frameIdentity.configurationRevision);
        Diagnostics::MixContentHashValue(hash, request.viewIdentity.sceneRuntimeId);
        Diagnostics::MixContentHashValue(hash, request.viewIdentity.cameraEntityPacked);
        Diagnostics::MixContentHashValue(hash, request.viewIdentity.subViewIndex);
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(request.viewIdentity.role));
        Diagnostics::MixContentHashValue(hash, request.sceneLightStateHash);
        Diagnostics::MixContentHashValue(
            hash, request.lightingConfigurationRevision);
        Diagnostics::MixContentHashValue(hash, request.qualityRevision);
        Diagnostics::MixContentHashValue(hash, static_cast<uint64>(request.quality));
        Diagnostics::MixContentHashValue(hash, request.pointLightCapacity);
        Diagnostics::MixContentHashValue(hash, request.spotLightCapacity);
        MixBool(hash, request.allowLocalLightTruncation);
        MixBool(hash, request.csm.requested);
        MixBool(hash, request.csm.inputsReady);
        Diagnostics::MixContentHashValue(hash, request.csm.cascadeCount);
        Diagnostics::MixContentHashValue(hash, request.csm.configurationRevision);
        MixBool(hash, request.ibl.requested);
        MixBool(hash, request.ibl.inputsReady);
        Diagnostics::MixContentHashValue(hash, request.ibl.resourceRevision);
        Diagnostics::MixContentHashValue(hash, request.ibl.prefilteredMipLevels);
        MixFloat(hash, request.ibl.normalizedIntensity);
        Diagnostics::MixContentHashValue(hash, candidates.size());
        for (const LightingLightCandidate& candidate : candidates)
        {
            MixCandidate(hash, candidate);
        }
        return hash == 0 ? 1 : hash;
    }

    [[nodiscard]] bool IsHigherPriority(
        const LightingLightCandidate& left,
        const LightingLightCandidate& right) noexcept
    {
        return left.priority != right.priority ? left.priority > right.priority :
                                                left.lightId < right.lightId;
    }

    [[nodiscard]] LightingFramePlanBuildResult Fail(
        LightingFramePlanCode code,
        std::string diagnostic,
        LightingFramePlanCapacityEvidence evidence = {})
    {
        LightingFramePlanBuildResult result;
        result.code = code;
        result.capacityEvidence = evidence;
        result.diagnostic = std::move(diagnostic);
        return result;
    }
} // namespace

Vec3 EvaluateBlackbodyColorTemperature(float32 kelvin) noexcept
{
    if (!std::isfinite(kelvin) || kelvin < 1000.0f || kelvin > 40000.0f)
    {
        return Vec3(0.0f);
    }

    const float32 temperature = kelvin / 100.0f;
    float32 red = 255.0f;
    float32 green = 255.0f;
    float32 blue = 255.0f;
    if (temperature > 66.0f)
    {
        red = 329.698727446f *
            std::pow(temperature - 60.0f, -0.1332047592f);
        green = 288.1221695283f *
            std::pow(temperature - 60.0f, -0.0755148492f);
    }
    else
    {
        green = 99.4708025861f * std::log(temperature) -
            161.1195681661f;
        blue = temperature <= 19.0f
            ? 0.0f
            : 138.5177312231f * std::log(temperature - 10.0f) -
                  305.0447927307f;
    }

    constexpr float32 colorQuantization = 65535.0f;
    return Vec3(
        QuantizeStableFloat(
            SRGBChannelToLinear(std::clamp(red, 0.0f, 255.0f) / 255.0f),
            colorQuantization),
        QuantizeStableFloat(
            SRGBChannelToLinear(std::clamp(green, 0.0f, 255.0f) / 255.0f),
            colorQuantization),
        QuantizeStableFloat(
            SRGBChannelToLinear(std::clamp(blue, 0.0f, 255.0f) / 255.0f),
            colorQuantization));
}

bool LightingFramePlan::IsValid() const noexcept
{
    if (m_identityHash == 0 || !m_frameIdentity.IsValid() ||
        !m_viewIdentity.IsValid() || m_sceneLightStateHash == 0 ||
        m_lightingConfigurationRevision == 0 || m_qualityRevision == 0 ||
        !IsDeclaredRenderQualityTier(m_quality) ||
        m_capacityEvidence.localLightCapacity == 0 ||
        m_capacityEvidence.pointLightCapacity == 0 ||
        m_capacityEvidence.spotLightCapacity == 0 ||
        !IsValidCSMRequest(m_csm) || !IsValidIBLRequest(m_ibl) ||
        m_capacityEvidence.selectedLocalLightCount != m_localLights.size() ||
        m_capacityEvidence.selectedLocalLightCount !=
            m_capacityEvidence.selectedPointLightCount +
                m_capacityEvidence.selectedSpotLightCount)
    {
        return false;
    }

    if (m_hasPrimaryDirectionalLight &&
        (m_primaryDirectionalLight.type != RenderLightType::Directional ||
         !IsValidCandidate(m_primaryDirectionalLight)))
    {
        return false;
    }

    for (const LightingLightCandidate& light : m_localLights)
    {
        if (light.type == RenderLightType::Directional || !IsValidCandidate(light))
        {
            return false;
        }
    }
    return true;
}

const RenderFrameIdentity& LightingFramePlan::GetFrameIdentity() const noexcept
{
    return m_frameIdentity;
}

const RenderViewIdentity& LightingFramePlan::GetViewIdentity() const noexcept
{
    return m_viewIdentity;
}

uint64 LightingFramePlan::GetSceneLightStateHash() const noexcept
{
    return m_sceneLightStateHash;
}

uint64 LightingFramePlan::GetLightingConfigurationRevision() const noexcept
{
    return m_lightingConfigurationRevision;
}

uint64 LightingFramePlan::GetQualityRevision() const noexcept
{
    return m_qualityRevision;
}

RenderQualityTier LightingFramePlan::GetQuality() const noexcept
{
    return m_quality;
}

const LightingCSMRequest& LightingFramePlan::GetCSMRequest() const noexcept
{
    return m_csm;
}

const LightingIBLRequest& LightingFramePlan::GetIBLRequest() const noexcept
{
    return m_ibl;
}

const LightingLightCandidate* LightingFramePlan::GetPrimaryDirectionalLight() const noexcept
{
    return m_hasPrimaryDirectionalLight ? &m_primaryDirectionalLight : nullptr;
}

const std::vector<LightingLightCandidate>& LightingFramePlan::GetLocalLights() const noexcept
{
    return m_localLights;
}

const LightingFramePlanCapacityEvidence& LightingFramePlan::GetCapacityEvidence() const noexcept
{
    return m_capacityEvidence;
}

uint64 LightingFramePlan::GetIdentityHash() const noexcept
{
    return m_identityHash;
}

LightingFramePlanBuildResult LightingFramePlanBuilder::Build(
    const LightingFramePlanRequest& request) const
{
    if (request.schemaVersion != RVX_LIGHTING_FRAME_PLAN_SCHEMA_VERSION)
    {
        return Fail(LightingFramePlanCode::InvalidSchema,
                    "Lighting frame plan schema version is unsupported");
    }
    if (!request.frameIdentity.IsValid())
    {
        return Fail(LightingFramePlanCode::InvalidFrameIdentity,
                    "Lighting frame plan requires a valid frame identity");
    }
    if (!request.viewIdentity.IsValid())
    {
        return Fail(LightingFramePlanCode::InvalidViewIdentity,
                    "Lighting frame plan requires a valid view identity");
    }
    if (request.sceneLightStateHash == 0)
    {
        return Fail(LightingFramePlanCode::InvalidSceneLightStateHash,
                    "Lighting frame plan requires a non-zero scene light-state hash");
    }
    if (request.lightingConfigurationRevision == 0 || request.qualityRevision == 0)
    {
        return Fail(LightingFramePlanCode::InvalidConfigurationRevision,
                    "Lighting frame plan requires non-zero lighting and quality revisions");
    }
    if (!IsDeclaredRenderQualityTier(request.quality))
    {
        return Fail(LightingFramePlanCode::InvalidQuality,
                    "Lighting frame plan requested an undeclared quality tier");
    }
    if (request.pointLightCapacity == 0 || request.spotLightCapacity == 0 ||
        request.pointLightCapacity >
            std::numeric_limits<uint32>::max() - request.spotLightCapacity)
    {
        return Fail(LightingFramePlanCode::InvalidCapacity,
                    "Lighting frame plan point/spot capacities must be non-zero and have a representable sum");
    }
    if (!IsValidCSMRequest(request.csm))
    {
        return Fail(LightingFramePlanCode::InvalidCSMRequest,
                    "CSM request/readiness evidence is inconsistent or out of range");
    }
    if (!IsValidIBLRequest(request.ibl))
    {
        return Fail(LightingFramePlanCode::InvalidIBLRequest,
                    "IBL request/readiness evidence is inconsistent or invalid");
    }

    LightingFramePlanRequest canonicalRequest = request;
    CanonicalizeLightingRequests(canonicalRequest);

    std::vector<LightingLightCandidate> canonicalCandidates = request.lightCandidates;
    std::sort(canonicalCandidates.begin(), canonicalCandidates.end(),
              [](const LightingLightCandidate& left,
                 const LightingLightCandidate& right)
              {
                  return left.lightId < right.lightId;
              });

    uint64 priorLightId = 0;
    for (LightingLightCandidate& candidate : canonicalCandidates)
    {
        if (!IsValidCandidate(candidate))
        {
            return Fail(LightingFramePlanCode::InvalidLightCandidate,
                        "Lighting frame plan contains an invalid normalized light candidate");
        }
        if (candidate.lightId == priorLightId)
        {
            return Fail(LightingFramePlanCode::DuplicateLightCandidate,
                        "Lighting frame plan contains duplicate stable light candidate IDs");
        }
        CanonicalizeCandidate(candidate);
        priorLightId = candidate.lightId;
    }

    std::vector<LightingLightCandidate> directionalLights;
    std::vector<LightingLightCandidate> pointLights;
    std::vector<LightingLightCandidate> spotLights;
    directionalLights.reserve(canonicalCandidates.size());
    pointLights.reserve(canonicalCandidates.size());
    spotLights.reserve(canonicalCandidates.size());
    for (const LightingLightCandidate& candidate : canonicalCandidates)
    {
        if (candidate.type == RenderLightType::Directional)
        {
            directionalLights.push_back(candidate);
        }
        else if (candidate.type == RenderLightType::Point)
        {
            pointLights.push_back(candidate);
        }
        else
        {
            spotLights.push_back(candidate);
        }
    }
    std::sort(directionalLights.begin(), directionalLights.end(), IsHigherPriority);
    std::sort(pointLights.begin(), pointLights.end(), IsHigherPriority);
    std::sort(spotLights.begin(), spotLights.end(), IsHigherPriority);

    if (pointLights.size() > std::numeric_limits<uint32>::max() ||
        spotLights.size() > std::numeric_limits<uint32>::max() ||
        pointLights.size() + spotLights.size() >
            std::numeric_limits<uint32>::max())
    {
        return Fail(LightingFramePlanCode::InvalidCapacity,
                    "Lighting frame plan local-light count exceeds the uint32 capacity domain");
    }

    LightingFramePlanCapacityEvidence capacityEvidence;
    capacityEvidence.pointLightCapacity = canonicalRequest.pointLightCapacity;
    capacityEvidence.requestedPointLightCount =
        static_cast<uint32>(pointLights.size());
    capacityEvidence.selectedPointLightCount = static_cast<uint32>(
        std::min<size_t>(pointLights.size(), canonicalRequest.pointLightCapacity));
    capacityEvidence.overflowPointLightCount =
        capacityEvidence.requestedPointLightCount -
        capacityEvidence.selectedPointLightCount;
    capacityEvidence.spotLightCapacity = canonicalRequest.spotLightCapacity;
    capacityEvidence.requestedSpotLightCount =
        static_cast<uint32>(spotLights.size());
    capacityEvidence.selectedSpotLightCount = static_cast<uint32>(
        std::min<size_t>(spotLights.size(), canonicalRequest.spotLightCapacity));
    capacityEvidence.overflowSpotLightCount =
        capacityEvidence.requestedSpotLightCount -
        capacityEvidence.selectedSpotLightCount;
    capacityEvidence.localLightCapacity =
        canonicalRequest.pointLightCapacity + canonicalRequest.spotLightCapacity;
    capacityEvidence.requestedLocalLightCount =
        capacityEvidence.requestedPointLightCount +
        capacityEvidence.requestedSpotLightCount;
    capacityEvidence.selectedLocalLightCount =
        capacityEvidence.selectedPointLightCount +
        capacityEvidence.selectedSpotLightCount;
    capacityEvidence.overflowLocalLightCount =
        capacityEvidence.requestedLocalLightCount - capacityEvidence.selectedLocalLightCount;
    capacityEvidence.truncationAllowed = canonicalRequest.allowLocalLightTruncation;
    capacityEvidence.overflowed = capacityEvidence.overflowLocalLightCount != 0;

    if (capacityEvidence.overflowed && !capacityEvidence.truncationAllowed)
    {
        return Fail(LightingFramePlanCode::CapacityExceeded,
                    "Lighting frame plan local-light capacity exceeded without an explicit truncation policy",
                    capacityEvidence);
    }

    const uint64 identityHash =
        ComputeIdentityHash(canonicalRequest, canonicalCandidates);
    if (request.expectedIdentityHash != 0 &&
        request.expectedIdentityHash != identityHash)
    {
        return Fail(LightingFramePlanCode::IdentityMismatch,
                    "Lighting frame plan input no longer matches the expected identity hash",
                    capacityEvidence);
    }

    if (capacityEvidence.overflowed)
    {
        pointLights.resize(capacityEvidence.selectedPointLightCount);
        spotLights.resize(capacityEvidence.selectedSpotLightCount);
    }
    std::vector<LightingLightCandidate> localLights;
    localLights.reserve(pointLights.size() + spotLights.size());
    localLights.insert(localLights.end(), pointLights.begin(), pointLights.end());
    localLights.insert(localLights.end(), spotLights.begin(), spotLights.end());
    std::sort(localLights.begin(), localLights.end(), IsHigherPriority);

    LightingFramePlanBuildResult result;
    result.code = LightingFramePlanCode::Ready;
    result.capacityEvidence = capacityEvidence;
    result.plan.m_frameIdentity = canonicalRequest.frameIdentity;
    result.plan.m_viewIdentity = canonicalRequest.viewIdentity;
    result.plan.m_sceneLightStateHash = canonicalRequest.sceneLightStateHash;
    result.plan.m_lightingConfigurationRevision =
        canonicalRequest.lightingConfigurationRevision;
    result.plan.m_qualityRevision = canonicalRequest.qualityRevision;
    result.plan.m_quality = canonicalRequest.quality;
    result.plan.m_csm = canonicalRequest.csm;
    result.plan.m_ibl = canonicalRequest.ibl;
    result.plan.m_hasPrimaryDirectionalLight = !directionalLights.empty();
    if (result.plan.m_hasPrimaryDirectionalLight)
    {
        result.plan.m_primaryDirectionalLight = directionalLights.front();
    }
    result.plan.m_localLights = std::move(localLights);
    result.plan.m_capacityEvidence = capacityEvidence;
    result.plan.m_identityHash = identityHash;
    return result;
}

const char* GetLightingFramePlanCodeName(LightingFramePlanCode code) noexcept
{
    switch (code)
    {
        case LightingFramePlanCode::Ready: return "ready";
        case LightingFramePlanCode::InvalidSchema: return "invalid-schema";
        case LightingFramePlanCode::InvalidFrameIdentity: return "invalid-frame-identity";
        case LightingFramePlanCode::InvalidViewIdentity: return "invalid-view-identity";
        case LightingFramePlanCode::InvalidSceneLightStateHash:
            return "invalid-scene-light-state-hash";
        case LightingFramePlanCode::InvalidConfigurationRevision:
            return "invalid-configuration-revision";
        case LightingFramePlanCode::InvalidQuality: return "invalid-quality";
        case LightingFramePlanCode::InvalidCapacity: return "invalid-capacity";
        case LightingFramePlanCode::InvalidLightCandidate: return "invalid-light-candidate";
        case LightingFramePlanCode::DuplicateLightCandidate:
            return "duplicate-light-candidate";
        case LightingFramePlanCode::InvalidCSMRequest: return "invalid-csm-request";
        case LightingFramePlanCode::InvalidIBLRequest: return "invalid-ibl-request";
        case LightingFramePlanCode::CapacityExceeded: return "capacity-exceeded";
        case LightingFramePlanCode::IdentityMismatch: return "identity-mismatch";
        default: return "unknown";
    }
}
} // namespace RVX
