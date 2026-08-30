#include "Render/Qualification/QualificationCaptureBridge.h"
#include "Render/Qualification/QualificationCaptureOwner.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstring>
#include <span>
#include <vector>

namespace RVX
{
namespace
{
    [[nodiscard]] Hash::SHA256Digest MakeReceiptDigest(uint8 marker = 71)
    {
        Hash::SHA256Digest digest{};
        digest.front() = marker;
        return digest;
    }

    [[nodiscard]] float32 ReadFloat(const std::array<uint8, 16>& bytes,
                                    size_t offset = 0)
    {
        float32 value = 0.0f;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    }

    [[nodiscard]] QualificationCaptureBundleIdentity MakeIdentity()
    {
        QualificationCaptureBundleIdentity identity;
        identity.bundleId = 17;
        identity.frameIdentity = {19, 23, 29, 31};
        identity.viewIdentity = {37, 41, 0, ViewRole::Main};
        identity.graphProvenance = {43, 47, 53, {}};
        return identity;
    }

    [[nodiscard]] std::array<QualificationCaptureAttachmentDescriptor,
                             RVX_QUALIFICATION_CAPTURE_ATTACHMENT_COUNT>
    MakeDescriptors()
    {
        std::array<QualificationCaptureAttachmentDescriptor,
                   RVX_QUALIFICATION_CAPTURE_ATTACHMENT_COUNT> descriptors{};
        for (size_t index = 0; index < descriptors.size(); ++index)
        {
            const auto semantic =
                static_cast<QualificationCaptureAttachmentSemantic>(index);
            descriptors[index].contract =
                GetQualificationCaptureAttachmentContract(semantic);
            descriptors[index].extent = {1, 1};
            descriptors[index].rowPitchBytes =
                descriptors[index].contract.format ==
                        QualificationCapturePixelFormat::RG32Float
                    ? 8
                    : descriptors[index].contract.format ==
                            QualificationCapturePixelFormat::RGBA32Float
                        ? 16
                        : 4;
            descriptors[index].provenance = {43, 47, 53, 101 + index,
                                             47, 301 + index};
        }
        return descriptors;
    }

    class ExactCompletionTracker final : public IQualificationCaptureCompletionTracker
    {
    public:
        QualificationCaptureCompletionToken expected{17, 61};

        [[nodiscard]] bool IsExactCurrentSubmission(
            const QualificationCaptureCompletionToken& token) const noexcept override
        {
            return token == expected;
        }

        [[nodiscard]] QualificationCaptureCompletionStatus Query(
            const QualificationCaptureCompletionToken&) const noexcept override
        {
            return QualificationCaptureCompletionStatus::Pending;
        }
    };
} // namespace

TEST(RenderQualityCaptureBridge, ConvertsBinary16NormalsSubnormalsAndInfinity)
{
    EXPECT_FLOAT_EQ(1.0f, QualificationCaptureBridgeTestAccess::HalfToFloat(0x3C00));
    EXPECT_NEAR(5.9604645e-8f,
                QualificationCaptureBridgeTestAccess::HalfToFloat(0x0001),
                1.0e-13f);
    EXPECT_TRUE(std::isinf(
        QualificationCaptureBridgeTestAccess::HalfToFloat(0x7C00)));
}

TEST(RenderQualityCaptureBridge, CanonicalizesBgraAndEightBitMasks)
{
    const std::array<uint8, 4> bgra{16, 32, 64, 255};
    std::array<uint8, 16> beauty{};
    ASSERT_TRUE(QualificationCaptureBridgeTestAccess::ConvertPixel(
        QualificationCaptureAttachmentSemantic::BeautyLDR,
        RHIFormat::BGRA8_UNORM_SRGB, bgra.data(), 0.1f, 100.0f, false,
        beauty.data()));
    EXPECT_FLOAT_EQ(64.0f / 255.0f, ReadFloat(beauty));
    EXPECT_FLOAT_EQ(32.0f / 255.0f, ReadFloat(beauty, 4));
    EXPECT_FLOAT_EQ(16.0f / 255.0f, ReadFloat(beauty, 8));
    EXPECT_FLOAT_EQ(1.0f, ReadFloat(beauty, 12));

    const std::array<uint8, 1> mask{127};
    std::array<uint8, 16> scalar{};
    ASSERT_TRUE(QualificationCaptureBridgeTestAccess::ConvertPixel(
        QualificationCaptureAttachmentSemantic::ReactiveMask,
        RHIFormat::R8_UNORM, mask.data(), 0.1f, 100.0f, false,
        scalar.data()));
    EXPECT_FLOAT_EQ(127.0f / 255.0f, ReadFloat(scalar));
}

TEST(RenderQualityCaptureBridge, DecodesOctNormalAndForwardReverseZDepth)
{
    const std::array<uint8, 4> octahedralPositiveZ{128, 128, 0, 0};
    std::array<uint8, 16> normal{};
    ASSERT_TRUE(QualificationCaptureBridgeTestAccess::ConvertPixel(
        QualificationCaptureAttachmentSemantic::Normal, RHIFormat::RGBA8_UNORM,
        octahedralPositiveZ.data(), 0.1f, 100.0f, false, normal.data()));
    EXPECT_NEAR(0.0f, ReadFloat(normal), 0.01f);
    EXPECT_NEAR(0.0f, ReadFloat(normal, 4), 0.01f);
    EXPECT_NEAR(1.0f, ReadFloat(normal, 8), 0.01f);

    std::array<uint8, 4> depth{};
    const float32 deviceDepth = 0.75f;
    std::memcpy(depth.data(), &deviceDepth, sizeof(deviceDepth));
    std::array<uint8, 16> forward{};
    std::array<uint8, 16> reverse{};
    ASSERT_TRUE(QualificationCaptureBridgeTestAccess::ConvertPixel(
        QualificationCaptureAttachmentSemantic::LinearDepth, RHIFormat::D32_FLOAT,
        depth.data(), 0.1f, 100.0f, false, forward.data()));
    const float32 reverseDeviceDepth = 0.25f;
    std::memcpy(depth.data(), &reverseDeviceDepth, sizeof(reverseDeviceDepth));
    ASSERT_TRUE(QualificationCaptureBridgeTestAccess::ConvertPixel(
        QualificationCaptureAttachmentSemantic::LinearDepth, RHIFormat::D32_FLOAT,
        depth.data(), 0.1f, 100.0f, true, reverse.data()));
    EXPECT_GT(ReadFloat(forward), 0.0f);
    EXPECT_GT(ReadFloat(reverse), 0.0f);
    EXPECT_NEAR(ReadFloat(forward), ReadFloat(reverse), 1.0e-5f);
}

TEST(RenderQualityCaptureBridge, UsesTopLeftCanonicalRowsForOpenGLReadback)
{
    EXPECT_EQ(0u, QualificationCaptureBridgeTestAccess::SourceRowForCanonicalTopLeft(
                      0, 3, false));
    EXPECT_EQ(2u, QualificationCaptureBridgeTestAccess::SourceRowForCanonicalTopLeft(
                      0, 3, true));
    EXPECT_EQ(0u, QualificationCaptureBridgeTestAccess::SourceRowForCanonicalTopLeft(
                      2, 3, true));
    EXPECT_EQ(RVX_INVALID_INDEX,
              QualificationCaptureBridgeTestAccess::SourceRowForCanonicalTopLeft(
                  3, 3, true));
}

TEST(RenderQualityCaptureBridge, FinalizedReceiptIdentityCanBeSubmitted)
{
    QualificationCaptureOwner owner;
    QualificationCaptureBundleIdentity identity = MakeIdentity();
    const auto descriptors = MakeDescriptors();
    ASSERT_TRUE(owner.Arm(identity));
    ASSERT_TRUE(owner.BindRecordedAttachments(identity, descriptors));
    const Hash::SHA256Digest digest = MakeReceiptDigest();
    ASSERT_TRUE(owner.ConfirmExecutionReceipt());

    ExactCompletionTracker tracker;
    ASSERT_TRUE(owner.NotifySubmission(identity, tracker.expected, tracker));
    // The exact full digest is not available until presentation qualification.
    EXPECT_TRUE(owner.FinalizePresentedReceipt(digest));
    identity.graphProvenance.executionReceiptDigest = digest;
}
} // namespace RVX
