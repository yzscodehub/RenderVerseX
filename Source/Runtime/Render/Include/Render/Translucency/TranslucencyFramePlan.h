#pragma once

/**
 * @file TranslucencyFramePlan.h
 * @brief Immutable mesh/particle routing and bounded WBOIT reference policy.
 *
 * The plan contains no RHI objects and performs no graph registration.  It
 * freezes one exactly-once routing decision before any translucency producer
 * records work, so composition cannot silently move a draw between weighted,
 * sorted-refraction, direct-additive, and unsupported lanes.
 */

#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "RenderContracts/ParticleRenderSnapshot.h"
#include "RenderContracts/RenderFrameTypes.h"
#include "RenderContracts/RenderMaterial.h"

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace RVX
{
    inline constexpr uint32 RVX_TRANSLUCENCY_FRAME_PLAN_SCHEMA_VERSION = 1;
    /**
     * @brief Supported per-pixel overlap budget for the RGBA16F accumulator.
     *
     * The shader bounds every weighted color contribution to 64.0.  Keeping
     * the qualification/reference budget at 512 therefore leaves the largest
     * possible accumulated color at 32768, below finite float16 maximum.
     */
    inline constexpr uint32 RVX_WBOIT_REFERENCE_MAX_SAMPLE_COUNT = 512;
    inline constexpr float32 RVX_WBOIT_REFERENCE_MAX_HDR_CHANNEL = 16.0f;
    inline constexpr float32 RVX_WBOIT_REFERENCE_MAX_WEIGHT = 4.0f;
    inline constexpr float32 RVX_WBOIT_REFERENCE_MAX_ACCUMULATED_CHANNEL =
        RVX_WBOIT_REFERENCE_MAX_SAMPLE_COUNT *
        RVX_WBOIT_REFERENCE_MAX_HDR_CHANNEL *
        RVX_WBOIT_REFERENCE_MAX_WEIGHT;

    /** @brief Map device depth to the shared near=0, far=1 WBOIT convention. */
    [[nodiscard]] float32 CanonicalizeWeightedBlendedOITDepth(
        float32 deviceDepth,
        bool reverseZ) noexcept;

    enum class TranslucencyRoute : uint8
    {
        WeightedBlended = 0,
        SortedRefraction,
        DirectAdditive,
        Unsupported,
        Count,
    };

    enum class TranslucencySourceKind : uint8
    {
        Mesh = 0,
        Particle,
        Count,
    };

    enum class TranslucencyCapabilityRequirement : uint8
    {
        Optional = 0,
        Required,
    };

    /** @brief Independent facts needed by the weighted-blended pipeline. */
    struct WeightedBlendedOITCapabilityReadiness
    {
        bool requested = false;
        bool compiled = false;
        bool hardwareSupported = false;
        bool independentBlendSupported = false;
        bool accumulationFormatBlendSupported = false;
        bool revealageFormatBlendSupported = false;
        bool implementationSupported = false;
        bool pipelineReady = false;
        bool inputsReady = false;

        [[nodiscard]] bool IsReady() const noexcept;

        friend bool operator==(
            const WeightedBlendedOITCapabilityReadiness&,
            const WeightedBlendedOITCapabilityReadiness&) = default;
    };

    enum class WeightedBlendedOITCapabilityResolution : uint8
    {
        Disabled = 0,
        Enabled,
        OptionalSortedFallback,
        RequiredUnavailable,
    };

    [[nodiscard]] WeightedBlendedOITCapabilityResolution
    ResolveWeightedBlendedOITCapability(
        TranslucencyCapabilityRequirement requirement,
        const WeightedBlendedOITCapabilityReadiness& readiness) noexcept;

    /**
     * @brief One transparent mesh draw from the already-frozen sorted list.
     *
     * `sourceIdentityHash` must bind the complete generation-qualified packet
     * identity.  `sourceOrdinal` is dense in the caller-owned transparent list.
     */
    struct TranslucencyMeshCandidate
    {
        uint32 sourceOrdinal = RVX_INVALID_INDEX;
        uint64 sourceIdentityHash = 0;
        MaterialShadingModel shadingModel =
            MaterialShadingModel::StandardLit;
        MaterialBlendMode blendMode = MaterialBlendMode::AlphaBlend;
        SurfacePathEligibility surfacePathEligibility =
            SurfacePathEligibility::ForwardTranslucency;
        bool materialPolicyValid = false;

        friend bool operator==(const TranslucencyMeshCandidate&,
                               const TranslucencyMeshCandidate&) = default;
    };

    /**
     * @brief One contiguous batch in a frozen ParticleBillboardDrawPlan.
     *
     * All ranges together must cover `[0, particleItemCount)` exactly once.
     */
    struct TranslucencyParticleCandidate
    {
        uint32 sourceOrdinal = RVX_INVALID_INDEX;
        uint64 sourceIdentityHash = 0;
        uint32 firstItem = 0;
        uint32 itemCount = 0;
        ParticleRenderSnapshotBlendMode blendMode =
            ParticleRenderSnapshotBlendMode::AlphaBlend;

        friend bool operator==(const TranslucencyParticleCandidate&,
                               const TranslucencyParticleCandidate&) = default;
    };

    [[nodiscard]] TranslucencyRoute ResolveTranslucencyRoute(
        const TranslucencyMeshCandidate& candidate,
        WeightedBlendedOITCapabilityResolution capability) noexcept;
    [[nodiscard]] TranslucencyRoute ResolveTranslucencyRoute(
        const TranslucencyParticleCandidate& candidate,
        WeightedBlendedOITCapabilityResolution capability) noexcept;

    /** @brief Frozen terminal route for one mesh draw or particle batch. */
    struct TranslucencyPlanEntry
    {
        TranslucencySourceKind sourceKind = TranslucencySourceKind::Mesh;
        uint32 sourceOrdinal = RVX_INVALID_INDEX;
        uint64 sourceIdentityHash = 0;
        uint32 firstItem = 0;
        uint32 itemCount = 0;
        TranslucencyRoute route = TranslucencyRoute::Unsupported;

        // Mesh policy facts. Particle entries retain their canonical defaults.
        MaterialShadingModel meshShadingModel =
            MaterialShadingModel::StandardLit;
        MaterialBlendMode meshBlendMode = MaterialBlendMode::AlphaBlend;
        SurfacePathEligibility meshSurfacePathEligibility =
            SurfacePathEligibility::ForwardTranslucency;
        bool meshMaterialPolicyValid = false;

        // Particle policy fact. Mesh entries retain the canonical default.
        ParticleRenderSnapshotBlendMode particleBlendMode =
            ParticleRenderSnapshotBlendMode::AlphaBlend;

        friend bool operator==(const TranslucencyPlanEntry&,
                               const TranslucencyPlanEntry&) = default;
    };

    /** @brief Exactly-once accounting for all terminal routes. */
    struct TranslucencyRouteSummary
    {
        uint32 meshCandidateCount = 0;
        uint32 particleBatchCandidateCount = 0;
        uint32 particleItemCount = 0;
        std::array<uint32, static_cast<size_t>(TranslucencyRoute::Count)>
            routeEntryCounts{};
        std::array<uint32, static_cast<size_t>(TranslucencyRoute::Count)>
            routeItemCounts{};

        [[nodiscard]] uint32 GetEntryCount(
            TranslucencyRoute route) const noexcept;
        [[nodiscard]] uint32 GetItemCount(
            TranslucencyRoute route) const noexcept;
        [[nodiscard]] bool IsExactlyOnce() const noexcept;

        friend bool operator==(const TranslucencyRouteSummary&,
                               const TranslucencyRouteSummary&) = default;
    };

    struct TranslucencyFramePlanRequest
    {
        uint32 schemaVersion = RVX_TRANSLUCENCY_FRAME_PLAN_SCHEMA_VERSION;
        RenderFrameIdentity frameIdentity{};
        RenderViewIdentity viewIdentity{};
        uint64 profileHash = 0;
        /** Hash of the complete frozen transparent mesh list, including order. */
        uint64 meshSourceIdentityHash = 0;
        /** Hash of the complete frozen particle draw plan, including ranges. */
        uint64 particleSourceIdentityHash = 0;
        uint32 particleItemCount = 0;
        TranslucencyCapabilityRequirement weightedBlendedRequirement =
            TranslucencyCapabilityRequirement::Optional;
        WeightedBlendedOITCapabilityReadiness weightedBlendedReadiness{};
        std::vector<TranslucencyMeshCandidate> meshCandidates;
        std::vector<TranslucencyParticleCandidate> particleCandidates;
        /** Optional stale-plan guard. Zero accepts the newly-derived identity. */
        uint64 expectedIdentityHash = 0;
    };

    enum class TranslucencyFramePlanCode : uint8
    {
        Ready = 0,
        ReadyWithOptionalFallback,
        InvalidSchema,
        InvalidIdentity,
        InvalidSourceInventory,
        DuplicateSourceIdentity,
        ParticleCoverageMismatch,
        ArithmeticOverflow,
        RequiredCapabilityUnavailable,
        MixedSourceSortedOrderingUnsupported,
        UnsupportedCandidate,
        IdentityMismatch,
    };

    class TranslucencyFramePlan final
    {
    public:
        [[nodiscard]] uint32 GetSchemaVersion() const noexcept
        {
            return m_schemaVersion;
        }

        [[nodiscard]] const RenderFrameIdentity& GetFrameIdentity() const noexcept
        {
            return m_frameIdentity;
        }

        [[nodiscard]] const RenderViewIdentity& GetViewIdentity() const noexcept
        {
            return m_viewIdentity;
        }

        [[nodiscard]] uint64 GetProfileHash() const noexcept
        {
            return m_profileHash;
        }

        [[nodiscard]] uint64 GetMeshSourceIdentityHash() const noexcept
        {
            return m_meshSourceIdentityHash;
        }

        [[nodiscard]] uint64 GetParticleSourceIdentityHash() const noexcept
        {
            return m_particleSourceIdentityHash;
        }

        [[nodiscard]] TranslucencyCapabilityRequirement
        GetWeightedBlendedRequirement() const noexcept
        {
            return m_weightedBlendedRequirement;
        }

        [[nodiscard]] const WeightedBlendedOITCapabilityReadiness&
        GetWeightedBlendedReadiness() const noexcept
        {
            return m_weightedBlendedReadiness;
        }

        [[nodiscard]] WeightedBlendedOITCapabilityResolution
        GetWeightedBlendedResolution() const noexcept
        {
            return m_weightedBlendedResolution;
        }

        [[nodiscard]] const std::vector<TranslucencyPlanEntry>& GetEntries()
            const noexcept
        {
            return m_entries;
        }

        [[nodiscard]] const TranslucencyRouteSummary& GetSummary() const noexcept
        {
            return m_summary;
        }

        [[nodiscard]] uint64 GetIdentityHash() const noexcept
        {
            return m_identityHash;
        }

        [[nodiscard]] const TranslucencyPlanEntry* FindEntry(
            TranslucencySourceKind kind,
            uint32 sourceOrdinal) const noexcept;
        [[nodiscard]] bool HasRequiredCapabilityFailure() const noexcept;
        /**
         * @brief True when mesh and particle alpha work would need one global
         * back-to-front order that the current direct recorders cannot express.
         */
        [[nodiscard]] bool HasMixedSourceSortedOrderingConflict() const noexcept;
        [[nodiscard]] bool IsStructurallyValid() const noexcept;
        [[nodiscard]] bool IsExecutable() const noexcept;

    private:
        friend class TranslucencyFramePlanBuilder;

        uint32 m_schemaVersion = 0;
        RenderFrameIdentity m_frameIdentity{};
        RenderViewIdentity m_viewIdentity{};
        uint64 m_profileHash = 0;
        uint64 m_meshSourceIdentityHash = 0;
        uint64 m_particleSourceIdentityHash = 0;
        TranslucencyCapabilityRequirement m_weightedBlendedRequirement =
            TranslucencyCapabilityRequirement::Optional;
        WeightedBlendedOITCapabilityReadiness m_weightedBlendedReadiness{};
        WeightedBlendedOITCapabilityResolution m_weightedBlendedResolution =
            WeightedBlendedOITCapabilityResolution::Disabled;
        std::vector<TranslucencyPlanEntry> m_entries;
        TranslucencyRouteSummary m_summary{};
        uint64 m_identityHash = 0;
    };

    struct TranslucencyFramePlanBuildResult
    {
        TranslucencyFramePlanCode code =
            TranslucencyFramePlanCode::InvalidIdentity;
        TranslucencyFramePlan plan{};
        std::string diagnostic{};

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return (code == TranslucencyFramePlanCode::Ready ||
                    code ==
                        TranslucencyFramePlanCode::ReadyWithOptionalFallback) &&
                   plan.IsExecutable();
        }
    };

    class TranslucencyFramePlanBuilder final
    {
    public:
        [[nodiscard]] TranslucencyFramePlanBuildResult Build(
            const TranslucencyFramePlanRequest& request) const;
    };

    [[nodiscard]] const char* GetTranslucencyRouteName(
        TranslucencyRoute route) noexcept;
    [[nodiscard]] const char* GetTranslucencyFramePlanCodeName(
        TranslucencyFramePlanCode code) noexcept;

    /** @brief One finite, bounded fragment for the canonical WBOIT reference. */
    struct WeightedBlendedOITReferenceSample
    {
        Vec3 color{0.0f, 0.0f, 0.0f};
        float32 alpha = 0.0f;
        /** Normalized device-independent front-to-back depth in [0, 1]. */
        float32 normalizedDepth = 0.0f;
        /** True when `color` already contains alpha multiplication. */
        bool premultiplied = false;

        friend bool operator==(const WeightedBlendedOITReferenceSample&,
                               const WeightedBlendedOITReferenceSample&) =
            default;
    };

    enum class WeightedBlendedOITReferenceCode : uint8
    {
        Success = 0,
        SampleLimitExceeded,
        NonFiniteInput,
        ColorOutOfRange,
        AlphaOutOfRange,
        DepthOutOfRange,
        InvalidPremultipliedColor,
        ArithmeticOverflow,
    };

    struct WeightedBlendedOITReferenceResult
    {
        WeightedBlendedOITReferenceCode code =
            WeightedBlendedOITReferenceCode::Success;
        uint32 inputSampleCount = 0;
        uint32 contributingSampleCount = 0;
        std::array<float64, 3> weightedPremultipliedColor{};
        float64 weightedAlpha = 0.0;
        float64 revealage = 1.0;
        /** Canonical layer value consumed by a premultiplied composite. */
        Vec4 resolvedPremultiplied{0.0f, 0.0f, 0.0f, 0.0f};

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return code == WeightedBlendedOITReferenceCode::Success;
        }

        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return Succeeded() && contributingSampleCount == 0;
        }
    };

    /**
     * @brief Evaluate the bounded, canonical weighted-blended reference.
     *
     * Inputs are sorted by their complete numeric value before double-
     * precision accumulation.  Consequently, permutations of the same
     * multiset produce bit-identical reference results on one implementation.
     */
    [[nodiscard]] WeightedBlendedOITReferenceResult
    EvaluateWeightedBlendedOITReference(
        std::span<const WeightedBlendedOITReferenceSample> samples);

    [[nodiscard]] const char* GetWeightedBlendedOITReferenceCodeName(
        WeightedBlendedOITReferenceCode code) noexcept;
} // namespace RVX
