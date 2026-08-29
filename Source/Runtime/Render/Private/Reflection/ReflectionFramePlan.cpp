#include "Render/Reflection/ReflectionFramePlan.h"

#include "Core/Diagnostics/ContentHash.h"
#include "Render/Visibility/RenderVisibility.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>

namespace RVX
{
namespace
{
    enum class ProbeSnapshotValidation : uint8
    {
        Valid = 0,
        Unavailable,
        Invalid,
        UnsupportedMode,
    };

    [[nodiscard]] bool IsFiniteFloat(float32 value) noexcept
    {
        return std::isfinite(value);
    }

    [[nodiscard]] bool IsFiniteVector(const Vec3& value) noexcept
    {
        return IsFiniteFloat(value.x) && IsFiniteFloat(value.y) &&
               IsFiniteFloat(value.z);
    }

    [[nodiscard]] bool IsFiniteVector(const Vec4& value) noexcept
    {
        return IsFiniteFloat(value.x) && IsFiniteFloat(value.y) &&
               IsFiniteFloat(value.z) && IsFiniteFloat(value.w);
    }

    [[nodiscard]] bool IsFiniteMatrix(const Mat4& value) noexcept
    {
        for (uint32 column = 0; column < 4; ++column)
        {
            for (uint32 row = 0; row < 4; ++row)
            {
                if (!IsFiniteFloat(value[column][row]))
                {
                    return false;
                }
            }
        }
        return true;
    }

    [[nodiscard]] bool IsFiniteNonNegativeRadiance(const Vec3& value) noexcept
    {
        return IsFiniteVector(value) && value.x >= 0.0f && value.y >= 0.0f &&
               value.z >= 0.0f;
    }

    [[nodiscard]] bool IsDeclaredProbeShape(RenderProbeShape shape) noexcept
    {
        switch (shape)
        {
            case RenderProbeShape::Point:
            case RenderProbeShape::Box:
            case RenderProbeShape::Sphere: return true;
            default: return false;
        }
    }

    [[nodiscard]] bool IsDeclaredIBLSource(ReflectionIBLSource source) noexcept
    {
        switch (source)
        {
            case ReflectionIBLSource::Texture:
            case ReflectionIBLSource::Analytic: return true;
            default: return false;
        }
    }

    [[nodiscard]] bool IsDeclaredFallbackPolicy(
        ReflectionFallbackPolicy policy) noexcept
    {
        return policy == ReflectionFallbackPolicy::ScreenSpaceProbeIBL;
    }

    [[nodiscard]] bool IsValidSpatialProbe(
        const RenderProbeSnapshot& probe) noexcept
    {
        if (!IsDeclaredProbeShape(probe.shape) ||
            !IsFiniteMatrix(probe.worldTransform) ||
            !IsFiniteVector(probe.influenceExtent) ||
            !IsFiniteFloat(probe.blendDistance) ||
            probe.influenceExtent.x <= 0.0f ||
            probe.influenceExtent.y <= 0.0f ||
            probe.influenceExtent.z <= 0.0f || probe.blendDistance < 0.0f)
        {
            return false;
        }

        const float32 minimumExtent = std::min(
            probe.influenceExtent.x,
            std::min(probe.influenceExtent.y, probe.influenceExtent.z));
        if (probe.blendDistance > minimumExtent)
        {
            return false;
        }

        constexpr float32 affineTolerance = 1.0e-5f;
        if (std::abs(probe.worldTransform[0][3]) > affineTolerance ||
            std::abs(probe.worldTransform[1][3]) > affineTolerance ||
            std::abs(probe.worldTransform[2][3]) > affineTolerance ||
            std::abs(probe.worldTransform[3][3] - 1.0f) > affineTolerance)
        {
            return false;
        }

        const float32 determinantValue =
            determinant(Mat3(probe.worldTransform));
        return IsFiniteFloat(determinantValue) &&
               std::abs(determinantValue) > 1.0e-8f;
    }

    [[nodiscard]] AABB ComputeProbeWorldBounds(
        const RenderProbeSnapshot& probe) noexcept
    {
        const Vec3 center = Vec3(probe.worldTransform[3]);
        const Vec3 axisX = Vec3(probe.worldTransform[0]);
        const Vec3 axisY = Vec3(probe.worldTransform[1]);
        const Vec3 axisZ = Vec3(probe.worldTransform[2]);
        const Vec3 extent =
            glm::abs(axisX) * probe.influenceExtent.x +
            glm::abs(axisY) * probe.influenceExtent.y +
            glm::abs(axisZ) * probe.influenceExtent.z;
        return AABB(center - extent, center + extent);
    }

    [[nodiscard]] ProbeSnapshotValidation ValidateProbeSnapshot(
        const RenderProbeSnapshot& probe,
        uint64 expectedSceneRuntimeId) noexcept
    {
        if (probe.mode == RenderProbeMode::Realtime)
        {
            return ProbeSnapshotValidation::UnsupportedMode;
        }
        if (probe.mode != RenderProbeMode::Baked &&
            probe.mode != RenderProbeMode::Custom)
        {
            return ProbeSnapshotValidation::Invalid;
        }
        if (probe.probeId == 0 || !probe.source.IsValid() ||
            probe.source.sceneRuntimeId != expectedSceneRuntimeId ||
            probe.kind != RenderProbeKind::Reflection ||
            !IsValidSpatialProbe(probe) || probe.cullingMask == 0 ||
            !IsFiniteVector(probe.boxProjectionSize) ||
            !IsFiniteVector(probe.boxProjectionOffset) ||
            probe.boxProjectionSize.x <= 0.0f ||
            probe.boxProjectionSize.y <= 0.0f ||
            probe.boxProjectionSize.z <= 0.0f ||
            !IsFiniteFloat(probe.nearClip) || !IsFiniteFloat(probe.farClip) ||
            probe.nearClip <= 0.0f || probe.farClip <= probe.nearClip ||
            (probe.useBoxProjection && probe.shape != RenderProbeShape::Box))
        {
            return ProbeSnapshotValidation::Invalid;
        }

        for (const Vec4& coefficient : probe.sphericalHarmonics)
        {
            if (!IsFiniteVector(coefficient))
            {
                return ProbeSnapshotValidation::Invalid;
            }
        }

        // False declares a deliberately unavailable local-data payload, not a
        // corrupt scene object. Structural fields still must validate, while
        // the absent texture is not admitted to visibility or fixed capacity.
        if (!probe.hasValidData)
        {
            return ProbeSnapshotValidation::Unavailable;
        }
        if (!probe.texture.IsValid())
        {
            return ProbeSnapshotValidation::Invalid;
        }
        return ProbeSnapshotValidation::Valid;
    }

    [[nodiscard]] bool IsValidSSRIdentity(
        const ReflectionSSRIdentity& identity) noexcept
    {
        if (!identity.requested)
        {
            return !identity.inputsReady &&
                   identity.configurationIdentityHash == 0 &&
                   identity.pipelineIdentityHash == 0 &&
                   identity.depthPyramidIdentityHash == 0 &&
                   identity.historyIdentityHash == 0;
        }

        if (identity.configurationIdentityHash == 0)
        {
            return false;
        }
        if (!identity.inputsReady)
        {
            return identity.pipelineIdentityHash == 0 &&
                   identity.depthPyramidIdentityHash == 0 &&
                   identity.historyIdentityHash == 0;
        }
        return identity.pipelineIdentityHash != 0 &&
               identity.depthPyramidIdentityHash != 0;
    }

    [[nodiscard]] bool IsValidIBLIdentity(
        const ReflectionIBLIdentity& identity) noexcept
    {
        if (!IsDeclaredIBLSource(identity.source) ||
            identity.lightingPlanIdentityHash == 0)
        {
            return false;
        }
        if (identity.source == ReflectionIBLSource::Texture)
        {
            return identity.resourceRevision != 0 &&
                   identity.prefilteredMipLevels != 0 &&
                   identity.analyticFallbackIdentityHash == 0;
        }
        return identity.resourceRevision == 0 &&
               identity.prefilteredMipLevels == 0 &&
               identity.analyticFallbackIdentityHash != 0;
    }

    [[nodiscard]] bool IsValidFallbackIdentity(
        const ReflectionFallbackIdentity& identity) noexcept
    {
        return IsDeclaredFallbackPolicy(identity.policy) &&
               identity.configurationIdentityHash != 0;
    }

    [[nodiscard]] float32 CanonicalizeSignedZero(float32 value) noexcept
    {
        return value == 0.0f ? 0.0f : value;
    }

    void CanonicalizeVector(Vec3& value) noexcept
    {
        value.x = CanonicalizeSignedZero(value.x);
        value.y = CanonicalizeSignedZero(value.y);
        value.z = CanonicalizeSignedZero(value.z);
    }

    void CanonicalizeVector(Vec4& value) noexcept
    {
        value.x = CanonicalizeSignedZero(value.x);
        value.y = CanonicalizeSignedZero(value.y);
        value.z = CanonicalizeSignedZero(value.z);
        value.w = CanonicalizeSignedZero(value.w);
    }

    void CanonicalizeMatrix(Mat4& value) noexcept
    {
        for (uint32 column = 0; column < 4; ++column)
        {
            for (uint32 row = 0; row < 4; ++row)
            {
                value[column][row] =
                    CanonicalizeSignedZero(value[column][row]);
            }
        }
    }

    void CanonicalizeProbe(RenderProbeSnapshot& probe) noexcept
    {
        CanonicalizeMatrix(probe.worldTransform);
        CanonicalizeVector(probe.influenceExtent);
        probe.blendDistance = CanonicalizeSignedZero(probe.blendDistance);
        CanonicalizeVector(probe.boxProjectionSize);
        CanonicalizeVector(probe.boxProjectionOffset);
        for (Vec4& coefficient : probe.sphericalHarmonics)
        {
            CanonicalizeVector(coefficient);
        }
        probe.nearClip = CanonicalizeSignedZero(probe.nearClip);
        probe.farClip = CanonicalizeSignedZero(probe.farClip);
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

    void MixVector(uint64& hash, const Vec4& value)
    {
        MixFloat(hash, value.x);
        MixFloat(hash, value.y);
        MixFloat(hash, value.z);
        MixFloat(hash, value.w);
    }

    void MixMatrix(uint64& hash, const Mat4& value)
    {
        for (uint32 column = 0; column < 4; ++column)
        {
            for (uint32 row = 0; row < 4; ++row)
            {
                MixFloat(hash, value[column][row]);
            }
        }
    }

    void MixProbe(uint64& hash, const RenderProbeSnapshot& probe)
    {
        Diagnostics::MixContentHashValue(hash, probe.probeId);
        Diagnostics::MixContentHashValue(hash, probe.source.sceneRuntimeId);
        Diagnostics::MixContentHashValue(hash, probe.source.entitySlot);
        Diagnostics::MixContentHashValue(hash, probe.source.entityGeneration);
        Diagnostics::MixContentHashValue(hash, static_cast<uint64>(probe.kind));
        Diagnostics::MixContentHashValue(hash, static_cast<uint64>(probe.shape));
        Diagnostics::MixContentHashValue(hash, static_cast<uint64>(probe.mode));
        MixMatrix(hash, probe.worldTransform);
        MixVector(hash, probe.influenceExtent);
        MixFloat(hash, probe.blendDistance);
        MixVector(hash, probe.boxProjectionSize);
        MixVector(hash, probe.boxProjectionOffset);
        Diagnostics::MixContentHashValue(hash, probe.texture.slot);
        Diagnostics::MixContentHashValue(hash, probe.texture.generation);
        for (const Vec4& coefficient : probe.sphericalHarmonics)
        {
            MixVector(hash, coefficient);
        }
        Diagnostics::MixContentHashValue(hash, probe.cullingMask);
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(static_cast<uint32>(probe.priority)));
        MixFloat(hash, probe.nearClip);
        MixFloat(hash, probe.farClip);
        MixBool(hash, probe.useBoxProjection);
        MixBool(hash, probe.useHDR);
        MixBool(hash, probe.hasValidData);
    }

    [[nodiscard]] uint64 ComputeIdentityHash(
        const ReflectionFramePlanRequest& request,
        const std::vector<RenderProbeSnapshot>& candidates)
    {
        uint64 hash = Diagnostics::RVX_DIAGNOSTICS_FNV1A64_OFFSET_BASIS;
        Diagnostics::MixContentHashValue(
            hash, RVX_REFLECTION_FRAME_PLAN_SCHEMA_VERSION);
        Diagnostics::MixContentHashValue(hash, request.frameIdentity.frameSequence);
        Diagnostics::MixContentHashValue(hash, request.frameIdentity.sceneRevision);
        Diagnostics::MixContentHashValue(
            hash, request.frameIdentity.surfaceGeneration);
        Diagnostics::MixContentHashValue(
            hash, request.frameIdentity.configurationRevision);
        Diagnostics::MixContentHashValue(hash, request.viewIdentity.sceneRuntimeId);
        Diagnostics::MixContentHashValue(
            hash, request.viewIdentity.cameraEntityPacked);
        Diagnostics::MixContentHashValue(hash, request.viewIdentity.subViewIndex);
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(request.viewIdentity.role));
        Diagnostics::MixContentHashValue(hash, request.sceneProbeStateHash);
        Diagnostics::MixContentHashValue(
            hash, request.reflectionConfigurationRevision);
        MixVector(hash, request.viewPosition);
        MixMatrix(hash, request.viewProjection);
        Diagnostics::MixContentHashValue(hash, request.viewCullingMask);
        Diagnostics::MixContentHashValue(hash, RVX_REFLECTION_PROBE_CAPACITY);

        MixBool(hash, request.ssr.requested);
        MixBool(hash, request.ssr.inputsReady);
        Diagnostics::MixContentHashValue(
            hash, request.ssr.configurationIdentityHash);
        Diagnostics::MixContentHashValue(hash, request.ssr.pipelineIdentityHash);
        Diagnostics::MixContentHashValue(
            hash, request.ssr.depthPyramidIdentityHash);
        Diagnostics::MixContentHashValue(hash, request.ssr.historyIdentityHash);

        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(request.ibl.source));
        Diagnostics::MixContentHashValue(
            hash, request.ibl.lightingPlanIdentityHash);
        Diagnostics::MixContentHashValue(hash, request.ibl.resourceRevision);
        Diagnostics::MixContentHashValue(
            hash, request.ibl.prefilteredMipLevels);
        Diagnostics::MixContentHashValue(
            hash, request.ibl.analyticFallbackIdentityHash);
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(request.fallback.policy));
        Diagnostics::MixContentHashValue(
            hash, request.fallback.configurationIdentityHash);

        Diagnostics::MixContentHashValue(hash, candidates.size());
        for (const RenderProbeSnapshot& candidate : candidates)
        {
            MixProbe(hash, candidate);
        }
        return hash == 0 ? 1 : hash;
    }

    [[nodiscard]] bool IsHigherPriority(
        const ReflectionProbeSelection& left,
        const ReflectionProbeSelection& right) noexcept
    {
        if (left.snapshot.priority != right.snapshot.priority)
        {
            return left.snapshot.priority > right.snapshot.priority;
        }
        if (left.quantizedInfluenceScore != right.quantizedInfluenceScore)
        {
            return left.quantizedInfluenceScore >
                   right.quantizedInfluenceScore;
        }
        return left.snapshot.probeId < right.snapshot.probeId;
    }

    [[nodiscard]] ReflectionFramePlanBuildResult Fail(
        ReflectionFramePlanCode code,
        std::string diagnostic,
        ReflectionFramePlanCapacityEvidence evidence = {})
    {
        ReflectionFramePlanBuildResult result;
        result.code = code;
        result.capacityEvidence = evidence;
        result.diagnostic = std::move(diagnostic);
        return result;
    }

    [[nodiscard]] ReflectionRouteResolution FailRoute(
        const char* diagnostic) noexcept
    {
        ReflectionRouteResolution result;
        result.diagnostic = diagnostic;
        return result;
    }

    [[nodiscard]] ReflectionResolutionRoute ClassifyRoute(
        float32 ssrWeight,
        float32 probeWeight,
        float32 iblWeight) noexcept
    {
        const bool hasSSR = ssrWeight > 0.0f;
        const bool hasProbe = probeWeight > 0.0f;
        const bool hasIBL = iblWeight > 0.0f;
        if (hasSSR && hasProbe && hasIBL)
            return ReflectionResolutionRoute::SSRProbeAndIBL;
        if (hasSSR && hasProbe)
            return ReflectionResolutionRoute::SSRAndProbe;
        if (hasSSR && hasIBL)
            return ReflectionResolutionRoute::SSRAndIBL;
        if (hasProbe && hasIBL)
            return ReflectionResolutionRoute::ProbeAndIBL;
        if (hasSSR)
            return ReflectionResolutionRoute::SSR;
        if (hasProbe)
            return ReflectionResolutionRoute::Probe;
        return hasIBL ? ReflectionResolutionRoute::IBL :
                        ReflectionResolutionRoute::Invalid;
    }
} // namespace

float32 EvaluateReflectionProbeInfluence(
    const RenderProbeSnapshot& probe,
    const Vec3& samplePosition) noexcept
{
    if (!IsValidSpatialProbe(probe) || !IsFiniteVector(samplePosition))
    {
        return 0.0f;
    }

    const Mat4 inverseTransform = inverse(probe.worldTransform);
    const Vec4 localHomogeneous =
        inverseTransform * Vec4(samplePosition, 1.0f);
    if (!IsFiniteVector(localHomogeneous) ||
        std::abs(localHomogeneous.w) <= 1.0e-8f)
    {
        return 0.0f;
    }
    const Vec3 localPosition = Vec3(localHomogeneous) / localHomogeneous.w;
    if (!IsFiniteVector(localPosition))
    {
        return 0.0f;
    }

    const Vec3 normalizedPosition(
        localPosition.x / probe.influenceExtent.x,
        localPosition.y / probe.influenceExtent.y,
        localPosition.z / probe.influenceExtent.z);
    float32 normalizedDistance = 0.0f;
    if (probe.shape == RenderProbeShape::Box)
    {
        normalizedDistance = std::max(
            std::abs(normalizedPosition.x),
            std::max(std::abs(normalizedPosition.y),
                     std::abs(normalizedPosition.z)));
    }
    else
    {
        normalizedDistance = length(normalizedPosition);
    }
    if (!IsFiniteFloat(normalizedDistance) || normalizedDistance > 1.0f)
    {
        return 0.0f;
    }

    if (probe.blendDistance == 0.0f)
    {
        return 1.0f;
    }
    const float32 minimumExtent = std::min(
        probe.influenceExtent.x,
        std::min(probe.influenceExtent.y, probe.influenceExtent.z));
    const float32 normalizedBlendDistance =
        probe.blendDistance / minimumExtent;
    return std::clamp(
        (1.0f - normalizedDistance) / normalizedBlendDistance,
        0.0f,
        1.0f);
}

uint32 QuantizeReflectionProbeInfluence(float32 influence) noexcept
{
    if (!IsFiniteFloat(influence) || influence <= 0.0f)
    {
        return 0;
    }
    const float32 normalized = std::clamp(influence, 0.0f, 1.0f);
    return static_cast<uint32>(std::lround(
        normalized * static_cast<float32>(
                         RVX_REFLECTION_INFLUENCE_SCORE_MAX)));
}

bool ReflectionFramePlan::IsValid() const noexcept
{
    if (m_identityHash == 0 || !m_frameIdentity.IsValid() ||
        !m_viewIdentity.IsValid() || m_sceneProbeStateHash == 0 ||
        m_reflectionConfigurationRevision == 0 ||
        !IsFiniteVector(m_viewPosition) || !IsFiniteMatrix(m_viewProjection) ||
        m_viewCullingMask == 0 ||
        !IsValidSSRIdentity(m_ssr) || !IsValidIBLIdentity(m_ibl) ||
        !IsValidFallbackIdentity(m_fallback) ||
        m_capacityEvidence.probeCapacity != RVX_REFLECTION_PROBE_CAPACITY ||
        m_capacityEvidence.selectedProbeCount != m_selectedProbes.size() ||
        m_capacityEvidence.selectedProbeCount >
            m_capacityEvidence.eligibleProbeCount ||
        m_capacityEvidence.selectedProbeCount > RVX_REFLECTION_PROBE_CAPACITY ||
        m_capacityEvidence.selectedProbeCount !=
            std::min(m_capacityEvidence.eligibleProbeCount,
                     RVX_REFLECTION_PROBE_CAPACITY) ||
        m_capacityEvidence.overflowProbeCount !=
            m_capacityEvidence.eligibleProbeCount -
                m_capacityEvidence.selectedProbeCount ||
        m_capacityEvidence.overflowed !=
            (m_capacityEvidence.overflowProbeCount != 0) ||
        m_capacityEvidence.candidateProbeCount !=
                m_capacityEvidence.unavailableProbeCount +
                m_capacityEvidence.eligibleProbeCount +
                m_capacityEvidence.layerRejectedProbeCount +
                m_capacityEvidence.frustumRejectedProbeCount)
    {
        return false;
    }

    for (size_t index = 0; index < m_selectedProbes.size(); ++index)
    {
        const ReflectionProbeSelection& selection = m_selectedProbes[index];
        if (ValidateProbeSnapshot(
                selection.snapshot, m_viewIdentity.sceneRuntimeId) !=
                ProbeSnapshotValidation::Valid ||
            !IsRenderLayerVisible(
                selection.snapshot.cullingMask, m_viewCullingMask) ||
            !IsRenderVisibilityAABBVisible(
                ComputeProbeWorldBounds(selection.snapshot),
                RenderVisibilityFrustum::FromViewProjection(
                    m_viewProjection)))
        {
            return false;
        }
        for (size_t priorIndex = 0; priorIndex < index; ++priorIndex)
        {
            if (m_selectedProbes[priorIndex].snapshot.probeId ==
                selection.snapshot.probeId)
            {
                return false;
            }
        }
        const uint32 expectedScore = QuantizeReflectionProbeInfluence(
            EvaluateReflectionProbeInfluence(
                selection.snapshot, m_viewPosition));
        const float32 expectedWeight =
            static_cast<float32>(expectedScore) /
            static_cast<float32>(RVX_REFLECTION_INFLUENCE_SCORE_MAX);
        if (expectedScore != selection.quantizedInfluenceScore ||
            expectedWeight != selection.influenceWeight)
        {
            return false;
        }
    }
    return std::is_sorted(
        m_selectedProbes.begin(), m_selectedProbes.end(), IsHigherPriority);
}

const RenderFrameIdentity& ReflectionFramePlan::GetFrameIdentity() const noexcept
{
    return m_frameIdentity;
}

const RenderViewIdentity& ReflectionFramePlan::GetViewIdentity() const noexcept
{
    return m_viewIdentity;
}

uint64 ReflectionFramePlan::GetSceneProbeStateHash() const noexcept
{
    return m_sceneProbeStateHash;
}

uint64 ReflectionFramePlan::GetReflectionConfigurationRevision() const noexcept
{
    return m_reflectionConfigurationRevision;
}

const Vec3& ReflectionFramePlan::GetViewPosition() const noexcept
{
    return m_viewPosition;
}

const Mat4& ReflectionFramePlan::GetViewProjection() const noexcept
{
    return m_viewProjection;
}

uint32 ReflectionFramePlan::GetViewCullingMask() const noexcept
{
    return m_viewCullingMask;
}

const ReflectionSSRIdentity& ReflectionFramePlan::GetSSRIdentity() const noexcept
{
    return m_ssr;
}

const ReflectionIBLIdentity& ReflectionFramePlan::GetIBLIdentity() const noexcept
{
    return m_ibl;
}

const ReflectionFallbackIdentity&
ReflectionFramePlan::GetFallbackIdentity() const noexcept
{
    return m_fallback;
}

const std::vector<ReflectionProbeSelection>&
ReflectionFramePlan::GetSelectedProbes() const noexcept
{
    return m_selectedProbes;
}

const ReflectionFramePlanCapacityEvidence&
ReflectionFramePlan::GetCapacityEvidence() const noexcept
{
    return m_capacityEvidence;
}

uint64 ReflectionFramePlan::GetIdentityHash() const noexcept
{
    return m_identityHash;
}

ReflectionFramePlanBuildResult ReflectionFramePlanBuilder::Build(
    const ReflectionFramePlanRequest& request) const
{
    if (request.schemaVersion != RVX_REFLECTION_FRAME_PLAN_SCHEMA_VERSION)
    {
        return Fail(ReflectionFramePlanCode::InvalidSchema,
                    "Reflection frame plan schema version is unsupported");
    }
    if (!request.frameIdentity.IsValid())
    {
        return Fail(ReflectionFramePlanCode::InvalidFrameIdentity,
                    "Reflection frame plan requires a valid frame identity");
    }
    if (!request.viewIdentity.IsValid())
    {
        return Fail(ReflectionFramePlanCode::InvalidViewIdentity,
                    "Reflection frame plan requires a valid view identity");
    }
    if (!IsFiniteVector(request.viewPosition) ||
        !IsFiniteMatrix(request.viewProjection) ||
        request.viewCullingMask == 0)
    {
        return Fail(ReflectionFramePlanCode::InvalidViewState,
                    "Reflection frame plan requires a finite view position and a non-zero culling mask");
    }
    if (request.sceneProbeStateHash == 0)
    {
        return Fail(ReflectionFramePlanCode::InvalidSceneProbeStateHash,
                    "Reflection frame plan requires a non-zero scene probe-state hash");
    }
    if (request.reflectionConfigurationRevision == 0)
    {
        return Fail(ReflectionFramePlanCode::InvalidConfigurationRevision,
                    "Reflection frame plan requires a non-zero configuration revision");
    }
    if (!IsValidSSRIdentity(request.ssr))
    {
        return Fail(ReflectionFramePlanCode::InvalidSSRIdentity,
                    "Reflection frame plan SSR identity/readiness evidence is inconsistent");
    }
    if (!IsValidIBLIdentity(request.ibl))
    {
        return Fail(ReflectionFramePlanCode::InvalidIBLIdentity,
                    "Reflection frame plan requires one exact texture or analytic IBL fallback identity");
    }
    if (!IsValidFallbackIdentity(request.fallback))
    {
        return Fail(ReflectionFramePlanCode::InvalidFallbackIdentity,
                    "Reflection frame plan fallback policy identity is invalid");
    }
    if (request.probeCandidates.size() >
        static_cast<size_t>(std::numeric_limits<uint32>::max()))
    {
        return Fail(ReflectionFramePlanCode::InvalidProbeSnapshot,
                    "Reflection probe candidate count exceeds the uint32 evidence domain");
    }

    ReflectionFramePlanRequest canonicalRequest = request;
    CanonicalizeVector(canonicalRequest.viewPosition);
    CanonicalizeMatrix(canonicalRequest.viewProjection);
    std::vector<RenderProbeSnapshot> canonicalCandidates =
        request.probeCandidates;
    std::sort(
        canonicalCandidates.begin(), canonicalCandidates.end(),
        [](const RenderProbeSnapshot& left, const RenderProbeSnapshot& right)
        {
            return left.probeId < right.probeId;
        });

    ReflectionFramePlanCapacityEvidence capacityEvidence;
    capacityEvidence.candidateProbeCount =
        static_cast<uint32>(canonicalCandidates.size());
    std::vector<bool> unavailableCandidates;
    unavailableCandidates.reserve(canonicalCandidates.size());
    uint64 priorProbeId = 0;
    for (RenderProbeSnapshot& candidate : canonicalCandidates)
    {
        const ProbeSnapshotValidation validation = ValidateProbeSnapshot(
            candidate, request.viewIdentity.sceneRuntimeId);
        if (validation == ProbeSnapshotValidation::UnsupportedMode)
        {
            return Fail(ReflectionFramePlanCode::UnsupportedProbeMode,
                        "Reflection frame plan supports only Baked and Custom reflection cubemaps");
        }
        if (validation == ProbeSnapshotValidation::Invalid)
        {
            return Fail(ReflectionFramePlanCode::InvalidProbeSnapshot,
                        "Reflection frame plan contains an invalid reflection-probe snapshot");
        }
        if (candidate.probeId == priorProbeId)
        {
            return Fail(ReflectionFramePlanCode::DuplicateProbeCandidate,
                        "Reflection frame plan contains duplicate stable probe IDs");
        }
        CanonicalizeProbe(candidate);
        priorProbeId = candidate.probeId;
        const bool unavailable =
            validation == ProbeSnapshotValidation::Unavailable;
        unavailableCandidates.push_back(unavailable);
        if (unavailable)
        {
            ++capacityEvidence.unavailableProbeCount;
        }
    }

    std::vector<ReflectionProbeSelection> selectedProbes;
    selectedProbes.reserve(canonicalCandidates.size());
    const RenderVisibilityFrustum viewFrustum =
        RenderVisibilityFrustum::FromViewProjection(
            canonicalRequest.viewProjection);
    for (size_t candidateIndex = 0;
         candidateIndex < canonicalCandidates.size();
         ++candidateIndex)
    {
        const RenderProbeSnapshot& candidate =
            canonicalCandidates[candidateIndex];
        if (unavailableCandidates[candidateIndex])
        {
            continue;
        }
        if (!IsRenderLayerVisible(
                candidate.cullingMask, canonicalRequest.viewCullingMask))
        {
            ++capacityEvidence.layerRejectedProbeCount;
            continue;
        }

        if (!IsRenderVisibilityAABBVisible(
                ComputeProbeWorldBounds(candidate), viewFrustum))
        {
            ++capacityEvidence.frustumRejectedProbeCount;
            continue;
        }

        const uint32 influenceScore = QuantizeReflectionProbeInfluence(
            EvaluateReflectionProbeInfluence(
                candidate, canonicalRequest.viewPosition));
        ReflectionProbeSelection selection;
        selection.snapshot = candidate;
        selection.quantizedInfluenceScore = influenceScore;
        selection.influenceWeight =
            static_cast<float32>(influenceScore) /
            static_cast<float32>(RVX_REFLECTION_INFLUENCE_SCORE_MAX);
        selectedProbes.push_back(std::move(selection));
    }

    std::sort(selectedProbes.begin(), selectedProbes.end(), IsHigherPriority);
    capacityEvidence.eligibleProbeCount =
        static_cast<uint32>(selectedProbes.size());
    capacityEvidence.selectedProbeCount = static_cast<uint32>(
        std::min<size_t>(selectedProbes.size(), RVX_REFLECTION_PROBE_CAPACITY));
    capacityEvidence.overflowProbeCount =
        capacityEvidence.eligibleProbeCount -
        capacityEvidence.selectedProbeCount;
    capacityEvidence.overflowed = capacityEvidence.overflowProbeCount != 0;
    selectedProbes.resize(capacityEvidence.selectedProbeCount);

    const uint64 identityHash =
        ComputeIdentityHash(canonicalRequest, canonicalCandidates);
    if (request.expectedIdentityHash != 0 &&
        request.expectedIdentityHash != identityHash)
    {
        return Fail(ReflectionFramePlanCode::IdentityMismatch,
                    "Reflection frame plan input no longer matches the expected identity hash",
                    capacityEvidence);
    }

    ReflectionFramePlanBuildResult result;
    result.code = ReflectionFramePlanCode::Ready;
    result.capacityEvidence = capacityEvidence;
    result.plan.m_frameIdentity = canonicalRequest.frameIdentity;
    result.plan.m_viewIdentity = canonicalRequest.viewIdentity;
    result.plan.m_sceneProbeStateHash = canonicalRequest.sceneProbeStateHash;
    result.plan.m_reflectionConfigurationRevision =
        canonicalRequest.reflectionConfigurationRevision;
    result.plan.m_viewPosition = canonicalRequest.viewPosition;
    result.plan.m_viewProjection = canonicalRequest.viewProjection;
    result.plan.m_viewCullingMask = canonicalRequest.viewCullingMask;
    result.plan.m_ssr = canonicalRequest.ssr;
    result.plan.m_ibl = canonicalRequest.ibl;
    result.plan.m_fallback = canonicalRequest.fallback;
    result.plan.m_selectedProbes = std::move(selectedProbes);
    result.plan.m_capacityEvidence = capacityEvidence;
    result.plan.m_identityHash = identityHash;
    return result;
}

ReflectionRouteResolution ResolveReflectionRoute(
    const ReflectionRouteSample& sample) noexcept
{
    if (!sample.iblValid ||
        !IsFiniteNonNegativeRadiance(sample.iblRadiance))
    {
        return FailRoute(
            "Reflection resolution requires a finite non-negative IBL fallback");
    }
    if (!IsFiniteNonNegativeRadiance(sample.ssrRadiance) ||
        !IsFiniteNonNegativeRadiance(sample.probeRadiance) ||
        !IsFiniteFloat(sample.ssrConfidence) ||
        !IsFiniteFloat(sample.probeConfidence) ||
        sample.ssrConfidence < 0.0f || sample.ssrConfidence > 1.0f ||
        sample.probeConfidence < 0.0f || sample.probeConfidence > 1.0f)
    {
        return FailRoute(
            "Reflection resolution received invalid radiance or confidence data");
    }
    if ((!sample.ssrHitValid && sample.ssrConfidence != 0.0f) ||
        (!sample.probeValid && sample.probeConfidence != 0.0f))
    {
        return FailRoute(
            "Reflection resolution validity and confidence evidence is inconsistent");
    }

    const float32 ssrWeight =
        sample.ssrHitValid ? sample.ssrConfidence : 0.0f;
    const float32 probeConfidence =
        sample.probeValid ? sample.probeConfidence : 0.0f;
    const float32 residualWeight = 1.0f - ssrWeight;
    const float32 probeWeight = residualWeight * probeConfidence;
    const float32 iblWeight = residualWeight * (1.0f - probeConfidence);

    ReflectionRouteResolution result;
    result.route = ClassifyRoute(ssrWeight, probeWeight, iblWeight);
    result.ssrWeight = ssrWeight;
    result.probeWeight = probeWeight;
    result.iblWeight = iblWeight;
    result.radiance = sample.ssrRadiance * ssrWeight +
                      sample.probeRadiance * probeWeight +
                      sample.iblRadiance * iblWeight;
    if (result.route == ReflectionResolutionRoute::Invalid ||
        !IsFiniteNonNegativeRadiance(result.radiance))
    {
        return FailRoute(
            "Reflection resolution could not produce a valid fallback route");
    }
    return result;
}

MaterializedReflectionRouteResolution ResolveMaterializedReflectionRoute(
    const MaterializedReflectionRouteSample& sample) noexcept
{
    const auto fail = [](const char* diagnostic)
    {
        MaterializedReflectionRouteResolution result;
        result.diagnostic = diagnostic;
        return result;
    };
    if (!IsFiniteNonNegativeRadiance(sample.sceneColor) ||
        !IsFiniteNonNegativeRadiance(sample.baseIndirectSpecular) ||
        !IsFiniteNonNegativeRadiance(sample.specularResponse) ||
        !IsFiniteNonNegativeRadiance(sample.ssrSceneRadiance) ||
        !IsFiniteNonNegativeRadiance(sample.localProbeRadiance) ||
        !IsFiniteFloat(sample.producerPreExposure) ||
        !IsFiniteFloat(sample.ssrConfidence) ||
        !IsFiniteFloat(sample.localProbeWeight) ||
        sample.producerPreExposure <= 0.0f ||
        sample.ssrConfidence < 0.0f || sample.ssrConfidence > 1.0f ||
        sample.localProbeWeight < 0.0f ||
        sample.localProbeWeight > 1.0f)
    {
        return fail(
            "Materialized reflection resolution received invalid energy-domain inputs");
    }
    if ((!sample.ssrHitValid && sample.ssrConfidence != 0.0f) ||
        (!sample.localProbeValid && sample.localProbeWeight != 0.0f))
    {
        return fail(
            "Materialized reflection route validity and weight evidence is inconsistent");
    }
    if (!sample.surfaceInputsValid)
    {
        if (sample.ssrConfidence != 0.0f ||
            sample.localProbeWeight != 0.0f)
        {
            return fail(
                "Materialized reflection route cannot consume an invalid receiver response");
        }
        MaterializedReflectionRouteResolution preserved;
        preserved.route = ReflectionResolutionRoute::IBL;
        preserved.resolvedSceneColor = sample.sceneColor;
        preserved.routedIndirectSpecular = sample.baseIndirectSpecular;
        return preserved;
    }

    const float32 probeWeight = sample.localProbeValid
        ? sample.localProbeWeight : 0.0f;
    const Vec3 materializedLocalProbe =
        sample.localProbeRadiance * sample.specularResponse *
        sample.producerPreExposure;
    const Vec3 probeOrGlobal = glm::mix(
        sample.baseIndirectSpecular, materializedLocalProbe, probeWeight);
    const float32 ssrWeight = sample.ssrHitValid
        ? sample.ssrConfidence : 0.0f;
    const Vec3 materializedSSR =
        sample.ssrSceneRadiance * sample.specularResponse;
    const Vec3 routed = glm::mix(
        probeOrGlobal, materializedSSR, ssrWeight);
    const Vec3 resolved = glm::max(
        sample.sceneColor - sample.baseIndirectSpecular + routed,
        Vec3(0.0f));
    if (!IsFiniteNonNegativeRadiance(routed) ||
        !IsFiniteNonNegativeRadiance(resolved))
    {
        return fail(
            "Materialized reflection resolution produced invalid radiance");
    }

    MaterializedReflectionRouteResolution result;
    result.route = ClassifyRoute(
        ssrWeight,
        (1.0f - ssrWeight) * probeWeight,
        (1.0f - ssrWeight) * (1.0f - probeWeight));
    result.resolvedSceneColor = resolved;
    result.routedIndirectSpecular = routed;
    if (result.route == ReflectionResolutionRoute::Invalid)
    {
        return fail(
            "Materialized reflection resolution could not classify its route");
    }
    return result;
}

const char* GetReflectionFramePlanCodeName(
    ReflectionFramePlanCode code) noexcept
{
    switch (code)
    {
        case ReflectionFramePlanCode::Ready: return "ready";
        case ReflectionFramePlanCode::InvalidSchema: return "invalid-schema";
        case ReflectionFramePlanCode::InvalidFrameIdentity:
            return "invalid-frame-identity";
        case ReflectionFramePlanCode::InvalidViewIdentity:
            return "invalid-view-identity";
        case ReflectionFramePlanCode::InvalidViewState:
            return "invalid-view-state";
        case ReflectionFramePlanCode::InvalidSceneProbeStateHash:
            return "invalid-scene-probe-state-hash";
        case ReflectionFramePlanCode::InvalidConfigurationRevision:
            return "invalid-configuration-revision";
        case ReflectionFramePlanCode::InvalidSSRIdentity:
            return "invalid-ssr-identity";
        case ReflectionFramePlanCode::InvalidIBLIdentity:
            return "invalid-ibl-identity";
        case ReflectionFramePlanCode::InvalidFallbackIdentity:
            return "invalid-fallback-identity";
        case ReflectionFramePlanCode::InvalidProbeSnapshot:
            return "invalid-probe-snapshot";
        case ReflectionFramePlanCode::UnsupportedProbeMode:
            return "unsupported-probe-mode";
        case ReflectionFramePlanCode::DuplicateProbeCandidate:
            return "duplicate-probe-candidate";
        case ReflectionFramePlanCode::IdentityMismatch:
            return "identity-mismatch";
        default: return "unknown";
    }
}
} // namespace RVX
