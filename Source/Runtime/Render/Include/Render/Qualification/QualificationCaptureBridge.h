#pragma once

/**
 * @file QualificationCaptureBridge.h
 * @brief RenderGraph-to-CPU bridge for complete GFX24 qualification bundles.
 *
 * The bridge owns the physical readback resources for one explicitly armed
 * bundle.  It never waits for the GPU: RenderGraph records source copies,
 * submission seals an exact graphics token, and the Render owner maps only
 * after that token has completed.
 */

#include "Render/Features/RenderGraphBlackboard.h"
#include "Render/Graph/RenderGraph.h"
#include "Render/Qualification/QualificationCaptureOwner.h"

#include <array>
#include <memory>

namespace RVX
{
    class IRHIDevice;
    class RenderSubmissionTracker;
    struct GPUCompletionToken;

    /** @brief One typed blackboard source retained by a qualification bundle. */
    struct QualificationCaptureGraphSource
    {
        QualificationCaptureAttachmentSemantic semantic =
            QualificationCaptureAttachmentSemantic::Count;
        RGTextureHandle texture{};
        SceneTextures::Metadata metadata{};
        RenderFeatureId producer{};
    };

    /** @brief Frozen same-frame graph inputs used to record all seven copies. */
    struct QualificationCaptureGraphInputs
    {
        RenderGraphBlackboardProvenance provenance{};
        RenderFrameIdentity frameIdentity{};
        RenderViewIdentity viewIdentity{};
        float32 nearPlane = 0.0f;
        float32 farPlane = 0.0f;
        /** Frozen source-row convention for the graph backend. */
        bool originBottomLeft = false;
        /** Frozen device-depth convention for the accepted perspective view. */
        bool reverseZ = false;
        /** Qualification captures deliberately reject non-perspective depth. */
        bool perspectiveProjection = false;
        std::array<QualificationCaptureGraphSource,
                   RVX_QUALIFICATION_CAPTURE_ATTACHMENT_COUNT> sources{};
    };

    /** @brief Narrow pure-conversion seam used by focused bridge tests. */
    class QualificationCaptureBridgeTestAccess final
    {
    public:
        [[nodiscard]] static float32 HalfToFloat(uint16 value) noexcept;

        [[nodiscard]] static bool ConvertPixel(
            QualificationCaptureAttachmentSemantic semantic,
            RHIFormat format,
            const uint8* source,
            float32 nearPlane,
            float32 farPlane,
            bool reverseZ,
            uint8* destination) noexcept;

        [[nodiscard]] static uint32 SourceRowForCanonicalTopLeft(
            uint32 canonicalTopLeftRow,
            uint32 height,
            bool originBottomLeft) noexcept;
    };

    /**
     * @brief Completion-owned adapter around the existing qualification owner.
     *
     * Normal frames are allocation-free. The bridge becomes active only after
     * an explicit owner-thread request and rejects the whole bundle on every
     * missing producer, unsupported physical format, copy, receipt, submission,
     * or mapping discrepancy.
     */
    class QualificationCaptureBridge final
    {
    public:
        QualificationCaptureBridge();
        ~QualificationCaptureBridge();

        [[nodiscard]] bool Arm() noexcept;

        /** Record real graph copies for the frozen typed-blackboard sources. */
        [[nodiscard]] bool Record(
            RenderGraph& graph,
            IRHIDevice& device,
            const QualificationCaptureGraphInputs& inputs);

        /** Confirm the actual post-execution receipt; it is never preconstructed. */
        [[nodiscard]] bool FinalizeExecutionReceipt(
            const RenderExecutionReceipt& receipt) noexcept;

        /** Bind the full receipt only after the exact frame successfully presents. */
        [[nodiscard]] bool FinalizePresentedReceipt(
            const RenderExecutionReceipt& receipt) noexcept;

        /** Transfer the exact already-submitted graphics completion token. */
        [[nodiscard]] bool NotifySubmission(
            const GPUCompletionToken& completion,
            const RenderSubmissionTracker& tracker) noexcept;

        /** Non-blocking completion poll and canonical CPU mapping. */
        [[nodiscard]] bool PollCompletion(
            const RenderSubmissionTracker& tracker);

        /** Fail an unsubmitted recording without allowing it to leak to a later frame. */
        void ReleaseUnsubmitted() noexcept;

        /** Fail a request that could not bind all required typed producers. */
        void RejectPending() noexcept;

        [[nodiscard]] bool HasPendingCompletion() const noexcept;
        [[nodiscard]] bool IsAwaitingGraphRecord() const noexcept;
        [[nodiscard]] const QualificationCaptureDiagnostics&
        GetDiagnostics() const noexcept;
        [[nodiscard]] std::shared_ptr<const QualificationCaptureBundle>
        GetCompletedBundle() const noexcept;

    private:
        struct PhysicalReadback;

        [[nodiscard]] bool Map(
            const QualificationCaptureBundleIdentity& identity,
            const QualificationCaptureAttachmentDescriptor& descriptor,
            QualificationCaptureMappedPayload& outPayload);
        [[nodiscard]] bool RejectRecording() noexcept;

        QualificationCaptureOwner m_owner{};
        std::array<std::unique_ptr<PhysicalReadback>,
                   RVX_QUALIFICATION_CAPTURE_ATTACHMENT_COUNT>
            m_readbacks{};
        QualificationCaptureBundleIdentity m_recordingIdentity{};
        std::unique_ptr<GPUCompletionToken> m_submissionToken{};
        uint64 m_nextBundleId = 0;
        float32 m_recordingNearPlane = 0.0f;
        float32 m_recordingFarPlane = 0.0f;
        bool m_recordingOriginBottomLeft = false;
        bool m_recordingReverseZ = false;
        bool m_armRequested = false;
        bool m_copyExecutionFailed = false;
    };
} // namespace RVX
