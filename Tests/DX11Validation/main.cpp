#include "Core/Core.h"
#include "RHI/RHI.h"
#include "Common/GpuTestUtils.h"

#include <gtest/gtest.h>

#include <cstring>

using namespace RVX;

// =============================================================================
// DX11 Validation Tests
// =============================================================================

TEST(DX11Validation, DeviceCreation)
{
    RHIDeviceDesc desc;
    desc.enableDebugLayer = true;

    auto device = CreateRHIDevice(RHIBackendType::DX11, desc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX11);
    EXPECT_EQ(device->GetBackendType(), RHIBackendType::DX11);
}

TEST(DX11Validation, Capabilities)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX11, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX11);

    const auto& caps = device->GetCapabilities();

    // DX11 should NOT support bindless or raytracing
    EXPECT_FALSE(caps.supportsBindless);
    EXPECT_FALSE(caps.supportsRaytracing);

    // Should have an adapter name
    EXPECT_FALSE(caps.adapterName.empty());

    RVX_CORE_INFO("DX11 Adapter: {}", caps.adapterName);
}

TEST(DX11Validation, BufferCreation)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX11, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX11);

    RHIBufferDesc bufferDesc;
    bufferDesc.size = 1024;
    bufferDesc.usage = RHIBufferUsage::Vertex;
    bufferDesc.memoryType = RHIMemoryType::Default;
    bufferDesc.debugName = "TestVertexBuffer";

    auto buffer = device->CreateBuffer(bufferDesc);
    ASSERT_NE(nullptr, buffer.Get());
    EXPECT_EQ(buffer->GetSize(), 1024u);
}

TEST(DX11Validation, UploadBuffer)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX11, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX11);

    RHIBufferDesc bufferDesc;
    bufferDesc.size = 256;
    bufferDesc.usage = RHIBufferUsage::Constant;
    bufferDesc.memoryType = RHIMemoryType::Upload;
    bufferDesc.debugName = "TestUploadBuffer";

    auto buffer = device->CreateBuffer(bufferDesc);
    ASSERT_NE(nullptr, buffer.Get());

    void* mappedData = buffer->Map();
    ASSERT_NE(nullptr, mappedData);

    float testData[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    std::memcpy(mappedData, testData, sizeof(testData));
    buffer->Unmap();
}

TEST(DX11Validation, TextureCreation)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX11, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX11);

    auto textureDesc = RHITextureDesc::Texture2D(512, 512, RHIFormat::RGBA8_UNORM);
    textureDesc.debugName = "TestTexture";
    auto texture = device->CreateTexture(textureDesc);
    ASSERT_NE(nullptr, texture.Get());
    EXPECT_EQ(texture->GetWidth(), 512u);
    EXPECT_EQ(texture->GetHeight(), 512u);
}

TEST(DX11Validation, RenderTargetTexture)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX11, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX11);

    auto textureDesc = RHITextureDesc::RenderTarget(1920, 1080, RHIFormat::RGBA16_FLOAT);
    textureDesc.debugName = "TestRenderTarget";
    auto texture = device->CreateTexture(textureDesc);
    ASSERT_NE(nullptr, texture.Get());
    EXPECT_TRUE(HasFlag(texture->GetUsage(), RHITextureUsage::RenderTarget));
}

TEST(DX11Validation, DepthStencilTexture)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX11, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX11);

    auto textureDesc = RHITextureDesc::DepthStencil(1920, 1080, RHIFormat::D24_UNORM_S8_UINT);
    textureDesc.debugName = "TestDepthStencil";
    auto texture = device->CreateTexture(textureDesc);
    ASSERT_NE(nullptr, texture.Get());
    EXPECT_TRUE(HasFlag(texture->GetUsage(), RHITextureUsage::DepthStencil));
}

TEST(DX11Validation, TextureView)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX11, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX11);

    auto textureDesc = RHITextureDesc::Texture2D(256, 256, RHIFormat::RGBA8_UNORM);
    auto texture = device->CreateTexture(textureDesc);
    ASSERT_NE(nullptr, texture.Get());

    RHITextureViewDesc viewDesc;
    viewDesc.format = RHIFormat::RGBA8_UNORM;
    auto view = device->CreateTextureView(texture.Get(), viewDesc);
    ASSERT_NE(nullptr, view.Get());
}

TEST(DX11Validation, Sampler)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX11, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX11);

    RHISamplerDesc samplerDesc;
    samplerDesc.magFilter = RHIFilterMode::Linear;
    samplerDesc.minFilter = RHIFilterMode::Linear;
    samplerDesc.mipFilter = RHIFilterMode::Linear;
    samplerDesc.addressU = RHIAddressMode::Repeat;
    samplerDesc.addressV = RHIAddressMode::Repeat;
    samplerDesc.addressW = RHIAddressMode::Repeat;
    samplerDesc.debugName = "TestSampler";

    auto sampler = device->CreateSampler(samplerDesc);
    ASSERT_NE(nullptr, sampler.Get());
}

TEST(DX11Validation, CommandContext)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX11, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX11);

    auto ctx = device->CreateCommandContext(RHICommandQueueType::Graphics);
    ASSERT_NE(nullptr, ctx.Get());

    ctx->Begin();
    ctx->End();

    device->SubmitCommandContext(ctx.Get(), nullptr);
    device->WaitIdle();
}

TEST(DX11Validation, MultipleBufferTypes)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX11, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX11);

    // Vertex buffer
    RHIBufferDesc vbDesc;
    vbDesc.size = 4096;
    vbDesc.usage = RHIBufferUsage::Vertex;
    vbDesc.memoryType = RHIMemoryType::Upload;
    vbDesc.stride = 32;
    auto vb = device->CreateBuffer(vbDesc);
    ASSERT_NE(nullptr, vb.Get());

    // Index buffer
    RHIBufferDesc ibDesc;
    ibDesc.size = 2048;
    ibDesc.usage = RHIBufferUsage::Index;
    ibDesc.memoryType = RHIMemoryType::Upload;
    auto ib = device->CreateBuffer(ibDesc);
    ASSERT_NE(nullptr, ib.Get());

    // Constant buffer
    RHIBufferDesc cbDesc;
    cbDesc.size = 256;
    cbDesc.usage = RHIBufferUsage::Constant;
    cbDesc.memoryType = RHIMemoryType::Upload;
    auto cb = device->CreateBuffer(cbDesc);
    ASSERT_NE(nullptr, cb.Get());
}

TEST(DX11Validation, BarrierOperations)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX11, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX11);

    // DX11 doesn't have explicit barriers like DX12/Vulkan
    // But our abstraction should still accept them gracefully (as no-ops)

    auto rtDesc = RHITextureDesc::RenderTarget(512, 512, RHIFormat::RGBA8_UNORM);
    auto texture = device->CreateTexture(rtDesc);
    ASSERT_NE(nullptr, texture.Get());

    auto ctx = device->CreateCommandContext(RHICommandQueueType::Graphics);
    ctx->Begin();

    // These should be no-ops on DX11 but shouldn't crash
    ctx->TextureBarrier({texture.Get(), RHIResourceState::Undefined, RHIResourceState::RenderTarget});
    ctx->TextureBarrier({texture.Get(), RHIResourceState::RenderTarget, RHIResourceState::ShaderResource});

    ctx->End();
    device->SubmitCommandContext(ctx.Get(), nullptr);
    device->WaitIdle();
}
