#include "Common/GpuTestUtils.h"
#include "Core/Core.h"
#include "Render/Context/RenderContext.h"
#include "RHI/RHI.h"
#include "RHI_BackendFactory/RHIBackendFactory.h"
#include "VulkanDevice.h"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

using namespace RVX;

// =============================================================================
// Vulkan Validation Tests
// =============================================================================

TEST(VulkanValidation, RenderContextSubmitsTrackedGraphicsFrameWithoutSurface)
{
    RenderContextConfig config;
    config.backendType = RHIBackendType::Vulkan;
    config.enableValidation = false;
    config.frameBuffering = 2;
    config.appName = "VulkanRenderContextValidation";

    RenderContext context;
    if (!context.Initialize(config))
    {
        GTEST_SKIP() << "Vulkan RenderContext is not available";
    }
    if (RVX::Test::IsSoftwareAdapterName(
            context.GetDevice()->GetCapabilities().adapterName))
    {
        context.Shutdown();
        GTEST_SKIP() << "Vulkan RenderContext uses a software adapter";
    }

    const uint32 frameSlot = context.GetFrameIndex();
    ASSERT_TRUE(context.BeginFrame());
    const GPUCompletionPoint submittedPoint = context.EndFrame();

    EXPECT_EQ(submittedPoint.domain, GPUQueueDomain::Graphics);
    EXPECT_GT(submittedPoint.value, 0u);
    ASSERT_NE(context.GetFrameSynchronizer(), nullptr);
    EXPECT_EQ(context.GetFrameSynchronizer()->GetFrameCompletionPoint(frameSlot),
              submittedPoint);

    context.WaitIdle();
    EXPECT_TRUE(context.GetFrameSynchronizer()->IsFrameComplete(frameSlot));
    context.Shutdown();
}

TEST(VulkanValidation, DeviceCreation)
{
    RHIDeviceDesc desc;
    desc.enableDebugLayer = true;

    auto device = CreateRHIDevice(RHIBackendType::Vulkan, desc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);
    EXPECT_EQ(device->GetBackendType(), RHIBackendType::Vulkan);
}

TEST(VulkanValidation, RayTracingCapabilitiesAreDisabledUntilBackendImplementation)
{
    RHIDeviceDesc desc;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, desc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);

    const RHICapabilities& caps = device->GetCapabilities();
    EXPECT_FALSE(caps.supportsRaytracing);
    EXPECT_FALSE(caps.supportsRaytracingPipeline);
    EXPECT_FALSE(caps.supportsRayQuery);
    EXPECT_FALSE(caps.supportsAccelerationStructureUpdate);
    EXPECT_FALSE(caps.supportsAccelerationStructureCompaction);
    EXPECT_EQ(caps.maxRayRecursionDepth, 0u);
    EXPECT_EQ(caps.shaderGroupHandleSize, 0u);
    EXPECT_EQ(caps.shaderGroupHandleAlignment, 0u);
    EXPECT_EQ(caps.shaderTableBaseAlignment, 0u);
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

TEST(VulkanValidation, TextureViewRolesAreExplicit)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);

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

TEST(VulkanValidation, QueryCapabilitiesReportUnsupportedUntilImplemented)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);

    const RHICapabilities& caps = device->GetCapabilities();
    EXPECT_FALSE(caps.supportsTimestampQueries);
    EXPECT_FALSE(caps.supportsOcclusionQueries);
    EXPECT_FALSE(caps.supportsPipelineStatisticsQueries);
    EXPECT_EQ(caps.timestampFrequency, 0u);

    RHIQueryPoolDesc queryDesc;
    queryDesc.type = RHIQueryType::Timestamp;
    queryDesc.count = 2;
    queryDesc.debugName = "UnsupportedTimestampQueries";
    EXPECT_EQ(device->CreateQueryPool(queryDesc).Get(), nullptr);
}

TEST(VulkanValidation, SynchronizationCapabilities)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);

    const RHICapabilities& caps = device->GetCapabilities();
    EXPECT_TRUE(caps.supportsHostFenceSignal);
    EXPECT_TRUE(caps.supportsDefaultQueueFenceSignal);
    EXPECT_TRUE(caps.supportsExplicitQueueFenceSignal);
    EXPECT_FALSE(caps.supportsQueueFenceWait);
    EXPECT_TRUE(caps.supportsMultiQueueBatchSubmit);
    EXPECT_FALSE(caps.emulatesQueueFences);
    EXPECT_EQ(caps.queueTopology.completionMode, RHIQueueCompletionMode::NativeTimeline);
    EXPECT_TRUE(ValidateRHICapabilities(caps));

    auto* vulkanDevice = dynamic_cast<VulkanDevice*>(device.get());
    ASSERT_NE(vulkanDevice, nullptr);
    const auto sameQueue = [](uint32 leftFamily,
                              VkQueue leftQueue,
                              uint32 rightFamily,
                              VkQueue rightQueue)
    {
        return leftFamily == rightFamily && leftQueue == rightQueue;
    };
    const bool computeAliasesGraphics = sameQueue(
        vulkanDevice->GetGraphicsQueueFamily(), vulkanDevice->GetGraphicsQueue(),
        vulkanDevice->GetComputeQueueFamily(), vulkanDevice->GetComputeQueue());
    const bool copyAliasesGraphics = sameQueue(
        vulkanDevice->GetGraphicsQueueFamily(), vulkanDevice->GetGraphicsQueue(),
        vulkanDevice->GetTransferQueueFamily(), vulkanDevice->GetTransferQueue());
    const bool copyAliasesCompute = sameQueue(
        vulkanDevice->GetComputeQueueFamily(), vulkanDevice->GetComputeQueue(),
        vulkanDevice->GetTransferQueueFamily(), vulkanDevice->GetTransferQueue());

    const GPUQueueDomain expectedCompute = computeAliasesGraphics
        ? GPUQueueDomain::Graphics
        : GPUQueueDomain::Compute;
    const GPUQueueDomain expectedCopy = copyAliasesGraphics
        ? GPUQueueDomain::Graphics
        : (copyAliasesCompute ? expectedCompute : GPUQueueDomain::Copy);
    EXPECT_EQ(caps.queueTopology.logicalQueueDomains[0], GPUQueueDomain::Graphics);
    EXPECT_EQ(caps.queueTopology.logicalQueueDomains[1], expectedCompute);
    EXPECT_EQ(caps.queueTopology.logicalQueueDomains[2], expectedCopy);
    EXPECT_EQ(caps.supportsAsyncCompute, !computeAliasesGraphics);
}

TEST(VulkanValidation, DescriptorAndBarrierCapabilities)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);

    const RHICapabilities& caps = device->GetCapabilities();
    EXPECT_TRUE(caps.supportsDescriptorSets);
    EXPECT_TRUE(caps.supportsDynamicDescriptorOffsets);
    EXPECT_GE(caps.maxDescriptorSets, 4u);
    EXPECT_TRUE(caps.supportsExplicitResourceBarriers);
    EXPECT_FALSE(caps.emulatesResourceBarriers);
    EXPECT_FALSE(caps.supportsSplitBarrier);
}

TEST(VulkanValidation, DescriptorValidationRejectsInvalidInputs)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);

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
    RHIBufferDesc bufferDesc;
    bufferDesc.SetSize(256)
        .SetUsage(RHIBufferUsage::Constant)
        .SetMemoryType(RHIMemoryType::Upload);
    auto buffer = device->CreateBuffer(bufferDesc);
    ASSERT_NE(nullptr, buffer.Get());
    validSetDesc.SetLayout(layout.Get()).BindBuffer(0, buffer.Get(), 0, 256);
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

TEST(VulkanValidation, SubmitReturnsMonotonicFenceValues)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);

    auto fence = device->CreateFence(0);
    ASSERT_NE(nullptr, fence.Get());

    auto firstContext = device->CreateCommandContext(RHICommandQueueType::Compute);
    ASSERT_NE(nullptr, firstContext.Get());
    firstContext->Begin();
    firstContext->End();
    uint64 firstValue = device->SubmitCommandContext(firstContext.Get(), fence.Get());

    auto secondContext = device->CreateCommandContext(RHICommandQueueType::Compute);
    ASSERT_NE(nullptr, secondContext.Get());
    secondContext->Begin();
    secondContext->End();
    uint64 secondValue = device->SubmitCommandContext(secondContext.Get(), fence.Get());

    EXPECT_NE(firstValue, 0u);
    EXPECT_GT(secondValue, firstValue);

    device->WaitForFence(fence.Get(), secondValue);
    EXPECT_GE(fence->GetCompletedValue(), secondValue);
}

TEST(VulkanValidation, BarrierNoWorkInputsAreSafe)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);

    auto ctx = device->CreateCommandContext(RHICommandQueueType::Compute);
    ASSERT_NE(nullptr, ctx.Get());
    ctx->Begin();

    ctx->BufferBarrier({nullptr, RHIResourceState::Common, RHIResourceState::CopyDest});
    ctx->TextureBarrier({nullptr, RHIResourceState::Common, RHIResourceState::ShaderResource});

    RHIBufferDesc bufferDesc;
    bufferDesc.size = 256;
    bufferDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::UnorderedAccess;
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
