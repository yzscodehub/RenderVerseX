#pragma once

#include "Common/GpuTestUtils.h"
#include "EngineDevelopmentComposition/EngineDevelopmentComposition.h"
#include "Render/Material/MaterialGPUData.h"
#include "Render/PipelineCache.h"
#include "Render/Renderer/ViewData.h"

#include <array>
#include <cstring>
#include <filesystem>

namespace RVX::Test
{
inline void RunTransparentTemporalRaster(RHIBackendType backend)
{
    auto device = CreateNativeValidationDevice(backend, RHIDeviceDesc{});
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, backend);
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
        "Source/Runtime/Render/Shaders";
    auto provider = CreateDevelopmentShaderProgramProviderFactory(root)();
    ASSERT_TRUE(provider);
    PipelineCache cache(*provider);
    ASSERT_TRUE(cache.Initialize(device.get(), root.string())) << cache.GetLastError();

    const auto upload = [&](const void* data, size_t bytes, RHIBufferUsage usage, uint32 stride)
    {
        RHIBufferDesc desc;
        desc.size = bytes; desc.usage = usage; desc.stride = stride;
        desc.memoryType = RHIMemoryType::Upload;
        auto buffer = device->CreateBuffer(desc);
        if (buffer)
        {
            void* mapped = buffer->Map();
            if (!mapped) return RHIBufferRef{};
            std::memcpy(mapped, data, bytes);
            buffer->Unmap();
        }
        return buffer;
    };
    const float positions[][3] = {{-.75f,-.75f,.5f},{.75f,-.75f,.5f},
                                   {.75f,.75f,.5f},{-.75f,.75f,.5f}};
    const float uv[][2] = {{0,0},{1,0},{1,1},{0,1}};
    const uint32 indices[] = {0,1,2,0,2,3};
    const uint32 bones[4][4]{};
    const float weights[4][4] = {{1,0,0,0},{1,0,0,0},{1,0,0,0},{1,0,0,0}};
    auto vb = upload(positions, sizeof(positions), RHIBufferUsage::Vertex, 12);
    auto uvs = upload(uv, sizeof(uv), RHIBufferUsage::Vertex, 8);
    auto ib = upload(indices, sizeof(indices), RHIBufferUsage::Index, 4);
    auto boneBuffer = upload(bones, sizeof(bones), RHIBufferUsage::Vertex, 16);
    auto weightBuffer = upload(weights, sizeof(weights), RHIBufferUsage::Vertex, 16);
    ASSERT_TRUE(vb && uvs && ib && boneBuffer && weightBuffer);
    Mat4 currentPalette = Mat4Identity(), previousPalette = Mat4Identity();
    previousPalette[3].x = -.125f;
    const auto paletteUsage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
    SkinningPaletteBufferBinding palettes;
    palettes.currentPaletteBuffer = upload(&currentPalette, sizeof(Mat4), paletteUsage, sizeof(Mat4));
    palettes.previousPaletteBuffer = upload(&previousPalette, sizeof(Mat4), paletteUsage, sizeof(Mat4));
    ASSERT_TRUE(palettes.currentPaletteBuffer && palettes.previousPaletteBuffer);

    struct Case { float alpha; bool history; bool weighted; bool refractive;
                  uint32 layers; float depthClear; bool textureZero; bool skinned; bool additive = false;
                  bool materialHistory = false; };
    const std::array<Case, 12> cases{{
        {1,true,false,false,2,1,false,false},
        {.9999f,true,false,false,1,1,false,false},
        {1,false,false,false,1,1,false,false},
        {.5f,true,true,false,2,1,false,false},
        {.5f,true,false,true,1,1,false,false},
        {0,true,false,false,1,1,false,false},
        {1,true,false,false,1,.25f,false,false},
        {1,true,false,false,1,1,true,false},
        {1,true,false,false,1,1,false,true},
        {.5f,true,false,false,1,1,false,false,true},
        {0,true,false,false,1,1,false,false,true},
        {1,true,false,false,1,1,false,false,false,true}
    }};
    constexpr uint32 extent = 8, pitch = 256;
    for (size_t caseIndex = 0; caseIndex < cases.size(); ++caseIndex)
    {
        SCOPED_TRACE(caseIndex);
        const auto& test = cases[caseIndex];
        const auto mode = test.skinned ? DefaultLitDirectVertexInputMode::Skinned :
                                        DefaultLitDirectVertexInputMode::Rigid;
        auto* pipeline = cache.GetTransparentTemporalRasterPipeline(
            mode, MaterialCullMode::NoCull, false, test.weighted);
        ASSERT_NE(pipeline, nullptr) << cache.GetLastError();
        EXPECT_EQ(pipeline, cache.GetTransparentTemporalRasterPipeline(
            mode, MaterialCullMode::NoCull, false, test.weighted));

        ViewData view;
        view.viewportWidth = extent; view.viewportHeight = extent;
        view.cameraForward = Vec3(0,0,1);
        RasterDrawBindingSnapshot binding;
        ASSERT_TRUE(test.skinned
            ? cache.CreateRasterDrawBindingSnapshot(view, 1, palettes, binding)
            : cache.CreateRasterDrawBindingSnapshot(view, 1, binding));
        Mat4 world = Mat4Identity();
        // Interior of the expected binary16 quantization cells, avoiding an
        // exact cell boundary perturbed by raster interpolation cancellation.
        // Keep exact encoded assertions below; no tolerance is widened.
        world[3].x = .25f + 1.0f / 16384.0f;
        world[3].y = .125f + 1.0f / 32768.0f;
        SkinningPaletteSliceBinding slice;
        if (test.skinned) { slice.matrixCount = 1; slice.previousValid = true; }
        ASSERT_TRUE(cache.UpdateRasterDrawBindingSnapshotObject(binding, world,
            Mat4Identity(), Mat4Identity(), Mat4Identity(), test.history, false, slice, test.materialHistory));

        MaterialGPUConstants material;
        material.baseColorFactor.a = test.alpha;
        material.blendMode = static_cast<uint32>(test.additive
            ? MaterialGPUBlendMode::Additive : MaterialGPUBlendMode::AlphaBlend);
        material.shadingModel = static_cast<uint32>(test.refractive
            ? MaterialGPUShadingModel::ThinTransmission : MaterialGPUShadingModel::StandardLit);
        material.textureFlags = test.textureZero ? 1u : 0u;
        constexpr size_t cbBytes = (sizeof(material) + 255u) & ~size_t(255u);
        std::array<uint8, cbBytes> cb{};
        std::memcpy(cb.data(), &material, sizeof(material));
        auto materialBuffer = upload(cb.data(), cb.size(), RHIBufferUsage::Constant, 0);
        auto table = upload(&material, sizeof(material), paletteUsage, sizeof(material));
        ASSERT_TRUE(materialBuffer && table);
        std::array<uint8, pitch> texel{};
        texel[0] = texel[1] = texel[2] = 255;
        texel[3] = test.textureZero ? 0 : 255;
        auto texUpload = upload(texel.data(), texel.size(), RHIBufferUsage::CopySrc, 0);
        auto texture = device->CreateTexture(RHITextureDesc::Texture2D(1,1,RHIFormat::RGBA8_UNORM,
            RHITextureUsage::ShaderResource | RHITextureUsage::CopyDst));
        ASSERT_TRUE(texture && texUpload);
        RHITextureViewDesc textureViewDesc;
        textureViewDesc.format = RHIFormat::RGBA8_UNORM;
        auto textureView = device->CreateTextureView(texture.Get(), textureViewDesc);
        auto sampler = device->CreateSampler(RHISamplerDesc::LinearClamp());
        ASSERT_TRUE(textureView && sampler);
        RHIDescriptorSetDesc materialDesc;
        materialDesc.layout = cache.GetMaterialSetLayout();
        for (const auto& entry : materialDesc.layout->GetEntries())
        {
            if (entry.type == RHIBindingType::SampledTexture)
                materialDesc.BindTexture(entry.binding, textureView.Get());
            else if (entry.type == RHIBindingType::Sampler)
                materialDesc.BindSampler(entry.binding, sampler.Get());
            else
                materialDesc.BindBuffer(entry.binding, entry.binding == 0 ? materialBuffer.Get() : table.Get());
        }
        auto materialSet = device->CreateDescriptorSet(materialDesc);
        ASSERT_TRUE(materialSet);

        const std::array<RHIFormat,3> formats{RHIFormat::RGBA16_FLOAT,RHIFormat::R32_UINT,RHIFormat::R16_FLOAT};
        std::array<RHITextureRef,3> targets;
        std::array<RHITextureViewRef,3> rtvs;
        std::array<RHIBufferRef,3> readbacks;
        for (size_t i=0; i<3; ++i)
        {
            auto desc = RHITextureDesc::RenderTarget(extent,extent,formats[i]);
            desc.usage = desc.usage | RHITextureUsage::CopySrc;
            desc.optimizedClearValue = RHIOptimizedClearValue::Color({0,0,0,0});
            targets[i] = device->CreateTexture(desc);
            ASSERT_TRUE(targets[i]);
            RHITextureViewDesc vd;
            vd.format = formats[i]; vd.type = RHITextureViewType::RenderTarget;
            rtvs[i] = device->CreateTextureView(targets[i].Get(),vd);
            RHIBufferDesc rd;
            rd.size = pitch*extent; rd.usage = RHIBufferUsage::CopyDst;
            rd.memoryType = RHIMemoryType::Readback;
            readbacks[i] = device->CreateBuffer(rd);
            ASSERT_TRUE(rtvs[i] && readbacks[i]);
        }
        auto dd = RHITextureDesc::DepthStencil(extent,extent,RHIFormat::D32_FLOAT);
        dd.optimizedClearValue = RHIOptimizedClearValue::DepthStencil({test.depthClear,0});
        auto depth = device->CreateTexture(dd);
        ASSERT_TRUE(depth);
        RHITextureViewDesc dvd;
        dvd.type = RHITextureViewType::DepthStencil; dvd.format = RHIFormat::D32_FLOAT;
        dvd.subresourceRange.aspect = RHITextureAspect::Depth;
        auto dsv = device->CreateTextureView(depth.Get(),dvd);
        ASSERT_TRUE(dsv);
        auto commands = device->CreateCommandContext(RHICommandQueueType::Graphics);
        ASSERT_TRUE(commands);
        commands->Begin();
        commands->TextureBarrier(texture.Get(),RHIResourceState::Undefined,RHIResourceState::CopyDest);
        RHIBufferTextureCopyDesc copy;
        copy.bufferRowPitch=pitch; copy.bufferImageHeight=1; copy.textureRegion={0,0,1,1};
        commands->CopyBufferToTexture(texUpload.Get(),texture.Get(),copy);
        commands->TextureBarrier(texture.Get(),RHIResourceState::CopyDest,RHIResourceState::ShaderResource);
        commands->TextureBarrier(depth.Get(),RHIResourceState::Undefined,RHIResourceState::DepthWrite);
        RHIRenderPassDesc clear;
        clear.SetDepthStencil(dsv.Get(),RHILoadOp::Clear,RHIStoreOp::Store,test.depthClear);
        clear.SetRenderArea(0,0,extent,extent);
        commands->BeginRenderPass(clear); commands->EndRenderPass();
        commands->TextureBarrier(depth.Get(),RHIResourceState::DepthWrite,RHIResourceState::DepthRead);
        RHIRenderPassDesc rp;
        for (size_t i=0;i<3;++i)
        {
            commands->TextureBarrier(targets[i].Get(),RHIResourceState::Undefined,RHIResourceState::RenderTarget);
            rp.AddColorAttachment(rtvs[i].Get(),RHILoadOp::Clear,RHIStoreOp::Store,{0,0,0,0});
        }
        rp.SetDepthStencil(dsv.Get(),RHILoadOp::Load,RHIStoreOp::Store,test.depthClear);
        rp.depthStencilAttachment.readOnly=true;
        rp.SetRenderArea(0,0,extent,extent);
        commands->BeginRenderPass(rp);
        commands->SetViewport({0,0,float(extent),float(extent),0,1});
        commands->SetScissor({0,0,extent,extent});
        commands->SetPipeline(pipeline);
        commands->SetDescriptorSet(0,binding.frameDescriptorSet.Get());
        const std::array<uint32,1> offset{0};
        commands->SetDescriptorSet(1,binding.objectDescriptorSet.Get(),offset);
        commands->SetDescriptorSet(2,materialSet.Get(),offset);
        commands->SetVertexBuffer(0,vb.Get());
        commands->SetVertexBuffer(2,uvs.Get());
        commands->SetVertexBuffer(7,uvs.Get());
        if(test.skinned) {commands->SetVertexBuffer(4,boneBuffer.Get()); commands->SetVertexBuffer(5,weightBuffer.Get());}
        commands->SetIndexBuffer(ib.Get(),RHIFormat::R32_UINT);
        for(uint32 layer=0;layer<test.layers;++layer) commands->DrawIndexed(6,1,0,0,0);
        commands->EndRenderPass();
        for(size_t i=0;i<3;++i)
        {
            commands->TextureBarrier(targets[i].Get(),RHIResourceState::RenderTarget,RHIResourceState::CopySource);
            copy.bufferRowPitch=pitch; copy.bufferImageHeight=extent; copy.textureRegion={0,0,extent,extent};
            commands->CopyTextureToBuffer(targets[i].Get(),readbacks[i].Get(),copy);
        }
        commands->End();
        auto fence=device->CreateFence(0);
        ASSERT_TRUE(fence);
        const auto submitted=device->SubmitCommandContext(commands.Get(),fence.Get());
        ASSERT_NE(submitted,0u);
        device->WaitForFence(fence.Get(),submitted);
        ASSERT_GE(fence->GetCompletedValue(),submitted);
        const auto* motion=static_cast<const uint8*>(readbacks[0]->Map());
        const auto* flags=static_cast<const uint8*>(readbacks[1]->Map());
        const auto* count=static_cast<const uint8*>(readbacks[2]->Map());
        ASSERT_TRUE(motion && flags && count);
        const uint32 row=(extent/2)*pitch, x=extent/2;
        const auto* values=reinterpret_cast<const uint16*>(motion+row+x*8);
        const bool drawn=(test.additive || (test.alpha>0 && !test.textureZero)) && test.depthClear>=.5f;
        EXPECT_EQ(*reinterpret_cast<const uint16*>(count+row+x*2),drawn?(test.layers==2?0x4000:0x3c00):0);
        const uint32 expected=128u|1u|((test.weighted||test.additive)?0u:2u)|
            (test.history?64u:0u)|(test.refractive?16u:0u)|(test.additive?8u:0u)|(test.materialHistory?32u:0u);
        EXPECT_EQ(*reinterpret_cast<const uint32*>(flags+row+x*4),drawn?expected:0u);
        if(drawn)
        {
            EXPECT_EQ(values[0],test.history?(test.skinned?0x3600:0x3400):0);
            EXPECT_EQ(values[1],test.history?0x3000:0);
            EXPECT_EQ(values[2],test.alpha==1||test.additive?0x3c00:(test.alpha==.5f?0x3800:0x3bff));
            EXPECT_EQ(values[3],0x3800);
        }
        for(auto& readback:readbacks) readback->Unmap();
    }
    const auto diagnostics=device->GetNativeValidationDiagnostics();
    EXPECT_TRUE(diagnostics.available && diagnostics.readComplete);
    EXPECT_EQ(diagnostics.warningCount,0u);
    EXPECT_EQ(diagnostics.errorCount,0u);
    EXPECT_EQ(diagnostics.corruptionCount,0u);
}
}
