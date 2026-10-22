#pragma once

#include "Common/GpuTestUtils.h"
#include "Common/RenderGraphValidationAccess.h"
#include "EngineDevelopmentComposition/EngineDevelopmentComposition.h"
#include "Render/Features/RenderFeatureGraphBuilder.h"
#include "Render/PipelineCache.h"
#include "Render/PostProcess/TransparentTemporalResolve.h"
#include "RenderContracts/TransparentTemporalPolicy.h"

#include <array>
#include <cstring>
#include <filesystem>

using namespace RVX;

namespace RVX::Test
{
inline void RunTransparentTemporalReference(RHIBackendType backend, bool allowHistory = true, bool rawCandidates = false)
{
    RHIDeviceDesc deviceDesc;
    auto device = RVX::Test::CreateNativeValidationDevice(backend, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, backend);
    const auto shaderDir = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
        "Source/Runtime/Render/Shaders";
    ASSERT_TRUE(std::filesystem::exists(shaderDir));
    auto provider = CreateDevelopmentShaderProgramProviderFactory(shaderDir)();
    ASSERT_NE(provider, nullptr);
    PipelineCache cache(*provider);
    ASSERT_TRUE(cache.Initialize(device.get(), shaderDir.string())) << cache.GetLastError();
    auto* pipeline = rawCandidates ? cache.GetTransparentTemporalCandidateResolvePipeline() :
        cache.GetTransparentTemporalResolvePipeline(allowHistory);
    ASSERT_NE(pipeline, nullptr) << cache.GetLastError();
    if (!rawCandidates)
    {
        EXPECT_EQ(pipeline, cache.GetTransparentTemporalResolvePipeline(allowHistory));
        EXPECT_NE(pipeline, cache.GetTransparentTemporalResolvePipeline(!allowHistory))
            << "Global history reset must not alias the history-enabled pixel entry";
    }
    auto* layout = rawCandidates ? cache.GetTransparentTemporalCandidateResolveSetLayout() :
        cache.GetTransparentTemporalResolveSetLayout();
    ASSERT_NE(layout, nullptr);
    ASSERT_EQ(layout->GetEntries().size(), rawCandidates ? 8u : 5u);

    constexpr uint32 width = 10;
    constexpr uint32 rowPitch = 256;
    std::vector<RHIFormat> formats{RHIFormat::RG16_FLOAT, RHIFormat::R8_UNORM,
        RHIFormat::RGBA16_FLOAT, RHIFormat::R32_UINT, rawCandidates ? RHIFormat::D32_FLOAT : RHIFormat::R32_FLOAT};
    std::array<uint16, width * 2> opaque{};
    std::array<uint8, width> reactive{};
    std::array<uint16, width * 4> surface{};
    // Raw raster candidates (128 | geometry-history 64) must not be admitted.
    const std::array<uint32, width> flags = rawCandidates ?
        std::array<uint32, width>{0, 227, 195, 227, 225, 243, 227, 227, 227, 163} :
        std::array<uint32, width>{0, 7, 3, 15, 5, 23, 7, 7, 195, 0};
    const std::array<uint16, width> layers{0, 0x3c00, 0x3c00, 0x4000, 0x3c00, 0x3c00, 0x3c00, 0x3c00, 0x3c00, 0x3c00};
    const std::array<uint8, width> meshReactive{0,255,255,255,255,255,255,255,255,255};
    std::array<float32, width> depths{};
    for (uint32 i = 0; i < width; ++i)
    {
        opaque[i * 2] = 0x3400;      // +0.25 in binary16
        opaque[i * 2 + 1] = 0xb400;  // -0.25
        reactive[i] = !rawCandidates || i == 0 || i == 8 ? 51 : 0;
        surface[i * 4] = 0x3800;    // +0.5
        surface[i * 4 + 1] = 0xb800;
        surface[i * 4 + 2] = i == 6 || (rawCandidates && i == 5) ? 0x3800 : 0x3c00; // contribution .5 or 1
        surface[i * 4 + 3] = i == 7 ? 0x4a00 : 0x4000; // depth 12 or 2
        depths[i] = rawCandidates ? 0.5f : 10.0f;
    }
    std::vector<const void*> payloads{
        opaque.data(), reactive.data(), surface.data(), flags.data(), depths.data()};
    std::vector<size_t> sizes{
        sizeof(opaque), sizeof(reactive), sizeof(surface), sizeof(flags), sizeof(depths)};
    if (rawCandidates)
    {
        formats.push_back(RHIFormat::R16_FLOAT); formats.push_back(RHIFormat::R8_UNORM);
        payloads.push_back(layers.data()); payloads.push_back(meshReactive.data());
        sizes.push_back(sizeof(layers)); sizes.push_back(sizeof(meshReactive));
    }
    std::vector<RHITextureRef> sources(formats.size());
    std::vector<RHITextureViewRef> views(formats.size());
    std::vector<RHIBufferRef> uploads(formats.size());
    auto commands = device->CreateCommandContext(RHICommandQueueType::Graphics);
    ASSERT_TRUE(commands);
    commands->Begin();
    for (uint32 i = 0; i < sources.size(); ++i)
    {
        auto sourceDesc = RHITextureDesc::Texture2D(width, 1, formats[i],
            RHITextureUsage::ShaderResource | RHITextureUsage::CopyDst | RHITextureUsage::CopySrc);
        if (formats[i] == RHIFormat::D32_FLOAT) sourceDesc.usage = sourceDesc.usage | RHITextureUsage::DepthStencil;
        sources[i] = device->CreateTexture(sourceDesc);
        ASSERT_TRUE(sources[i]);
        RHITextureViewDesc view;
        view.format = formats[i];
        view.type = RHITextureViewType::ShaderResource;
        views[i] = device->CreateTextureView(sources[i].Get(), view);
        ASSERT_TRUE(views[i]);
        RHIBufferDesc upload;
        upload.size = rowPitch;
        upload.usage = RHIBufferUsage::CopySrc;
        upload.memoryType = RHIMemoryType::Upload;
        uploads[i] = device->CreateBuffer(upload);
        ASSERT_TRUE(uploads[i]);
        void* mapped = uploads[i]->Map();
        ASSERT_NE(mapped, nullptr);
        std::memset(mapped, 0, rowPitch);
        std::memcpy(mapped, payloads[i], sizes[i]);
        uploads[i]->Unmap();
        RHIBufferTextureCopyDesc copy;
        copy.bufferRowPitch = rowPitch;
        copy.textureRegion = {0, 0, width, 1};
        commands->TextureBarrier(sources[i].Get(), RHIResourceState::Undefined, RHIResourceState::CopyDest);
        commands->CopyBufferToTexture(uploads[i].Get(), sources[i].Get(), copy);
        commands->TextureBarrier(sources[i].Get(), RHIResourceState::CopyDest, RHIResourceState::ShaderResource);
    }
    RenderGraph graph;
    RenderGraphValidationAccess::SetDevice(graph, device.get());
    const RenderFrameIdentity frame{1, 1, 1, 1};
    const RenderViewIdentity viewIdentity{1, 1};
    RenderFeatureGraphBuilder builder(graph, {graph.GetGraphIdentity(),
        graph.GetRecordingGeneration(), frame.frameSequence, 0, 1});
    std::vector<RGTextureHandle> imported(sources.size());
    for (uint32 i = 0; i < imported.size(); ++i)
        imported[i] = graph.ImportTexture(sources[i], RHIResourceState::ShaderResource);
    TransparentTemporalSurfaceInputs surfaceInputs{
        frame, viewIdentity, {width, 1}, imported[2], imported[3], imported[4]};
    surfaceInputs.historyAvailable = allowHistory;
    if (rawCandidates)
    {
        surfaceInputs.rawCandidates = true;
        surfaceInputs.opaqueLinearDepth = {};
        surfaceInputs.opaqueDeviceDepth = imported[4];
        surfaceInputs.layerCount = imported[5];
        surfaceInputs.meshReactive = imported[6];
        surfaceInputs.inverseProjectionMatrix = Mat4Identity();
        surfaceInputs.inverseProjectionMatrix[2][2] = 20.0f;
    }
    TransparentTemporalResolveOutputs resolved;
    std::string failure;
    auto stale = surfaceInputs;
    ++stale.frameIdentity.sceneRevision;
    EXPECT_FALSE(BuildTransparentTemporalResolve(builder, cache, frame, viewIdentity,
        imported[0], imported[1], stale, resolved, failure));
    stale = surfaceInputs;
    ++stale.flags.recordingGeneration;
    EXPECT_FALSE(BuildTransparentTemporalResolve(builder, cache, frame, viewIdentity,
        imported[0], imported[1], stale, resolved, failure));
    ASSERT_TRUE(BuildTransparentTemporalResolve(builder, cache, frame, viewIdentity,
        imported[0], imported[1], surfaceInputs, resolved, failure)) << failure;
    EXPECT_NE(resolved.velocity.index, imported[0].index);
    EXPECT_NE(resolved.reactive.index, imported[1].index);
    RenderGraphCompileOptions options;
    options.queuePolicy = RGQueuePolicy::GraphicsOnly;
    options.capabilities = device->GetCapabilities();
    options.hasCapabilitySnapshot = true;
    RenderGraphValidationAccess::Compile(graph, options);
    ASSERT_TRUE(graph.GetCompileStats().compileValid);
    RenderGraphValidationAccess::Execute(graph, *commands);
    ASSERT_TRUE(resolved.receipt->recorded.load());
    ASSERT_FALSE(resolved.receipt->failed.load());
    std::array<RHITextureRef, 3> targets;
    targets[0] = RHITextureRef(RenderGraphValidationAccess::GetTexture(graph, resolved.velocity));
    targets[1] = RHITextureRef(RenderGraphValidationAccess::GetTexture(graph, resolved.reactive));
    targets[2] = RHITextureRef(RenderGraphValidationAccess::GetTexture(graph, resolved.linearDepth));
    std::array<RHIBufferRef, 4> readbacks;
    for (uint32 i = 0; i < readbacks.size(); ++i)
    {
        RHIBufferDesc desc;
        desc.size = rowPitch;
        desc.usage = RHIBufferUsage::CopyDst;
        desc.memoryType = RHIMemoryType::Readback;
        readbacks[i] = device->CreateBuffer(desc);
        ASSERT_TRUE(readbacks[i]);
        auto* source = i < 3 ? targets[i].Get() : sources[0].Get();
        commands->TextureBarrier(source, RHIResourceState::ShaderResource,
                                 RHIResourceState::CopySource);
        RHIBufferTextureCopyDesc copy;
        copy.bufferRowPitch = rowPitch;
        copy.textureRegion = {0, 0, width, 1};
        commands->CopyTextureToBuffer(source, readbacks[i].Get(), copy);
    }
    commands->End();
    auto fence = device->CreateFence(0);
    ASSERT_TRUE(fence);
    const auto submitted = device->SubmitCommandContext(commands.Get(), fence.Get());
    ASSERT_NE(submitted, 0u);
    device->WaitForFence(fence.Get(), submitted);
    ASSERT_GE(fence->GetCompletedValue(), submitted);
    const auto* motion = static_cast<const uint16*>(readbacks[0]->Map());
    const auto* mask = static_cast<const uint8*>(readbacks[1]->Map());
    const auto* temporalDepth = static_cast<const float32*>(readbacks[2]->Map());
    const auto* unchanged = static_cast<const uint16*>(readbacks[3]->Map());
    ASSERT_NE(motion, nullptr);
    ASSERT_NE(mask, nullptr);
    ASSERT_NE(unchanged, nullptr);
    ASSERT_NE(temporalDepth, nullptr);
    const std::array<int, width> expectedReactive = rawCandidates ?
        std::array<int, width>{51, 0, 255, 255, 255, 255, 128, 255, 255, 255} :
        std::array<int, width>{51, 51, 255, 255, 255, 255, 128, 51, 255, 51};
    for (uint32 i = 0; i < width; ++i)
    {
        EXPECT_EQ(motion[i * 2], i == 1 && allowHistory ? 0x3800 : 0x3400) << i;
        EXPECT_EQ(motion[i * 2 + 1], i == 1 && allowHistory ? 0xb800 : 0xb400) << i;
        EXPECT_NEAR(static_cast<int>(mask[i]), i == 1 && !allowHistory ? 255 : expectedReactive[i], 1) << i;
        EXPECT_EQ(unchanged[i * 2], opaque[i * 2]);
        EXPECT_EQ(unchanged[i * 2 + 1], opaque[i * 2 + 1]);
        EXPECT_FLOAT_EQ(temporalDepth[i], (i == 1 || i == 2 || (rawCandidates && i == 9)) ? 2.0f : 10.0f) << i;
    }
    for (auto& readback : readbacks) readback->Unmap();
    const auto validation = device->GetNativeValidationDiagnostics();
    EXPECT_TRUE(validation.available);
    EXPECT_TRUE(validation.readComplete);
    EXPECT_EQ(validation.errorCount, 0u);
    EXPECT_EQ(validation.corruptionCount, 0u);
}
}
