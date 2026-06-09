#include "Core/Core.h"
#include "RHI/RHI.h"
#include "Common/GpuTestUtils.h"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

using namespace RVX;

// =============================================================================
// DX12 Validation Tests
// =============================================================================

TEST(DX12Validation, DeviceCreation)
{
    RHIDeviceDesc desc;
    desc.enableDebugLayer = true;

    auto device = CreateRHIDevice(RHIBackendType::DX12, desc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX12);
    EXPECT_EQ(device->GetBackendType(), RHIBackendType::DX12);
}

TEST(DX12Validation, BufferCreation)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX12);

    // Test Default buffer
    RHIBufferDesc bufferDesc;
    bufferDesc.size = 1024;
    bufferDesc.usage = RHIBufferUsage::Vertex;
    bufferDesc.memoryType = RHIMemoryType::Default;
    bufferDesc.debugName = "TestVertexBuffer";

    auto buffer = device->CreateBuffer(bufferDesc);
    ASSERT_NE(nullptr, buffer.Get());
    EXPECT_EQ(buffer->GetSize(), 1024u);
    EXPECT_EQ(buffer->GetMemoryType(), RHIMemoryType::Default);
}

TEST(DX12Validation, UploadBuffer)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX12);

    // Test Upload buffer with data
    RHIBufferDesc bufferDesc;
    bufferDesc.size = 256;
    bufferDesc.usage = RHIBufferUsage::Constant;
    bufferDesc.memoryType = RHIMemoryType::Upload;
    bufferDesc.debugName = "TestUploadBuffer";

    auto buffer = device->CreateBuffer(bufferDesc);
    ASSERT_NE(nullptr, buffer.Get());

    // Map and write data
    void* mappedData = buffer->Map();
    ASSERT_NE(nullptr, mappedData);

    float testData[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    std::memcpy(mappedData, testData, sizeof(testData));
    buffer->Unmap();
}

TEST(DX12Validation, TextureCreation)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX12);

    auto textureDesc = RHITextureDesc::Texture2D(512, 512, RHIFormat::RGBA8_UNORM);
    textureDesc.debugName = "TestTexture";
    auto texture = device->CreateTexture(textureDesc);
    ASSERT_NE(nullptr, texture.Get());
    EXPECT_EQ(texture->GetWidth(), 512u);
    EXPECT_EQ(texture->GetHeight(), 512u);
    EXPECT_EQ(texture->GetFormat(), RHIFormat::RGBA8_UNORM);
}

TEST(DX12Validation, RenderTargetTexture)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX12);

    auto textureDesc = RHITextureDesc::RenderTarget(1920, 1080, RHIFormat::RGBA16_FLOAT);
    textureDesc.debugName = "TestRenderTarget";
    auto texture = device->CreateTexture(textureDesc);
    ASSERT_NE(nullptr, texture.Get());
    EXPECT_TRUE(HasFlag(texture->GetUsage(), RHITextureUsage::RenderTarget));
}

TEST(DX12Validation, DepthStencilTexture)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX12);

    auto textureDesc = RHITextureDesc::DepthStencil(1920, 1080, RHIFormat::D24_UNORM_S8_UINT);
    textureDesc.debugName = "TestDepthStencil";
    auto texture = device->CreateTexture(textureDesc);
    ASSERT_NE(nullptr, texture.Get());
    EXPECT_TRUE(HasFlag(texture->GetUsage(), RHITextureUsage::DepthStencil));
    EXPECT_EQ(texture->GetFormat(), RHIFormat::D24_UNORM_S8_UINT);
}

TEST(DX12Validation, TextureView)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX12);

    auto textureDesc = RHITextureDesc::Texture2D(256, 256, RHIFormat::RGBA8_UNORM);
    auto texture = device->CreateTexture(textureDesc);
    ASSERT_NE(nullptr, texture.Get());

    RHITextureViewDesc viewDesc;
    viewDesc.format = RHIFormat::RGBA8_UNORM;
    auto view = device->CreateTextureView(texture.Get(), viewDesc);
    ASSERT_NE(nullptr, view.Get());
    EXPECT_EQ(view->GetTexture(), texture.Get());
}

TEST(DX12Validation, TextureViewRolesAndStorageTextureBinding)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX12);

    auto sampledTextureDesc = RHITextureDesc::Texture2D(64, 64, RHIFormat::RGBA8_UNORM);
    auto sampledTexture = device->CreateTexture(sampledTextureDesc);
    ASSERT_NE(nullptr, sampledTexture.Get());

    RHITextureViewDesc sampledViewDesc;
    sampledViewDesc.format = RHIFormat::RGBA8_UNORM;
    sampledViewDesc.type = RHITextureViewType::ShaderResource;
    auto sampledView = device->CreateTextureView(sampledTexture.Get(), sampledViewDesc);
    ASSERT_NE(nullptr, sampledView.Get());

    RHITextureViewDesc invalidRTVDesc = sampledViewDesc;
    invalidRTVDesc.type = RHITextureViewType::RenderTarget;
    EXPECT_EQ(nullptr, device->CreateTextureView(sampledTexture.Get(), invalidRTVDesc).Get());

    auto renderTargetDesc = RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA8_UNORM);
    auto renderTarget = device->CreateTexture(renderTargetDesc);
    ASSERT_NE(nullptr, renderTarget.Get());

    RHITextureViewDesc rtvDesc;
    rtvDesc.format = RHIFormat::RGBA8_UNORM;
    rtvDesc.type = RHITextureViewType::RenderTarget;
    auto rtv = device->CreateTextureView(renderTarget.Get(), rtvDesc);
    ASSERT_NE(nullptr, rtv.Get());

    auto storageTextureDesc = RHITextureDesc::Texture2D(
        64, 64, RHIFormat::R32_FLOAT,
        RHITextureUsage::ShaderResource | RHITextureUsage::UnorderedAccess);
    auto storageTexture = device->CreateTexture(storageTextureDesc);
    ASSERT_NE(nullptr, storageTexture.Get());

    RHITextureViewDesc storageSRVDesc;
    storageSRVDesc.format = RHIFormat::R32_FLOAT;
    storageSRVDesc.type = RHITextureViewType::ShaderResource;
    auto storageSRV = device->CreateTextureView(storageTexture.Get(), storageSRVDesc);
    ASSERT_NE(nullptr, storageSRV.Get());

    RHITextureViewDesc storageUAVDesc = storageSRVDesc;
    storageUAVDesc.type = RHITextureViewType::UnorderedAccess;
    auto storageUAV = device->CreateTextureView(storageTexture.Get(), storageUAVDesc);
    ASSERT_NE(nullptr, storageUAV.Get());

    RHIDescriptorSetLayoutDesc layoutDesc;
    layoutDesc.AddBinding(0, RHIBindingType::StorageTexture, RHIShaderStage::Compute);
    auto layout = device->CreateDescriptorSetLayout(layoutDesc);
    ASSERT_NE(nullptr, layout.Get());

    RHIDescriptorSetDesc setDesc;
    setDesc.SetLayout(layout.Get());
    auto set = device->CreateDescriptorSet(setDesc);
    ASSERT_NE(nullptr, set.Get());

    EXPECT_TRUE(set->Update({RHIDescriptorBinding{0, nullptr, 0, 0, storageUAV.Get(), nullptr}}));
    EXPECT_FALSE(set->Update({RHIDescriptorBinding{0, nullptr, 0, 0, storageSRV.Get(), nullptr}}));

    RHIDescriptorSetDesc validInitialSetDesc;
    validInitialSetDesc.SetLayout(layout.Get()).BindTexture(0, storageUAV.Get());
    EXPECT_NE(nullptr, device->CreateDescriptorSet(validInitialSetDesc).Get());

    RHIDescriptorSetDesc invalidInitialSetDesc;
    invalidInitialSetDesc.SetLayout(layout.Get()).BindTexture(0, storageSRV.Get());
    EXPECT_EQ(nullptr, device->CreateDescriptorSet(invalidInitialSetDesc).Get());
}

TEST(DX12Validation, Sampler)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX12);

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

TEST(DX12Validation, CommandContext)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX12);

    auto ctx = device->CreateCommandContext(RHICommandQueueType::Graphics);
    ASSERT_NE(nullptr, ctx.Get());

    ctx->Begin();
    ctx->End();

    device->SubmitCommandContext(ctx.Get(), nullptr);
    device->WaitIdle();
}

TEST(DX12Validation, Fence)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX12);

    auto fence = device->CreateFence(0);
    ASSERT_NE(nullptr, fence.Get());
    EXPECT_EQ(fence->GetCompletedValue(), 0u);

    // Signal from CPU
    fence->Signal(1);
    fence->Wait(1);
    EXPECT_TRUE(fence->GetCompletedValue() >= 1u);
}

TEST(DX12Validation, SynchronizationCapabilities)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX12);

    const RHICapabilities& caps = device->GetCapabilities();
    EXPECT_FALSE(caps.supportsHostFenceSignal);
    EXPECT_TRUE(caps.supportsDefaultQueueFenceSignal);
    EXPECT_TRUE(caps.supportsExplicitQueueFenceSignal);
    EXPECT_TRUE(caps.supportsQueueFenceWait);
    EXPECT_FALSE(caps.supportsMultiQueueBatchSubmit);
    EXPECT_FALSE(caps.emulatesQueueFences);
}

TEST(DX12Validation, DescriptorAndBarrierCapabilities)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX12);

    const RHICapabilities& caps = device->GetCapabilities();
    EXPECT_TRUE(caps.supportsDescriptorSets);
    EXPECT_TRUE(caps.supportsDynamicDescriptorOffsets);
    EXPECT_GE(caps.maxDescriptorSets, 4u);
    EXPECT_TRUE(caps.supportsExplicitResourceBarriers);
    EXPECT_FALSE(caps.emulatesResourceBarriers);
    EXPECT_TRUE(caps.supportsSplitBarrier);
}

TEST(DX12Validation, DescriptorValidationRejectsInvalidInputs)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX12);

    RHIDescriptorSetLayoutDesc duplicateLayout;
    duplicateLayout.AddBinding(0, RHIBindingType::UniformBuffer);
    duplicateLayout.AddBinding(0, RHIBindingType::SampledTexture);
    EXPECT_EQ(device->CreateDescriptorSetLayout(duplicateLayout).Get(), nullptr);

    RHIDescriptorSetDesc nullSetDesc;
    EXPECT_EQ(device->CreateDescriptorSet(nullSetDesc).Get(), nullptr);

    RHIDescriptorSetLayoutDesc validLayoutDesc;
    validLayoutDesc.AddBinding(0, RHIBindingType::UniformBuffer);
    auto layout = device->CreateDescriptorSetLayout(validLayoutDesc);
    ASSERT_NE(nullptr, layout.Get());

    RHIPipelineLayoutDesc invalidPipelineLayout;
    invalidPipelineLayout.setLayouts.push_back(nullptr);
    EXPECT_EQ(device->CreatePipelineLayout(invalidPipelineLayout).Get(), nullptr);

    RHIDescriptorSetDesc validSetDesc;
    validSetDesc.SetLayout(layout.Get());
    auto set = device->CreateDescriptorSet(validSetDesc);
    ASSERT_NE(nullptr, set.Get());
    EXPECT_TRUE(set->Update({}));

    RHIDescriptorBinding unknownBinding;
    unknownBinding.binding = 99;
    EXPECT_FALSE(set->Update(std::vector<RHIDescriptorBinding>{unknownBinding}));

    RHIDescriptorBinding nullBufferBinding;
    nullBufferBinding.binding = 0;
    EXPECT_FALSE(set->Update(std::vector<RHIDescriptorBinding>{nullBufferBinding}));
}

TEST(DX12Validation, SubmitReturnsMonotonicFenceValues)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX12);

    auto fence = device->CreateFence(0);
    ASSERT_NE(nullptr, fence.Get());

    auto firstContext = device->CreateCommandContext(RHICommandQueueType::Graphics);
    ASSERT_NE(nullptr, firstContext.Get());
    firstContext->Begin();
    firstContext->End();
    uint64 firstValue = device->SubmitCommandContext(firstContext.Get(), fence.Get());

    auto secondContext = device->CreateCommandContext(RHICommandQueueType::Graphics);
    ASSERT_NE(nullptr, secondContext.Get());
    secondContext->Begin();
    secondContext->End();
    uint64 secondValue = device->SubmitCommandContext(secondContext.Get(), fence.Get());

    EXPECT_NE(firstValue, 0u);
    EXPECT_GT(secondValue, firstValue);

    device->WaitForFence(fence.Get(), secondValue);
    EXPECT_GE(fence->GetCompletedValue(), secondValue);
}

TEST(DX12Validation, BarrierNoWorkInputsAreSafe)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX12);

    auto ctx = device->CreateCommandContext(RHICommandQueueType::Graphics);
    ASSERT_NE(nullptr, ctx.Get());
    ctx->Begin();

    ctx->BufferBarrier({nullptr, RHIResourceState::Common, RHIResourceState::CopyDest});
    ctx->TextureBarrier({nullptr, RHIResourceState::Common, RHIResourceState::RenderTarget});

    RHIBufferDesc bufferDesc;
    bufferDesc.size = 256;
    bufferDesc.usage = RHIBufferUsage::Vertex;
    bufferDesc.memoryType = RHIMemoryType::Default;
    bufferDesc.stride = sizeof(float);
    auto buffer = device->CreateBuffer(bufferDesc);
    ASSERT_NE(nullptr, buffer.Get());
    ctx->BufferBarrier({buffer.Get(), RHIResourceState::Common, RHIResourceState::Common});
    ctx->BeginBarrier({buffer.Get(), RHIResourceState::Common, RHIResourceState::Common});
    ctx->EndBarrier({buffer.Get(), RHIResourceState::Common, RHIResourceState::Common});

    ctx->End();
    EXPECT_EQ(device->SubmitCommandContext(ctx.Get(), nullptr), 0u);
    device->WaitIdle();
}

TEST(DX12Validation, Heap)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX12);

    RHIHeapDesc heapDesc;
    heapDesc.size = 64 * 1024 * 1024;  // 64 MB
    heapDesc.type = RHIHeapType::Default;
    heapDesc.flags = RHIHeapFlags::AllowRenderTargets;
    heapDesc.debugName = "TestHeap";

    auto heap = device->CreateHeap(heapDesc);
    ASSERT_NE(nullptr, heap.Get());
    EXPECT_EQ(heap->GetSize(), 64u * 1024u * 1024u);
    EXPECT_EQ(heap->GetType(), RHIHeapType::Default);
}

TEST(DX12Validation, PlacedTexture)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX12);

    // Create heap
    RHIHeapDesc heapDesc;
    heapDesc.size = 64 * 1024 * 1024;  // 64 MB
    heapDesc.type = RHIHeapType::Default;
    heapDesc.flags = RHIHeapFlags::AllowRenderTargets;

    auto heap = device->CreateHeap(heapDesc);
    ASSERT_NE(nullptr, heap.Get());

    // Create placed texture
    auto textureDesc = RHITextureDesc::RenderTarget(1024, 1024, RHIFormat::RGBA16_FLOAT);
    textureDesc.debugName = "PlacedRenderTarget";

    auto memReq = device->GetTextureMemoryRequirements(textureDesc);
    EXPECT_TRUE(memReq.size > 0);
    EXPECT_TRUE(memReq.alignment > 0);

    auto texture = device->CreatePlacedTexture(heap.Get(), 0, textureDesc);
    ASSERT_NE(nullptr, texture.Get());
    EXPECT_EQ(texture->GetWidth(), 1024u);
    EXPECT_EQ(texture->GetHeight(), 1024u);
}

TEST(DX12Validation, PlacedBuffer)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX12);

    // Create heap for buffers
    RHIHeapDesc heapDesc;
    heapDesc.size = 16 * 1024 * 1024;  // 16 MB
    heapDesc.type = RHIHeapType::Default;
    heapDesc.flags = RHIHeapFlags::AllowBuffers;

    auto heap = device->CreateHeap(heapDesc);
    ASSERT_NE(nullptr, heap.Get());

    // Create placed buffer
    RHIBufferDesc bufferDesc;
    bufferDesc.size = 1024 * 1024;  // 1 MB
    bufferDesc.usage = RHIBufferUsage::UnorderedAccess | RHIBufferUsage::Structured;
    bufferDesc.memoryType = RHIMemoryType::Default;
    bufferDesc.stride = 16;
    bufferDesc.debugName = "PlacedStructuredBuffer";

    auto memReq = device->GetBufferMemoryRequirements(bufferDesc);
    EXPECT_TRUE(memReq.size > 0);

    auto buffer = device->CreatePlacedBuffer(heap.Get(), 0, bufferDesc);
    ASSERT_NE(nullptr, buffer.Get());
    EXPECT_EQ(buffer->GetSize(), 1024u * 1024u);
}

TEST(DX12Validation, MultipleBufferTypes)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX12);

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

    // Structured buffer
    RHIBufferDesc sbDesc;
    sbDesc.size = 1024;
    sbDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
    sbDesc.memoryType = RHIMemoryType::Default;
    sbDesc.stride = 16;
    auto sb = device->CreateBuffer(sbDesc);
    ASSERT_NE(nullptr, sb.Get());
}
