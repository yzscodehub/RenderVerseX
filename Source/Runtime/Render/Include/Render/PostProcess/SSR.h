#pragma once

/**
 * @file SSR.h
 * @brief RenderGraph-owned Hi-Z screen-space reflection primitive.
 */

#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "Render/Graph/RenderGraph.h"
#include "RenderContracts/RenderFrameTypes.h"

#include <array>
#include <memory>
#include <optional>
#include <string>

namespace RVX
{
    inline constexpr uint32 RVX_SSR_LOCAL_PROBE_CAPACITY = 8;
    /**
     * @brief ABI version for the qualification-only R8 fallback-radiance
     *        route-mask semantics.
     *
     * Version one guarantees that LocalProbe and GlobalIBL classifications
     * appear only after their respective HDR radiance contribution was
     * finite and nonzero in the resolve shader. It is intentionally distinct
     * from the RenderExecutionReceipt schema: this value describes bytes
     * read back from one physical route-mask image.
     */
    inline constexpr uint32
        RVX_SSR_QUALIFICATION_ROUTE_MASK_RADIANCE_SCHEMA_VERSION = 1;
    class PipelineCache;
    class IRHIDevice;
    class RenderSubmissionTracker;
    struct ViewData;
    struct GPUCompletionToken;
    struct SSRExecutionState;

    enum class SSRQuality : uint8
    {
        Low = 0,
        Medium,
        High,
        Ultra,
    };

    enum class SSRImplementationTier : uint8
    {
        Unsupported = 0,
        HiZRayMarch,
    };

    [[nodiscard]] const char* GetSSRImplementationTierName(
        SSRImplementationTier tier) noexcept;

    enum class ReflectionResolveRoute : uint8
    {
        Unresolved = 0,
        Background,
        NonReflective,
        ScreenSpace,
        LocalProbe,
        GlobalIBL,
    };

    [[nodiscard]] const char* GetReflectionResolveRouteName(
        ReflectionResolveRoute route) noexcept;

    enum class SSRProbeInfluenceShape : uint32
    {
        Box = 0,
        Sphere = 1,
    };

    struct SSRConfig
    {
        SSRQuality quality = SSRQuality::High;
        float32 maxDistance = 100.0f;
        float32 thickness = 0.1f;
        uint32 maxSteps = 64;
        uint32 binarySearchSteps = 6;
        float32 roughnessThreshold = 0.65f;
        float32 edgeFade = 0.08f;
        bool halfResolution = false;
        bool temporalFilter = false;
        bool stochastic = false;
    };

    /** Stable value identity of the implemented Hi-Z SSR configuration. */
    [[nodiscard]] uint64 ComputeSSRConfigurationIdentity(
        const SSRConfig& config) noexcept;

    /**
     * @brief Predict the exact depth-pyramid identity before graph recording.
     *
     * Composition uses this value while sealing ReflectionFramePlan; the SSR
     * primitive publishes the same value with its graph outputs so a stale
     * plan cannot be paired with a later frame's pyramid.
     */
    [[nodiscard]] uint64 ComputeSSRDepthPyramidIdentity(
        const RenderFrameIdentity& frameIdentity,
        const RenderViewIdentity& viewIdentity,
        const RenderOutputExtent& extent,
        uint64 configurationIdentity,
        uint64 pipelineIdentity) noexcept;

    /** Exact graph and reflection sources selected by composition. */
    struct SSRFrameInputs
    {
        RenderFrameIdentity frameIdentity{};
        RenderViewIdentity viewIdentity{};
        uint64 reflectionPlanIdentity = 0;
        uint64 lightingPlanIdentity = 0;
        uint64 depthPyramidIdentity = 0;
        bool screenSpaceRequested = true;
        RGTextureHandle sceneColor{};
        RGTextureHandle deviceDepth{};
        /** Deferred GBuffer1, otherwise absent and reconstructed from depth. */
        RGTextureHandle normalRoughness{};
        std::array<RGTextureHandle, RVX_SSR_LOCAL_PROBE_CAPACITY>
            probeTextures{};
        /** Exact pre-exposed global-IBL baseline already present in sceneColor. */
        RGTextureHandle baseIndirectSpecular{};
        /** Dimensionless BRDF/Fresnel/visibility response for routed radiance. */
        RGTextureHandle specularResponse{};
        /** Exact producer exposure state shared by sceneColor and the baseline. */
        RGBufferHandle exposureState{};
        uint64 exposureStateContentHash = 0;
        std::array<Vec3, RVX_SSR_LOCAL_PROBE_CAPACITY> probePositions{};
        std::array<Vec3, RVX_SSR_LOCAL_PROBE_CAPACITY> probeExtents{};
        std::array<Mat4, RVX_SSR_LOCAL_PROBE_CAPACITY>
            probeWorldTransforms{};
        std::array<Mat4, RVX_SSR_LOCAL_PROBE_CAPACITY>
            probeInverseWorldTransforms{};
        std::array<Vec3, RVX_SSR_LOCAL_PROBE_CAPACITY>
            probeBoxProjectionSizes{};
        std::array<Vec3, RVX_SSR_LOCAL_PROBE_CAPACITY>
            probeBoxProjectionOffsets{};
        std::array<float32, RVX_SSR_LOCAL_PROBE_CAPACITY>
            probeNormalizedBlendDistances{};
        std::array<SSRProbeInfluenceShape, RVX_SSR_LOCAL_PROBE_CAPACITY>
            probeShapes{};
        std::array<bool, RVX_SSR_LOCAL_PROBE_CAPACITY>
            probeUsesBoxProjection{};
        std::array<int32, RVX_SSR_LOCAL_PROBE_CAPACITY> probePriorities{};
        std::array<float32, RVX_SSR_LOCAL_PROBE_CAPACITY> probeWeights{};
        std::array<bool, RVX_SSR_LOCAL_PROBE_CAPACITY> probeReady{};
    };

    struct SSRComputeStats
    {
        bool requested = false;
        bool supported = false;
        bool scheduled = false;
        bool executed = false;
        bool colorAvailable = false;
        bool depthAvailable = false;
        bool normalAvailable = false;
        bool roughnessAvailable = false;
        bool temporalHistoryRequired = false;
        bool hiZBuilt = false;
        bool globalIBLReady = false;
        bool fallbackChainResolved = false;
        bool screenSpaceRequested = false;
        bool screenSpaceApplied = false;
        bool probesApplied = false;
        bool reflectionRouteEvidenceReady = false;
        uint32 readyProbeCount = 0;
        uint32 hiZMipCount = 0;
        uint32 hiZGraphPassCount = 0;
        uint32 traceGraphPassCount = 0;
        uint32 resolveGraphPassCount = 0;
        uint64 reflectionPlanIdentity = 0;
        uint64 lightingPlanIdentity = 0;
        uint64 depthPyramidIdentity = 0;
        uint64 reflectionSurfaceIdentity = 0;
        uint64 exposureStateContentHash = 0;
        uint64 reflectionRouteSampleCount = 0;
        uint64 backgroundPixelCount = 0;
        uint64 nonReflectivePixelCount = 0;
        uint64 screenSpaceHitPixelCount = 0;
        uint64 localProbePixelCount = 0;
        uint64 globalIBLPixelCount = 0;
        SSRImplementationTier implementationTier =
            SSRImplementationTier::Unsupported;
        ReflectionResolveRoute minimumFallbackRoute =
            ReflectionResolveRoute::Unresolved;
        std::string missingInputReason;
        std::string fallbackReason;
    };

    struct SSROutput
    {
        RGTextureHandle hiZPyramid{};
        RGTextureHandle rawReflection{};
        RGTextureHandle confidence{};
        RGTextureHandle resolvedSceneColor{};
        RGTextureHandle routeMask{};
        uint32 hiZMipCount = 0;
        uint64 reflectionPlanIdentity = 0;
        uint64 lightingPlanIdentity = 0;
        uint64 depthPyramidIdentity = 0;
        bool screenSpaceScheduled = false;
        bool probeResolveScheduled = false;

        [[nodiscard]] bool IsValid() const noexcept
        {
            const bool screenSpaceResources = !screenSpaceScheduled ||
                (hiZPyramid.IsValid() && hiZMipCount != 0 &&
                 depthPyramidIdentity != 0);
            return screenSpaceResources && rawReflection.IsValid() &&
                   confidence.IsValid() && resolvedSceneColor.IsValid() &&
                   routeMask.IsValid() &&
                   reflectionPlanIdentity != 0 && lightingPlanIdentity != 0 &&
                   (screenSpaceScheduled || probeResolveScheduled);
        }
    };

    /** Exact graph identity locked by one qualification-only route readback. */
    struct SSRQualificationProbeIdentity
    {
        RenderFrameIdentity frameIdentity{};
        RenderViewIdentity viewIdentity{};
        RenderOutputExtent extent{};
        uint64 reflectionPlanIdentity = 0;
        uint64 lightingPlanIdentity = 0;
        uint64 depthPyramidIdentity = 0;
        uint64 configurationIdentity = 0;
        uint64 reflectionSurfaceIdentity = 0;
        uint64 exposureStateContentHash = 0;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return frameIdentity.IsValid() && viewIdentity.IsValid() &&
                   extent.IsValid() && reflectionPlanIdentity != 0 &&
                    lightingPlanIdentity != 0 && depthPyramidIdentity != 0 &&
                    configurationIdentity != 0 &&
                    reflectionSurfaceIdentity != 0 &&
                    exposureStateContentHash != 0;
        }

        friend bool operator==(const SSRQualificationProbeIdentity&,
                               const SSRQualificationProbeIdentity&) = default;
    };

    /** CPU-readable classification of the physical R8 route-mask output. */
    struct SSRQualificationProbeStats
    {
        bool requested = false;
        bool traceDrawRecorded = false;
        bool resolveDrawRecorded = false;
        bool copyRecorded = false;
        bool submissionAccepted = false;
        bool presented = false;
        bool gpuCompleted = false;
        bool ready = false;
        bool resolutionAware = false;
        /** Version of the physical R8 route-mask qualification protocol. */
        uint32 routeMaskRadianceSchemaVersion = 0;
        /** True only when at least one LocalProbe-tagged pixel proved local HDR radiance. */
        bool localProbeRadianceQualified = false;
        /** True only when at least one GlobalIBL-tagged pixel proved IBL HDR radiance. */
        bool globalIBLRadianceQualified = false;
        SSRQualificationProbeIdentity identity{};
        uint64 sampleCount = 0;
        uint64 receiverSampleCount = 0;
        uint64 backgroundPixelCount = 0;
        uint64 nonReflectivePixelCount = 0;
        uint64 screenSpaceHitPixelCount = 0;
        uint64 localProbePixelCount = 0;
        uint64 globalIBLPixelCount = 0;
        std::string failure;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return requested && traceDrawRecorded && resolveDrawRecorded &&
                   copyRecorded && submissionAccepted && presented &&
                   gpuCompleted && ready && resolutionAware &&
                   routeMaskRadianceSchemaVersion ==
                       RVX_SSR_QUALIFICATION_ROUTE_MASK_RADIANCE_SCHEMA_VERSION &&
                   localProbeRadianceQualified ==
                       (localProbePixelCount != 0) &&
                   globalIBLRadianceQualified ==
                       (globalIBLPixelCount != 0) &&
                   identity.IsValid() && sampleCount != 0 &&
                   receiverSampleCount != 0 &&
                   receiverSampleCount == screenSpaceHitPixelCount +
                       localProbePixelCount + globalIBLPixelCount &&
                   backgroundPixelCount + nonReflectivePixelCount +
                           receiverSampleCount ==
                       sampleCount &&
                   screenSpaceHitPixelCount + localProbePixelCount +
                            globalIBLPixelCount ==
                        receiverSampleCount;
        }
    };

    /**
     * @brief Deterministic depth-pyramid, trace, and fallback resolver.
     *
     * The primitive owns no pass ordering. It only registers typed graph work
     * and never exposes its RHI resources to samples or gameplay code.
     */
    class SSR final
    {
    public:
        SSR() = default;
        ~SSR();

        SSR(const SSR&) = delete;
        SSR& operator=(const SSR&) = delete;

        void SetResources(PipelineCache* pipelineCache);
        void Shutdown() noexcept;

        void SetConfig(const SSRConfig& config) noexcept;
        [[nodiscard]] const SSRConfig& GetConfig() const noexcept
        {
            return m_config;
        }

        void SetEnabled(bool enabled) noexcept { m_enabled = enabled; }
        [[nodiscard]] bool IsRequestedEnabled() const noexcept
        {
            return m_enabled;
        }
        [[nodiscard]] bool IsSupported() const noexcept;
        [[nodiscard]] bool IsEnabled() const noexcept
        {
            return m_enabled && IsSupported();
        }
        [[nodiscard]] const std::string& GetUnsupportedReason() const noexcept
        {
            return m_unsupportedReason;
        }

        [[nodiscard]] SSROutput AddToGraph(
            RenderGraph& graph,
            const ViewData& view,
            const SSRFrameInputs& inputs);

        /** Accept callback evidence only after the exact graph execution passes. */
        void FinalizeGraphExecution(bool executionAccepted) noexcept;

        [[nodiscard]] const SSRComputeStats& GetLastComputeStats() const noexcept
        {
            return m_lastComputeStats;
        }

        /** Arm one physical route-mask observation for qualification only. */
        [[nodiscard]] bool ArmQualificationProbe() noexcept;
        [[nodiscard]] bool NotifyQualificationProbeSubmission(
            const SSRQualificationProbeIdentity& identity,
            const GPUCompletionToken& completion,
            const RenderSubmissionTracker& tracker) noexcept;
        [[nodiscard]] bool MarkQualificationProbePresented(
            const SSRQualificationProbeIdentity& identity) noexcept;
        [[nodiscard]] bool PollQualificationProbe(
            const RenderSubmissionTracker& tracker,
            SSRQualificationProbeStats& outStats) noexcept;
        [[nodiscard]] bool HasPendingQualificationProbe() const noexcept;
        void AbandonQualificationProbe() noexcept;
        [[nodiscard]] std::optional<SSRQualificationProbeIdentity>
        GetPendingQualificationProbeIdentity() const noexcept;

    private:
        [[nodiscard]] bool HasPipelineResources() const noexcept;
        [[nodiscard]] bool ValidateConfig() const noexcept;
        [[nodiscard]] bool EnsureQualificationReadback(
            uint32 width, uint32 height) noexcept;
        void ClearQualificationProbeState(bool retainReadback) noexcept;
        void RecordUnsupported(std::string diagnostic);

        PipelineCache* m_pipelineCache = nullptr;
        IRHIDevice* m_resourceDevice = nullptr;
        SSRConfig m_config{};
        bool m_enabled = true;
        mutable std::string m_unsupportedReason =
            "SSR renderer resources are unavailable";
        RHISamplerRef m_sampler;
        SSRComputeStats m_lastComputeStats{};
        std::shared_ptr<SSRExecutionState> m_pendingExecutionState;
        RHIBufferRef m_probeReadback;
        SSRQualificationProbeIdentity m_probeIdentity{};
        SSRQualificationProbeStats m_lastProbeStats{};
        uint64 m_probeGeneration = 0;
        uint64 m_probeCompletionValue = 0;
        uint32 m_probeWidth = 0;
        uint32 m_probeHeight = 0;
        uint32 m_probeRowPitch = 0;
        bool m_probeRequested = false;
        bool m_probeIdentityLocked = false;
        bool m_probeTraceDrawRecorded = false;
        bool m_probeResolveDrawRecorded = false;
        bool m_probeCopyRecorded = false;
        bool m_probeSubmissionAccepted = false;
        bool m_probePresented = false;
        bool m_probeAbandoned = false;
    };
} // namespace RVX
