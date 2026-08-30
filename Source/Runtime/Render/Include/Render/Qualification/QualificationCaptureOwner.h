#pragma once

/**
 * @file QualificationCaptureOwner.h
 * @brief Completion-owned assembly of GFX24 qualification capture bundles.
 */

#include "RenderContracts/QualificationCapture.h"

#include <memory>
#include <span>

namespace RVX
{
    /** @brief Opaque submission token supplied by the Render submission bridge. */
    struct QualificationCaptureCompletionToken
    {
        uint64 submissionIdentity = 0;
        uint64 completionValue = 0;

        [[nodiscard]] constexpr bool IsValid() const noexcept
        {
            return submissionIdentity != 0 && completionValue != 0;
        }

        friend constexpr bool operator==(
            const QualificationCaptureCompletionToken&,
            const QualificationCaptureCompletionToken&) = default;
    };

    enum class QualificationCaptureCompletionStatus : uint8
    {
        Pending = 0,
        Completed,
        Lost,
        Rejected,
    };

    /**
     * @brief Submission bridge deliberately separated from CPU mapping.
     *
     * Implementations bind token validity to the exact graphics submission
     * which owns the graph copies.  Polling must not wait, submit work, or
     * advance a frame.
     */
    class IQualificationCaptureCompletionTracker
    {
    public:
        virtual ~IQualificationCaptureCompletionTracker() = default;

        [[nodiscard]] virtual bool IsExactCurrentSubmission(
            const QualificationCaptureCompletionToken& token) const noexcept = 0;
        [[nodiscard]] virtual QualificationCaptureCompletionStatus Query(
            const QualificationCaptureCompletionToken& token) const noexcept = 0;
    };

    /** @brief The only policy-free boundary permitted to map GPU-owned bytes. */
    class IQualificationCaptureReadbackMapper
    {
    public:
        virtual ~IQualificationCaptureReadbackMapper() = default;

        /**
         * @brief Map the exact already-completed source for descriptor.
         *
         * The owner never invokes this while completion is Pending.  The
         * result must repeat identity, semantic, provenance, and a claimed
         * SHA-256 digest; all are verified before publication.
         */
        [[nodiscard]] virtual bool Map(
            const QualificationCaptureBundleIdentity& identity,
            const QualificationCaptureAttachmentDescriptor& descriptor,
            QualificationCaptureMappedPayload& outPayload) = 0;
    };

    enum class QualificationCaptureState : uint8
    {
        Idle = 0,
        Armed,
        Recorded,
        PendingCompletion,
        Completed,
        Rejected,
    };

    enum class QualificationCaptureFailure : uint8
    {
        None = 0,
        NotArmed,
        AlreadyActive,
        InvalidIdentity,
        IdentityMismatch,
        AttachmentSetInvalid,
        AttachmentMetadataInvalid,
        DuplicateAttachment,
        MissingAttachment,
        CopyNotRecorded,
        ExecutionReceiptNotFinalized,
        ExecutionReceiptAlreadyFinalized,
        SubmissionRejected,
        CompletionPending,
        CompletionLost,
        MapperFailed,
        PayloadMissing,
        PayloadSizeMismatch,
        PayloadHashMismatch,
        PayloadNonFinite,
        PayloadIdentityMismatch,
        PayloadProvenanceMismatch,
    };

    [[nodiscard]] inline const char* GetQualificationCaptureFailureName(
        QualificationCaptureFailure failure) noexcept
    {
        switch (failure)
        {
            case QualificationCaptureFailure::None: return "None";
            case QualificationCaptureFailure::NotArmed: return "NotArmed";
            case QualificationCaptureFailure::AlreadyActive: return "AlreadyActive";
            case QualificationCaptureFailure::InvalidIdentity: return "InvalidIdentity";
            case QualificationCaptureFailure::IdentityMismatch: return "IdentityMismatch";
            case QualificationCaptureFailure::AttachmentSetInvalid:
                return "AttachmentSetInvalid";
            case QualificationCaptureFailure::AttachmentMetadataInvalid:
                return "AttachmentMetadataInvalid";
            case QualificationCaptureFailure::DuplicateAttachment:
                return "DuplicateAttachment";
            case QualificationCaptureFailure::MissingAttachment: return "MissingAttachment";
            case QualificationCaptureFailure::CopyNotRecorded: return "CopyNotRecorded";
            case QualificationCaptureFailure::ExecutionReceiptNotFinalized:
                return "ExecutionReceiptNotFinalized";
            case QualificationCaptureFailure::ExecutionReceiptAlreadyFinalized:
                return "ExecutionReceiptAlreadyFinalized";
            case QualificationCaptureFailure::SubmissionRejected:
                return "SubmissionRejected";
            case QualificationCaptureFailure::CompletionPending:
                return "CompletionPending";
            case QualificationCaptureFailure::CompletionLost: return "CompletionLost";
            case QualificationCaptureFailure::MapperFailed: return "MapperFailed";
            case QualificationCaptureFailure::PayloadMissing: return "PayloadMissing";
            case QualificationCaptureFailure::PayloadSizeMismatch:
                return "PayloadSizeMismatch";
            case QualificationCaptureFailure::PayloadHashMismatch:
                return "PayloadHashMismatch";
            case QualificationCaptureFailure::PayloadNonFinite:
                return "PayloadNonFinite";
            case QualificationCaptureFailure::PayloadIdentityMismatch:
                return "PayloadIdentityMismatch";
            case QualificationCaptureFailure::PayloadProvenanceMismatch:
                return "PayloadProvenanceMismatch";
        }
        return "Unknown";
    }

    /** @brief Observable fail-closed state for test reports and qualification receipts. */
    struct QualificationCaptureDiagnostics
    {
        QualificationCaptureState state = QualificationCaptureState::Idle;
        QualificationCaptureFailure failure = QualificationCaptureFailure::None;
        QualificationCaptureBundleIdentity identity{};
        QualificationCaptureCompletionToken completion{};
        uint32 recordedAttachmentCount = 0;
        uint32 mappedAttachmentCount = 0;
        bool copyRecorded = false;
        bool submissionAccepted = false;
        bool completionObserved = false;
        bool bundlePublished = false;
    };

    /**
     * @brief One-shot owner for a complete same-frame visual evidence bundle.
     *
     * Arm -> BindRecordedAttachments -> ConfirmExecutionReceipt ->
     * NotifySubmission -> FinalizePresentedReceipt -> PollCompletion is the
     * only legal progression. The post-execution receipt permits submission,
     * while only the post-Present receipt supplies the full SHA-256 binding.
     * Any discrepancy invalidates the entire bundle. In particular, a partial
     * capture is never published and the owner never maps readback data before
     * both exact completion and final receipt provenance close.
     */
    class QualificationCaptureOwner
    {
    public:
        [[nodiscard]] bool Arm(
            const QualificationCaptureBundleIdentity& identity) noexcept;

        [[nodiscard]] bool BindRecordedAttachments(
            const QualificationCaptureBundleIdentity& identity,
            std::span<const QualificationCaptureAttachmentDescriptor> attachments) noexcept;

        [[nodiscard]] bool ConfirmExecutionReceipt() noexcept;

        [[nodiscard]] bool FinalizePresentedReceipt(
            const Hash::SHA256Digest& executionReceiptDigest) noexcept;

        /** Mark a submitted capture to reject after its GPU ownership closes. */
        void RejectPendingCompletion() noexcept;

        [[nodiscard]] bool NotifySubmission(
            const QualificationCaptureBundleIdentity& identity,
            const QualificationCaptureCompletionToken& completion,
            const IQualificationCaptureCompletionTracker& tracker) noexcept;

        [[nodiscard]] bool PollCompletion(
            const IQualificationCaptureCompletionTracker& tracker,
            IQualificationCaptureReadbackMapper& mapper);

        /** Discard a recording which never reached submission. */
        void ReleaseUnsubmitted(
            const QualificationCaptureBundleIdentity& identity) noexcept;

        [[nodiscard]] bool HasPendingCompletion() const noexcept;
        [[nodiscard]] QualificationCaptureState GetState() const noexcept;
        [[nodiscard]] const QualificationCaptureDiagnostics&
        GetDiagnostics() const noexcept
        {
            return m_diagnostics;
        }

        /** @brief Empty until the whole completed bundle has passed validation. */
        [[nodiscard]] std::shared_ptr<const QualificationCaptureBundle>
        GetCompletedBundle() const noexcept
        {
            return m_completedBundle;
        }

        /** Clear the completed immutable result before arming a fresh bundle. */
        void ResetCompletedBundle() noexcept;

    private:
        [[nodiscard]] bool Reject(QualificationCaptureFailure failure) noexcept;
        [[nodiscard]] bool ValidateRecordedAttachment(
            const QualificationCaptureAttachmentDescriptor& descriptor,
            std::array<bool, RVX_QUALIFICATION_CAPTURE_ATTACHMENT_COUNT>& seen,
            uint32& count) noexcept;
        [[nodiscard]] bool ValidateMappedPayload(
            const QualificationCaptureAttachmentDescriptor& descriptor,
            QualificationCaptureMappedPayload& payload,
            QualificationCaptureAttachment& outAttachment);

        QualificationCaptureState m_state = QualificationCaptureState::Idle;
        QualificationCaptureBundleIdentity m_identity{};
        QualificationCaptureCompletionToken m_completion{};
        std::array<QualificationCaptureAttachmentDescriptor,
                   RVX_QUALIFICATION_CAPTURE_ATTACHMENT_COUNT> m_descriptors{};
        QualificationCaptureDiagnostics m_diagnostics{};
        std::shared_ptr<const QualificationCaptureBundle> m_completedBundle{};
        bool m_executionReceiptConfirmed = false;
        bool m_presentationRejected = false;
    };
} // namespace RVX
