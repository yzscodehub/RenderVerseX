#include "Samples/RenderQualityQualificationWriter.h"

#include "Core/Hash/SHA256.h"
#include "Render/Qualification/RenderExecutionReceiptDigest.h"
#include "Samples/SampleCLI.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

namespace RVX
{
namespace
{
    [[nodiscard]] RenderExecutionReceipt MakeReceipt()
    {
        RenderExecutionReceipt receipt;
        receipt.schemaVersion = 1;
        receipt.available = true;
        receipt.graphComposed = true;
        receipt.executionSucceeded = true;
        receipt.presented = true;
        receipt.succeeded = true;
        receipt.build.sourceCommit = std::string(40, 'a');
        receipt.build.preset = "win_x64_gfx24_debug";
        receipt.frameIdentity = {91, 17, 3, 5};
        receipt.viewIdentity = {7, 0x0000000200000001ULL, 0, ViewRole::Main};
        receipt.backend = RHIBackendType::DX12;
        receipt.requestedRecipe = RenderPipelineRecipe::DeferredQuality;
        receipt.resolvedRecipe = RenderPipelineRecipe::DeferredQuality;
        receipt.quality = RenderQualityTier::High;
        receipt.profileHash = ComputeRenderPipelineProfileHash(
            receipt.resolvedRecipe, receipt.quality);

        RenderFeatureExecutionReceipt feature;
        feature.featureId = BuiltinRenderFeatureIds::LegacyForwardMainChain;
        feature.featureSchemaVersion = 1;
        feature.phase = RenderFeaturePhase::Geometry;
        feature.requirement = RenderFeatureRequirement::Required;
        feature.readiness.compiled = true;
        feature.readiness.hardwareSupported = true;
        feature.readiness.implementationSupported = true;
        feature.readiness.pipelineReady = true;
        feature.readiness.inputsReady = true;
        feature.readiness.executed = true;
        feature.selected = true;
        feature.graphPassCount = 1;
        feature.executedGraphPassCount = 1;
        feature.graphPassNames = {"OpaquePass"};
        receipt.features.push_back(std::move(feature));
        return receipt;
    }

    [[nodiscard]] QualificationCaptureBundle MakeBundle(
        const RenderExecutionReceipt& receipt)
    {
        QualificationCaptureBundle bundle;
        bundle.identity.bundleId = 31;
        bundle.identity.frameIdentity = receipt.frameIdentity;
        bundle.identity.viewIdentity = receipt.viewIdentity;
        bundle.identity.graphProvenance = {
            41,
            43,
            47,
            ComputeRenderExecutionReceiptDigest(receipt)};
        for (size_t index = 0; index < bundle.attachments.size(); ++index)
        {
            const auto semantic =
                static_cast<QualificationCaptureAttachmentSemantic>(index);
            QualificationCaptureAttachment& attachment = bundle.attachments[index];
            attachment.descriptor.contract =
                GetQualificationCaptureAttachmentContract(semantic);
            attachment.descriptor.extent = {1, 1};
            const uint32 channels = attachment.descriptor.contract.format ==
                    QualificationCapturePixelFormat::RGBA32Float
                ? 4U
                : attachment.descriptor.contract.format ==
                        QualificationCapturePixelFormat::RG32Float
                    ? 2U
                    : 1U;
            attachment.descriptor.rowPitchBytes = channels * sizeof(float32);
            attachment.descriptor.provenance = {
                41,
                43,
                47,
                static_cast<uint64>(101 + index),
                static_cast<uint64>(201 + index),
                static_cast<uint64>(301 + index)};
            attachment.bytes.resize(attachment.descriptor.rowPitchBytes);
            for (uint32 channel = 0; channel < channels; ++channel)
            {
                const float32 value = 0.1f +
                    static_cast<float32>(index) * 0.01f +
                    static_cast<float32>(channel) * 0.001f;
                std::memcpy(attachment.bytes.data() + channel * sizeof(float32),
                            &value,
                            sizeof(value));
            }
            attachment.payloadHash = Hash::ComputeSHA256(
                attachment.bytes.data(), attachment.bytes.size());
        }
        return bundle;
    }

    [[nodiscard]] RenderQualityQualificationWriteRequest MakeRequest(
        const std::filesystem::path& root)
    {
        RenderQualityQualificationWriteRequest request;
        request.outputRoot = root;
        request.sampleId = "render-quality-lab";
        request.cameraBookmarkId = "hero";
        request.runId = "writer-validation-run";
        request.captureTick = 120;
        request.visualContractVersion = 1;
        request.submittedFrame = 91;
        request.completedFrame = 91;
        return request;
    }

    [[nodiscard]] std::filesystem::path MakeTemporaryRoot()
    {
        const uint64 stamp = static_cast<uint64>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        return std::filesystem::temp_directory_path() /
            ("rvx-render-quality-writer-" + std::to_string(stamp));
    }

    class TemporaryRoot final
    {
    public:
        TemporaryRoot()
            : m_path(MakeTemporaryRoot())
        {
        }

        ~TemporaryRoot()
        {
            std::error_code error;
            std::filesystem::remove_all(m_path, error);
        }

        [[nodiscard]] const std::filesystem::path& Get() const noexcept
        {
            return m_path;
        }

    private:
        std::filesystem::path m_path;
    };
} // namespace

TEST(RenderQualityQualificationWriterValidation,
     SerializesReceiptDeterministicallyAndHashesExactBytes)
{
    const RenderExecutionReceipt receipt = MakeReceipt();
    const std::string first = SerializeRenderExecutionReceiptJson(receipt);
    const std::string second = SerializeRenderExecutionReceiptJson(receipt);
    EXPECT_EQ(first, second);
    EXPECT_EQ(64U, ComputeRenderExecutionReceiptSha256(receipt).size());
}

TEST(RenderQualityQualificationWriterValidation,
     FullCanonicalDigestChangesForPreviouslyUnboundSemanticFields)
{
    const RenderExecutionReceipt receipt = MakeReceipt();
    RenderExecutionReceipt changed = receipt;
    changed.diagnostic = "distinct render semantic diagnostic";
    EXPECT_NE(ComputeRenderExecutionReceiptSha256(receipt),
              ComputeRenderExecutionReceiptSha256(changed));
}

TEST(RenderQualityQualificationWriterValidation,
     PublishesStrictBundleAfterAllRawAttachments)
{
    TemporaryRoot temporary;
    const RenderExecutionReceipt receipt = MakeReceipt();
    const QualificationCaptureBundle bundle = MakeBundle(receipt);
    RenderQualityQualificationWriteResult result;
    std::string error;
    ASSERT_TRUE(WriteRenderQualityQualificationBundle(
        MakeRequest(temporary.Get()), bundle, receipt, result, &error)) << error;
    ASSERT_TRUE(std::filesystem::exists(result.bundlePath));
    EXPECT_EQ(64U, result.renderExecutionReceiptSha256.size());

    std::ifstream manifest(result.bundlePath, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(manifest)), {});
    EXPECT_NE(std::string::npos,
              text.find("\"resourceName\": \"PostProcessOutputs.presentableColor\""));
    EXPECT_NE(std::string::npos,
              text.find("\"graphPasses\": [\"RenderQualityCapture.BeautyLDR\"]"));
    EXPECT_NE(std::string::npos, text.find("\"renderExecutionReceiptSha256\""));
    for (const char* name : {"BeautyLDR.raw", "PreToneHDR.raw", "LinearDepth.raw",
                             "Normal.raw", "Velocity.raw", "AmbientOcclusion.raw",
                             "ReactiveMask.raw"})
    {
        EXPECT_TRUE(std::filesystem::exists(temporary.Get() / name));
    }
}

TEST(RenderQualityQualificationWriterValidation,
     RejectsBundleBoundToAnotherSameFrameAndViewReceipt)
{
    TemporaryRoot temporary;
    const RenderExecutionReceipt receipt = MakeReceipt();
    const QualificationCaptureBundle bundle = MakeBundle(receipt);
    RenderExecutionReceipt differentFinalReceipt = receipt;
    // Identity deliberately remains exact; only the final semantic receipt
    // changes. A partial identity hash would have accepted this substitution.
    differentFinalReceipt.diagnostic = "different final presentation evidence";

    RenderQualityQualificationWriteResult result;
    std::string error;
    EXPECT_FALSE(WriteRenderQualityQualificationBundle(
        MakeRequest(temporary.Get()), bundle, differentFinalReceipt, result,
        &error));
    EXPECT_NE(std::string::npos, error.find("digest"));
    EXPECT_FALSE(std::filesystem::exists(temporary.Get() / "bundle.json"));
}

TEST(RenderQualityQualificationWriterValidation,
     RejectsInvalidPayloadWithoutPublishingSuccessManifest)
{
    TemporaryRoot temporary;
    const RenderExecutionReceipt receipt = MakeReceipt();
    QualificationCaptureBundle bundle = MakeBundle(receipt);
    const float32 nonFinite = std::numeric_limits<float32>::quiet_NaN();
    std::memcpy(bundle.attachments[0].bytes.data(), &nonFinite, sizeof(nonFinite));
    bundle.attachments[0].payloadHash = Hash::ComputeSHA256(
        bundle.attachments[0].bytes.data(), bundle.attachments[0].bytes.size());
    RenderQualityQualificationWriteResult result;
    std::string error;
    EXPECT_FALSE(WriteRenderQualityQualificationBundle(
        MakeRequest(temporary.Get()), bundle, receipt, result, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(std::filesystem::exists(temporary.Get() / "bundle.json"));
}

TEST(RenderQualityQualificationWriterValidation,
     RefusesToOverwriteAnExistingAttachmentOrPublishManifest)
{
    TemporaryRoot temporary;
    std::filesystem::create_directories(temporary.Get());
    {
        std::ofstream foreign(temporary.Get() / "BeautyLDR.raw",
                             std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(foreign);
        foreign << "foreign artifact";
    }

    const RenderExecutionReceipt receipt = MakeReceipt();
    const QualificationCaptureBundle bundle = MakeBundle(receipt);
    RenderQualityQualificationWriteResult result;
    std::string error;
    EXPECT_FALSE(WriteRenderQualityQualificationBundle(
        MakeRequest(temporary.Get()), bundle, receipt, result, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(std::filesystem::exists(temporary.Get() / "bundle.json"));

    std::ifstream foreign(temporary.Get() / "BeautyLDR.raw", std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(foreign)), {});
    EXPECT_EQ("foreign artifact", text);
}
} // namespace RVX
