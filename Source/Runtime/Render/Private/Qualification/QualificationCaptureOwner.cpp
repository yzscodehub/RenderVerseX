#include "Render/Qualification/QualificationCaptureOwner.h"

#include "Core/Hash/SHA256.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace RVX
{
namespace
{
    [[nodiscard]] constexpr uint32 GetBytesPerPixel(
        QualificationCapturePixelFormat format) noexcept
    {
        switch (format)
        {
            case QualificationCapturePixelFormat::R32Float: return 4;
            case QualificationCapturePixelFormat::RG32Float: return 8;
            case QualificationCapturePixelFormat::RGBA32Float: return 16;
            case QualificationCapturePixelFormat::Unknown: break;
        }
        return 0;
    }

    [[nodiscard]] bool HasValidRowPitch(
        const QualificationCaptureAttachmentDescriptor& descriptor) noexcept
    {
        const uint32 bytesPerPixel = GetBytesPerPixel(descriptor.contract.format);
        if (bytesPerPixel == 0 || descriptor.extent.width == 0 ||
            descriptor.extent.width > std::numeric_limits<uint32>::max() /
                bytesPerPixel)
        {
            return false;
        }
        return descriptor.rowPitchBytes == descriptor.extent.width * bytesPerPixel;
    }

    [[nodiscard]] bool GetRequiredPayloadBytes(
        const QualificationCaptureAttachmentDescriptor& descriptor,
        uint64& outBytes) noexcept
    {
        outBytes = 0;
        if (!descriptor.extent.IsValid() || descriptor.rowPitchBytes == 0 ||
            descriptor.extent.height > std::numeric_limits<uint64>::max() /
                descriptor.rowPitchBytes)
        {
            return false;
        }
        outBytes = static_cast<uint64>(descriptor.rowPitchBytes) *
            descriptor.extent.height;
        return outBytes != 0;
    }

    [[nodiscard]] constexpr size_t GetAttachmentIndex(
        QualificationCaptureAttachmentSemantic semantic) noexcept
    {
        return static_cast<size_t>(semantic);
    }
} // namespace

bool QualificationCaptureOwner::Arm(
    const QualificationCaptureBundleIdentity& identity) noexcept
{
    if (m_state == QualificationCaptureState::Armed ||
        m_state == QualificationCaptureState::Recorded ||
        m_state == QualificationCaptureState::PendingCompletion ||
        m_state == QualificationCaptureState::Completed)
    {
        return false;
    }
    if (!identity.IsRecordable() ||
        IsQualificationCaptureReceiptDigestValid(
            identity.graphProvenance.executionReceiptDigest))
    {
        m_identity = identity;
        return Reject(QualificationCaptureFailure::InvalidIdentity);
    }

    m_identity = identity;
    m_completion = {};
    m_descriptors = {};
    m_executionReceiptConfirmed = false;
    m_presentationRejected = false;
    m_diagnostics = {};
    m_diagnostics.state = QualificationCaptureState::Armed;
    m_diagnostics.identity = identity;
    m_state = QualificationCaptureState::Armed;
    return true;
}

bool QualificationCaptureOwner::ConfirmExecutionReceipt() noexcept
{
    if (m_state != QualificationCaptureState::Recorded)
    {
        return false;
    }
    if (m_executionReceiptConfirmed)
    {
        return Reject(
            QualificationCaptureFailure::ExecutionReceiptAlreadyFinalized);
    }
    m_executionReceiptConfirmed = true;
    return true;
}

bool QualificationCaptureOwner::FinalizePresentedReceipt(
    const Hash::SHA256Digest& executionReceiptDigest) noexcept
{
    if (m_state != QualificationCaptureState::PendingCompletion ||
        !m_executionReceiptConfirmed)
    {
        return false;
    }
    if (IsQualificationCaptureReceiptDigestValid(
            m_identity.graphProvenance.executionReceiptDigest))
    {
        return Reject(
            QualificationCaptureFailure::ExecutionReceiptAlreadyFinalized);
    }
    if (!IsQualificationCaptureReceiptDigestValid(executionReceiptDigest))
    {
        return Reject(
            QualificationCaptureFailure::ExecutionReceiptNotFinalized);
    }

    m_identity.graphProvenance.executionReceiptDigest = executionReceiptDigest;
    m_diagnostics.identity = m_identity;
    return true;
}

void QualificationCaptureOwner::RejectPendingCompletion() noexcept
{
    if (m_state == QualificationCaptureState::PendingCompletion)
    {
        m_presentationRejected = true;
    }
}

bool QualificationCaptureOwner::BindRecordedAttachments(
    const QualificationCaptureBundleIdentity& identity,
    std::span<const QualificationCaptureAttachmentDescriptor> attachments) noexcept
{
    if (m_state != QualificationCaptureState::Armed)
    {
        return false;
    }
    if (identity != m_identity)
    {
        return Reject(QualificationCaptureFailure::IdentityMismatch);
    }
    if (attachments.size() != RVX_QUALIFICATION_CAPTURE_ATTACHMENT_COUNT)
    {
        return Reject(attachments.size() < RVX_QUALIFICATION_CAPTURE_ATTACHMENT_COUNT
                          ? QualificationCaptureFailure::MissingAttachment
                          : QualificationCaptureFailure::AttachmentSetInvalid);
    }

    std::array<bool, RVX_QUALIFICATION_CAPTURE_ATTACHMENT_COUNT> seen{};
    std::array<QualificationCaptureAttachmentDescriptor,
               RVX_QUALIFICATION_CAPTURE_ATTACHMENT_COUNT> descriptors{};
    uint32 attachmentCount = 0;
    for (const QualificationCaptureAttachmentDescriptor& descriptor : attachments)
    {
        if (!ValidateRecordedAttachment(descriptor, seen, attachmentCount))
        {
            return false;
        }
        descriptors[GetAttachmentIndex(descriptor.contract.semantic)] = descriptor;
    }
    if (attachmentCount != RVX_QUALIFICATION_CAPTURE_ATTACHMENT_COUNT)
    {
        return Reject(QualificationCaptureFailure::MissingAttachment);
    }
    for (const bool present : seen)
    {
        if (!present)
        {
            return Reject(QualificationCaptureFailure::MissingAttachment);
        }
    }

    m_descriptors = descriptors;
    m_state = QualificationCaptureState::Recorded;
    m_diagnostics.state = m_state;
    m_diagnostics.recordedAttachmentCount = attachmentCount;
    m_diagnostics.copyRecorded = true;
    return true;
}

bool QualificationCaptureOwner::NotifySubmission(
    const QualificationCaptureBundleIdentity& identity,
    const QualificationCaptureCompletionToken& completion,
    const IQualificationCaptureCompletionTracker& tracker) noexcept
{
    if (m_state != QualificationCaptureState::Recorded)
    {
        return false;
    }
    if (!m_identity.IsRecordable() || !m_executionReceiptConfirmed ||
        IsQualificationCaptureReceiptDigestValid(
            m_identity.graphProvenance.executionReceiptDigest))
    {
        return Reject(
            QualificationCaptureFailure::ExecutionReceiptNotFinalized);
    }
    if (identity != m_identity)
    {
        return Reject(QualificationCaptureFailure::IdentityMismatch);
    }
    if (!completion.IsValid() || !tracker.IsExactCurrentSubmission(completion))
    {
        return Reject(QualificationCaptureFailure::SubmissionRejected);
    }

    m_completion = completion;
    m_state = QualificationCaptureState::PendingCompletion;
    m_diagnostics.state = m_state;
    m_diagnostics.completion = completion;
    m_diagnostics.submissionAccepted = true;
    return true;
}

bool QualificationCaptureOwner::PollCompletion(
    const IQualificationCaptureCompletionTracker& tracker,
    IQualificationCaptureReadbackMapper& mapper)
{
    if (m_state != QualificationCaptureState::PendingCompletion)
    {
        return m_state == QualificationCaptureState::Completed;
    }

    switch (tracker.Query(m_completion))
    {
        case QualificationCaptureCompletionStatus::Pending:
            m_diagnostics.failure = QualificationCaptureFailure::CompletionPending;
            return false;
        case QualificationCaptureCompletionStatus::Lost:
            return Reject(QualificationCaptureFailure::CompletionLost);
        case QualificationCaptureCompletionStatus::Rejected:
            return Reject(QualificationCaptureFailure::SubmissionRejected);
        case QualificationCaptureCompletionStatus::Completed: break;
    }

    m_diagnostics.completionObserved = true;
    if (m_presentationRejected)
    {
        return Reject(QualificationCaptureFailure::ExecutionReceiptNotFinalized);
    }
    if (!m_identity.IsValid())
    {
        m_diagnostics.failure = QualificationCaptureFailure::ExecutionReceiptNotFinalized;
        return false;
    }

    std::array<QualificationCaptureAttachment,
               RVX_QUALIFICATION_CAPTURE_ATTACHMENT_COUNT> completedAttachments{};
    uint32 mappedCount = 0;
    for (const QualificationCaptureAttachmentDescriptor& descriptor : m_descriptors)
    {
        QualificationCaptureMappedPayload payload;
        bool mapped = false;
        try
        {
            mapped = mapper.Map(m_identity, descriptor, payload);
        }
        catch (...)
        {
            return Reject(QualificationCaptureFailure::MapperFailed);
        }
        if (!mapped)
        {
            return Reject(QualificationCaptureFailure::MapperFailed);
        }

        QualificationCaptureAttachment& output = completedAttachments[
            GetAttachmentIndex(descriptor.contract.semantic)];
        try
        {
            if (!ValidateMappedPayload(descriptor, payload, output))
            {
                return false;
            }
        }
        catch (...)
        {
            return Reject(QualificationCaptureFailure::MapperFailed);
        }
        ++mappedCount;
    }

    QualificationCaptureBundle completedBundle;
    completedBundle.identity = m_identity;
    completedBundle.attachments = std::move(completedAttachments);
    try
    {
        m_completedBundle = std::make_shared<const QualificationCaptureBundle>(
            std::move(completedBundle));
    }
    catch (...)
    {
        return Reject(QualificationCaptureFailure::MapperFailed);
    }

    m_state = QualificationCaptureState::Completed;
    m_diagnostics.state = m_state;
    m_diagnostics.failure = QualificationCaptureFailure::None;
    m_diagnostics.mappedAttachmentCount = mappedCount;
    m_diagnostics.bundlePublished = true;
    return true;
}

void QualificationCaptureOwner::ReleaseUnsubmitted(
    const QualificationCaptureBundleIdentity& identity) noexcept
{
    if (m_state == QualificationCaptureState::Recorded && identity == m_identity)
    {
        static_cast<void>(
            Reject(QualificationCaptureFailure::SubmissionRejected));
    }
}

bool QualificationCaptureOwner::HasPendingCompletion() const noexcept
{
    return m_state == QualificationCaptureState::PendingCompletion;
}

QualificationCaptureState QualificationCaptureOwner::GetState() const noexcept
{
    return m_state;
}

void QualificationCaptureOwner::ResetCompletedBundle() noexcept
{
    if (m_state != QualificationCaptureState::Completed)
    {
        return;
    }

    m_completedBundle.reset();
    m_identity = {};
    m_completion = {};
    m_descriptors = {};
    m_diagnostics = {};
    m_executionReceiptConfirmed = false;
    m_presentationRejected = false;
    m_state = QualificationCaptureState::Idle;
}

bool QualificationCaptureOwner::Reject(QualificationCaptureFailure failure) noexcept
{
    m_completedBundle.reset();
    m_state = QualificationCaptureState::Rejected;
    m_diagnostics.state = m_state;
    m_diagnostics.failure = failure;
    m_diagnostics.identity = m_identity;
    m_diagnostics.completion = m_completion;
    return false;
}

bool QualificationCaptureOwner::ValidateRecordedAttachment(
    const QualificationCaptureAttachmentDescriptor& descriptor,
    std::array<bool, RVX_QUALIFICATION_CAPTURE_ATTACHMENT_COUNT>& seen,
    uint32& count) noexcept
{
    if (!descriptor.IsValid() || !HasValidRowPitch(descriptor))
    {
        return Reject(QualificationCaptureFailure::AttachmentMetadataInvalid);
    }
    if (descriptor.provenance.graphIdentity !=
            m_identity.graphProvenance.graphIdentity ||
        descriptor.provenance.graphRecordingGeneration !=
            m_identity.graphProvenance.graphRecordingGeneration ||
        descriptor.provenance.graphRecordEpoch !=
            m_identity.graphProvenance.graphRecordEpoch ||
        descriptor.provenance.graphResourceGeneration !=
            m_identity.graphProvenance.graphRecordingGeneration)
    {
        return Reject(QualificationCaptureFailure::AttachmentMetadataInvalid);
    }
    const QualificationCaptureAttachmentContract expected =
        GetQualificationCaptureAttachmentContract(descriptor.contract.semantic);
    if (descriptor.contract != expected)
    {
        return Reject(QualificationCaptureFailure::AttachmentMetadataInvalid);
    }

    const size_t index = GetAttachmentIndex(descriptor.contract.semantic);
    if (index >= seen.size())
    {
        return Reject(QualificationCaptureFailure::AttachmentSetInvalid);
    }
    if (seen[index])
    {
        return Reject(QualificationCaptureFailure::DuplicateAttachment);
    }
    seen[index] = true;
    ++count;
    return true;
}

bool QualificationCaptureOwner::ValidateMappedPayload(
    const QualificationCaptureAttachmentDescriptor& descriptor,
    QualificationCaptureMappedPayload& payload,
    QualificationCaptureAttachment& outAttachment)
{
    if (payload.identity != m_identity ||
        payload.semantic != descriptor.contract.semantic)
    {
        return Reject(QualificationCaptureFailure::PayloadIdentityMismatch);
    }
    if (payload.provenance != descriptor.provenance)
    {
        return Reject(QualificationCaptureFailure::PayloadProvenanceMismatch);
    }
    if (payload.bytes.empty())
    {
        return Reject(QualificationCaptureFailure::PayloadMissing);
    }

    uint64 requiredBytes = 0;
    if (!GetRequiredPayloadBytes(descriptor, requiredBytes) ||
        payload.bytes.size() != requiredBytes)
    {
        return Reject(QualificationCaptureFailure::PayloadSizeMismatch);
    }

    const Hash::SHA256Digest observedHash = Hash::ComputeSHA256(
        payload.bytes.data(), payload.bytes.size());
    if (observedHash != payload.claimedPayloadHash)
    {
        return Reject(QualificationCaptureFailure::PayloadHashMismatch);
    }

    for (size_t offset = 0; offset < payload.bytes.size(); offset += sizeof(float))
    {
        float value = 0.0f;
        std::memcpy(&value, payload.bytes.data() + offset, sizeof(value));
        if (!std::isfinite(value))
        {
            return Reject(QualificationCaptureFailure::PayloadNonFinite);
        }
    }

    outAttachment.descriptor = descriptor;
    outAttachment.payloadHash = observedHash;
    outAttachment.bytes = std::move(payload.bytes);
    return true;
}
} // namespace RVX
