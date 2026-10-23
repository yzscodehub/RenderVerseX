#pragma once

#include "Common/GpuTestUtils.h"
#include "EngineDevelopmentComposition/EngineDevelopmentComposition.h"
#include "Render/PipelineCache.h"

#include <array>
#include <bit>
#include <cstring>
#include <filesystem>
#include <vector>

namespace RVX::Test
{
inline void RunTAAStableGridReference(RHIBackendType backend)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateNativeValidationDevice(backend, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, backend);
    const auto shaderRoot = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
        "Source/Runtime/Render/Shaders";
    auto provider = CreateDevelopmentShaderProgramProviderFactory(shaderRoot)();
    ASSERT_TRUE(provider);
    PipelineCache cache(*provider);
    ASSERT_TRUE(cache.Initialize(device.get(), shaderRoot.string())) << cache.GetLastError();
    auto* pipeline = cache.GetTAAResolvePipeline(RHIFormat::RGBA32_FLOAT);
    auto* layout = cache.GetTAASetLayout();
    ASSERT_TRUE(pipeline && layout) << cache.GetLastError();

    constexpr uint32 width = 8, height = 4, pitch = 256;
    struct Case { Vec2 current, previous, motion; bool history; bool reactive;
                  bool opaque = false; bool depthReject = false; };
    const std::array<Case, 8> cases{{
        // Exact binary fractions isolate resolve arithmetic from the native
        // sampler's 8-bit fractional filtering precision. Production Halton
        // jitter is additionally exercised by the full-resolution series.
        {{0.25f, -0.25f}, {-0.375f, 0.375f}, {0, 0}, false, false},
        {{-0.375f, 0.375f}, {0.25f, -0.25f}, {0, 0}, true, true},
        {{0.25f, -0.25f}, {-0.375f, 0.375f}, {0, 0}, true, false},
        {{-0.375f, 0.375f}, {0.25f, -0.25f}, {0.5f, -0.25f}, true, false},
        {{0.25f, -0.25f}, {-0.375f, 0.375f}, {0, 0}, false, false, true},
        {{0.25f, -0.25f}, {-0.375f, 0.375f}, {0, 0}, true, false, true},
        {{-0.375f, 0.375f}, {0.25f, -0.25f}, {0.5f, -0.25f}, true, false, true},
        {{0.25f, -0.25f}, {-0.375f, 0.375f}, {0, 0}, true, false, true, true},
    }};
    for (uint32 caseIndex = 0; caseIndex < cases.size(); ++caseIndex)
    {
        SCOPED_TRACE(caseIndex);
        const auto& test = cases[caseIndex];
        const Vec2 currentRaster(-test.current.x, test.current.y);
        const Vec2 previousRaster(-test.previous.x, test.previous.y);
        const Vec2 velocityPixels = test.motion + currentRaster - previousRaster;
        const std::array<RHIFormat, 6> formats{
            RHIFormat::RGBA32_FLOAT, RHIFormat::RGBA32_FLOAT,
            RHIFormat::R32_FLOAT, RHIFormat::R32_FLOAT,
            RHIFormat::RG32_FLOAT, RHIFormat::R32_FLOAT};
        const std::array<uint32, 6> channels{4, 4, 1, 1, 2, 1};
        std::array<RHITextureRef, 6> sources;
        std::array<RHITextureViewRef, 6> srvs;
        std::array<RHIBufferRef, 6> uploads;
        auto commands = device->CreateCommandContext(RHICommandQueueType::Graphics);
        ASSERT_TRUE(commands);
        commands->Begin();
        for (uint32 i = 0; i < sources.size(); ++i)
        {
            auto desc = RHITextureDesc::Texture2D(width, height, formats[i],
                RHITextureUsage::ShaderResource | RHITextureUsage::CopyDst);
            sources[i] = device->CreateTexture(desc);
            ASSERT_TRUE(sources[i]);
            RHITextureViewDesc view;
            view.type = RHITextureViewType::ShaderResource;
            view.format = formats[i];
            srvs[i] = device->CreateTextureView(sources[i].Get(), view);
            ASSERT_TRUE(srvs[i]);
            RHIBufferDesc upload;
            upload.size = pitch * height;
            upload.usage = RHIBufferUsage::CopySrc;
            upload.memoryType = RHIMemoryType::Upload;
            uploads[i] = device->CreateBuffer(upload);
            ASSERT_TRUE(uploads[i]);
            auto* bytes = static_cast<uint8*>(uploads[i]->Map());
            ASSERT_TRUE(bytes);
            std::memset(bytes, 0, pitch * height);
            for (uint32 y = 0; y < height; ++y)
            {
                auto* row = reinterpret_cast<float32*>(bytes + y * pitch);
                for (uint32 x = 0; x < width; ++x)
                {
                    auto* p = row + x * channels[i];
                    if (i < 2)
                    {
                        // Independent affine world-color reference. Raster
                        // samples move; a stable output pixel keeps this color.
                        const float32 sx = static_cast<float32>(x) -
                            (i == 0 ? currentRaster.x + test.motion.x : 0.0f);
                        const float32 sy = static_cast<float32>(y) -
                            (i == 0 ? currentRaster.y + test.motion.y : 0.0f);
                        p[0] = 0.10f + 0.02f * sx;
                        p[1] = 0.08f + 0.025f * sy;
                        p[2] = 0.20f;
                        p[3] = 1.0f;
                        if (i == 1 && test.reactive)
                            p[0] = p[1] = p[2] = 8.0f;
                        // Opaque history was authored at scale 1, current at 2.
                        // Candidate-domain y/z is deliberately a different ratio.
                        if (i == 1 && test.opaque)
                            for (uint32 c = 0; c < 3; ++c) p[c] *= 0.5f;
                        if (i == 1 && test.depthReject)
                            p[0] += 0.004f;
                    }
                    else if (i == 4)
                    {
                        p[0] = 2.0f * velocityPixels.x / width;
                        p[1] = -2.0f * velocityPixels.y / height;
                    }
                    else
                        p[0] = i == 5 ? (test.reactive ? 1.0f : 0.0f) :
                            (test.opaque && i == 2 ? 0.75f :
                                (test.depthReject && i == 3 ? 8.0f : 2.0f));
                }
            }
            uploads[i]->Unmap();
            RHIBufferTextureCopyDesc copy;
            copy.bufferRowPitch = pitch;
            copy.textureRegion = {0, 0, width, height};
            commands->TextureBarrier(sources[i].Get(), RHIResourceState::Undefined,
                                     RHIResourceState::CopyDest);
            commands->CopyBufferToTexture(uploads[i].Get(), sources[i].Get(), copy);
            commands->TextureBarrier(sources[i].Get(), RHIResourceState::CopyDest,
                                     RHIResourceState::ShaderResource);
        }
        const std::array<float32, 24> constants{
            float32(width), float32(height), 1.0f / width, 1.0f / height,
            0.88f, 0.97f, 1.0f, 1000.0f,
            0.01f, 1.0f, 0.0f, test.history ? 1.0f : 0.0f,
            test.current.x, test.current.y, test.previous.x, test.previous.y,
            test.opaque ? 1.0f : 0.0f, test.opaque ? 1.0f : 0.0f, 0, 0,
            -1.0f, -0.5f, -1.0f, 0.0f};
        RHIBufferDesc cbDesc;
        cbDesc.size = 256; cbDesc.usage = RHIBufferUsage::Constant;
        cbDesc.memoryType = RHIMemoryType::Upload;
        auto cb = device->CreateBuffer(cbDesc);
        ASSERT_TRUE(cb);
        void* mapped = cb->Map();
        ASSERT_TRUE(mapped);
        std::memset(mapped, 0, 256);
        std::memcpy(mapped, constants.data(), sizeof(constants));
        cb->Unmap();

        const uint32 one = std::bit_cast<uint32>(1.0f);
        const std::array<uint32, 12> exposureWords{
            test.opaque ? std::bit_cast<uint32>(2.0f) : one, one,
            test.opaque ? std::bit_cast<uint32>(0.25f) : one,
            0, 0, 1, 0, 0, 1, 0, 0, 1};
        RHIBufferDesc exposureDesc;
        exposureDesc.size = sizeof(exposureWords);
        exposureDesc.stride = sizeof(exposureWords);
        exposureDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
        exposureDesc.memoryType = RHIMemoryType::Upload;
        auto exposure = device->CreateBuffer(exposureDesc);
        ASSERT_TRUE(exposure);
        mapped = exposure->Map();
        ASSERT_TRUE(mapped);
        std::memcpy(mapped, exposureWords.data(), sizeof(exposureWords));
        exposure->Unmap();
        auto sampler = device->CreateSampler(RHISamplerDesc::LinearClamp());
        ASSERT_TRUE(sampler);
        RHIDescriptorSetDesc setDesc;
        setDesc.SetLayout(layout).BindBuffer(0, cb.Get(), 0, 256);
        for (uint32 i = 0; i < srvs.size(); ++i)
            setDesc.BindTexture(i + 1, srvs[i].Get());
        setDesc.BindSampler(7, sampler.Get()).BindBuffer(8, exposure.Get());
        auto set = device->CreateDescriptorSet(setDesc);
        ASSERT_TRUE(set);

        std::array<RHITextureRef, 3> targets;
        std::array<RHITextureViewRef, 3> rtvs;
        const std::array<RHIFormat, 3> targetFormats{
            RHIFormat::RGBA32_FLOAT, RHIFormat::RGBA16_FLOAT, RHIFormat::R32_FLOAT};
        RHIRenderPassDesc pass;
        for (uint32 i = 0; i < targets.size(); ++i)
        {
            auto desc = RHITextureDesc::RenderTarget(width, height, targetFormats[i]);
            desc.usage = desc.usage | RHITextureUsage::CopySrc;
            targets[i] = device->CreateTexture(desc);
            ASSERT_TRUE(targets[i]);
            RHITextureViewDesc view;
            view.type = RHITextureViewType::RenderTarget; view.format = targetFormats[i];
            rtvs[i] = device->CreateTextureView(targets[i].Get(), view);
            ASSERT_TRUE(rtvs[i]);
            commands->TextureBarrier(targets[i].Get(), RHIResourceState::Undefined,
                                     RHIResourceState::RenderTarget);
            pass.AddColorAttachment(rtvs[i].Get(), RHILoadOp::DontCare, RHIStoreOp::Store);
        }
        pass.SetRenderArea(0, 0, width, height);
        commands->BeginRenderPass(pass);
        commands->SetPipeline(pipeline);
        commands->SetDescriptorSet(0, set.Get());
        RHIViewport viewport;
        viewport.width = width; viewport.height = height;
        viewport.minDepth = 0.0f; viewport.maxDepth = 1.0f;
        commands->SetViewport(viewport);
        RHIRect scissor;
        scissor.width = width; scissor.height = height;
        commands->SetScissor(scissor);
        commands->Draw(3, 1, 0, 0);
        commands->EndRenderPass();
        RHIBufferDesc readDesc;
        readDesc.size = pitch * height; readDesc.usage = RHIBufferUsage::CopyDst;
        readDesc.memoryType = RHIMemoryType::Readback;
        auto readback = device->CreateBuffer(readDesc);
        ASSERT_TRUE(readback);
        commands->TextureBarrier(targets[0].Get(), RHIResourceState::RenderTarget,
                                 RHIResourceState::CopySource);
        RHIBufferTextureCopyDesc copy;
        copy.bufferRowPitch = pitch; copy.textureRegion = {0, 0, width, height};
        commands->CopyTextureToBuffer(targets[0].Get(), readback.Get(), copy);
        commands->End();
        auto fence = device->CreateFence(0);
        ASSERT_TRUE(fence);
        const auto submitted = device->SubmitCommandContext(commands.Get(), fence.Get());
        ASSERT_NE(submitted, 0u);
        device->WaitForFence(fence.Get(), submitted);
        ASSERT_GE(fence->GetCompletedValue(), submitted);
        const auto* bytes = static_cast<const uint8*>(readback->Map());
        ASSERT_TRUE(bytes);
        for (uint32 y = 1; y + 1 < height; ++y)
        {
            const auto* row = reinterpret_cast<const float32*>(bytes + y * pitch);
            // The opaque moving case reprojects by 1.125 texels. Keep the
            // affine-reference ROI inside both images (not the clamp border).
            for (uint32 x = 2; x + 2 < width; ++x)
            {
                const Vec2 gridOffset(0.0f); // Both history domains are stable-grid.
                EXPECT_NEAR(row[x * 4], 0.10f + 0.02f * (x - test.motion.x - gridOffset.x), 0.00002f);
                EXPECT_NEAR(row[x * 4 + 1], 0.08f + 0.025f * (y - test.motion.y - gridOffset.y), 0.00002f);
                EXPECT_NEAR(row[x * 4 + 2], 0.20f, 0.00002f);
            }
        }
        readback->Unmap();
    }
    const auto diagnostics = device->GetNativeValidationDiagnostics();
    EXPECT_TRUE(diagnostics.available && diagnostics.readComplete);
    EXPECT_EQ(diagnostics.warningCount, 0u);
    EXPECT_EQ(diagnostics.errorCount, 0u);
    EXPECT_EQ(diagnostics.corruptionCount, 0u);
}
}
