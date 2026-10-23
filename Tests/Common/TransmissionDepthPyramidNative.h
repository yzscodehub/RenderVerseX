#pragma once

#include "Common/GpuTestUtils.h"
#include "Common/RenderGraphValidationAccess.h"
#include "EngineDevelopmentComposition/EngineDevelopmentComposition.h"
#include "Render/Features/RenderFeatureGraphBuilder.h"
#include "Render/PipelineCache.h"
#include "Render/PostProcess/TransmissionDepthPyramid.h"
#include "RenderContracts/TransmissionFootprint.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace RVX;

namespace RVX::Test
{
namespace TransmissionDepthPyramidNative
{
    constexpr uint32 SourceWidth = 13u;
    constexpr uint32 SourceHeight = 7u;
    constexpr uint32 RowPitch = 256u;

    struct SnapshotCopyData
    {
        RGTextureHandle source{};
        RGTextureHandle destination{};
        std::shared_ptr<SceneDepthSnapshotCopyReceipt> receipt{};
    };

    [[nodiscard]] inline RenderPassRecordIdentity MakeIdentity(
        RenderGraph& graph,
        const RenderGraphBlackboardProvenance& provenance) noexcept
    {
        return {&graph,
                graph.GetGraphIdentity(),
                graph.GetRecordingGeneration(),
                provenance.frameSequence,
                provenance.viewOrdinal,
                provenance.recordEpoch};
    }

    [[nodiscard]] inline RenderOutputExtent MipExtent(
        RenderOutputExtent base,
        uint32 mip) noexcept
    {
        for (uint32 index = 0; index < mip; ++index)
        {
            base.width = std::max(1u, base.width / 2u);
            base.height = std::max(1u, base.height / 2u);
        }
        return base;
    }

    [[nodiscard]] inline float32 SourceCanonicalDepth(
        uint32 x,
        uint32 y) noexcept
    {
        // A non-monotonic NPOT pattern makes omitted edge contributors visible.
        const float32 gradient = 0.92f - 0.031f * static_cast<float32>(x) -
            0.047f * static_cast<float32>(y);
        return (x == SourceWidth - 1u && y == SourceHeight - 1u) ?
            0.035f : gradient;
    }

    [[nodiscard]] inline std::vector<std::vector<float32>>
    BuildExpectedMips(
        RenderQualityTier quality,
        bool reverseZ)
    {
        const RenderOutputExtent sourceExtent{SourceWidth, SourceHeight};
        const bool fullResolutionBase = quality == RenderQualityTier::Ultra;
        const RenderOutputExtent base = fullResolutionBase ? sourceExtent :
            RenderOutputExtent{(SourceWidth + 1u) / 2u,
                               (SourceHeight + 1u) / 2u};
        const uint32 mipCount = CalculateOpaqueSceneColorPyramidMipCount(base);
        std::vector<std::vector<float32>> results;
        results.reserve(mipCount);
        std::vector<float32> source(static_cast<size_t>(SourceWidth) * SourceHeight);
        for (uint32 y = 0; y < SourceHeight; ++y)
        {
            for (uint32 x = 0; x < SourceWidth; ++x)
            {
                source[static_cast<size_t>(y) * SourceWidth + x] =
                    SourceCanonicalDepth(x, y);
            }
        }

        for (uint32 mip = 0; mip < mipCount; ++mip)
        {
            const RenderOutputExtent targetExtent = MipExtent(base, mip);
            const RenderOutputExtent inputExtent = mip == 0 ? sourceExtent :
                MipExtent(base, mip - 1u);
            const std::vector<float32>& input = mip == 0 ? source : results.back();
            const bool fullResolution = mip == 0 && fullResolutionBase;
            std::vector<float32> output(
                static_cast<size_t>(targetExtent.width) * targetExtent.height,
                1.0f);
            for (uint32 y = 0; y < targetExtent.height; ++y)
            {
                for (uint32 x = 0; x < targetExtent.width; ++x)
                {
                    const Vec2 uv(
                        (static_cast<float32>(x) + 0.5f) /
                            static_cast<float32>(targetExtent.width),
                        (static_cast<float32>(y) + 0.5f) /
                            static_cast<float32>(targetExtent.height));
                    TransmissionTexelBounds bounds =
                        ResolveTransmissionReductionFootprint(
                            uv,
                            {inputExtent.width, inputExtent.height},
                            fullResolution);
                    if (fullResolution)
                    {
                        bounds.first = {x > 0 ? x - 1 : 0, y > 0 ? y - 1 : 0};
                        bounds.last = {std::min(x + 2, inputExtent.width - 1),
                                       std::min(y + 2, inputExtent.height - 1)};
                    }
                    float32 nearest = 1.0f;
                    for (uint32 inputY = bounds.first.y;
                         inputY <= bounds.last.y;
                         ++inputY)
                    {
                        for (uint32 inputX = bounds.first.x;
                             inputX <= bounds.last.x;
                             ++inputX)
                        {
                            float32 depth = input[static_cast<size_t>(inputY) *
                                inputExtent.width + inputX];
                            if (mip == 0)
                            {
                                depth = CanonicalTransmissionDepth(
                                    reverseZ ? 1.0f - depth : depth,
                                    reverseZ);
                            }
                            nearest = std::min(nearest, depth);
                        }
                    }
                    output[static_cast<size_t>(y) * targetExtent.width + x] =
                        nearest;
                }
            }
            results.push_back(std::move(output));
        }
        return results;
    }

    inline void AddActualSnapshotCopy(
        RenderFeatureGraphBuilder& builder,
        RGTextureHandle source,
        RGTextureHandle destination,
        const std::shared_ptr<SceneDepthSnapshotCopyReceipt>& receipt)
    {
        ASSERT_TRUE(builder.AddPass<SnapshotCopyData>(
            "TransmissionDepthPyramidNativeSnapshotCopy",
            RenderGraphPassType::Graphics,
            [source, destination, receipt](RenderGraphBuilder& graphBuilder,
                                           SnapshotCopyData& data)
            {
                data.source = graphBuilder.Read(
                    source, RHIResourceState::CopySource);
                data.destination = graphBuilder.Write(
                    destination, RHIResourceState::CopyDest);
                graphBuilder.SetExportState(
                    data.destination, RHIResourceState::ShaderResource);
                data.receipt = receipt;
            },
            [](const SnapshotCopyData& data,
               RenderGraphPassContext& context)
            {
                RHITexture* const source = context.GetTexture(data.source);
                RHITexture* const destination =
                    context.GetTexture(data.destination);
                if (data.receipt == nullptr || source == nullptr ||
                    destination == nullptr || source == destination ||
                    source->GetFormat() != RHIFormat::D32_FLOAT ||
                    destination->GetFormat() != RHIFormat::D32_FLOAT)
                {
                    context.MarkExecutionFailed(
                        "Transmission depth native snapshot copy is invalid");
                    return;
                }
                context.Commands().CopyTexture(source, destination);
                data.receipt->copied.store(true, std::memory_order_release);
            }));
    }

    inline void VerifyMissingCopyFails(
        IRHIDevice& device,
        PipelineCache& cache,
        RenderQualityTier quality,
        bool reverseZ)
    {
        RenderGraph graph;
        RenderGraphValidationAccess::SetDevice(graph, &device);
        const RenderGraphBlackboardProvenance provenance{
            graph.GetGraphIdentity(), graph.GetRecordingGeneration(), 71u, 0u, 1u};
        RenderFeatureGraphBuilder builder(graph, provenance);
        RHITextureDesc snapshotDescription = RHITextureDesc::DepthStencil(
            SourceWidth, SourceHeight, RHIFormat::D32_FLOAT);
        snapshotDescription.usage = snapshotDescription.usage |
            RHITextureUsage::ShaderResource | RHITextureUsage::CopyDst;
        const RGTextureHandle snapshot = builder.CreateTexture(snapshotDescription);
        const auto copyReceipt = std::make_shared<SceneDepthSnapshotCopyReceipt>();
        const SceneDepthSnapshotRecordInput source{
            MakeIdentity(graph, provenance), snapshot,
            {SourceWidth, SourceHeight}, RHIFormat::D32_FLOAT,
            RHISampleCount::Count1, copyReceipt};
        TransmissionDepthPyramidOutput output;
        std::string failure;
        ASSERT_TRUE(BuildTransmissionDepthPyramid(
            builder, cache, source, quality, reverseZ, output, failure)) << failure;
        auto commands = device.CreateCommandContext(RHICommandQueueType::Graphics);
        ASSERT_TRUE(commands);
        commands->Begin();
        RenderGraphCompileOptions options;
        options.queuePolicy = RGQueuePolicy::GraphicsOnly;
        options.capabilities = device.GetCapabilities();
        options.hasCapabilitySnapshot = true;
        RenderGraphValidationAccess::Compile(graph, options);
        // A transient source without a producer is rejected before execution.
        EXPECT_FALSE(graph.GetCompileStats().compileValid);
        commands->End();
        EXPECT_FALSE(copyReceipt->copied.load(std::memory_order_acquire));
        EXPECT_EQ(output.receipt->completedMipCount.load(std::memory_order_acquire), 0u);
        EXPECT_FALSE(output.receipt->completed.load(std::memory_order_acquire));
    }
}

inline void RunTransmissionDepthPyramidGraph(
    RHIBackendType backend,
    RenderQualityTier quality,
    bool reverseZ)
{
    using namespace TransmissionDepthPyramidNative;

    RHIDeviceDesc deviceDescription;
    auto device = RVX::Test::CreateNativeValidationDevice(
        backend, deviceDescription);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, backend);
    const std::filesystem::path shaderDirectory =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
        "Source/Runtime/Render/Shaders";
    ASSERT_TRUE(std::filesystem::exists(shaderDirectory));
    auto provider = CreateDevelopmentShaderProgramProviderFactory(
        shaderDirectory)();
    ASSERT_NE(provider, nullptr);
    PipelineCache cache(*provider);
    ASSERT_TRUE(cache.Initialize(device.get(), shaderDirectory.string())) <<
        cache.GetLastError();
    ASSERT_NE(cache.GetTransmissionDepthPyramidPipeline(), nullptr) <<
        cache.GetLastError();

    auto commands = device->CreateCommandContext(RHICommandQueueType::Graphics);
    ASSERT_TRUE(commands);
    RHITextureDesc originalDescription = RHITextureDesc::DepthStencil(
        SourceWidth, SourceHeight, RHIFormat::D32_FLOAT);
    originalDescription.usage = originalDescription.usage |
        RHITextureUsage::ShaderResource | RHITextureUsage::CopySrc |
        RHITextureUsage::CopyDst;
    RHITextureRef originalDepth = device->CreateTexture(originalDescription);
    ASSERT_TRUE(originalDepth);
    RHIBufferDesc uploadDescription;
    uploadDescription.size = static_cast<uint64>(RowPitch) * SourceHeight;
    uploadDescription.usage = RHIBufferUsage::CopySrc;
    uploadDescription.memoryType = RHIMemoryType::Upload;
    uploadDescription.debugName = "TransmissionDepthPyramidNativeUpload";
    RHIBufferRef upload = device->CreateBuffer(uploadDescription);
    ASSERT_TRUE(upload);
    void* const mappedUpload = upload->Map();
    ASSERT_NE(mappedUpload, nullptr);
    std::memset(mappedUpload, 0, static_cast<size_t>(uploadDescription.size));
    for (uint32 y = 0; y < SourceHeight; ++y)
    {
        auto* const row = reinterpret_cast<float32*>(
            static_cast<uint8*>(mappedUpload) + static_cast<size_t>(y) * RowPitch);
        for (uint32 x = 0; x < SourceWidth; ++x)
        {
            const float32 canonical = SourceCanonicalDepth(x, y);
            row[x] = reverseZ ? 1.0f - canonical : canonical;
        }
    }
    upload->Unmap();

    commands->Begin();
    commands->TextureBarrier(
        originalDepth.Get(), RHIResourceState::Undefined, RHIResourceState::CopyDest);
    RHIBufferTextureCopyDesc uploadCopy;
    uploadCopy.bufferRowPitch = RowPitch;
    uploadCopy.bufferImageHeight = SourceHeight;
    uploadCopy.textureRegion = {0, 0, SourceWidth, SourceHeight};
    commands->CopyBufferToTexture(upload.Get(), originalDepth.Get(), uploadCopy);

    RenderGraph graph;
    RenderGraphValidationAccess::SetDevice(graph, device.get());
    const RenderGraphBlackboardProvenance provenance{
        graph.GetGraphIdentity(), graph.GetRecordingGeneration(), 70u, 0u, 1u};
    RenderFeatureGraphBuilder builder(graph, provenance);
    const RGTextureHandle original = builder.ImportTexture(
        originalDepth, RHIResourceState::CopyDest);
    RHITextureDesc snapshotDescription = RHITextureDesc::DepthStencil(
        SourceWidth, SourceHeight, RHIFormat::D32_FLOAT);
    snapshotDescription.usage = snapshotDescription.usage |
        RHITextureUsage::ShaderResource | RHITextureUsage::CopyDst;
    snapshotDescription.debugName = "TransmissionDepthPyramidNativeSnapshot";
    const RGTextureHandle snapshot = builder.CreateTexture(snapshotDescription);
    const auto copyReceipt = std::make_shared<SceneDepthSnapshotCopyReceipt>();
    AddActualSnapshotCopy(builder, original, snapshot, copyReceipt);
    const SceneDepthSnapshotRecordInput source{
        MakeIdentity(graph, provenance), snapshot,
        {SourceWidth, SourceHeight}, RHIFormat::D32_FLOAT,
        RHISampleCount::Count1, copyReceipt};

    TransmissionDepthPyramidOutput output;
    std::string failure;
    SceneDepthSnapshotRecordInput stale = source;
    ++stale.identity.frameSequence;
    EXPECT_FALSE(BuildTransmissionDepthPyramid(
        builder, cache, stale, quality, reverseZ, output, failure));
    ASSERT_TRUE(BuildTransmissionDepthPyramid(
        builder, cache, source, quality, reverseZ, output, failure)) << failure;
    const RenderOutputExtent expectedBase = quality == RenderQualityTier::Ultra
        ? RenderOutputExtent{SourceWidth, SourceHeight}
        : RenderOutputExtent{(SourceWidth + 1u) / 2u,
                             (SourceHeight + 1u) / 2u};
    EXPECT_EQ(output.baseExtent, expectedBase);
    ASSERT_EQ(output.mipCount,
              CalculateOpaqueSceneColorPyramidMipCount(output.baseExtent));

    RenderGraphCompileOptions options;
    options.queuePolicy = RGQueuePolicy::GraphicsOnly;
    options.capabilities = device->GetCapabilities();
    options.hasCapabilitySnapshot = true;
    RenderGraphValidationAccess::Compile(graph, options);
    ASSERT_TRUE(graph.GetCompileStats().compileValid);
    RenderGraphValidationAccess::Execute(graph, *commands);
    ASSERT_TRUE(copyReceipt->copied.load(std::memory_order_acquire));
    ASSERT_FALSE(output.receipt->failed.load(std::memory_order_acquire));
    ASSERT_TRUE(output.receipt->completed.load(std::memory_order_acquire));
    ASSERT_EQ(output.receipt->completedMipCount.load(std::memory_order_acquire),
              output.mipCount);

    RHITextureRef pyramidTexture(
        RenderGraphValidationAccess::GetTexture(graph, output.texture));
    ASSERT_TRUE(pyramidTexture);
    commands->TextureBarrier(pyramidTexture.Get(), RHIResourceState::ShaderResource,
                             RHIResourceState::CopySource);
    std::vector<RHIBufferRef> readbacks;
    readbacks.reserve(output.mipCount);
    for (uint32 mip = 0; mip < output.mipCount; ++mip)
    {
        const RenderOutputExtent extent = MipExtent(output.baseExtent, mip);
        RHIBufferDesc description;
        description.size = static_cast<uint64>(RowPitch) * extent.height;
        description.usage = RHIBufferUsage::CopyDst;
        description.memoryType = RHIMemoryType::Readback;
        description.debugName = "TransmissionDepthPyramidNativeReadback";
        RHIBufferRef readback = device->CreateBuffer(description);
        ASSERT_TRUE(readback);
        RHIBufferTextureCopyDesc copy;
        copy.bufferRowPitch = RowPitch;
        copy.bufferImageHeight = extent.height;
        copy.textureSubresource = mip;
        copy.textureRegion = {0, 0, extent.width, extent.height};
        commands->CopyTextureToBuffer(pyramidTexture.Get(), readback.Get(), copy);
        readbacks.push_back(std::move(readback));
    }
    commands->End();
    RHIFenceRef fence = device->CreateFence(0);
    ASSERT_TRUE(fence);
    const uint64 submitted = device->SubmitCommandContext(commands.Get(), fence.Get());
    ASSERT_NE(submitted, 0u);
    device->WaitForFence(fence.Get(), submitted);
    ASSERT_GE(fence->GetCompletedValue(), submitted);

    const auto expected = BuildExpectedMips(quality, reverseZ);
    ASSERT_EQ(expected.size(), readbacks.size());
    for (uint32 mip = 0; mip < output.mipCount; ++mip)
    {
        const RenderOutputExtent extent = MipExtent(output.baseExtent, mip);
        const auto* const values = static_cast<const float32*>(readbacks[mip]->Map());
        ASSERT_NE(values, nullptr);
        for (uint32 y = 0; y < extent.height; ++y)
        {
            const auto* const row = reinterpret_cast<const float32*>(
                reinterpret_cast<const uint8*>(values) + static_cast<size_t>(y) * RowPitch);
            for (uint32 x = 0; x < extent.width; ++x)
            {
                EXPECT_NEAR(row[x], expected[mip][static_cast<size_t>(y) *
                    extent.width + x], 1.0e-5f) << "mip=" << mip << " x=" << x << " y=" << y;
            }
        }
        readbacks[mip]->Unmap();
    }
    const auto validation = device->GetNativeValidationDiagnostics();
    EXPECT_TRUE(validation.available);
    EXPECT_TRUE(validation.readComplete);
    EXPECT_EQ(validation.errorCount, 0u);
    EXPECT_EQ(validation.corruptionCount, 0u);

    VerifyMissingCopyFails(*device, cache, quality, reverseZ);
}
} // namespace RVX::Test
