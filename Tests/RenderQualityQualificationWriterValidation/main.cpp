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
#include <vector>

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
        const RenderExecutionReceipt& receipt,
        const uint64 bundleId = 31)
    {
        QualificationCaptureBundle bundle;
        bundle.identity.bundleId = bundleId;
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
        const std::filesystem::path& root,
        const uint64 captureTick = 120,
        const uint64 frameSequence = 91)
    {
        RenderQualityQualificationWriteRequest request;
        request.outputRoot = root;
        request.sampleId = "render-quality-lab";
        request.cameraBookmarkId = "hero";
        request.runId = "writer-validation-run";
        request.renderPath = "direct";
        request.instancingMode = "Auto";
        request.captureTick = captureTick;
        request.visualContractVersion = 1;
        request.submittedFrame = frameSequence;
        request.completedFrame = frameSequence;
        return request;
    }

    [[nodiscard]] std::string ComputeTestRenderConfigurationHash(
        const RenderQualityAttachmentSeriesConfigurationIdentity& configuration)
    {
        std::string binding;
        const auto append = [&binding](const char* name,
                                       const std::string& value)
        {
            binding += name;
            binding += ":";
            binding += std::to_string(value.size());
            binding += ":";
            binding += value;
            binding += "\n";
        };
        const auto appendNumber = [&append](const char* name, uint64 value)
        {
            append(name, std::to_string(value));
        };
        append("schema", "RVX.RenderQualityConfigurationIdentity");
        appendNumber("schemaVersion", 1U);
        append("renderPath", configuration.renderPath);
        append("instancingMode", configuration.instancingMode);
        append("requestedRecipe", configuration.requestedRecipe);
        append("resolvedRecipe", configuration.resolvedRecipe);
        append("quality", "high");
        append("backend", "dx12");
        appendNumber("profileHash", configuration.profileHash);
        return Hash::FormatSHA256Digest(Hash::ComputeSHA256(binding));
    }

    [[nodiscard]] RenderQualityAttachmentSeriesWriteRequest MakeSeriesRequest(
        const std::filesystem::path& root,
        std::vector<RenderQualityAttachmentSeriesTick> ticks,
        RenderQualityAttachmentSeriesConfigurationIdentity configuration =
            {})
    {
        if (configuration.renderPath.empty())
        {
            configuration.renderPath = "direct";
            configuration.instancingMode = "Auto";
            configuration.requestedRecipe = "deferred";
            configuration.resolvedRecipe = "deferred";
            configuration.profileHash = ComputeRenderPipelineProfileHash(
                RenderPipelineRecipe::DeferredQuality,
                RenderQualityTier::High);
            configuration.renderConfigurationHash =
                ComputeTestRenderConfigurationHash(configuration);
        }
        RenderQualityAttachmentSeriesWriteRequest request;
        request.outputPath = root / "series.json";
        request.sourceCommit = std::string(40, 'a');
        request.runId = "writer-validation-run";
        request.preset = "win_x64_gfx24_debug";
        request.qualificationIdentity = {
            .sampleId = "render-quality-lab",
            .pipeline = "deferred",
            .quality = "high",
            .backend = "dx12",
            .view = "hero",
            .visualContractVersion = 1};
        request.hardwareIdentity = {
            .backend = "dx12",
            .adapterName = "RVX Test Adapter",
            .driverVersion = "1.2.3",
            .osBuild = "Windows-10.0.19045",
            .capabilityHash = std::string(64, 'b')};
        request.renderConfigurationIdentity = std::move(configuration);
        request.manifestSha256 = std::string(64, 'c');
        request.ticks = std::move(ticks);
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

TEST(RenderQualityQualificationWriterValidation,
     RejectsMissingOrForgedRuntimeConfigurationIdentity)
{
    TemporaryRoot temporary;
    const RenderExecutionReceipt receipt = MakeReceipt();
    const QualificationCaptureBundle bundle = MakeBundle(receipt);
    RenderQualityQualificationWriteResult result;
    std::string error;

    RenderQualityQualificationWriteRequest missing =
        MakeRequest(temporary.Get() / "missing");
    missing.renderPath.clear();
    EXPECT_FALSE(WriteRenderQualityQualificationBundle(
        missing, bundle, receipt, result, &error));
    EXPECT_NE(std::string::npos, error.find("runtime configuration"));

    ASSERT_TRUE(WriteRenderQualityQualificationBundle(
        MakeRequest(temporary.Get() / "tick-000120"), bundle, receipt,
        result, &error)) << error;
    RenderQualityAttachmentSeriesTick tick{
        .captureTick = 120U,
        .bundleId = 31U,
        .bundlePath = "tick-000120/bundle.json",
        .bundleSha256 = result.bundleSha256,
        .receiptSha256 = result.renderExecutionReceiptSha256,
        .frameSequence = receipt.frameIdentity.frameSequence,
        .submittedFrame = receipt.frameIdentity.frameSequence,
        .completedFrame = receipt.frameIdentity.frameSequence};
    RenderQualityAttachmentSeriesConfigurationIdentity forged =
        result.renderConfigurationIdentity;
    forged.renderPath = "gpu-driven";
    forged.renderConfigurationHash =
        ComputeTestRenderConfigurationHash(forged);
    RenderQualityAttachmentSeriesWriteResult seriesResult;
    EXPECT_FALSE(WriteRenderQualityAttachmentSeries(
        MakeSeriesRequest(temporary.Get(), {tick}, forged), seriesResult,
        &error));
    EXPECT_NE(std::string::npos, error.find("Bundle V2"));
}

TEST(RenderQualityQualificationWriterValidation,
     PublishesManifestLastForStrictlyIncreasingAttachmentSeries)
{
    TemporaryRoot temporary;
    std::vector<RenderQualityAttachmentSeriesTick> ticks;
    RenderQualityAttachmentSeriesConfigurationIdentity configuration;
    constexpr std::array<uint64, 3> CaptureTicks{120U, 360U, 361U};
    for (size_t index = 0; index < CaptureTicks.size(); ++index)
    {
        RenderExecutionReceipt receipt = MakeReceipt();
        receipt.frameIdentity.frameSequence = 91U + index;
        const std::string directory = "tick-" +
            std::string(index == 0U ? "000120" : index == 1U ? "000360"
                                                              : "000361");
        const QualificationCaptureBundle bundle =
            MakeBundle(receipt, 31U + index);
        RenderQualityQualificationWriteResult bundleResult;
        std::string error;
        ASSERT_TRUE(WriteRenderQualityQualificationBundle(
            MakeRequest(temporary.Get() / directory,
                        CaptureTicks[index],
                        receipt.frameIdentity.frameSequence),
            bundle, receipt, bundleResult, &error)) << error;
        ASSERT_TRUE(std::filesystem::exists(bundleResult.bundlePath));
        if (index == 0U)
        {
            configuration = bundleResult.renderConfigurationIdentity;
        }
        else
        {
            EXPECT_EQ(configuration,
                      bundleResult.renderConfigurationIdentity);
        }
        ticks.push_back({.captureTick = CaptureTicks[index],
                         .bundleId = 31U + index,
                         .bundlePath = directory + "/bundle.json",
                         .bundleSha256 = bundleResult.bundleSha256,
                         .receiptSha256 =
                             bundleResult.renderExecutionReceiptSha256,
                         .frameSequence = receipt.frameIdentity.frameSequence,
                         .submittedFrame = receipt.frameIdentity.frameSequence,
                         .completedFrame = receipt.frameIdentity.frameSequence});
    }

    EXPECT_FALSE(std::filesystem::exists(temporary.Get() / "series.json"));
    RenderQualityAttachmentSeriesWriteResult seriesResult;
    std::string error;
    ASSERT_TRUE(WriteRenderQualityAttachmentSeries(
        MakeSeriesRequest(temporary.Get(), ticks, configuration), seriesResult,
        &error)) << error;
    ASSERT_TRUE(std::filesystem::exists(seriesResult.seriesPath));
    EXPECT_EQ(64U, seriesResult.seriesSha256.size());

    std::ifstream manifest(seriesResult.seriesPath, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(manifest)), {});
    EXPECT_NE(std::string::npos,
              text.find("\"schema\": \"RVX.RenderQualityAttachmentSeries\""));
    EXPECT_NE(std::string::npos, text.find("tick-000120/bundle.json"));
    EXPECT_NE(std::string::npos, text.find("tick-000360/bundle.json"));
    EXPECT_NE(std::string::npos, text.find("tick-000361/bundle.json"));
    EXPECT_NE(std::string::npos,
              text.find("\"renderConfigurationIdentity\""));
}

TEST(RenderQualityQualificationWriterValidation,
     RejectsUnorderedEscapingAndHashMismatchedAttachmentSeries)
{
    TemporaryRoot temporary;
    RenderExecutionReceipt receipt = MakeReceipt();
    const QualificationCaptureBundle bundle = MakeBundle(receipt, 31U);
    RenderQualityQualificationWriteResult bundleResult;
    std::string error;
    ASSERT_TRUE(WriteRenderQualityQualificationBundle(
        MakeRequest(temporary.Get() / "tick-000120"), bundle, receipt,
        bundleResult, &error)) << error;

    RenderQualityAttachmentSeriesTick first{
        .captureTick = 120U,
        .bundleId = 31U,
        .bundlePath = "tick-000120/bundle.json",
        .bundleSha256 = bundleResult.bundleSha256,
        .receiptSha256 =
            bundleResult.renderExecutionReceiptSha256,
        .frameSequence = 91U,
        .submittedFrame = 91U,
        .completedFrame = 91U};
    RenderQualityAttachmentSeriesTick duplicate = first;
    duplicate.bundleId = 32U;
    duplicate.frameSequence = 92U;
    duplicate.submittedFrame = 92U;
    duplicate.completedFrame = 92U;
    RenderQualityAttachmentSeriesWriteResult seriesResult;
    EXPECT_FALSE(WriteRenderQualityAttachmentSeries(
        MakeSeriesRequest(temporary.Get(), {first, duplicate}), seriesResult,
        &error));
    EXPECT_FALSE(std::filesystem::exists(temporary.Get() / "series.json"));

    RenderQualityAttachmentSeriesTick escaping = first;
    escaping.bundlePath = "../bundle.json";
    EXPECT_FALSE(WriteRenderQualityAttachmentSeries(
        MakeSeriesRequest(temporary.Get(), {escaping}), seriesResult, &error));
    EXPECT_FALSE(std::filesystem::exists(temporary.Get() / "series.json"));

    RenderQualityAttachmentSeriesTick badHash = first;
    badHash.bundleSha256 = std::string(64, 'd');
    EXPECT_FALSE(WriteRenderQualityAttachmentSeries(
        MakeSeriesRequest(temporary.Get(), {badHash}), seriesResult, &error));
    EXPECT_FALSE(std::filesystem::exists(temporary.Get() / "series.json"));

    const std::string invalidSchema = "{\"schema\":\"invalid\"}";
    {
        std::ofstream corrupt(temporary.Get() / "tick-000120" / "bundle.json",
                              std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(corrupt);
        corrupt << invalidSchema;
    }
    RenderQualityAttachmentSeriesTick badSchema = first;
    badSchema.bundleSha256 = Hash::FormatSHA256Digest(
        Hash::ComputeSHA256(invalidSchema));
    EXPECT_FALSE(WriteRenderQualityAttachmentSeries(
        MakeSeriesRequest(temporary.Get(), {badSchema}), seriesResult, &error));
    EXPECT_FALSE(std::filesystem::exists(temporary.Get() / "series.json"));
}
} // namespace RVX
