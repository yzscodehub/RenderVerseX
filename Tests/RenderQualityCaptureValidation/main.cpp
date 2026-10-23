#include "Core/Hash/SHA256.h"
#include "Render/Qualification/QualificationCaptureOwner.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace RVX
{
namespace
{
    [[nodiscard]] Hash::SHA256Digest MakeReceiptDigest(uint8 marker = 47)
    {
        Hash::SHA256Digest digest{};
        digest.front() = marker;
        return digest;
    }

    [[nodiscard]] QualificationCaptureBundleIdentity MakeIdentity(
        uint64 bundleId = 41,
        uint64 frameSequence = 19,
        uint64 cameraEntityPacked = 73)
    {
        QualificationCaptureBundleIdentity identity;
        identity.bundleId = bundleId;
        identity.frameIdentity = {frameSequence, 5, 7, 11};
        identity.viewIdentity = {29, cameraEntityPacked, 0, ViewRole::Main};
        identity.graphProvenance = {31, 37, 43, {}};
        return identity;
    }

    [[nodiscard]] uint32 BytesPerPixel(
        QualificationCapturePixelFormat format)
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

    [[nodiscard]] std::array<QualificationCaptureAttachmentDescriptor,
                             RVX_QUALIFICATION_CAPTURE_ATTACHMENT_COUNT>
    MakeDescriptors()
    {
        std::array<QualificationCaptureAttachmentDescriptor,
                   RVX_QUALIFICATION_CAPTURE_ATTACHMENT_COUNT> descriptors{};
        for (size_t index = 0; index < descriptors.size(); ++index)
        {
            const auto semantic = static_cast<QualificationCaptureAttachmentSemantic>(
                index);
            descriptors[index].contract =
                GetQualificationCaptureAttachmentContract(semantic);
            descriptors[index].extent = semantic ==
                    QualificationCaptureAttachmentSemantic::AmbientOcclusion
                ? RenderOutputExtent{8, 4}
                : RenderOutputExtent{16, 8};
            descriptors[index].rowPitchBytes = descriptors[index].extent.width *
                BytesPerPixel(
                descriptors[index].contract.format);
            descriptors[index].provenance = {
                31,
                37,
                43,
                101 + static_cast<uint64>(index),
                37,
                301 + static_cast<uint64>(index)};
        }
        return descriptors;
    }

    class FakeCompletionTracker final : public IQualificationCaptureCompletionTracker
    {
    public:
        bool accepted = true;
        QualificationCaptureCompletionStatus status =
            QualificationCaptureCompletionStatus::Pending;
        QualificationCaptureCompletionToken expected{97, 101};
        mutable uint32 queryCount = 0;

        [[nodiscard]] bool IsExactCurrentSubmission(
            const QualificationCaptureCompletionToken& token) const noexcept override
        {
            return accepted && token == expected;
        }

        [[nodiscard]] QualificationCaptureCompletionStatus Query(
            const QualificationCaptureCompletionToken& token) const noexcept override
        {
            ++queryCount;
            return token == expected ? status :
                QualificationCaptureCompletionStatus::Rejected;
        }
    };

    enum class MapperMode : uint8
    {
        Valid = 0,
        MapFailure,
        IdentityMismatch,
        ProvenanceMismatch,
        HashMismatch,
        SizeMismatch,
        EmptyPayload,
        NonFinitePayload,
    };

    class FakeReadbackMapper final : public IQualificationCaptureReadbackMapper
    {
    public:
        MapperMode mode = MapperMode::Valid;
        uint32 mapCount = 0;

        [[nodiscard]] bool Map(
            const QualificationCaptureBundleIdentity& identity,
            const QualificationCaptureAttachmentDescriptor& descriptor,
            QualificationCaptureMappedPayload& outPayload) override
        {
            ++mapCount;
            if (mode == MapperMode::MapFailure)
            {
                return false;
            }

            outPayload.identity = identity;
            outPayload.semantic = descriptor.contract.semantic;
            outPayload.provenance = descriptor.provenance;
            const size_t byteCount = static_cast<size_t>(descriptor.rowPitchBytes) *
                descriptor.extent.height;
            outPayload.bytes.assign(byteCount,
                                    static_cast<uint8>(mapCount));
            if (mode == MapperMode::IdentityMismatch)
            {
                ++outPayload.identity.frameIdentity.frameSequence;
            }
            if (mode == MapperMode::ProvenanceMismatch)
            {
                ++outPayload.provenance.graphResourceGeneration;
            }
            if (mode == MapperMode::SizeMismatch)
            {
                outPayload.bytes.pop_back();
            }
            if (mode == MapperMode::EmptyPayload)
            {
                outPayload.bytes.clear();
            }
            if (mode == MapperMode::NonFinitePayload &&
                outPayload.bytes.size() >= sizeof(float))
            {
                const float nonFinite = std::numeric_limits<float>::infinity();
                std::memcpy(outPayload.bytes.data(), &nonFinite, sizeof(nonFinite));
            }
            outPayload.claimedPayloadHash = Hash::ComputeSHA256(
                outPayload.bytes.data(), outPayload.bytes.size());
            if (mode == MapperMode::HashMismatch)
            {
                ++outPayload.claimedPayloadHash.front();
            }
            return true;
        }
    };

    struct OwnerFixture
    {
        QualificationCaptureOwner owner;
        QualificationCaptureBundleIdentity identity = MakeIdentity();
        std::array<QualificationCaptureAttachmentDescriptor,
                   RVX_QUALIFICATION_CAPTURE_ATTACHMENT_COUNT> descriptors =
            MakeDescriptors();
        FakeCompletionTracker tracker;
        FakeReadbackMapper mapper;

        void ArmAndBind()
        {
            ASSERT_TRUE(owner.Arm(identity));
            ASSERT_TRUE(owner.BindRecordedAttachments(identity, descriptors));
        }

        void Submit()
        {
            const Hash::SHA256Digest digest = MakeReceiptDigest();
            ASSERT_TRUE(owner.ConfirmExecutionReceipt());
            ASSERT_TRUE(owner.NotifySubmission(identity, tracker.expected, tracker));
            ASSERT_TRUE(owner.FinalizePresentedReceipt(digest));
            identity.graphProvenance.executionReceiptDigest = digest;
        }
    };

    TEST(QualificationCaptureContract, HasOneStrictContractForEveryRequiredSurface)
    {
        constexpr std::array semantics{
            QualificationCaptureAttachmentSemantic::BeautyLDR,
            QualificationCaptureAttachmentSemantic::PreToneHDR,
            QualificationCaptureAttachmentSemantic::LinearDepth,
            QualificationCaptureAttachmentSemantic::Normal,
            QualificationCaptureAttachmentSemantic::Velocity,
            QualificationCaptureAttachmentSemantic::AmbientOcclusion,
            QualificationCaptureAttachmentSemantic::ReactiveMask};
        EXPECT_EQ(RVX_QUALIFICATION_CAPTURE_ATTACHMENT_COUNT, semantics.size());
        for (const QualificationCaptureAttachmentSemantic semantic : semantics)
        {
            const QualificationCaptureAttachmentContract contract =
                GetQualificationCaptureAttachmentContract(semantic);
            EXPECT_TRUE(contract.IsValid());
            EXPECT_EQ(semantic, contract.semantic);
        }
    }

    TEST(QualificationCaptureOwner, RejectsMissingAndDuplicateAttachmentSets)
    {
        QualificationCaptureOwner owner;
        const QualificationCaptureBundleIdentity identity = MakeIdentity();
        const auto descriptors = MakeDescriptors();
        ASSERT_TRUE(owner.Arm(identity));
        EXPECT_FALSE(owner.BindRecordedAttachments(
            identity, std::span(descriptors.data(), descriptors.size() - 1)));
        EXPECT_EQ(QualificationCaptureFailure::MissingAttachment,
                  owner.GetDiagnostics().failure);

        ASSERT_TRUE(owner.Arm(identity));
        auto duplicate = descriptors;
        duplicate.back() = duplicate.front();
        EXPECT_FALSE(owner.BindRecordedAttachments(identity, duplicate));
        EXPECT_EQ(QualificationCaptureFailure::DuplicateAttachment,
                  owner.GetDiagnostics().failure);
    }

    TEST(QualificationCaptureOwner, RejectsCrossFrameAndCrossViewBindings)
    {
        QualificationCaptureOwner owner;
        const QualificationCaptureBundleIdentity identity = MakeIdentity();
        const auto descriptors = MakeDescriptors();
        ASSERT_TRUE(owner.Arm(identity));

        QualificationCaptureBundleIdentity differentFrame = identity;
        ++differentFrame.frameIdentity.frameSequence;
        EXPECT_FALSE(owner.BindRecordedAttachments(differentFrame, descriptors));
        EXPECT_EQ(QualificationCaptureFailure::IdentityMismatch,
                  owner.GetDiagnostics().failure);

        ASSERT_TRUE(owner.Arm(identity));
        QualificationCaptureBundleIdentity differentView = identity;
        ++differentView.viewIdentity.cameraEntityPacked;
        EXPECT_FALSE(owner.BindRecordedAttachments(differentView, descriptors));
        EXPECT_EQ(QualificationCaptureFailure::IdentityMismatch,
                  owner.GetDiagnostics().failure);
    }

    TEST(QualificationCaptureOwner, RejectsBadMetadataAndMismatchedProvenance)
    {
        QualificationCaptureOwner owner;
        const QualificationCaptureBundleIdentity identity = MakeIdentity();
        auto descriptors = MakeDescriptors();
        descriptors.front().rowPitchBytes = 1;
        ASSERT_TRUE(owner.Arm(identity));
        EXPECT_FALSE(owner.BindRecordedAttachments(identity, descriptors));
        EXPECT_EQ(QualificationCaptureFailure::AttachmentMetadataInvalid,
                  owner.GetDiagnostics().failure);

        OwnerFixture fixture;
        fixture.ArmAndBind();
        fixture.Submit();
        fixture.tracker.status = QualificationCaptureCompletionStatus::Completed;
        fixture.mapper.mode = MapperMode::ProvenanceMismatch;
        EXPECT_FALSE(fixture.owner.PollCompletion(fixture.tracker, fixture.mapper));
        EXPECT_EQ(QualificationCaptureFailure::PayloadProvenanceMismatch,
                  fixture.owner.GetDiagnostics().failure);
        EXPECT_EQ(nullptr, fixture.owner.GetCompletedBundle());
    }

    TEST(QualificationCaptureOwner, RejectsPaddedCanonicalPayloadRows)
    {
        QualificationCaptureOwner owner;
        const QualificationCaptureBundleIdentity identity = MakeIdentity();
        auto descriptors = MakeDescriptors();
        descriptors.front().rowPitchBytes += sizeof(float);
        ASSERT_TRUE(owner.Arm(identity));
        EXPECT_FALSE(owner.BindRecordedAttachments(identity, descriptors));
        EXPECT_EQ(QualificationCaptureFailure::AttachmentMetadataInvalid,
                  owner.GetDiagnostics().failure);
    }

    TEST(QualificationCaptureOwner, AcceptsSameFrameAttachmentsAtTheirNativeExtents)
    {
        QualificationCaptureOwner owner;
        const QualificationCaptureBundleIdentity identity = MakeIdentity();
        const auto descriptors = MakeDescriptors();
        EXPECT_NE(descriptors[static_cast<size_t>(
                      QualificationCaptureAttachmentSemantic::AmbientOcclusion)].extent,
                  descriptors[static_cast<size_t>(
                      QualificationCaptureAttachmentSemantic::BeautyLDR)].extent);
        ASSERT_TRUE(owner.Arm(identity));
        EXPECT_TRUE(owner.BindRecordedAttachments(identity, descriptors));
        EXPECT_EQ(QualificationCaptureState::Recorded, owner.GetState());
    }

    TEST(QualificationCaptureOwner, DoesNotMapBeforeExactCompletionThenPublishesWholeBundle)
    {
        OwnerFixture fixture;
        fixture.ArmAndBind();
        fixture.Submit();

        EXPECT_FALSE(fixture.owner.PollCompletion(fixture.tracker, fixture.mapper));
        EXPECT_EQ(0u, fixture.mapper.mapCount);
        EXPECT_TRUE(fixture.owner.HasPendingCompletion());
        EXPECT_EQ(QualificationCaptureFailure::CompletionPending,
                  fixture.owner.GetDiagnostics().failure);

        fixture.tracker.status = QualificationCaptureCompletionStatus::Completed;
        EXPECT_TRUE(fixture.owner.PollCompletion(fixture.tracker, fixture.mapper));
        EXPECT_EQ(RVX_QUALIFICATION_CAPTURE_ATTACHMENT_COUNT, fixture.mapper.mapCount);
        EXPECT_FALSE(fixture.owner.HasPendingCompletion());
        const std::shared_ptr<const QualificationCaptureBundle> bundle =
            fixture.owner.GetCompletedBundle();
        ASSERT_NE(nullptr, bundle);
        EXPECT_EQ(fixture.identity, bundle->identity);
        EXPECT_EQ(RVX_QUALIFICATION_CAPTURE_ATTACHMENT_COUNT,
                  fixture.owner.GetDiagnostics().mappedAttachmentCount);
        EXPECT_TRUE(fixture.owner.GetDiagnostics().bundlePublished);

        EXPECT_TRUE(fixture.owner.PollCompletion(fixture.tracker, fixture.mapper));
        EXPECT_EQ(RVX_QUALIFICATION_CAPTURE_ATTACHMENT_COUNT, fixture.mapper.mapCount);
    }

    TEST(QualificationCaptureOwner, RequiresPostExecutionReceiptBeforeSubmission)
    {
        OwnerFixture fixture;
        fixture.ArmAndBind();
        EXPECT_FALSE(fixture.owner.NotifySubmission(
            fixture.identity, fixture.tracker.expected, fixture.tracker));
        EXPECT_EQ(QualificationCaptureFailure::ExecutionReceiptNotFinalized,
                  fixture.owner.GetDiagnostics().failure);

        QualificationCaptureOwner prebound;
        QualificationCaptureBundleIdentity preboundIdentity = MakeIdentity();
        preboundIdentity.graphProvenance.executionReceiptDigest =
            MakeReceiptDigest();
        EXPECT_FALSE(prebound.Arm(preboundIdentity));
        EXPECT_EQ(QualificationCaptureFailure::InvalidIdentity,
                  prebound.GetDiagnostics().failure);
    }

    TEST(QualificationCaptureOwner, RejectsUnacceptedAndLostSubmission)
    {
        OwnerFixture rejected;
        rejected.ArmAndBind();
        const Hash::SHA256Digest digest = MakeReceiptDigest();
        ASSERT_TRUE(rejected.owner.ConfirmExecutionReceipt());
        rejected.tracker.accepted = false;
        EXPECT_FALSE(rejected.owner.NotifySubmission(
            rejected.identity, rejected.tracker.expected, rejected.tracker));
        EXPECT_EQ(QualificationCaptureFailure::SubmissionRejected,
                  rejected.owner.GetDiagnostics().failure);

        OwnerFixture lost;
        lost.ArmAndBind();
        lost.Submit();
        lost.tracker.status = QualificationCaptureCompletionStatus::Lost;
        EXPECT_FALSE(lost.owner.PollCompletion(lost.tracker, lost.mapper));
        EXPECT_EQ(0u, lost.mapper.mapCount);
        EXPECT_EQ(QualificationCaptureFailure::CompletionLost,
                  lost.owner.GetDiagnostics().failure);
    }

    TEST(QualificationCaptureOwner,
         RejectsCompletionUntilPresentationBindsFinalDigest)
    {
        OwnerFixture fixture;
        fixture.ArmAndBind();
        ASSERT_TRUE(fixture.owner.ConfirmExecutionReceipt());
        ASSERT_TRUE(fixture.owner.NotifySubmission(
            fixture.identity, fixture.tracker.expected, fixture.tracker));
        fixture.tracker.status = QualificationCaptureCompletionStatus::Completed;
        EXPECT_FALSE(fixture.owner.PollCompletion(fixture.tracker, fixture.mapper));
        EXPECT_EQ(QualificationCaptureFailure::ExecutionReceiptNotFinalized,
                  fixture.owner.GetDiagnostics().failure);
        EXPECT_EQ(nullptr, fixture.owner.GetCompletedBundle());

        const Hash::SHA256Digest digest = MakeReceiptDigest();
        ASSERT_TRUE(fixture.owner.FinalizePresentedReceipt(digest));
        fixture.identity.graphProvenance.executionReceiptDigest = digest;
        EXPECT_TRUE(fixture.owner.PollCompletion(fixture.tracker, fixture.mapper));
        ASSERT_NE(nullptr, fixture.owner.GetCompletedBundle());
        EXPECT_EQ(digest,
                  fixture.owner.GetCompletedBundle()
                      ->identity.graphProvenance.executionReceiptDigest);
    }

    TEST(QualificationCaptureOwner, RejectsStalePayloadAndNeverPublishesPartialBundle)
    {
        OwnerFixture fixture;
        fixture.ArmAndBind();
        fixture.Submit();
        fixture.tracker.status = QualificationCaptureCompletionStatus::Completed;
        fixture.mapper.mode = MapperMode::IdentityMismatch;
        EXPECT_FALSE(fixture.owner.PollCompletion(fixture.tracker, fixture.mapper));
        EXPECT_EQ(1u, fixture.mapper.mapCount);
        EXPECT_EQ(QualificationCaptureFailure::PayloadIdentityMismatch,
                  fixture.owner.GetDiagnostics().failure);
        EXPECT_EQ(nullptr, fixture.owner.GetCompletedBundle());
        EXPECT_FALSE(fixture.owner.GetDiagnostics().bundlePublished);
    }

    TEST(QualificationCaptureOwner, RejectsInvalidPayloadSizesAndHashes)
    {
        OwnerFixture hashMismatch;
        hashMismatch.ArmAndBind();
        hashMismatch.Submit();
        hashMismatch.tracker.status = QualificationCaptureCompletionStatus::Completed;
        hashMismatch.mapper.mode = MapperMode::HashMismatch;
        EXPECT_FALSE(hashMismatch.owner.PollCompletion(
            hashMismatch.tracker, hashMismatch.mapper));
        EXPECT_EQ(QualificationCaptureFailure::PayloadHashMismatch,
                  hashMismatch.owner.GetDiagnostics().failure);

        OwnerFixture sizeMismatch;
        sizeMismatch.ArmAndBind();
        sizeMismatch.Submit();
        sizeMismatch.tracker.status = QualificationCaptureCompletionStatus::Completed;
        sizeMismatch.mapper.mode = MapperMode::SizeMismatch;
        EXPECT_FALSE(sizeMismatch.owner.PollCompletion(
            sizeMismatch.tracker, sizeMismatch.mapper));
        EXPECT_EQ(QualificationCaptureFailure::PayloadSizeMismatch,
                  sizeMismatch.owner.GetDiagnostics().failure);

        OwnerFixture missingPayload;
        missingPayload.ArmAndBind();
        missingPayload.Submit();
        missingPayload.tracker.status = QualificationCaptureCompletionStatus::Completed;
        missingPayload.mapper.mode = MapperMode::EmptyPayload;
        EXPECT_FALSE(missingPayload.owner.PollCompletion(
            missingPayload.tracker, missingPayload.mapper));
        EXPECT_EQ(QualificationCaptureFailure::PayloadMissing,
                  missingPayload.owner.GetDiagnostics().failure);

        OwnerFixture nonFinite;
        nonFinite.ArmAndBind();
        nonFinite.Submit();
        nonFinite.tracker.status = QualificationCaptureCompletionStatus::Completed;
        nonFinite.mapper.mode = MapperMode::NonFinitePayload;
        EXPECT_FALSE(nonFinite.owner.PollCompletion(
            nonFinite.tracker, nonFinite.mapper));
        EXPECT_EQ(QualificationCaptureFailure::PayloadNonFinite,
                  nonFinite.owner.GetDiagnostics().failure);
    }

    TEST(QualificationCaptureOwner, RejectsMapperFailureWithoutPublishingPartialBundle)
    {
        OwnerFixture fixture;
        fixture.ArmAndBind();
        fixture.Submit();
        fixture.tracker.status = QualificationCaptureCompletionStatus::Completed;
        fixture.mapper.mode = MapperMode::MapFailure;
        EXPECT_FALSE(fixture.owner.PollCompletion(fixture.tracker, fixture.mapper));
        EXPECT_EQ(1u, fixture.mapper.mapCount);
        EXPECT_EQ(QualificationCaptureFailure::MapperFailed,
                  fixture.owner.GetDiagnostics().failure);
        EXPECT_FALSE(fixture.owner.GetDiagnostics().bundlePublished);
        EXPECT_EQ(nullptr, fixture.owner.GetCompletedBundle());
    }

    TEST(QualificationCaptureOwner, ReleasesOnlyTheExactUnsubmittedRecording)
    {
        OwnerFixture fixture;
        fixture.ArmAndBind();
        QualificationCaptureBundleIdentity unrelated = fixture.identity;
        ++unrelated.bundleId;
        fixture.owner.ReleaseUnsubmitted(unrelated);
        EXPECT_EQ(QualificationCaptureState::Recorded, fixture.owner.GetState());
        fixture.owner.ReleaseUnsubmitted(fixture.identity);
        EXPECT_EQ(QualificationCaptureState::Rejected, fixture.owner.GetState());
        EXPECT_EQ(QualificationCaptureFailure::SubmissionRejected,
                  fixture.owner.GetDiagnostics().failure);
    }
} // namespace
} // namespace RVX
