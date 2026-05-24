#include "Core/Core.h"
#include "RHI/RHI.h"
#include "Common/GpuTestUtils.h"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

using namespace RVX;

// =============================================================================
// Vulkan Validation Tests
// =============================================================================

TEST(VulkanValidation, DeviceCreation)
{
    RHIDeviceDesc desc;
    desc.enableDebugLayer = true;

    auto device = CreateRHIDevice(RHIBackendType::Vulkan, desc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);
    EXPECT_EQ(device->GetBackendType(), RHIBackendType::Vulkan);
}

TEST(VulkanValidation, BufferCreation)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);

    RHIBufferDesc bufferDesc;
    bufferDesc.size = 1024;
    bufferDesc.usage = RHIBufferUsage::Vertex;
    bufferDesc.memoryType = RHIMemoryType::Default;
    bufferDesc.debugName = "TestVertexBuffer";

    auto buffer = device->CreateBuffer(bufferDesc);
    ASSERT_NE(nullptr, buffer.Get());
    EXPECT_EQ(buffer->GetSize(), 1024u);
}

TEST(VulkanValidation, UploadBuffer)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);

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

TEST(VulkanValidation, TextureCreation)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);

    auto textureDesc = RHITextureDesc::Texture2D(512, 512, RHIFormat::RGBA8_UNORM);
    textureDesc.debugName = "TestTexture";
    auto texture = device->CreateTexture(textureDesc);
    ASSERT_NE(nullptr, texture.Get());
    EXPECT_EQ(texture->GetWidth(), 512u);
    EXPECT_EQ(texture->GetHeight(), 512u);
}

TEST(VulkanValidation, RenderTargetTexture)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);

    auto textureDesc = RHITextureDesc::RenderTarget(1920, 1080, RHIFormat::RGBA16_FLOAT);
    textureDesc.debugName = "TestRenderTarget";
    auto texture = device->CreateTexture(textureDesc);
    ASSERT_NE(nullptr, texture.Get());
    EXPECT_TRUE(HasFlag(texture->GetUsage(), RHITextureUsage::RenderTarget));
}

TEST(VulkanValidation, DepthStencilTexture)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);

    auto textureDesc = RHITextureDesc::DepthStencil(1920, 1080, RHIFormat::D24_UNORM_S8_UINT);
    textureDesc.debugName = "TestDepthStencil";
    auto texture = device->CreateTexture(textureDesc);
    ASSERT_NE(nullptr, texture.Get());
    EXPECT_TRUE(HasFlag(texture->GetUsage(), RHITextureUsage::DepthStencil));
}

TEST(VulkanValidation, TextureView)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);

    auto textureDesc = RHITextureDesc::Texture2D(256, 256, RHIFormat::RGBA8_UNORM);
    auto texture = device->CreateTexture(textureDesc);
    ASSERT_NE(nullptr, texture.Get());

    RHITextureViewDesc viewDesc;
    viewDesc.format = RHIFormat::RGBA8_UNORM;
    auto view = device->CreateTextureView(texture.Get(), viewDesc);
    ASSERT_NE(nullptr, view.Get());
}

TEST(VulkanValidation, Sampler)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);

    RHISamplerDesc samplerDesc;
    samplerDesc.magFilter = RHIFilterMode::Linear;
    samplerDesc.minFilter = RHIFilterMode::Linear;
    samplerDesc.debugName = "TestSampler";

    auto sampler = device->CreateSampler(samplerDesc);
    ASSERT_NE(nullptr, sampler.Get());
}

TEST(VulkanValidation, CommandContext)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);

    auto ctx = device->CreateCommandContext(RHICommandQueueType::Graphics);
    ASSERT_NE(nullptr, ctx.Get());

    ctx->Begin();
    ctx->End();

    device->SubmitCommandContext(ctx.Get(), nullptr);
    device->WaitIdle();
}

TEST(VulkanValidation, BarrierBatching)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);

    // Create multiple textures
    std::vector<RHITextureRef> textures;
    for (int i = 0; i < 5; ++i)
    {
        auto desc = RHITextureDesc::RenderTarget(256, 256, RHIFormat::RGBA8_UNORM);
        desc.debugName = "BarrierTestTexture";
        textures.push_back(device->CreateTexture(desc));
        ASSERT_NE(nullptr, textures.back().Get());
    }

    auto ctx = device->CreateCommandContext(RHICommandQueueType::Graphics);
    ctx->Begin();

    // Issue multiple barriers - should be batched internally
    for (auto& tex : textures)
    {
        ctx->TextureBarrier({tex.Get(), RHIResourceState::Undefined, RHIResourceState::RenderTarget});
    }

    // More barriers
    for (auto& tex : textures)
    {
        ctx->TextureBarrier({tex.Get(), RHIResourceState::RenderTarget, RHIResourceState::ShaderResource});
    }

    ctx->End();

    device->SubmitCommandContext(ctx.Get(), nullptr);
    device->WaitIdle();
}

TEST(VulkanValidation, Fence)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);

    auto fence = device->CreateFence(0);
    ASSERT_NE(nullptr, fence.Get());
    EXPECT_EQ(fence->GetCompletedValue(), 0u);

    fence->Signal(1);
    fence->Wait(1);
    EXPECT_TRUE(fence->GetCompletedValue() >= 1u);
}

TEST(VulkanValidation, Heap)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);

    RHIHeapDesc heapDesc;
    heapDesc.size = 64 * 1024 * 1024;
    heapDesc.type = RHIHeapType::Default;
    heapDesc.flags = RHIHeapFlags::AllowRenderTargets;
    heapDesc.debugName = "TestHeap";

    auto heap = device->CreateHeap(heapDesc);
    ASSERT_NE(nullptr, heap.Get());
    EXPECT_EQ(heap->GetSize(), 64u * 1024u * 1024u);
}

TEST(VulkanValidation, PlacedTexture)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);

    RHIHeapDesc heapDesc;
    heapDesc.size = 64 * 1024 * 1024;
    heapDesc.type = RHIHeapType::Default;
    heapDesc.flags = RHIHeapFlags::AllowRenderTargets;

    auto heap = device->CreateHeap(heapDesc);
    ASSERT_NE(nullptr, heap.Get());

    auto textureDesc = RHITextureDesc::RenderTarget(1024, 1024, RHIFormat::RGBA16_FLOAT);
    textureDesc.debugName = "PlacedRenderTarget";

    auto memReq = device->GetTextureMemoryRequirements(textureDesc);
    EXPECT_TRUE(memReq.size > 0);

    auto texture = device->CreatePlacedTexture(heap.Get(), 0, textureDesc);
    ASSERT_NE(nullptr, texture.Get());
}

TEST(VulkanValidation, PlacedBuffer)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);

    RHIHeapDesc heapDesc;
    heapDesc.size = 16 * 1024 * 1024;
    heapDesc.type = RHIHeapType::Default;
    heapDesc.flags = RHIHeapFlags::AllowBuffers;

    auto heap = device->CreateHeap(heapDesc);
    ASSERT_NE(nullptr, heap.Get());

    RHIBufferDesc bufferDesc;
    bufferDesc.size = 1024 * 1024;
    bufferDesc.usage = RHIBufferUsage::UnorderedAccess | RHIBufferUsage::Structured;
    bufferDesc.memoryType = RHIMemoryType::Default;
    bufferDesc.stride = 16;
    bufferDesc.debugName = "PlacedStructuredBuffer";

    auto buffer = device->CreatePlacedBuffer(heap.Get(), 0, bufferDesc);
    ASSERT_NE(nullptr, buffer.Get());
    EXPECT_EQ(buffer->GetSize(), 1024u * 1024u);
}

TEST(VulkanValidation, ComputeContext)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);

    auto ctx = device->CreateCommandContext(RHICommandQueueType::Compute);
    ASSERT_NE(nullptr, ctx.Get());

    ctx->Begin();
    ctx->End();

    device->SubmitCommandContext(ctx.Get(), nullptr);
    device->WaitIdle();
}

TEST(VulkanValidation, CopyContext)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);

    auto ctx = device->CreateCommandContext(RHICommandQueueType::Copy);
    ASSERT_NE(nullptr, ctx.Get());

    ctx->Begin();
    ctx->End();

    device->SubmitCommandContext(ctx.Get(), nullptr);
    device->WaitIdle();
}
