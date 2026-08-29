#pragma once

/**
 * @file ReflectionFramePlan.h
 * @brief Immutable reflection-probe selection and fallback routing contracts.
 */

#include "Core/Types.h"
#include "RenderContracts/RenderFrameTypes.h"
#include "RenderContracts/RenderSceneUpdate.h"

#include <string>
#include <vector>

namespace RVX
{
    inline constexpr uint32 RVX_REFLECTION_FRAME_PLAN_SCHEMA_VERSION = 3;
    inline constexpr uint32 RVX_REFLECTION_PROBE_CAPACITY = 8;
    inline constexpr uint32 RVX_REFLECTION_INFLUENCE_SCORE_MAX = 1'000'000;

    /** @brief Fail-closed outcome for a ReflectionFramePlanBuilder request. */
    enum class ReflectionFramePlanCode : uint8
    {
        Ready = 0,
        InvalidSchema,
        InvalidFrameIdentity,
        InvalidViewIdentity,
        InvalidViewState,
        InvalidSceneProbeStateHash,
        InvalidConfigurationRevision,
        InvalidSSRIdentity,
        InvalidIBLIdentity,
        InvalidFallbackIdentity,
        InvalidProbeSnapshot,
        UnsupportedProbeMode,
        DuplicateProbeCandidate,
        IdentityMismatch,
    };

    /** @brief Source retained when screen-space and local-probe data miss. */
    enum class ReflectionIBLSource : uint8
    {
        Texture = 0,
        Analytic,
    };

    /** @brief The sole GFX19 per-pixel fallback policy. */
    enum class ReflectionFallbackPolicy : uint8
    {
        ScreenSpaceProbeIBL = 0,
    };

    /**
     * @brief Identity of the exact SSR inputs selected for this frame/view.
     *
     * A requested-but-unready path preserves its configuration identity while
     * publishing no stale pipeline, depth-pyramid, or history identities.
     */
    struct ReflectionSSRIdentity
    {
        bool requested = false;
        bool inputsReady = false;
        uint64 configurationIdentityHash = 0;
        uint64 pipelineIdentityHash = 0;
        uint64 depthPyramidIdentityHash = 0;
        /** Zero is valid on a first frame or after an explicit history reset. */
        uint64 historyIdentityHash = 0;
    };

    /** @brief Exact global IBL source used as the mandatory final fallback. */
    struct ReflectionIBLIdentity
    {
        ReflectionIBLSource source = ReflectionIBLSource::Texture;
        /** Identity of the immutable LightingFramePlan supplying global IBL. */
        uint64 lightingPlanIdentityHash = 0;
        /** Required only for a texture source. */
        uint64 resourceRevision = 0;
        /** Required only for a texture source. */
        uint32 prefilteredMipLevels = 0;
        /** Required only for an analytic source. */
        uint64 analyticFallbackIdentityHash = 0;
    };

    /** @brief Versioned identity of the declared SSR -> Probe -> IBL policy. */
    struct ReflectionFallbackIdentity
    {
        ReflectionFallbackPolicy policy =
            ReflectionFallbackPolicy::ScreenSpaceProbeIBL;
        uint64 configurationIdentityHash = 0;
    };

    /** @brief One validated, renderer-owned local reflection-probe selection. */
    struct ReflectionProbeSelection
    {
        RenderProbeSnapshot snapshot{};
        /** Stable [0, RVX_REFLECTION_INFLUENCE_SCORE_MAX] ranking value. */
        uint32 quantizedInfluenceScore = 0;
        /** Exact normalized value reconstructed from quantizedInfluenceScore. */
        float32 influenceWeight = 0.0f;
    };

    /** @brief Observable fixed-capacity and filtering outcome. */
    struct ReflectionFramePlanCapacityEvidence
    {
        uint32 probeCapacity = RVX_REFLECTION_PROBE_CAPACITY;
        uint32 candidateProbeCount = 0;
        /** Deliberately unavailable probe contents excluded before admission. */
        uint32 unavailableProbeCount = 0;
        uint32 eligibleProbeCount = 0;
        uint32 selectedProbeCount = 0;
        uint32 overflowProbeCount = 0;
        uint32 layerRejectedProbeCount = 0;
        uint32 frustumRejectedProbeCount = 0;
        bool overflowed = false;
    };

    /** @brief Complete value-only input to ReflectionFramePlanBuilder. */
    struct ReflectionFramePlanRequest
    {
        uint32 schemaVersion = RVX_REFLECTION_FRAME_PLAN_SCHEMA_VERSION;
        RenderFrameIdentity frameIdentity{};
        RenderViewIdentity viewIdentity{};
        uint64 sceneProbeStateHash = 0;
        uint64 reflectionConfigurationRevision = 0;
        Vec3 viewPosition{0.0f};
        Mat4 viewProjection = Mat4Identity();
        uint32 viewCullingMask = ~0U;
        ReflectionSSRIdentity ssr{};
        ReflectionIBLIdentity ibl{};
        ReflectionFallbackIdentity fallback{};
        std::vector<RenderProbeSnapshot> probeCandidates{};
        /** When non-zero, changed input must fail instead of publishing a plan. */
        uint64 expectedIdentityHash = 0;
    };

    class ReflectionFramePlanBuilder;

    /**
     * @brief Immutable per-frame/per-view reflection selection.
     *
     * The plan owns complete selected snapshots and exposes only const access.
     * Probe order is priority descending, quantized influence descending, then
     * stable probe ID ascending.
     */
    class ReflectionFramePlan final
    {
    public:
        ReflectionFramePlan() = default;

        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] const RenderFrameIdentity& GetFrameIdentity() const noexcept;
        [[nodiscard]] const RenderViewIdentity& GetViewIdentity() const noexcept;
        [[nodiscard]] uint64 GetSceneProbeStateHash() const noexcept;
        [[nodiscard]] uint64 GetReflectionConfigurationRevision() const noexcept;
        [[nodiscard]] const Vec3& GetViewPosition() const noexcept;
        [[nodiscard]] const Mat4& GetViewProjection() const noexcept;
        [[nodiscard]] uint32 GetViewCullingMask() const noexcept;
        [[nodiscard]] const ReflectionSSRIdentity& GetSSRIdentity() const noexcept;
        [[nodiscard]] const ReflectionIBLIdentity& GetIBLIdentity() const noexcept;
        [[nodiscard]] const ReflectionFallbackIdentity& GetFallbackIdentity() const noexcept;
        [[nodiscard]] const std::vector<ReflectionProbeSelection>&
        GetSelectedProbes() const noexcept;
        [[nodiscard]] const ReflectionFramePlanCapacityEvidence&
        GetCapacityEvidence() const noexcept;
        [[nodiscard]] uint64 GetIdentityHash() const noexcept;

    private:
        friend class ReflectionFramePlanBuilder;

        RenderFrameIdentity m_frameIdentity{};
        RenderViewIdentity m_viewIdentity{};
        uint64 m_sceneProbeStateHash = 0;
        uint64 m_reflectionConfigurationRevision = 0;
        Vec3 m_viewPosition{0.0f};
        Mat4 m_viewProjection = Mat4Identity();
        uint32 m_viewCullingMask = 0;
        ReflectionSSRIdentity m_ssr{};
        ReflectionIBLIdentity m_ibl{};
        ReflectionFallbackIdentity m_fallback{};
        std::vector<ReflectionProbeSelection> m_selectedProbes{};
        ReflectionFramePlanCapacityEvidence m_capacityEvidence{};
        uint64 m_identityHash = 0;
    };

    struct ReflectionFramePlanBuildResult
    {
        ReflectionFramePlanCode code = ReflectionFramePlanCode::InvalidSchema;
        ReflectionFramePlan plan{};
        ReflectionFramePlanCapacityEvidence capacityEvidence{};
        std::string diagnostic{};

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return code == ReflectionFramePlanCode::Ready;
        }
    };

    /** @brief Stateless builder that validates, selects, and seals a plan. */
    class ReflectionFramePlanBuilder final
    {
    public:
        [[nodiscard]] ReflectionFramePlanBuildResult Build(
            const ReflectionFramePlanRequest& request) const;
    };

    /**
     * @brief CPU reference for local-probe spatial influence.
     *
     * Returns zero for invalid geometry or samples outside the influence
     * volume. Box probes use a box boundary; Point and Sphere use an ellipsoid.
     */
    [[nodiscard]] float32 EvaluateReflectionProbeInfluence(
        const RenderProbeSnapshot& probe,
        const Vec3& samplePosition) noexcept;

    /** @brief Quantize normalized probe influence for deterministic ranking. */
    [[nodiscard]] uint32 QuantizeReflectionProbeInfluence(
        float32 influence) noexcept;

    /** @brief Exact source combination selected by the CPU route reference. */
    enum class ReflectionResolutionRoute : uint8
    {
        Invalid = 0,
        IBL,
        Probe,
        ProbeAndIBL,
        SSR,
        SSRAndIBL,
        SSRAndProbe,
        SSRProbeAndIBL,
    };

    /** @brief One pixel/reference point supplied to the route resolver. */
    struct ReflectionRouteSample
    {
        Vec3 ssrRadiance{0.0f};
        float32 ssrConfidence = 0.0f;
        bool ssrHitValid = false;
        Vec3 probeRadiance{0.0f};
        float32 probeConfidence = 0.0f;
        bool probeValid = false;
        Vec3 iblRadiance{0.0f};
        /** A valid plan always supplies texture or analytic IBL. */
        bool iblValid = false;
    };

    /** @brief CPU reference result and explicit contribution evidence. */
    struct ReflectionRouteResolution
    {
        ReflectionResolutionRoute route = ReflectionResolutionRoute::Invalid;
        Vec3 radiance{0.0f};
        float32 ssrWeight = 0.0f;
        float32 probeWeight = 0.0f;
        float32 iblWeight = 0.0f;
        const char* diagnostic = "";

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return route != ReflectionResolutionRoute::Invalid;
        }
    };

    /**
     * @brief Resolve SSR -> local probe -> global IBL without a black miss path.
     *
     * IBL always contributes the residual weight. A missing probe therefore
     * cannot replace or clear the global fallback.
     */
    [[nodiscard]] ReflectionRouteResolution ResolveReflectionRoute(
        const ReflectionRouteSample& sample) noexcept;

    /** Inputs share the exact lighting producer's pre-exposed HDR domain. */
    struct MaterializedReflectionRouteSample
    {
        Vec3 sceneColor{0.0f};
        Vec3 baseIndirectSpecular{0.0f};
        Vec3 specularResponse{0.0f};
        float32 producerPreExposure = 1.0f;
        bool surfaceInputsValid = false;
        Vec3 ssrSceneRadiance{0.0f};
        float32 ssrConfidence = 0.0f;
        bool ssrHitValid = false;
        Vec3 localProbeRadiance{0.0f};
        float32 localProbeWeight = 0.0f;
        bool localProbeValid = false;
    };

    struct MaterializedReflectionRouteResolution
    {
        ReflectionResolutionRoute route = ReflectionResolutionRoute::Invalid;
        Vec3 resolvedSceneColor{0.0f};
        Vec3 routedIndirectSpecular{0.0f};
        const char* diagnostic = "";

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return route != ReflectionResolutionRoute::Invalid;
        }
    };

    /** Energy-correct CPU reference for scene - base + routed reflection. */
    [[nodiscard]] MaterializedReflectionRouteResolution
    ResolveMaterializedReflectionRoute(
        const MaterializedReflectionRouteSample& sample) noexcept;

    [[nodiscard]] const char* GetReflectionFramePlanCodeName(
        ReflectionFramePlanCode code) noexcept;
} // namespace RVX
