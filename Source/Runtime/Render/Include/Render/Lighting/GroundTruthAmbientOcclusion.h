#pragma once

/**
 * @file GroundTruthAmbientOcclusion.h
 * @brief Renderer-owned, history-safe GTAO graph primitive.
 */

#include "Core/Types.h"
#include "Render/Graph/RenderGraph.h"
#include "Render/Renderer/ViewData.h"
#include "RenderContracts/RenderAmbientOcclusion.h"
#include "RHI/RHIQueueTopology.h"

#include <memory>
#include <string>

namespace RVX
{
    class IRHIDevice;
    class PipelineCache;
    class RenderSubmissionTracker;
    struct GPUCompletionToken;
    struct GroundTruthAmbientOcclusionExecutionState;

    /** Exact graph inputs consumed by one GTAO candidate. */
    struct GroundTruthAmbientOcclusionFrameInputs
    {
        RGTextureHandle deviceDepth;
        /** Optional Deferred GBuffer1 oct-normal source. */
        RGTextureHandle normalSource;
        /** Required current-NDC-minus-prior-NDC velocity field. */
        RGTextureHandle velocity;
        RenderViewHistoryIdentity historyIdentity{};
        RenderHistoryResetReason historyResetReasons =
            RenderHistoryResetReason::HistoryUnavailable;
        /** Identity of the exact depth/normal field selected by composition. */
        uint64 surfaceFieldHash = 0;
        /** True only when composition has offered a committed matching history. */
        bool historyAvailable = false;
    };

    /** Immutable metadata paired with the graph-owned R8 visibility result. */
    struct GroundTruthAmbientOcclusionMetadata
    {
        RHIFormat format = RHIFormat::Unknown;
        RenderAmbientOcclusionConsumerMask consumerMask = 0;
        uint64 settingsHash = 0;
        uint64 ambientOcclusionFieldHash = 0;
        RenderViewHistoryIdentity candidateIdentity{};
        bool normalSourceUsed = false;
        bool normalReconstructed = false;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return format == RHIFormat::R8_UNORM &&
                   IsValidRenderAmbientOcclusionConsumerMask(consumerMask) &&
                   settingsHash != 0 && ambientOcclusionFieldHash != 0 &&
                   candidateIdentity.IsValid();
        }
    };

    /** A graph-local output plus the exact metadata future lighting consumes. */
    struct GroundTruthAmbientOcclusionOutput
    {
        RGTextureHandle visibility;
        GroundTruthAmbientOcclusionMetadata metadata{};

        [[nodiscard]] bool IsValid() const noexcept
        {
            return visibility.IsValid() && metadata.IsValid();
        }
    };

    /** Receipt-oriented observability for the last GTAO candidate. */
    struct GroundTruthAmbientOcclusionStats
    {
        bool requested = false;
        bool pipelineReady = false;
        bool inputsReady = false;
        bool executed = false;
        bool depthAvailable = false;
        bool normalSourceAvailable = false;
        bool normalReconstructed = false;
        bool velocityAvailable = false;
        bool historyAvailableBefore = false;
        bool historySampled = false;
        bool historyCandidatePrepared = false;
        bool historyCommitted = false;
        bool historyAbandoned = false;
        bool qualificationProbeRequested = false;
        bool qualificationProbeRecorded = false;
        bool qualificationProbeReady = false;
        uint32 surfacePreparationGraphPassCount = 0;
        uint32 horizonSearchGraphPassCount = 0;
        uint32 bilateralDenoiseGraphPassCount = 0;
        uint32 temporalResolveGraphPassCount = 0;
        uint64 settingsHash = 0;
        uint64 surfaceFieldHash = 0;
        uint64 ambientOcclusionFieldHash = 0;
        RenderViewHistoryIdentity historyIdentity{};
        RenderHistoryResetReason historyResetReasons =
            RenderHistoryResetReason::HistoryUnavailable;
        std::string fallbackReason;
        std::string qualificationProbeFailure;
    };

    /** One-shot physical observation of the final GPU-generated R8 visibility. */
    struct GroundTruthAmbientOcclusionQualificationProbe
    {
        bool requested = false;
        bool recorded = false;
        bool ready = false;
        RenderViewHistoryIdentity historyIdentity{};
        uint64 settingsHash = 0;
        uint64 ambientOcclusionFieldHash = 0;
        uint64 sampleCount = 0;
        uint64 occludedCount = 0;
        float32 minimumVisibility = 1.0f;
        float32 meanVisibility = 1.0f;
        float32 maximumVisibility = 1.0f;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return requested && recorded && ready && historyIdentity.IsValid() &&
                   settingsHash != 0 && ambientOcclusionFieldHash != 0 &&
                   sampleCount != 0 && occludedCount <= sampleCount &&
                   minimumVisibility >= 0.0f && minimumVisibility <= meanVisibility &&
                   meanVisibility <= maximumVisibility && maximumVisibility <= 1.0f;
        }
    };

    /**
     * @brief Five-stage deterministic GTAO implementation.
     *
     * Surface preparation preserves exact device depth in an R32 field,
     * followed by metric world reconstruction, horizon integration,
     * horizontal/vertical bilateral filtering, and a temporal resolve into
     * R8 visibility. The
     * final temporal target is a persistent candidate and becomes reusable
     * only after the exact view was presented. Disabled settings instead
     * publish a graph-owned all-one R8 visibility field.
     */
    class GroundTruthAmbientOcclusion final
    {
    public:
        GroundTruthAmbientOcclusion() = default;
        ~GroundTruthAmbientOcclusion();

        GroundTruthAmbientOcclusion(const GroundTruthAmbientOcclusion&) = delete;
        GroundTruthAmbientOcclusion& operator=(const GroundTruthAmbientOcclusion&) = delete;

        void SetResources(PipelineCache* pipelineCache);
        void SetSettings(const RenderAmbientOcclusionSettings& settings);
        [[nodiscard]] const RenderAmbientOcclusionSettings& GetSettings() const noexcept
        {
            return m_settings;
        }

        [[nodiscard]] GroundTruthAmbientOcclusionOutput AddToGraph(
            RenderGraph& graph,
            const ViewData& view,
            const GroundTruthAmbientOcclusionFrameInputs& inputs);

        /**
         * @brief Accept or reject callback results for the exact recorded graph.
         *
         * A successful RenderGraph callback is only a provisional result. The
         * feature registry must also prove that every pass belongs to, and
         * executed for, the exact frozen graph before history or qualification
         * state can become externally visible.
         */
        void FinalizeGraphExecution(bool executionAccepted) noexcept;

        /**
         * @brief True only once exact submission accepted the pending candidate.
         *
         * Present alone cannot publish a candidate. The caller must first
         * report the exact pending identity through NotifyHistorySubmission().
         */
        [[nodiscard]] bool CanCommitHistoryAfterPresent(
            const RenderViewHistoryIdentity& identity) const noexcept;
        /** Record that the exact candidate graph reached the GPU queue. */
        [[nodiscard]] bool NotifyHistorySubmission(
            const RenderViewHistoryIdentity& identity) noexcept;
        [[nodiscard]] bool CommitHistoryAfterPresent(
            const RenderViewHistoryIdentity& identity) noexcept;
        void AbandonPendingHistory() noexcept;
        void ResetHistory() noexcept;
        void Shutdown() noexcept;

        /**
         * @brief Arm one exact-view GPU-to-readback visibility observation.
         *
         * No readback allocation, graph pass, GPU copy, or CPU map occurs
         * until an armed identity reaches a real enabled GTAO temporal output.
         */
        [[nodiscard]] bool ArmQualificationProbe(
            const RenderViewHistoryIdentity& identity) noexcept;
        /** Bind the recorded copy to the exact current Graphics submission. */
        [[nodiscard]] bool NotifyQualificationProbeSubmission(
            const RenderViewHistoryIdentity& identity,
            const GPUCompletionToken& completion,
            const RenderSubmissionTracker& tracker) noexcept;
        /** Map and summarize only after the submitted Graphics point completes. */
        [[nodiscard]] bool PollQualificationProbe(
            const RenderSubmissionTracker& tracker,
            GroundTruthAmbientOcclusionQualificationProbe& outProbe) noexcept;
        /** Discard an unsubmitted probe, or close a submitted one after its fence. */
        void AbandonQualificationProbe() noexcept;
        [[nodiscard]] bool HasPendingQualificationProbe() const noexcept;

        [[nodiscard]] bool HasValidHistory() const noexcept { return m_historyValid; }
        [[nodiscard]] const GroundTruthAmbientOcclusionStats& GetLastStats() const noexcept
        {
            return m_lastStats;
        }

    private:
        friend struct GroundTruthAmbientOcclusionTestAccess;

        /**
         * @brief Merge the per-view temporal reset latch into the immutable
         * history decision supplied by frame composition.
         *
         * The view latch is deliberately authoritative.  A caller can carry
         * an incomplete reset-reason list during a transition, but it must
         * never make GTAO sample a history that the view has explicitly
         * invalidated.
         */
        [[nodiscard]] static RenderHistoryResetReason
        ApplyViewTemporalReset(
            const ViewData& view,
            RenderHistoryResetReason resetReasons) noexcept;

        /**
         * @brief Return true only for a fully reusable temporal GTAO history.
         *
         * Kept as one decision point so observability cannot claim that a
         * history was sampled when the shader's temporal blend is disabled.
         */
        [[nodiscard]] static bool CanSampleHistory(
            const GroundTruthAmbientOcclusionFrameInputs& inputs,
            const ViewData& view,
            bool historyValid,
            uint64 committedHistorySettingsHash,
            uint64 settingsHash,
            RenderHistoryResetReason effectiveResetReasons) noexcept;

        bool HasPipelineResources() const noexcept;
        bool EnsureHistoryResources(uint32 width, uint32 height);
        bool EnsureQualificationReadback(uint32 width, uint32 height);
        void ClearQualificationProbeState(bool retainReadback) noexcept;
        void ResetRuntimeResources() noexcept;
        void RecordUnsupported(const char* reason) noexcept;

        PipelineCache* m_pipelineCache = nullptr;
        IRHIDevice* m_device = nullptr;
        RenderAmbientOcclusionSettings m_settings{};
        RHITextureRef m_historyVisibility[2]; // R8_UNORM visibility
        RHITextureRef m_historyDepth[2]; // R32_FLOAT previous device depth
        RHISamplerRef m_sampler;
        uint32 m_width = 0;
        uint32 m_height = 0;
        uint32 m_committedHistoryIndex = 0;
        uint32 m_pendingHistoryIndex = 0;
        uint64 m_committedHistorySettingsHash = 0;
        uint64 m_pendingHistorySettingsHash = 0;
        bool m_historyResourceStatesInitialized = false;
        bool m_historyValid = false;
        bool m_hasPendingHistory = false;
        /** Exact candidate identity reached the accepted GPU submission boundary. */
        bool m_pendingHistorySubmissionAccepted = false;
        RenderViewHistoryIdentity m_pendingHistoryIdentity{};
        RHIBufferRef m_probeReadback;
        RenderViewHistoryIdentity m_probeExpectedIdentity{};
        RenderViewHistoryIdentity m_probeIdentity{};
        uint64 m_probeSettingsHash = 0;
        uint64 m_probeAmbientOcclusionFieldHash = 0;
        uint64 m_probeCompletionValue = 0;
        uint32 m_probeWidth = 0;
        uint32 m_probeHeight = 0;
        uint32 m_probeRowPitch = 0;
        bool m_probeRequested = false;
        bool m_probeScheduled = false;
        bool m_probeRecorded = false;
        bool m_probeSubmissionAccepted = false;
        bool m_probeAbandoned = false;
        std::shared_ptr<GroundTruthAmbientOcclusionExecutionState>
            m_pendingExecutionState;
        GroundTruthAmbientOcclusionStats m_lastStats{};
    };
} // namespace RVX
