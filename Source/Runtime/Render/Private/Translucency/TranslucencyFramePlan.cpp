#include "Render/Translucency/TranslucencyFramePlan.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <set>
#include <tuple>
#include <type_traits>
#include <utility>

namespace RVX
{
namespace
{
    constexpr uint64 RVX_TRANSLUCENCY_FNV_OFFSET = 14695981039346656037ull;
    constexpr uint64 RVX_TRANSLUCENCY_FNV_PRIME = 1099511628211ull;
    constexpr float64 RVX_WBOIT_MIN_WEIGHT = 0.01;
    constexpr float64 RVX_WBOIT_MAX_WEIGHT =
        RVX_WBOIT_REFERENCE_MAX_WEIGHT;

    void HashUnsigned(uint64& hash, uint64 value, uint32 byteCount) noexcept
    {
        for (uint32 byteIndex = 0; byteIndex < byteCount; ++byteIndex)
        {
            hash ^= static_cast<uint8>(value & 0xffu);
            hash *= RVX_TRANSLUCENCY_FNV_PRIME;
            value >>= 8u;
        }
    }

    template<typename T>
    void HashUnsignedValue(uint64& hash, T value) noexcept
    {
        static_assert(std::is_unsigned_v<T>);
        HashUnsigned(hash, static_cast<uint64>(value), sizeof(T));
    }

    template<typename T>
    void HashEnum(uint64& hash, T value) noexcept
    {
        using Underlying = std::underlying_type_t<T>;
        using Unsigned = std::make_unsigned_t<Underlying>;
        HashUnsignedValue(hash, static_cast<Unsigned>(value));
    }

    void HashBool(uint64& hash, bool value) noexcept
    {
        HashUnsigned(hash, value ? 1u : 0u, 1u);
    }

    void HashFrameIdentity(uint64& hash,
                           const RenderFrameIdentity& identity) noexcept
    {
        HashUnsignedValue(hash, identity.frameSequence);
        HashUnsignedValue(hash, identity.sceneRevision);
        HashUnsignedValue(hash, identity.surfaceGeneration);
        HashUnsignedValue(hash, identity.configurationRevision);
    }

    void HashViewIdentity(uint64& hash,
                          const RenderViewIdentity& identity) noexcept
    {
        HashUnsignedValue(hash, identity.sceneRuntimeId);
        HashUnsignedValue(hash, identity.cameraEntityPacked);
        HashUnsignedValue(hash, identity.subViewIndex);
        HashEnum(hash, identity.role);
    }

    void HashReadiness(
        uint64& hash,
        const WeightedBlendedOITCapabilityReadiness& readiness) noexcept
    {
        HashBool(hash, readiness.requested);
        HashBool(hash, readiness.compiled);
        HashBool(hash, readiness.hardwareSupported);
        HashBool(hash, readiness.independentBlendSupported);
        HashBool(hash, readiness.accumulationFormatBlendSupported);
        HashBool(hash, readiness.revealageFormatBlendSupported);
        HashBool(hash, readiness.implementationSupported);
        HashBool(hash, readiness.pipelineReady);
        HashBool(hash, readiness.inputsReady);
    }

    [[nodiscard]] constexpr bool IsKnownRoute(
        TranslucencyRoute route) noexcept
    {
        return route >= TranslucencyRoute::WeightedBlended &&
               route < TranslucencyRoute::Count;
    }

    [[nodiscard]] constexpr bool IsKnownSourceKind(
        TranslucencySourceKind kind) noexcept
    {
        return kind >= TranslucencySourceKind::Mesh &&
               kind < TranslucencySourceKind::Count;
    }

    [[nodiscard]] constexpr bool IsKnownRequirement(
        TranslucencyCapabilityRequirement requirement) noexcept
    {
        return requirement == TranslucencyCapabilityRequirement::Optional ||
               requirement == TranslucencyCapabilityRequirement::Required;
    }

    [[nodiscard]] constexpr bool IsKnownResolution(
        WeightedBlendedOITCapabilityResolution resolution) noexcept
    {
        return resolution >= WeightedBlendedOITCapabilityResolution::Disabled &&
               resolution <=
                   WeightedBlendedOITCapabilityResolution::RequiredUnavailable;
    }

    [[nodiscard]] constexpr bool IsKnownMeshShadingModel(
        MaterialShadingModel shadingModel) noexcept
    {
        return shadingModel >= MaterialShadingModel::StandardLit &&
               shadingModel <= MaterialShadingModel::ThinTransmission;
    }

    [[nodiscard]] constexpr bool IsKnownMaterialBlendMode(
        MaterialBlendMode blendMode) noexcept
    {
        return blendMode >= MaterialBlendMode::Opaque &&
               blendMode <= MaterialBlendMode::Additive;
    }

    [[nodiscard]] constexpr bool IsKnownSurfacePath(
        SurfacePathEligibility path) noexcept
    {
        return path >= SurfacePathEligibility::DeferredOpaque &&
               path <= SurfacePathEligibility::Unsupported;
    }

    [[nodiscard]] constexpr bool IsKnownParticleBlendMode(
        ParticleRenderSnapshotBlendMode blendMode) noexcept
    {
        return blendMode >= ParticleRenderSnapshotBlendMode::Additive &&
               blendMode <= ParticleRenderSnapshotBlendMode::Premultiplied;
    }

    [[nodiscard]] TranslucencyRoute ResolveNonRefractiveAlphaRoute(
        WeightedBlendedOITCapabilityResolution capability) noexcept
    {
        switch (capability)
        {
            case WeightedBlendedOITCapabilityResolution::Enabled:
                return TranslucencyRoute::WeightedBlended;
            case WeightedBlendedOITCapabilityResolution::Disabled:
            case WeightedBlendedOITCapabilityResolution::OptionalSortedFallback:
                // The existing back-to-front path is the only declared
                // optional fallback. It shares the sorted lane with glass,
                // while only ThinTransmission uses its refraction inputs.
                return TranslucencyRoute::SortedRefraction;
            case WeightedBlendedOITCapabilityResolution::RequiredUnavailable:
            default:
                return TranslucencyRoute::Unsupported;
        }
    }

    [[nodiscard]] bool CheckedAdd(uint32 left,
                                  uint32 right,
                                  uint32& outValue) noexcept
    {
        if (right > std::numeric_limits<uint32>::max() - left)
        {
            return false;
        }
        outValue = left + right;
        return true;
    }

    [[nodiscard]] bool AddSummaryEntry(TranslucencyRouteSummary& summary,
                                       TranslucencyRoute route,
                                       uint32 itemCount) noexcept
    {
        if (!IsKnownRoute(route) || itemCount == 0)
        {
            return false;
        }
        const size_t routeIndex = static_cast<size_t>(route);
        uint32 entryCount = 0;
        uint32 routedItemCount = 0;
        if (!CheckedAdd(summary.routeEntryCounts[routeIndex], 1u, entryCount) ||
            !CheckedAdd(
                summary.routeItemCounts[routeIndex], itemCount,
                routedItemCount))
        {
            return false;
        }
        summary.routeEntryCounts[routeIndex] = entryCount;
        summary.routeItemCounts[routeIndex] = routedItemCount;
        return true;
    }

    [[nodiscard]] uint64 ComputePlanIdentity(
        const TranslucencyFramePlan& plan) noexcept
    {
        uint64 hash = RVX_TRANSLUCENCY_FNV_OFFSET;
        HashUnsignedValue(hash, plan.GetSchemaVersion());
        HashFrameIdentity(hash, plan.GetFrameIdentity());
        HashViewIdentity(hash, plan.GetViewIdentity());
        HashUnsignedValue(hash, plan.GetProfileHash());
        HashUnsignedValue(hash, plan.GetMeshSourceIdentityHash());
        HashUnsignedValue(hash, plan.GetParticleSourceIdentityHash());
        HashEnum(hash, plan.GetWeightedBlendedRequirement());
        HashReadiness(hash, plan.GetWeightedBlendedReadiness());
        HashEnum(hash, plan.GetWeightedBlendedResolution());

        const TranslucencyRouteSummary& summary = plan.GetSummary();
        HashUnsignedValue(hash, summary.meshCandidateCount);
        HashUnsignedValue(hash, summary.particleBatchCandidateCount);
        HashUnsignedValue(hash, summary.particleItemCount);
        for (size_t routeIndex = 0;
             routeIndex < static_cast<size_t>(TranslucencyRoute::Count);
             ++routeIndex)
        {
            HashUnsignedValue(hash, summary.routeEntryCounts[routeIndex]);
            HashUnsignedValue(hash, summary.routeItemCounts[routeIndex]);
        }

        HashUnsignedValue(
            hash, static_cast<uint64>(plan.GetEntries().size()));
        for (const TranslucencyPlanEntry& entry : plan.GetEntries())
        {
            HashEnum(hash, entry.sourceKind);
            HashUnsignedValue(hash, entry.sourceOrdinal);
            HashUnsignedValue(hash, entry.sourceIdentityHash);
            HashUnsignedValue(hash, entry.firstItem);
            HashUnsignedValue(hash, entry.itemCount);
            HashEnum(hash, entry.route);
            HashEnum(hash, entry.meshShadingModel);
            HashEnum(hash, entry.meshBlendMode);
            HashEnum(hash, entry.meshSurfacePathEligibility);
            HashBool(hash, entry.meshMaterialPolicyValid);
            HashEnum(hash, entry.particleBlendMode);
        }
        return hash == 0 ? 1u : hash;
    }

    [[nodiscard]] TranslucencyFramePlanBuildResult Fail(
        TranslucencyFramePlanCode code,
        std::string diagnostic,
        TranslucencyFramePlan plan = {})
    {
        TranslucencyFramePlanBuildResult result;
        result.code = code;
        result.plan = std::move(plan);
        result.diagnostic = std::move(diagnostic);
        return result;
    }

    [[nodiscard]] bool IsFiniteColor(const Vec3& color) noexcept
    {
        return std::isfinite(color.x) && std::isfinite(color.y) &&
               std::isfinite(color.z);
    }

    [[nodiscard]] bool IsColorInReferenceRange(const Vec3& color) noexcept
    {
        return color.x >= 0.0f &&
               color.x <= RVX_WBOIT_REFERENCE_MAX_HDR_CHANNEL &&
               color.y >= 0.0f &&
               color.y <= RVX_WBOIT_REFERENCE_MAX_HDR_CHANNEL &&
               color.z >= 0.0f &&
               color.z <= RVX_WBOIT_REFERENCE_MAX_HDR_CHANNEL;
    }

    [[nodiscard]] bool IsValidPremultipliedColor(
        const WeightedBlendedOITReferenceSample& sample) noexcept
    {
        if (!sample.premultiplied)
        {
            return true;
        }
        const float32 maximumPremultipliedChannel =
            RVX_WBOIT_REFERENCE_MAX_HDR_CHANNEL * sample.alpha;
        return sample.color.x <= maximumPremultipliedChannel &&
               sample.color.y <= maximumPremultipliedChannel &&
               sample.color.z <= maximumPremultipliedChannel;
    }

    [[nodiscard]] float64 ComputeReferenceWeight(
        const WeightedBlendedOITReferenceSample& sample) noexcept
    {
        const float64 alphaTerm = 0.1 +
            static_cast<float64>(sample.alpha) * 1.5;
        const float64 depthTerm = 1.0 -
            static_cast<float64>(sample.normalizedDepth) * 0.9;
        const float64 alphaWeight =
            alphaTerm * alphaTerm * alphaTerm;
        const float64 depthWeight =
            depthTerm * depthTerm * depthTerm;
        return std::clamp(alphaWeight * depthWeight,
                          RVX_WBOIT_MIN_WEIGHT,
                          RVX_WBOIT_MAX_WEIGHT);
    }

    [[nodiscard]] auto MakeReferenceSortKey(
        const WeightedBlendedOITReferenceSample& sample) noexcept
    {
        return std::tuple{
            std::bit_cast<uint32>(sample.normalizedDepth),
            std::bit_cast<uint32>(sample.alpha),
            static_cast<uint8>(sample.premultiplied),
            std::bit_cast<uint32>(sample.color.x),
            std::bit_cast<uint32>(sample.color.y),
            std::bit_cast<uint32>(sample.color.z)};
    }
} // namespace

float32 CanonicalizeWeightedBlendedOITDepth(
    float32 deviceDepth,
    bool reverseZ) noexcept
{
    const float32 saturatedDepth = std::clamp(deviceDepth, 0.0f, 1.0f);
    return reverseZ ? 1.0f - saturatedDepth : saturatedDepth;
}

WeightedBlendedOITScissor ResolveWeightedBlendedOITScissor(
    RenderOutputExtent extent) noexcept
{
    return WeightedBlendedOITScissor{
        .policy = WeightedBlendedOITScissorPolicy::FullViewportFallback,
        .x = 0,
        .y = 0,
        .width = extent.width,
        .height = extent.height};
}

bool WeightedBlendedOITCapabilityReadiness::IsReady() const noexcept
{
    return requested && compiled && hardwareSupported &&
           independentBlendSupported && accumulationFormatBlendSupported &&
           revealageFormatBlendSupported && implementationSupported &&
           pipelineReady && inputsReady;
}

WeightedBlendedOITCapabilityResolution ResolveWeightedBlendedOITCapability(
    TranslucencyCapabilityRequirement requirement,
    const WeightedBlendedOITCapabilityReadiness& readiness) noexcept
{
    if (!IsKnownRequirement(requirement))
    {
        return WeightedBlendedOITCapabilityResolution::RequiredUnavailable;
    }
    if (!readiness.requested)
    {
        return requirement == TranslucencyCapabilityRequirement::Required
            ? WeightedBlendedOITCapabilityResolution::RequiredUnavailable
            : WeightedBlendedOITCapabilityResolution::Disabled;
    }
    if (readiness.IsReady())
    {
        return WeightedBlendedOITCapabilityResolution::Enabled;
    }
    return requirement == TranslucencyCapabilityRequirement::Required
        ? WeightedBlendedOITCapabilityResolution::RequiredUnavailable
        : WeightedBlendedOITCapabilityResolution::OptionalSortedFallback;
}

TranslucencyRoute ResolveTranslucencyRoute(
    const TranslucencyMeshCandidate& candidate,
    WeightedBlendedOITCapabilityResolution capability) noexcept
{
    if (!IsKnownMeshShadingModel(candidate.shadingModel) ||
        !IsKnownMaterialBlendMode(candidate.blendMode) ||
        !IsKnownSurfacePath(candidate.surfacePathEligibility) ||
        !IsKnownResolution(capability) || !candidate.materialPolicyValid ||
        candidate.surfacePathEligibility !=
            SurfacePathEligibility::ForwardTranslucency)
    {
        return TranslucencyRoute::Unsupported;
    }

    switch (candidate.shadingModel)
    {
        case MaterialShadingModel::StandardLit:
        case MaterialShadingModel::Unlit:
            if (candidate.blendMode == MaterialBlendMode::AlphaBlend ||
                candidate.blendMode == MaterialBlendMode::Premultiplied)
            {
                return ResolveNonRefractiveAlphaRoute(capability);
            }
            if (candidate.shadingModel == MaterialShadingModel::Unlit &&
                candidate.blendMode == MaterialBlendMode::Additive)
            {
                return TranslucencyRoute::DirectAdditive;
            }
            return TranslucencyRoute::Unsupported;

        case MaterialShadingModel::ThinTransmission:
            return candidate.blendMode == MaterialBlendMode::AlphaBlend ||
                           candidate.blendMode ==
                               MaterialBlendMode::Premultiplied
                       ? TranslucencyRoute::SortedRefraction
                       : TranslucencyRoute::Unsupported;

        case MaterialShadingModel::ToonLit:
        default:
            return TranslucencyRoute::Unsupported;
    }
}

TranslucencyRoute ResolveTranslucencyRoute(
    const TranslucencyParticleCandidate& candidate,
    WeightedBlendedOITCapabilityResolution capability) noexcept
{
    if (!IsKnownParticleBlendMode(candidate.blendMode) ||
        !IsKnownResolution(capability))
    {
        return TranslucencyRoute::Unsupported;
    }
    switch (candidate.blendMode)
    {
        case ParticleRenderSnapshotBlendMode::AlphaBlend:
        case ParticleRenderSnapshotBlendMode::Premultiplied:
            return ResolveNonRefractiveAlphaRoute(capability);
        case ParticleRenderSnapshotBlendMode::Additive:
            return TranslucencyRoute::DirectAdditive;
        case ParticleRenderSnapshotBlendMode::Multiply:
        default:
            return TranslucencyRoute::Unsupported;
    }
}

uint32 TranslucencyRouteSummary::GetEntryCount(
    TranslucencyRoute route) const noexcept
{
    return IsKnownRoute(route)
        ? routeEntryCounts[static_cast<size_t>(route)]
        : 0;
}

uint32 TranslucencyRouteSummary::GetItemCount(
    TranslucencyRoute route) const noexcept
{
    return IsKnownRoute(route)
        ? routeItemCounts[static_cast<size_t>(route)]
        : 0;
}

bool TranslucencyRouteSummary::IsExactlyOnce() const noexcept
{
    uint64 routedEntryCount = 0;
    uint64 routedItemCount = 0;
    for (size_t routeIndex = 0;
         routeIndex < static_cast<size_t>(TranslucencyRoute::Count);
         ++routeIndex)
    {
        routedEntryCount += routeEntryCounts[routeIndex];
        routedItemCount += routeItemCounts[routeIndex];
    }
    const uint64 expectedEntryCount =
        static_cast<uint64>(meshCandidateCount) +
        particleBatchCandidateCount;
    const uint64 expectedItemCount =
        static_cast<uint64>(meshCandidateCount) + particleItemCount;
    return routedEntryCount == expectedEntryCount &&
           routedItemCount == expectedItemCount;
}

const TranslucencyPlanEntry* TranslucencyFramePlan::FindEntry(
    TranslucencySourceKind kind,
    uint32 sourceOrdinal) const noexcept
{
    if (!IsKnownSourceKind(kind) || sourceOrdinal == RVX_INVALID_INDEX)
    {
        return nullptr;
    }
    const auto found = std::find_if(
        m_entries.begin(), m_entries.end(),
        [kind, sourceOrdinal](const TranslucencyPlanEntry& entry)
        {
            return entry.sourceKind == kind &&
                   entry.sourceOrdinal == sourceOrdinal;
        });
    return found == m_entries.end() ? nullptr : &*found;
}

bool TranslucencyFramePlan::HasRequiredCapabilityFailure() const noexcept
{
    return m_weightedBlendedRequirement ==
               TranslucencyCapabilityRequirement::Required &&
           m_weightedBlendedResolution ==
               WeightedBlendedOITCapabilityResolution::RequiredUnavailable;
}

bool TranslucencyFramePlan::HasMixedSourceSortedOrderingConflict() const noexcept
{
    bool hasSortedMesh = false;
    bool hasSortedParticle = false;
    for (const TranslucencyPlanEntry& entry : m_entries)
    {
        if (entry.route != TranslucencyRoute::SortedRefraction)
        {
            continue;
        }
        hasSortedMesh = hasSortedMesh ||
            entry.sourceKind == TranslucencySourceKind::Mesh;
        hasSortedParticle = hasSortedParticle ||
            entry.sourceKind == TranslucencySourceKind::Particle;
    }
    return hasSortedMesh && hasSortedParticle;
}

bool TranslucencyFramePlan::IsStructurallyValid() const noexcept
{
    if (m_schemaVersion != RVX_TRANSLUCENCY_FRAME_PLAN_SCHEMA_VERSION ||
        !m_frameIdentity.IsValid() || !m_viewIdentity.IsValid() ||
        m_profileHash == 0 || m_meshSourceIdentityHash == 0 ||
        m_particleSourceIdentityHash == 0 || m_identityHash == 0 ||
        !IsKnownRequirement(m_weightedBlendedRequirement) ||
        !IsKnownResolution(m_weightedBlendedResolution) ||
        m_weightedBlendedResolution != ResolveWeightedBlendedOITCapability(
            m_weightedBlendedRequirement, m_weightedBlendedReadiness) ||
        m_entries.size() !=
            static_cast<size_t>(m_summary.meshCandidateCount) +
                m_summary.particleBatchCandidateCount)
    {
        return false;
    }

    TranslucencyRouteSummary rebuilt;
    rebuilt.meshCandidateCount = m_summary.meshCandidateCount;
    rebuilt.particleBatchCandidateCount =
        m_summary.particleBatchCandidateCount;
    rebuilt.particleItemCount = m_summary.particleItemCount;
    std::set<uint64> meshIdentities;
    std::set<uint64> particleIdentities;
    uint32 expectedParticleFirstItem = 0;
    for (size_t entryIndex = 0; entryIndex < m_entries.size(); ++entryIndex)
    {
        const TranslucencyPlanEntry& entry = m_entries[entryIndex];
        if (!IsKnownSourceKind(entry.sourceKind) ||
            !IsKnownRoute(entry.route) || entry.sourceIdentityHash == 0)
        {
            return false;
        }

        TranslucencyRoute expectedRoute = TranslucencyRoute::Unsupported;
        if (entry.sourceKind == TranslucencySourceKind::Mesh)
        {
            if (entryIndex >= m_summary.meshCandidateCount ||
                entry.sourceOrdinal != entryIndex || entry.firstItem != 0 ||
                entry.itemCount != 1 ||
                entry.particleBlendMode !=
                    ParticleRenderSnapshotBlendMode::AlphaBlend ||
                !meshIdentities.insert(entry.sourceIdentityHash).second)
            {
                return false;
            }
            const TranslucencyMeshCandidate candidate{
                entry.sourceOrdinal,
                entry.sourceIdentityHash,
                entry.meshShadingModel,
                entry.meshBlendMode,
                entry.meshSurfacePathEligibility,
                entry.meshMaterialPolicyValid};
            expectedRoute = ResolveTranslucencyRoute(
                candidate, m_weightedBlendedResolution);
        }
        else
        {
            const size_t particleEntryIndex =
                entryIndex - m_summary.meshCandidateCount;
            if (entryIndex < m_summary.meshCandidateCount ||
                entry.sourceOrdinal != particleEntryIndex ||
                entry.firstItem != expectedParticleFirstItem ||
                entry.itemCount == 0 || entry.meshMaterialPolicyValid ||
                entry.meshShadingModel != MaterialShadingModel::StandardLit ||
                entry.meshBlendMode != MaterialBlendMode::AlphaBlend ||
                entry.meshSurfacePathEligibility !=
                    SurfacePathEligibility::ForwardTranslucency ||
                !particleIdentities.insert(entry.sourceIdentityHash).second ||
                !CheckedAdd(expectedParticleFirstItem, entry.itemCount,
                            expectedParticleFirstItem))
            {
                return false;
            }
            const TranslucencyParticleCandidate candidate{
                entry.sourceOrdinal,
                entry.sourceIdentityHash,
                entry.firstItem,
                entry.itemCount,
                entry.particleBlendMode};
            expectedRoute = ResolveTranslucencyRoute(
                candidate, m_weightedBlendedResolution);
        }
        if (entry.route != expectedRoute ||
            !AddSummaryEntry(rebuilt, entry.route, entry.itemCount))
        {
            return false;
        }
    }

    return expectedParticleFirstItem == m_summary.particleItemCount &&
           rebuilt == m_summary && m_summary.IsExactlyOnce() &&
           m_identityHash == ComputePlanIdentity(*this);
}

bool TranslucencyFramePlan::IsExecutable() const noexcept
{
    return IsStructurallyValid() && !HasRequiredCapabilityFailure() &&
           !HasMixedSourceSortedOrderingConflict() &&
           m_summary.GetItemCount(TranslucencyRoute::Unsupported) == 0;
}

TranslucencyFramePlanBuildResult TranslucencyFramePlanBuilder::Build(
    const TranslucencyFramePlanRequest& request) const
{
    if (request.schemaVersion != RVX_TRANSLUCENCY_FRAME_PLAN_SCHEMA_VERSION)
    {
        return Fail(TranslucencyFramePlanCode::InvalidSchema,
                    "Translucency frame-plan schema version is unsupported");
    }
    if (!request.frameIdentity.IsValid() || !request.viewIdentity.IsValid() ||
        request.profileHash == 0 || !IsKnownRequirement(
            request.weightedBlendedRequirement))
    {
        return Fail(TranslucencyFramePlanCode::InvalidIdentity,
                    "Translucency planning requires valid frame, view, "
                    "profile, and requirement identities");
    }
    if (request.meshSourceIdentityHash == 0 ||
        request.particleSourceIdentityHash == 0 ||
        request.meshCandidates.size() >
            std::numeric_limits<uint32>::max() ||
        request.particleCandidates.size() >
            std::numeric_limits<uint32>::max())
    {
        return Fail(TranslucencyFramePlanCode::InvalidSourceInventory,
                    "Translucency source inventories are missing a stable "
                    "identity or exceed uint32 limits");
    }

    TranslucencyFramePlan plan;
    plan.m_schemaVersion = request.schemaVersion;
    plan.m_frameIdentity = request.frameIdentity;
    plan.m_viewIdentity = request.viewIdentity;
    plan.m_profileHash = request.profileHash;
    plan.m_meshSourceIdentityHash = request.meshSourceIdentityHash;
    plan.m_particleSourceIdentityHash = request.particleSourceIdentityHash;
    plan.m_weightedBlendedRequirement =
        request.weightedBlendedRequirement;
    plan.m_weightedBlendedReadiness = request.weightedBlendedReadiness;
    plan.m_weightedBlendedResolution =
        ResolveWeightedBlendedOITCapability(
            request.weightedBlendedRequirement,
            request.weightedBlendedReadiness);
    plan.m_summary.meshCandidateCount =
        static_cast<uint32>(request.meshCandidates.size());
    plan.m_summary.particleBatchCandidateCount =
        static_cast<uint32>(request.particleCandidates.size());
    plan.m_summary.particleItemCount = request.particleItemCount;
    plan.m_entries.reserve(request.meshCandidates.size() +
                           request.particleCandidates.size());

    std::set<uint64> meshIdentities;
    for (size_t index = 0; index < request.meshCandidates.size(); ++index)
    {
        const TranslucencyMeshCandidate& candidate =
            request.meshCandidates[index];
        if (candidate.sourceOrdinal != index ||
            candidate.sourceIdentityHash == 0 ||
            !IsKnownMeshShadingModel(candidate.shadingModel) ||
            !IsKnownMaterialBlendMode(candidate.blendMode) ||
            !IsKnownSurfacePath(candidate.surfacePathEligibility))
        {
            return Fail(TranslucencyFramePlanCode::InvalidSourceInventory,
                        "Mesh translucency candidates must use dense "
                        "ordinals and known policy values");
        }
        if (!meshIdentities.insert(candidate.sourceIdentityHash).second)
        {
            return Fail(TranslucencyFramePlanCode::DuplicateSourceIdentity,
                        "Mesh translucency candidates contain a duplicate frozen source identity");
        }

        const TranslucencyRoute route = ResolveTranslucencyRoute(
            candidate, plan.m_weightedBlendedResolution);
        TranslucencyPlanEntry entry;
        entry.sourceKind = TranslucencySourceKind::Mesh;
        entry.sourceOrdinal = candidate.sourceOrdinal;
        entry.sourceIdentityHash = candidate.sourceIdentityHash;
        entry.itemCount = 1;
        entry.route = route;
        entry.meshShadingModel = candidate.shadingModel;
        entry.meshBlendMode = candidate.blendMode;
        entry.meshSurfacePathEligibility =
            candidate.surfacePathEligibility;
        entry.meshMaterialPolicyValid = candidate.materialPolicyValid;
        plan.m_entries.push_back(entry);
        if (!AddSummaryEntry(plan.m_summary, route, 1u))
        {
            return Fail(TranslucencyFramePlanCode::ArithmeticOverflow,
                        "Mesh translucency route accounting overflowed uint32");
        }
    }

    std::set<uint64> particleIdentities;
    uint32 expectedFirstItem = 0;
    for (size_t index = 0; index < request.particleCandidates.size(); ++index)
    {
        const TranslucencyParticleCandidate& candidate =
            request.particleCandidates[index];
        if (candidate.sourceOrdinal != index ||
            candidate.sourceIdentityHash == 0 || candidate.itemCount == 0 ||
            !IsKnownParticleBlendMode(candidate.blendMode))
        {
            return Fail(TranslucencyFramePlanCode::InvalidSourceInventory,
                        "Particle translucency candidates must use dense "
                        "ordinals, non-empty ranges, and known blend modes");
        }
        if (!particleIdentities.insert(candidate.sourceIdentityHash).second)
        {
            return Fail(TranslucencyFramePlanCode::DuplicateSourceIdentity,
                        "Particle translucency candidates contain a duplicate "
                        "frozen source identity");
        }
        uint32 nextFirstItem = 0;
        if (candidate.firstItem != expectedFirstItem ||
            !CheckedAdd(expectedFirstItem, candidate.itemCount, nextFirstItem))
        {
            return Fail(TranslucencyFramePlanCode::ParticleCoverageMismatch,
                        "Particle translucency ranges overlap, contain a gap, or overflow uint32");
        }
        expectedFirstItem = nextFirstItem;

        const TranslucencyRoute route = ResolveTranslucencyRoute(
            candidate, plan.m_weightedBlendedResolution);
        TranslucencyPlanEntry entry;
        entry.sourceKind = TranslucencySourceKind::Particle;
        entry.sourceOrdinal = candidate.sourceOrdinal;
        entry.sourceIdentityHash = candidate.sourceIdentityHash;
        entry.firstItem = candidate.firstItem;
        entry.itemCount = candidate.itemCount;
        entry.route = route;
        entry.particleBlendMode = candidate.blendMode;
        plan.m_entries.push_back(entry);
        if (!AddSummaryEntry(plan.m_summary, route, candidate.itemCount))
        {
            return Fail(TranslucencyFramePlanCode::ArithmeticOverflow,
                        "Particle translucency route accounting overflowed uint32");
        }
    }
    if (expectedFirstItem != request.particleItemCount)
    {
        return Fail(TranslucencyFramePlanCode::ParticleCoverageMismatch,
                    "Particle translucency ranges do not cover the declared "
                    "item count exactly once");
    }

    plan.m_identityHash = ComputePlanIdentity(plan);
    if (!plan.IsStructurallyValid())
    {
        return Fail(TranslucencyFramePlanCode::InvalidSourceInventory,
                    "Translucency plan failed its derived exactly-once contract");
    }
    if (request.expectedIdentityHash != 0 &&
        request.expectedIdentityHash != plan.m_identityHash)
    {
        return Fail(TranslucencyFramePlanCode::IdentityMismatch,
                    "Translucency plan no longer matches the expected identity",
                    std::move(plan));
    }
    if (plan.HasRequiredCapabilityFailure())
    {
        return Fail(
            TranslucencyFramePlanCode::RequiredCapabilityUnavailable,
            "Required weighted-blended OIT capability is unavailable",
            std::move(plan));
    }
    if (plan.HasMixedSourceSortedOrderingConflict())
    {
        return Fail(
            TranslucencyFramePlanCode::MixedSourceSortedOrderingUnsupported,
            "Mesh and particle sorted translucency require one global "
            "back-to-front submission order; the current direct recorders "
            "cannot preserve that order",
            std::move(plan));
    }
    if (plan.m_summary.GetItemCount(TranslucencyRoute::Unsupported) != 0)
    {
        return Fail(TranslucencyFramePlanCode::UnsupportedCandidate,
                    "One or more translucency candidates have no legal terminal route",
                    std::move(plan));
    }

    TranslucencyFramePlanBuildResult result;
    result.code = plan.m_weightedBlendedResolution ==
                          WeightedBlendedOITCapabilityResolution::
                              OptionalSortedFallback
        ? TranslucencyFramePlanCode::ReadyWithOptionalFallback
        : TranslucencyFramePlanCode::Ready;
    result.plan = std::move(plan);
    return result;
}

const char* GetTranslucencyRouteName(TranslucencyRoute route) noexcept
{
    switch (route)
    {
        case TranslucencyRoute::WeightedBlended: return "WeightedBlended";
        case TranslucencyRoute::SortedRefraction: return "SortedRefraction";
        case TranslucencyRoute::DirectAdditive: return "DirectAdditive";
        case TranslucencyRoute::Unsupported: return "Unsupported";
        case TranslucencyRoute::Count:
        default: return "Invalid";
    }
}

const char* GetTranslucencyFramePlanCodeName(
    TranslucencyFramePlanCode code) noexcept
{
    switch (code)
    {
        case TranslucencyFramePlanCode::Ready: return "Ready";
        case TranslucencyFramePlanCode::ReadyWithOptionalFallback:
            return "ReadyWithOptionalFallback";
        case TranslucencyFramePlanCode::InvalidSchema: return "InvalidSchema";
        case TranslucencyFramePlanCode::InvalidIdentity: return "InvalidIdentity";
        case TranslucencyFramePlanCode::InvalidSourceInventory:
            return "InvalidSourceInventory";
        case TranslucencyFramePlanCode::DuplicateSourceIdentity:
            return "DuplicateSourceIdentity";
        case TranslucencyFramePlanCode::ParticleCoverageMismatch:
            return "ParticleCoverageMismatch";
        case TranslucencyFramePlanCode::ArithmeticOverflow:
            return "ArithmeticOverflow";
        case TranslucencyFramePlanCode::RequiredCapabilityUnavailable:
            return "RequiredCapabilityUnavailable";
        case TranslucencyFramePlanCode::MixedSourceSortedOrderingUnsupported:
            return "MixedSourceSortedOrderingUnsupported";
        case TranslucencyFramePlanCode::UnsupportedCandidate:
            return "UnsupportedCandidate";
        case TranslucencyFramePlanCode::IdentityMismatch:
            return "IdentityMismatch";
        default: return "Invalid";
    }
}

WeightedBlendedOITReferenceResult EvaluateWeightedBlendedOITReference(
    std::span<const WeightedBlendedOITReferenceSample> samples)
{
    WeightedBlendedOITReferenceResult result;
    if (samples.size() > RVX_WBOIT_REFERENCE_MAX_SAMPLE_COUNT)
    {
        result.code =
            WeightedBlendedOITReferenceCode::SampleLimitExceeded;
        return result;
    }
    result.inputSampleCount = static_cast<uint32>(samples.size());

    std::vector<WeightedBlendedOITReferenceSample> ordered;
    ordered.reserve(samples.size());
    for (const WeightedBlendedOITReferenceSample& sample : samples)
    {
        if (!IsFiniteColor(sample.color) || !std::isfinite(sample.alpha) ||
            !std::isfinite(sample.normalizedDepth))
        {
            result.code = WeightedBlendedOITReferenceCode::NonFiniteInput;
            return result;
        }
        if (!IsColorInReferenceRange(sample.color))
        {
            result.code = WeightedBlendedOITReferenceCode::ColorOutOfRange;
            return result;
        }
        if (sample.alpha < 0.0f || sample.alpha > 1.0f)
        {
            result.code = WeightedBlendedOITReferenceCode::AlphaOutOfRange;
            return result;
        }
        if (sample.normalizedDepth < 0.0f ||
            sample.normalizedDepth > 1.0f)
        {
            result.code = WeightedBlendedOITReferenceCode::DepthOutOfRange;
            return result;
        }
        if (!IsValidPremultipliedColor(sample))
        {
            result.code =
                WeightedBlendedOITReferenceCode::InvalidPremultipliedColor;
            return result;
        }
        ordered.push_back(sample);
    }

    std::sort(
        ordered.begin(), ordered.end(),
        [](const WeightedBlendedOITReferenceSample& left,
           const WeightedBlendedOITReferenceSample& right)
        {
            return MakeReferenceSortKey(left) < MakeReferenceSortKey(right);
        });

    for (const WeightedBlendedOITReferenceSample& sample : ordered)
    {
        if (sample.alpha == 0.0f)
        {
            continue;
        }
        const float64 alpha = sample.alpha;
        const float64 weight = ComputeReferenceWeight(sample);
        const std::array<float64, 3> premultiplied = sample.premultiplied
            ? std::array<float64, 3>{
                  sample.color.x, sample.color.y, sample.color.z}
            : std::array<float64, 3>{
                  static_cast<float64>(sample.color.x) * alpha,
                  static_cast<float64>(sample.color.y) * alpha,
                  static_cast<float64>(sample.color.z) * alpha};
        for (size_t channel = 0; channel < 3; ++channel)
        {
            result.weightedPremultipliedColor[channel] +=
                premultiplied[channel] * weight;
        }
        result.weightedAlpha += alpha * weight;
        result.revealage *= 1.0 - alpha;
        ++result.contributingSampleCount;
        if (!std::isfinite(result.weightedPremultipliedColor[0]) ||
            !std::isfinite(result.weightedPremultipliedColor[1]) ||
            !std::isfinite(result.weightedPremultipliedColor[2]) ||
            !std::isfinite(result.weightedAlpha) ||
            !std::isfinite(result.revealage) ||
            result.weightedPremultipliedColor[0] >
                RVX_WBOIT_REFERENCE_MAX_ACCUMULATED_CHANNEL ||
            result.weightedPremultipliedColor[1] >
                RVX_WBOIT_REFERENCE_MAX_ACCUMULATED_CHANNEL ||
            result.weightedPremultipliedColor[2] >
                RVX_WBOIT_REFERENCE_MAX_ACCUMULATED_CHANNEL ||
            result.weightedAlpha >
                static_cast<float64>(
                    RVX_WBOIT_REFERENCE_MAX_SAMPLE_COUNT) *
                    RVX_WBOIT_REFERENCE_MAX_WEIGHT)
        {
            result.code =
                WeightedBlendedOITReferenceCode::ArithmeticOverflow;
            return result;
        }
    }

    result.revealage = std::clamp(result.revealage, 0.0, 1.0);
    if (result.contributingSampleCount == 0)
    {
        return result;
    }
    if (result.weightedAlpha <= 0.0)
    {
        result.code = WeightedBlendedOITReferenceCode::ArithmeticOverflow;
        return result;
    }

    const float64 resolvedAlpha = 1.0 - result.revealage;
    std::array<float64, 3> resolvedPremultiplied{};
    for (size_t channel = 0; channel < 3; ++channel)
    {
        const float64 straightColor =
            result.weightedPremultipliedColor[channel] /
            result.weightedAlpha;
        resolvedPremultiplied[channel] = std::clamp(
            straightColor * resolvedAlpha,
            0.0,
            static_cast<float64>(RVX_WBOIT_REFERENCE_MAX_HDR_CHANNEL));
    }
    result.resolvedPremultiplied = Vec4{
        static_cast<float32>(resolvedPremultiplied[0]),
        static_cast<float32>(resolvedPremultiplied[1]),
        static_cast<float32>(resolvedPremultiplied[2]),
        static_cast<float32>(resolvedAlpha)};
    if (!std::isfinite(result.resolvedPremultiplied.x) ||
        !std::isfinite(result.resolvedPremultiplied.y) ||
        !std::isfinite(result.resolvedPremultiplied.z) ||
        !std::isfinite(result.resolvedPremultiplied.w))
    {
        result.code = WeightedBlendedOITReferenceCode::ArithmeticOverflow;
    }
    return result;
}

const char* GetWeightedBlendedOITReferenceCodeName(
    WeightedBlendedOITReferenceCode code) noexcept
{
    switch (code)
    {
        case WeightedBlendedOITReferenceCode::Success: return "Success";
        case WeightedBlendedOITReferenceCode::SampleLimitExceeded:
            return "SampleLimitExceeded";
        case WeightedBlendedOITReferenceCode::NonFiniteInput:
            return "NonFiniteInput";
        case WeightedBlendedOITReferenceCode::ColorOutOfRange:
            return "ColorOutOfRange";
        case WeightedBlendedOITReferenceCode::AlphaOutOfRange:
            return "AlphaOutOfRange";
        case WeightedBlendedOITReferenceCode::DepthOutOfRange:
            return "DepthOutOfRange";
        case WeightedBlendedOITReferenceCode::InvalidPremultipliedColor:
            return "InvalidPremultipliedColor";
        case WeightedBlendedOITReferenceCode::ArithmeticOverflow:
            return "ArithmeticOverflow";
        default: return "Invalid";
    }
}
} // namespace RVX
