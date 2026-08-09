#include "Common/GpuTestUtils.h"
#include "Core/Core.h"
#include "Render/Context/RenderContext.h"
#include "Render/Graph/RenderGraph.h"
#include "Render/GPUUploadService.h"
#include "RHI/RHI.h"
#include "RHI_BackendFactory/RHIBackendFactory.h"
#include "ShaderCompiler/ShaderCompiler.h"
#include "VulkanCommon.h"
#include "VulkanDevice.h"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <memory>
#include <vector>

using namespace RVX;

namespace
{
    RHIShaderRef CompileInlineVulkanGraphicsShader(IRHIDevice& device,
                                                   RHIShaderStage stage,
                                                   const char* entryPoint,
                                                   const char* source,
                                                   const char* targetProfile,
                                                   const char* debugName)
    {
        std::unique_ptr<IShaderCompiler> compiler = CreateShaderCompiler();
        if (!compiler)
        {
            ADD_FAILURE() << "Shader compiler is unavailable for " << debugName;
            return {};
        }

        ShaderCompileOptions options;
        options.stage = stage;
        options.entryPoint = entryPoint;
        options.sourceCode = source;
        options.sourcePath = debugName;
        options.targetProfile = targetProfile;
        options.targetBackend = RHIBackendType::Vulkan;
        options.enableDebugInfo = true;

        ShaderCompileResult compiled = compiler->Compile(options);
        if (!compiled.success)
        {
            ADD_FAILURE() << "Failed to compile " << debugName << ": "
                          << compiled.errorMessage;
            return {};
        }

        RHIShaderDesc shaderDesc;
        shaderDesc.stage = stage;
        shaderDesc.bytecode = compiled.bytecode.data();
        shaderDesc.bytecodeSize = static_cast<uint64>(compiled.bytecode.size());
        shaderDesc.entryPoint = entryPoint;
        shaderDesc.debugName = debugName;
        return device.CreateShader(shaderDesc);
    }

    RHIBufferRef CreateVulkanUploadBuffer(IRHIDevice& device,
                                          RHIBufferUsage usage,
                                          uint32 stride,
                                          const void* data,
                                          uint64 dataSize,
                                          const char* debugName)
    {
        RHIBufferDesc bufferDesc;
        bufferDesc.size = dataSize;
        bufferDesc.usage = usage;
        bufferDesc.memoryType = RHIMemoryType::Upload;
        bufferDesc.stride = stride;
        bufferDesc.debugName = debugName;
        RHIBufferRef buffer = device.CreateBuffer(bufferDesc);
        if (!buffer)
        {
            ADD_FAILURE() << "Failed to create upload buffer " << debugName;
            return {};
        }

        void* mapped = buffer->Map();
        if (!mapped)
        {
            ADD_FAILURE() << "Failed to map upload buffer " << debugName;
            return {};
        }
        std::memcpy(mapped, data, static_cast<size_t>(dataSize));
        buffer->Unmap();
        return buffer;
    }

    RHITextureViewRef CreateVulkanRenderTargetView(IRHIDevice& device,
                                                    RHITexture* texture)
    {
        RHITextureViewDesc viewDesc;
        viewDesc.type = RHITextureViewType::RenderTarget;
        viewDesc.format = RHIFormat::RGBA8_UNORM;
        viewDesc.dimension = RHITextureDimension::Texture2D;
        viewDesc.debugName = "VulkanNativeIndexedIndirectRTV";
        return device.CreateTextureView(texture, viewDesc);
    }

    RHITextureViewRef CreateVulkanDepthStencilView(IRHIDevice& device,
                                                    RHITexture* texture)
    {
        RHITextureViewDesc viewDesc;
        viewDesc.type = RHITextureViewType::DepthStencil;
        viewDesc.format = RHIFormat::D32_FLOAT;
        viewDesc.dimension = RHITextureDimension::Texture2D;
        viewDesc.debugName = "VulkanDynamicRenderingDSV";
        return device.CreateTextureView(texture, viewDesc);
    }
} // namespace

// =============================================================================
// Vulkan Validation Tests
// =============================================================================

TEST(VulkanValidation, RenderContextSubmitsTrackedGraphicsFrameWithoutSurface)
{
    RenderContextConfig config;
    config.backendType = RHIBackendType::Vulkan;
    config.enableValidation = true;
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

    auto* vulkanDevice = dynamic_cast<VulkanDevice*>(context.GetDevice());
    ASSERT_NE(vulkanDevice, nullptr);
    const VulkanValidationMessageCounts messagesBefore =
        vulkanDevice->GetValidationMessageCounts();

    constexpr uint32 frameCount = 4;
    for (uint32 frame = 0; frame < frameCount; ++frame)
    {
        const uint32 frameSlot = context.GetFrameIndex();
        ASSERT_TRUE(context.BeginFrame());
        const GPUCompletionPoint submittedPoint = context.EndFrame();

        EXPECT_EQ(submittedPoint.domain, GPUQueueDomain::Graphics);
        EXPECT_GT(submittedPoint.value, 0u);
        ASSERT_NE(context.GetFrameSynchronizer(), nullptr);
        EXPECT_EQ(context.GetFrameSynchronizer()->GetFrameCompletionPoint(frameSlot),
                  submittedPoint);
    }

    ASSERT_NE(context.GetFrameSynchronizer(), nullptr);
    context.WaitIdle();
    EXPECT_TRUE(context.GetFrameSynchronizer()->IsFrameComplete(context.GetFrameIndex()));
    const VulkanValidationMessageCounts messagesAfter =
        vulkanDevice->GetValidationMessageCounts();
    EXPECT_EQ(messagesAfter.errors, messagesBefore.errors);
    EXPECT_EQ(messagesAfter.warnings, messagesBefore.warnings);
    context.Shutdown();
}

TEST(VulkanValidation, RenderContextFramesQueueDagWithRecordingGraphicsGateway)
{
    RenderContextConfig config;
    config.backendType = RHIBackendType::Vulkan;
    config.enableValidation = true;
    config.frameBuffering = 2;
    config.appName = "VulkanRenderContextQueueGatewayValidation";

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

    ASSERT_TRUE(
        context.GetDevice()->GetCapabilities().supportsQueueSubmissionPlan);
    auto* vulkanDevice = dynamic_cast<VulkanDevice*>(context.GetDevice());
    ASSERT_NE(vulkanDevice, nullptr);
    const VulkanValidationMessageCounts messagesBefore =
        vulkanDevice->GetValidationMessageCounts();

    RHICommandContextRef computeContext =
        context.GetDevice()->CreateCommandContext(
            RHICommandQueueType::Compute);
    RHICommandContextRef graphTerminalContext =
        context.GetDevice()->CreateCommandContext(
            RHICommandQueueType::Graphics);
    ASSERT_NE(computeContext.Get(), nullptr);
    ASSERT_NE(graphTerminalContext.Get(), nullptr);
    computeContext->Reset();
    computeContext->Begin();
    computeContext->SetMarker("QueueGatewayCompute");
    computeContext->End();
    graphTerminalContext->Reset();
    graphTerminalContext->Begin();
    graphTerminalContext->SetMarker("QueueGatewayGraphTerminal");
    graphTerminalContext->End();

    RHIQueueSubmissionPlan plan;
    plan.batches.push_back(
        {RHICommandQueueType::Compute, {computeContext.Get()}, {}});
    plan.batches.push_back(
        {RHICommandQueueType::Graphics,
         {graphTerminalContext.Get()},
         {0u}});
    plan.terminalGraphicsBatchIndex = 1u;
    ASSERT_TRUE(ValidateRHIQueueSubmissionPlan(plan));

    RHIBufferDesc bufferDesc;
    bufferDesc.size = 256u;
    bufferDesc.usage = RHIBufferUsage::CopyDst;
    bufferDesc.memoryType = RHIMemoryType::Default;
    bufferDesc.debugName = "VulkanQueueGatewayBarrierBuffer";
    RHIBufferRef barrierBuffer = context.GetDevice()->CreateBuffer(bufferDesc);
    ASSERT_NE(barrierBuffer.Get(), nullptr);

    ASSERT_TRUE(context.BeginFrame());
    RHICommandContext* graphicsPrelude = context.GetGraphicsContext();
    ASSERT_NE(graphicsPrelude, nullptr);
    std::vector<RHICommandContextRef> ownedContexts;
    ownedContexts.push_back(std::move(computeContext));
    ownedContexts.push_back(std::move(graphTerminalContext));
    ASSERT_TRUE(context.AdoptQueueSubmission(std::move(plan),
                                             std::move(ownedContexts)));

    RHICommandContext* graphicsGateway = context.GetGraphicsContext();
    ASSERT_NE(graphicsGateway, nullptr);
    EXPECT_NE(graphicsGateway, graphicsPrelude);
    EXPECT_EQ(graphicsGateway->GetQueueType(),
              RHICommandQueueType::Graphics);
    graphicsGateway->BufferBarrier(barrierBuffer.Get(),
                                   RHIResourceState::Common,
                                   RHIResourceState::CopyDest);
    graphicsGateway->BufferBarrier(barrierBuffer.Get(),
                                   RHIResourceState::CopyDest,
                                   RHIResourceState::Common);

    const GPUCompletionPoint submittedPoint = context.EndFrame();
    EXPECT_EQ(submittedPoint.domain, GPUQueueDomain::Graphics);
    EXPECT_GT(submittedPoint.value, 0u);
    context.WaitIdle();

    const VulkanValidationMessageCounts messagesAfter =
        vulkanDevice->GetValidationMessageCounts();
    EXPECT_EQ(messagesAfter.errors, messagesBefore.errors);
    EXPECT_EQ(messagesAfter.warnings, messagesBefore.warnings);
    barrierBuffer.Reset();
    context.Shutdown();
}

TEST(VulkanValidation, RenderContextAbortFrameDoesNotStarveHeadlessFrameFence)
{
    RenderContextConfig config;
    config.backendType = RHIBackendType::Vulkan;
    config.enableValidation = true;
    config.frameBuffering = 2;
    config.appName = "VulkanRenderContextAbortValidation";

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

    auto* vulkanDevice = dynamic_cast<VulkanDevice*>(context.GetDevice());
    ASSERT_NE(vulkanDevice, nullptr);
    const VulkanValidationMessageCounts messagesBefore =
        vulkanDevice->GetValidationMessageCounts();

    ASSERT_TRUE(context.BeginFrame());
    context.AbortFrame();

    constexpr uint32 completedFrameCount = 4;
    for (uint32 frame = 0; frame < completedFrameCount; ++frame)
    {
        ASSERT_TRUE(context.BeginFrame());
        const GPUCompletionPoint submittedPoint = context.EndFrame();
        EXPECT_EQ(submittedPoint.domain, GPUQueueDomain::Graphics);
        EXPECT_GT(submittedPoint.value, 0u);
    }

    context.WaitIdle();
    const VulkanValidationMessageCounts messagesAfter =
        vulkanDevice->GetValidationMessageCounts();
    EXPECT_EQ(messagesAfter.errors, messagesBefore.errors);
    EXPECT_EQ(messagesAfter.warnings, messagesBefore.warnings);
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
    EXPECT_FALSE(caps.supportsMeshShaders);
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

TEST(VulkanValidation, OptionalShaderBarrierStagesRequireEnabledCapabilities)
{
    const RHIExecutionScope optionalScopes =
        RHIExecutionScope::MeshShader |
        RHIExecutionScope::AmplificationShader |
        RHIExecutionScope::RayTracingShader;

    const VulkanPipelineStageSupport disabledStages{};
    EXPECT_EQ(ToVkPipelineStageFlags2(optionalScopes, disabledStages),
              VK_PIPELINE_STAGE_2_NONE);

    const VkPipelineStageFlags2 coreGraphicsStages =
        ToVkPipelineStageFlags2(
            GetRHIExecutionScope(RHIShaderStage::AllGraphics),
            disabledStages);
    EXPECT_NE(coreGraphicsStages & VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, 0u);
    EXPECT_NE(coreGraphicsStages & VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0u);
    EXPECT_EQ(coreGraphicsStages & VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT, 0u);
    EXPECT_EQ(coreGraphicsStages & VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT,
              0u);
    EXPECT_EQ(coreGraphicsStages & VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT,
              0u);
    EXPECT_EQ(coreGraphicsStages & VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT, 0u);
    EXPECT_EQ(coreGraphicsStages & VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT, 0u);

    VulkanPipelineStageSupport geometryStages{};
    geometryStages.geometryShaderEnabled = true;
    const VkPipelineStageFlags2 geometryStageMask = ToVkPipelineStageFlags2(
        RHIExecutionScope::GeometryShader |
            RHIExecutionScope::HullShader |
            RHIExecutionScope::DomainShader,
        geometryStages);
    EXPECT_NE(geometryStageMask & VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT, 0u);
    EXPECT_EQ(geometryStageMask & VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT,
              0u);
    EXPECT_EQ(geometryStageMask & VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT,
              0u);

    VulkanPipelineStageSupport tessellationStages{};
    tessellationStages.tessellationShaderEnabled = true;
    const VkPipelineStageFlags2 tessellationStageMask = ToVkPipelineStageFlags2(
        RHIExecutionScope::GeometryShader |
            RHIExecutionScope::HullShader |
            RHIExecutionScope::DomainShader,
        tessellationStages);
    EXPECT_EQ(tessellationStageMask & VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT, 0u);
    EXPECT_NE(tessellationStageMask & VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT,
              0u);
    EXPECT_NE(tessellationStageMask & VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT,
              0u);

    EXPECT_EQ(ToVkPipelineStageFlags2(RHIExecutionScope::AllCommands,
                                       disabledStages),
              VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);

    VulkanPipelineStageSupport meshSupport{};
    meshSupport.meshShadersEnabled = true;
    const VkPipelineStageFlags2 meshStages =
        ToVkPipelineStageFlags2(optionalScopes, meshSupport);
    EXPECT_NE(meshStages & VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT, 0u);
    EXPECT_NE(meshStages & VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT, 0u);
    EXPECT_EQ(meshStages & VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, 0u);

    VulkanPipelineStageSupport rayTracingSupport{};
    rayTracingSupport.rayTracingEnabled = true;
    const VkPipelineStageFlags2 rayTracingStages =
        ToVkPipelineStageFlags2(optionalScopes, rayTracingSupport);
    EXPECT_EQ(rayTracingStages & VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT, 0u);
    EXPECT_EQ(rayTracingStages & VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT, 0u);
    EXPECT_NE(
        rayTracingStages & VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
        0u);
}

TEST(VulkanValidation, IndexedIndirectCountDispatchSelectionFailsClosed)
{
    VulkanIndexedIndirectCountDispatchInput coreInput;
    coreInput.apiVersion = VK_API_VERSION_1_2;
    coreInput.drawIndirectCountFeatureEnabled = true;
    coreInput.coreEntryPointLoaded = true;
    EXPECT_EQ(SelectVulkanIndexedIndirectCountDispatch(coreInput),
              VulkanIndexedIndirectCountDispatch::Core12);

    VulkanIndexedIndirectCountDispatchInput khrInput;
    khrInput.apiVersion = VK_API_VERSION_1_1;
    khrInput.khrExtensionEnabled = true;
    khrInput.khrEntryPointLoaded = true;
    EXPECT_EQ(SelectVulkanIndexedIndirectCountDispatch(khrInput),
              VulkanIndexedIndirectCountDispatch::KHR);

    // Vulkan 1.2+ never falls back to KHR when the core feature is disabled.
    VulkanIndexedIndirectCountDispatchInput disabledCore = khrInput;
    disabledCore.apiVersion = VK_API_VERSION_1_2;
    EXPECT_EQ(SelectVulkanIndexedIndirectCountDispatch(disabledCore),
              VulkanIndexedIndirectCountDispatch::None);

    VulkanIndexedIndirectCountDispatchInput missingEntry = coreInput;
    missingEntry.coreEntryPointLoaded = false;
    EXPECT_EQ(SelectVulkanIndexedIndirectCountDispatch(missingEntry),
              VulkanIndexedIndirectCountDispatch::None);
}

TEST(VulkanValidation, RequiredDeviceConfigurationSelectionFailsClosed)
{
    VulkanRequiredDeviceFeatureSupport supported;
    supported.apiVersion = VK_API_VERSION_1_3;
    supported.timelineSemaphore = true;
    supported.dynamicRendering = true;
    supported.synchronization2 = true;
    EXPECT_TRUE(IsVulkanRequiredDeviceConfigurationSupported(supported));

    VulkanRequiredDeviceFeatureSupport missingApi = supported;
    missingApi.apiVersion = VK_API_VERSION_1_2;
    EXPECT_FALSE(IsVulkanRequiredDeviceConfigurationSupported(missingApi));

    VulkanRequiredDeviceFeatureSupport missingTimeline = supported;
    missingTimeline.timelineSemaphore = false;
    EXPECT_FALSE(IsVulkanRequiredDeviceConfigurationSupported(missingTimeline));

    VulkanRequiredDeviceFeatureSupport missingDynamicRendering = supported;
    missingDynamicRendering.dynamicRendering = false;
    EXPECT_FALSE(
        IsVulkanRequiredDeviceConfigurationSupported(missingDynamicRendering));

    VulkanRequiredDeviceFeatureSupport missingSynchronization2 = supported;
    missingSynchronization2.synchronization2 = false;
    EXPECT_FALSE(
        IsVulkanRequiredDeviceConfigurationSupported(missingSynchronization2));
}

TEST(VulkanValidation, OptionalMemoryBudgetExtensionSelectionReflectsAvailability)
{
    EXPECT_TRUE(SelectVulkanMemoryBudgetExtensionEnabled(true));
    EXPECT_FALSE(SelectVulkanMemoryBudgetExtensionEnabled(false));
}

TEST(VulkanValidation, IndexedIndirectExecutionSelectionFailsClosedWithoutMultiDraw)
{
    VulkanIndexedIndirectExecutionSelectionInput input;
    input.multiDrawIndirectEnabled = true;
    input.countDispatch = VulkanIndexedIndirectCountDispatch::Core12;
    input.hardwareMaxDrawCount = 4096;
    VulkanIndexedIndirectExecutionSelection selection =
        SelectVulkanIndexedIndirectExecutionSelection(input);
    EXPECT_TRUE(selection.supportsFixedCount);
    EXPECT_TRUE(selection.supportsCountBuffer);
    EXPECT_EQ(selection.maxDrawCount, 4096u);

    input.multiDrawIndirectEnabled = false;
    selection = SelectVulkanIndexedIndirectExecutionSelection(input);
    EXPECT_TRUE(selection.supportsFixedCount);
    EXPECT_FALSE(selection.supportsCountBuffer);
    EXPECT_EQ(selection.maxDrawCount, 1u);
}

TEST(VulkanValidation, IndexedIndirectCountCapabilityReflectsEnabledDispatch)
{
    RHIDeviceDesc deviceDesc;
    deviceDesc.enableDebugLayer = true;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);

    auto* vulkanDevice = dynamic_cast<VulkanDevice*>(device.get());
    ASSERT_NE(vulkanDevice, nullptr);
    const VulkanIndexedIndirectCountDispatch dispatch =
        vulkanDevice->GetIndexedIndirectCountDispatch();
    EXPECT_EQ(device->GetCapabilities().indexedIndirectExecution.supportsCountBuffer,
              dispatch != VulkanIndexedIndirectCountDispatch::None);

    if (device->GetCapabilities().vulkan.apiVersion >= VK_API_VERSION_1_2)
    {
        EXPECT_NE(dispatch, VulkanIndexedIndirectCountDispatch::KHR);
    }
    if (dispatch == VulkanIndexedIndirectCountDispatch::Core12)
    {
        EXPECT_NE(vulkanDevice->GetCmdDrawIndexedIndirectCount(), nullptr);
    }
    if (dispatch == VulkanIndexedIndirectCountDispatch::KHR)
    {
        EXPECT_NE(vulkanDevice->GetCmdDrawIndexedIndirectCountKHR(), nullptr);
    }
}

TEST(VulkanValidation, PublishedCapabilitiesReflectEnabledVulkanImplementationState)
{
    RHIDeviceDesc deviceDesc;
    deviceDesc.enableDebugLayer = true;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);

    auto* vulkanDevice = dynamic_cast<VulkanDevice*>(device.get());
    ASSERT_NE(vulkanDevice, nullptr);
    const RHICapabilities& capabilities = device->GetCapabilities();

    EXPECT_FALSE(capabilities.supportsBindless);
    EXPECT_FALSE(capabilities.supportsDepthBounds);
    EXPECT_FALSE(capabilities.supportsDynamicLineWidth);
    EXPECT_FALSE(capabilities.supportsSecondaryCommandBuffer);
    EXPECT_EQ(capabilities.supportsMemoryBudgetQuery,
              vulkanDevice->IsMemoryBudgetEnabled());

    const bool nativeCountDispatch =
        vulkanDevice->GetIndexedIndirectCountDispatch() !=
        VulkanIndexedIndirectCountDispatch::None;
    EXPECT_EQ(capabilities.indexedIndirectExecution.supportsCountBuffer,
              vulkanDevice->IsMultiDrawIndirectEnabled() && nativeCountDispatch);
    if (!vulkanDevice->IsMultiDrawIndirectEnabled())
    {
        EXPECT_EQ(capabilities.indexedIndirectExecution.maxDrawCount, 1u);
    }
}

TEST(VulkanValidation, ValidationLayerCleanForRawGraphicsSubmitAndPlacedBuffer)
{
    RHIDeviceDesc deviceDesc;
    deviceDesc.enableDebugLayer = true;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);
    auto* vulkanDevice = dynamic_cast<VulkanDevice*>(device.get());
    ASSERT_NE(vulkanDevice, nullptr);

    auto context = device->CreateCommandContext(RHICommandQueueType::Graphics);
    ASSERT_NE(context.Get(), nullptr);
    context->Begin();
    context->End();
    EXPECT_EQ(device->SubmitCommandContext(context.Get(), nullptr), 0u);
    device->WaitIdle();

    RHIHeapDesc heapDesc;
    heapDesc.size = 2u * 1024u * 1024u;
    heapDesc.type = RHIHeapType::Default;
    heapDesc.flags = RHIHeapFlags::AllowBuffers;
    auto heap = device->CreateHeap(heapDesc);
    ASSERT_NE(heap.Get(), nullptr);

    RHIBufferDesc bufferDesc;
    bufferDesc.size = 1024u * 1024u;
    bufferDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::UnorderedAccess;
    bufferDesc.memoryType = RHIMemoryType::Default;
    bufferDesc.stride = 16;
    auto buffer = device->CreatePlacedBuffer(heap.Get(), 0, bufferDesc);
    ASSERT_NE(buffer.Get(), nullptr);

    const VulkanValidationMessageCounts messages =
        vulkanDevice->GetValidationMessageCounts();
    EXPECT_EQ(messages.errors, 0u);
    EXPECT_EQ(messages.warnings, 0u);
}

TEST(VulkanValidation, DynamicRenderingHonorsAndBoundsChecksRenderArea)
{
    RHIDeviceDesc deviceDesc;
    deviceDesc.enableDebugLayer = true;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);
    auto* vulkanDevice = dynamic_cast<VulkanDevice*>(device.get());
    ASSERT_NE(vulkanDevice, nullptr);

    auto createColorTarget = [&]()
    {
        auto texture = device->CreateTexture(
            RHITextureDesc::RenderTarget(32, 32, RHIFormat::RGBA8_UNORM));
        EXPECT_NE(texture.Get(), nullptr);
        return texture;
    };
    auto createDepthTarget = [&]()
    {
        auto texture = device->CreateTexture(
            RHITextureDesc::DepthStencil(16, 16, RHIFormat::D32_FLOAT));
        EXPECT_NE(texture.Get(), nullptr);
        return texture;
    };

    const VulkanValidationMessageCounts messagesBefore =
        vulkanDevice->GetValidationMessageCounts();

    // The explicit 16x16 area is valid for the smaller depth attachment. The
    // previous Vulkan path ignored it and used the 32x32 color extent instead.
    RHITextureRef boundedColor = createColorTarget();
    RHITextureRef boundedDepth = createDepthTarget();
    ASSERT_NE(boundedColor.Get(), nullptr);
    ASSERT_NE(boundedDepth.Get(), nullptr);
    RHITextureViewRef boundedColorView =
        CreateVulkanRenderTargetView(*device, boundedColor.Get());
    RHITextureViewRef boundedDepthView =
        CreateVulkanDepthStencilView(*device, boundedDepth.Get());
    ASSERT_NE(boundedColorView.Get(), nullptr);
    ASSERT_NE(boundedDepthView.Get(), nullptr);

    auto boundedContext = device->CreateCommandContext(RHICommandQueueType::Graphics);
    ASSERT_NE(boundedContext.Get(), nullptr);
    boundedContext->Begin();
    boundedContext->TextureBarrier(
        {boundedColor.Get(), RHIResourceState::Undefined, RHIResourceState::RenderTarget});
    boundedContext->TextureBarrier(
        {boundedDepth.Get(), RHIResourceState::Undefined, RHIResourceState::DepthWrite});
    RHIRenderPassDesc boundedPass;
    boundedPass.AddColorAttachment(boundedColorView.Get());
    boundedPass.SetDepthStencil(boundedDepthView.Get());
    boundedPass.SetRenderArea(0, 0, 16, 16);
    boundedContext->BeginRenderPass(boundedPass);
    boundedContext->EndRenderPass();
    boundedContext->End();
    EXPECT_EQ(device->SubmitCommandContext(boundedContext.Get(), nullptr), 0u);
    device->WaitIdle();

    const VulkanValidationMessageCounts messagesAfterBoundedPass =
        vulkanDevice->GetValidationMessageCounts();
    EXPECT_EQ(messagesAfterBoundedPass.errors, messagesBefore.errors);
    EXPECT_EQ(messagesAfterBoundedPass.warnings, messagesBefore.warnings);

    // An area that exceeds the depth attachment must be rejected before
    // vkCmdBeginRendering so it cannot produce an invalid command buffer.
    RHITextureRef rejectedColor = createColorTarget();
    RHITextureRef rejectedDepth = createDepthTarget();
    ASSERT_NE(rejectedColor.Get(), nullptr);
    ASSERT_NE(rejectedDepth.Get(), nullptr);
    RHITextureViewRef rejectedColorView =
        CreateVulkanRenderTargetView(*device, rejectedColor.Get());
    RHITextureViewRef rejectedDepthView =
        CreateVulkanDepthStencilView(*device, rejectedDepth.Get());
    ASSERT_NE(rejectedColorView.Get(), nullptr);
    ASSERT_NE(rejectedDepthView.Get(), nullptr);

    auto rejectedContext = device->CreateCommandContext(RHICommandQueueType::Graphics);
    ASSERT_NE(rejectedContext.Get(), nullptr);
    rejectedContext->Begin();
    rejectedContext->TextureBarrier(
        {rejectedColor.Get(), RHIResourceState::Undefined, RHIResourceState::RenderTarget});
    rejectedContext->TextureBarrier(
        {rejectedDepth.Get(), RHIResourceState::Undefined, RHIResourceState::DepthWrite});
    RHIRenderPassDesc rejectedPass;
    rejectedPass.AddColorAttachment(rejectedColorView.Get());
    rejectedPass.SetDepthStencil(rejectedDepthView.Get());
    rejectedPass.SetRenderArea(0, 0, 17, 16);
    rejectedContext->BeginRenderPass(rejectedPass);
    rejectedContext->EndRenderPass();
    rejectedContext->End();
    EXPECT_EQ(device->SubmitCommandContext(rejectedContext.Get(), nullptr), 0u);
    device->WaitIdle();

    const VulkanValidationMessageCounts messagesAfterRejectedPass =
        vulkanDevice->GetValidationMessageCounts();
    EXPECT_EQ(messagesAfterRejectedPass.errors, messagesBefore.errors);
    EXPECT_EQ(messagesAfterRejectedPass.warnings, messagesBefore.warnings);
}

TEST(VulkanValidation, NativeIndexedIndirectCommandsRecordOnAvailablePath)
{
    RHIDeviceDesc deviceDesc;
    deviceDesc.enableDebugLayer = true;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);
    auto* vulkanDevice = dynamic_cast<VulkanDevice*>(device.get());
    ASSERT_NE(vulkanDevice, nullptr);

    constexpr const char* kVertexShaderSource = R"(
        struct Output { float4 position : SV_Position; };
        Output VSMain(uint vertexId : SV_VertexID)
        {
            const float2 position = vertexId == 0
                ? float2(-0.5, -0.5)
                : (vertexId == 1 ? float2(0.5, -0.5) : float2(0.0, 0.5));
            Output output;
            output.position = float4(position, 0.0, 1.0);
            return output;
        }
    )";
    constexpr const char* kPixelShaderSource = R"(
        float4 PSMain() : SV_Target0 { return float4(0.2, 0.8, 0.1, 1.0); }
    )";

    RHIShaderRef vertexShader = CompileInlineVulkanGraphicsShader(
        *device, RHIShaderStage::Vertex, "VSMain", kVertexShaderSource,
        "vs_6_0", "VulkanNativeIndexedIndirectVS");
    RHIShaderRef pixelShader = CompileInlineVulkanGraphicsShader(
        *device, RHIShaderStage::Pixel, "PSMain", kPixelShaderSource,
        "ps_6_0", "VulkanNativeIndexedIndirectPS");
    ASSERT_NE(vertexShader.Get(), nullptr);
    ASSERT_NE(pixelShader.Get(), nullptr);

    RHIPipelineLayoutDesc layoutDesc;
    layoutDesc.debugName = "VulkanNativeIndexedIndirectLayout";
    auto pipelineLayout = device->CreatePipelineLayout(layoutDesc);
    ASSERT_NE(pipelineLayout.Get(), nullptr);

    RHIGraphicsPipelineDesc pipelineDesc;
    pipelineDesc.vertexShader = vertexShader.Get();
    pipelineDesc.pixelShader = pixelShader.Get();
    pipelineDesc.pipelineLayout = pipelineLayout.Get();
    pipelineDesc.rasterizerState = RHIRasterizerState::NoCull();
    pipelineDesc.depthStencilState = RHIDepthStencilState::Disabled();
    pipelineDesc.numRenderTargets = 1;
    pipelineDesc.renderTargetFormats[0] = RHIFormat::RGBA8_UNORM;
    pipelineDesc.depthStencilFormat = RHIFormat::Unknown;
    pipelineDesc.primitiveTopology = RHIPrimitiveTopology::TriangleList;
    pipelineDesc.debugName = "VulkanNativeIndexedIndirectPipeline";
    auto pipeline = device->CreateGraphicsPipeline(pipelineDesc);
    ASSERT_NE(pipeline.Get(), nullptr);

    const std::array<uint32, 3> indices{0u, 1u, 2u};
    auto indexBuffer = CreateVulkanUploadBuffer(
        *device, RHIBufferUsage::Index, sizeof(uint32), indices.data(),
        sizeof(indices), "VulkanNativeIndexedIndirectIndices");
    const std::array<uint32, 5> command{3u, 1u, 0u, 0u, 0u};
    auto argumentBuffer = CreateVulkanUploadBuffer(
        *device, RHIBufferUsage::IndirectArgs, sizeof(command), command.data(),
        sizeof(command), "VulkanNativeIndexedIndirectArguments");
    const uint32 drawCount = 1;
    auto countBuffer = CreateVulkanUploadBuffer(
        *device, RHIBufferUsage::IndirectArgs, sizeof(uint32), &drawCount,
        sizeof(drawCount), "VulkanNativeIndexedIndirectCount");
    ASSERT_NE(indexBuffer.Get(), nullptr);
    ASSERT_NE(argumentBuffer.Get(), nullptr);
    ASSERT_NE(countBuffer.Get(), nullptr);

    auto renderTarget = device->CreateTexture(
        RHITextureDesc::RenderTarget(32, 32, RHIFormat::RGBA8_UNORM));
    ASSERT_NE(renderTarget.Get(), nullptr);
    auto renderTargetView = CreateVulkanRenderTargetView(*device, renderTarget.Get());
    ASSERT_NE(renderTargetView.Get(), nullptr);

    auto context = device->CreateCommandContext(RHICommandQueueType::Graphics);
    auto fence = device->CreateFence(0);
    ASSERT_NE(context.Get(), nullptr);
    ASSERT_NE(fence.Get(), nullptr);

    const VulkanValidationMessageCounts messagesBefore =
        vulkanDevice->GetValidationMessageCounts();
    context->Begin();
    context->BufferBarrier(
        {indexBuffer.Get(), RHIResourceState::Common, RHIResourceState::IndexBuffer});
    context->BufferBarrier({argumentBuffer.Get(), RHIResourceState::Common,
                            RHIResourceState::IndirectArgument});
    context->BufferBarrier({countBuffer.Get(), RHIResourceState::Common,
                            RHIResourceState::IndirectArgument});
    context->TextureBarrier({renderTarget.Get(), RHIResourceState::Undefined,
                             RHIResourceState::RenderTarget});

    RHIRenderPassDesc renderPass;
    renderPass.AddColorAttachment(renderTargetView.Get(), RHILoadOp::Clear,
                                  RHIStoreOp::Store, {0.0f, 0.0f, 0.0f, 1.0f});
    context->BeginRenderPass(renderPass);
    context->SetViewport({0.0f, 0.0f, 32.0f, 32.0f, 0.0f, 1.0f});
    context->SetScissor({0, 0, 32, 32});
    context->SetPipeline(pipeline.Get());
    context->SetIndexBuffer(indexBuffer.Get(), RHIFormat::R32_UINT);
    context->DrawIndexedIndirect(argumentBuffer.Get(), 0, 1, sizeof(command));
    if (device->GetCapabilities().indexedIndirectExecution.supportsCountBuffer)
    {
        context->DrawIndexedIndirectCount(argumentBuffer.Get(),
                                          0,
                                          countBuffer.Get(),
                                          0,
                                          1,
                                          sizeof(command));
    }
    context->EndRenderPass();
    context->End();

    const uint64 submittedValue = device->SubmitCommandContext(context.Get(), fence.Get());
    ASSERT_NE(submittedValue, 0u);
    device->WaitForFence(fence.Get(), submittedValue);
    device->WaitIdle();

    const VulkanValidationMessageCounts messagesAfter =
        vulkanDevice->GetValidationMessageCounts();
    EXPECT_EQ(messagesAfter.errors, messagesBefore.errors);
    EXPECT_EQ(messagesAfter.warnings, messagesBefore.warnings);
}

TEST(VulkanValidation, InvalidIndexedIndirectCallsAreRejectedBeforeNativeRecording)
{
    RHIDeviceDesc deviceDesc;
    deviceDesc.enableDebugLayer = true;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);

    auto* vulkanDevice = dynamic_cast<VulkanDevice*>(device.get());
    ASSERT_NE(vulkanDevice, nullptr);

    const std::array<uint32, 5> command{3u, 1u, 0u, 0u, 0u};
    auto argumentBuffer = CreateVulkanUploadBuffer(
        *device, RHIBufferUsage::IndirectArgs, sizeof(command), command.data(),
        sizeof(command), "VulkanInvalidIndexedIndirectArguments");
    const uint32 drawCount = 1;
    auto countBuffer = CreateVulkanUploadBuffer(
        *device, RHIBufferUsage::IndirectArgs, sizeof(uint32), &drawCount,
        sizeof(drawCount), "VulkanInvalidIndexedIndirectCount");
    auto wrongUsageBuffer = CreateVulkanUploadBuffer(
        *device, RHIBufferUsage::Vertex, sizeof(command), command.data(),
        sizeof(command), "VulkanInvalidIndexedIndirectWrongUsage");
    ASSERT_NE(argumentBuffer.Get(), nullptr);
    ASSERT_NE(countBuffer.Get(), nullptr);
    ASSERT_NE(wrongUsageBuffer.Get(), nullptr);

    auto context = device->CreateCommandContext(RHICommandQueueType::Graphics);
    auto fence = device->CreateFence(0);
    ASSERT_NE(context.Get(), nullptr);
    ASSERT_NE(fence.Get(), nullptr);

    const VulkanValidationMessageCounts messagesBefore =
        vulkanDevice->GetValidationMessageCounts();
    context->Begin();

    // Zero work is a legal no-op and must not dereference either buffer.
    context->DrawIndexedIndirect(nullptr, 0, 0, 0);
    context->DrawIndexedIndirectCount(nullptr, 0, nullptr, 0, 0, 0);

    // Each non-zero call violates a public RHI contract. If any reaches the
    // native command buffer it is also invalid outside a render pass, which
    // the validation-layer counter below detects.
    context->DrawIndexedIndirect(wrongUsageBuffer.Get(), 0, 1,
                                 sizeof(command));
    context->DrawIndexedIndirect(argumentBuffer.Get(), 2, 1,
                                 sizeof(command));
    context->DrawIndexedIndirect(argumentBuffer.Get(), 0, 2,
                                 sizeof(command));

    if (device->GetCapabilities().indexedIndirectExecution.supportsCountBuffer)
    {
        context->DrawIndexedIndirectCount(argumentBuffer.Get(), 0, nullptr, 0,
                                          1, sizeof(command));
        context->DrawIndexedIndirectCount(argumentBuffer.Get(), 0,
                                          wrongUsageBuffer.Get(), 0, 1,
                                          sizeof(command));
        context->DrawIndexedIndirectCount(argumentBuffer.Get(), 0,
                                          countBuffer.Get(), sizeof(uint32), 1,
                                          sizeof(command));
        context->DrawIndexedIndirectCount(argumentBuffer.Get(), 0,
                                          countBuffer.Get(), 0, 1,
                                          sizeof(command) - sizeof(uint32));
    }
    context->End();

    const uint64 submittedValue =
        device->SubmitCommandContext(context.Get(), fence.Get());
    ASSERT_NE(submittedValue, 0u);
    device->WaitForFence(fence.Get(), submittedValue);
    device->WaitIdle();

    const VulkanValidationMessageCounts messagesAfter =
        vulkanDevice->GetValidationMessageCounts();
    EXPECT_EQ(messagesAfter.errors, messagesBefore.errors);
    EXPECT_EQ(messagesAfter.warnings, messagesBefore.warnings);
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
    EXPECT_TRUE(caps.supportsQueueSubmissionPlan);
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
    uint8 expectedActiveDomainCount = 1;
    if (expectedCompute != GPUQueueDomain::Graphics)
    {
        ++expectedActiveDomainCount;
    }
    if (expectedCopy != GPUQueueDomain::Graphics &&
        expectedCopy != expectedCompute)
    {
        ++expectedActiveDomainCount;
    }
    EXPECT_EQ(caps.queueTopology.activeDomainCount,
              expectedActiveDomainCount);
    EXPECT_EQ(caps.supportsAsyncCompute, !computeAliasesGraphics);
}

TEST(VulkanValidation, QueueSubmissionPlanExecutesCopyComputeGraphicsDag)
{
    RHIDeviceDesc deviceDesc;
    deviceDesc.enableDebugLayer = true;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);
    ASSERT_TRUE(device->GetCapabilities().supportsQueueSubmissionPlan);

    auto* vulkanDevice = static_cast<VulkanDevice*>(device.get());
    const VulkanValidationMessageCounts validationBefore =
        vulkanDevice->GetValidationMessageCounts();
    const std::array<RHICommandQueueType, 6> queueTypes = {
        RHICommandQueueType::Copy,
        RHICommandQueueType::Compute,
        RHICommandQueueType::Graphics,
        RHICommandQueueType::Compute,
        RHICommandQueueType::Graphics,
        RHICommandQueueType::Graphics,
    };
    std::array<RHICommandContextRef, 6> contexts;
    RHIQueueSubmissionPlan plan;
    for (uint32 index = 0; index < contexts.size(); ++index)
    {
        contexts[index] = device->CreateCommandContext(queueTypes[index]);
        ASSERT_NE(contexts[index].Get(), nullptr);
        contexts[index]->Begin();
        contexts[index]->End();
        RHIQueueSubmissionBatch batch;
        batch.queueType = queueTypes[index];
        batch.contexts.push_back(contexts[index].Get());
        if (index != 0)
        {
            batch.prerequisiteBatchIndices.push_back(index - 1u);
        }
        plan.batches.push_back(std::move(batch));
    }
    plan.terminalGraphicsBatchIndex = 5;
    ASSERT_TRUE(ValidateRHIQueueSubmissionPlan(plan));

    RHIFenceRef terminalFence = device->CreateFence(0);
    ASSERT_NE(terminalFence.Get(), nullptr);
    const uint64 submittedValue =
        device->SubmitQueuePlan(plan, terminalFence.Get());
    ASSERT_NE(submittedValue, 0u);
    device->WaitForFence(terminalFence.Get(), submittedValue);
    device->WaitIdle();

    const VulkanValidationMessageCounts validationAfter =
        vulkanDevice->GetValidationMessageCounts();
    EXPECT_EQ(validationAfter.errors, validationBefore.errors);
    EXPECT_EQ(validationAfter.warnings, validationBefore.warnings);
}

TEST(VulkanValidation,
     RenderGraphExportReleasesUntouchedComputeOwnedBufferRanges)
{
    RHIDeviceDesc deviceDesc;
    deviceDesc.enableDebugLayer = true;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);
    ASSERT_TRUE(device->GetCapabilities().supportsQueueSubmissionPlan);

    auto* vulkanDevice = static_cast<VulkanDevice*>(device.get());
    if (vulkanDevice->GetComputeQueueFamily() ==
        vulkanDevice->GetGraphicsQueueFamily())
    {
        GTEST_SKIP() << "Adapter has no dedicated Compute queue family";
    }
    const VulkanValidationMessageCounts validationBefore =
        vulkanDevice->GetValidationMessageCounts();

    constexpr uint64 bufferSize = 384;
    constexpr uint64 dirtyOffset = 96;
    constexpr uint64 dirtySize = 192;
    RHIBufferDesc bufferDesc;
    bufferDesc.size = bufferSize;
    bufferDesc.usage = RHIBufferUsage::CopyDst |
                       RHIBufferUsage::Structured;
    bufferDesc.memoryType = RHIMemoryType::Default;
    bufferDesc.stride = sizeof(uint32);
    bufferDesc.debugName = "VulkanExternalRangeExport";
    RHIBufferRef buffer = device->CreateBuffer(bufferDesc);
    ASSERT_NE(buffer.Get(), nullptr);

    const RHIAccessSnapshot undefinedCompute = MakeRHIAccessSnapshot(
        RHIResourceState::Undefined,
        RHIShaderStage::None,
        GPUQueueDomain::Compute,
        RHIContentValidity::Invalid);
    const RHIAccessSnapshot computeRead = MakeRHIAccessSnapshot(
        RHIResourceState::ShaderResource,
        RHIShaderStage::Compute,
        GPUQueueDomain::Compute,
        RHIContentValidity::Valid);
    RHICommandContextRef establishOwner =
        device->CreateCommandContext(RHICommandQueueType::Compute);
    ASSERT_NE(establishOwner.Get(), nullptr);
    establishOwner->Begin();
    establishOwner->BufferBarrier(MakeRHIBufferBarrier(
        buffer.Get(),
        undefinedCompute,
        computeRead,
        0,
        RVX_WHOLE_SIZE,
        RHIDiscardIntent::Discard));
    establishOwner->End();
    RHIFenceRef establishFence = device->CreateFence(0);
    ASSERT_NE(establishFence.Get(), nullptr);
    const uint64 establishValue =
        device->SubmitCommandContext(establishOwner.Get(),
                                     establishFence.Get());
    ASSERT_NE(establishValue, 0u);
    device->WaitForFence(establishFence.Get(), establishValue);

    RenderGraph graph;
    graph.SetDevice(device.get());
    ASSERT_TRUE(graph.SetQueueExecutionMode(
        RenderGraph::QueueExecutionMode::MultiQueue));
    const RGBufferHandle resource = graph.ImportBuffer(
        buffer.Get(), RHIBufferAccessSnapshot{computeRead, {}});
    struct Data
    {
        RGBufferHandle buffer;
    };
    graph.AddPass<Data>(
        "VulkanPartialCopyUpdate",
        RenderGraphPassType::Copy,
        [resource](RenderGraphBuilder& builder, Data& data)
        {
            data.buffer = builder.Write(
                resource.Range(dirtyOffset, dirtySize),
                RHIResourceState::CopyDest);
        },
        [](const Data&, RHICommandContext&) {});
    graph.SetExportAccess(
        resource,
        MakeRHIAccessSnapshot(
            RHIResourceState::ShaderResource,
            RHIShaderStage::AllGraphics,
            GPUQueueDomain::Graphics,
            RHIContentValidity::Valid));
    graph.Compile();
    ASSERT_TRUE(graph.GetCompileStats().compileValid);

    RenderGraph::RecordedQueueSubmission recorded;
    ASSERT_TRUE(graph.RecordQueueSubmission(recorded));
    ASSERT_TRUE(ValidateRHIQueueSubmissionPlan(recorded.plan));
    RHIFenceRef terminalFence = device->CreateFence(0);
    ASSERT_NE(terminalFence.Get(), nullptr);
    const uint64 terminalValue =
        device->SubmitQueuePlan(recorded.plan, terminalFence.Get());
    ASSERT_NE(terminalValue, 0u);
    device->WaitForFence(terminalFence.Get(), terminalValue);
    device->WaitIdle();

    const VulkanValidationMessageCounts validationAfter =
        vulkanDevice->GetValidationMessageCounts();
    EXPECT_EQ(validationAfter.errors, validationBefore.errors);
    EXPECT_EQ(validationAfter.warnings, validationBefore.warnings);
}

TEST(VulkanValidation, UploadGatewayTransfersCopyOwnershipToGraphics)
{
    RHIDeviceDesc deviceDesc;
    deviceDesc.enableDebugLayer = true;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);

    auto* vulkanDevice = static_cast<VulkanDevice*>(device.get());
    const VulkanValidationMessageCounts messagesBefore =
        vulkanDevice->GetValidationMessageCounts();

    GPUUploadService uploadService;
    uploadService.Initialize(device.get());
    ASSERT_TRUE(uploadService.IsInitialized());

    const std::array<uint32, 4> source = {1, 2, 3, 4};
    GPUUploadBufferDesc desc;
    desc.size = sizeof(source);
    desc.usage = RHIBufferUsage::Vertex;
    desc.stride = sizeof(uint32);
    desc.debugName = "VulkanCopyOwnershipUpload";
    const GPUUploadBufferResult result =
        uploadService.UploadBufferDataWithResult(
            desc, source.data(), sizeof(source));
    ASSERT_TRUE(result.succeeded);
    ASSERT_TRUE(result.isPending);
    EXPECT_EQ(result.finalAccess.domain, GPUQueueDomain::Graphics);
    EXPECT_EQ(uploadService.FlushAndWaitForUploads(), 1U);
    EXPECT_TRUE(uploadService.IsUploadComplete(result.uploadId));

    uploadService.Shutdown();
    device->WaitIdle();
    const VulkanValidationMessageCounts messagesAfter =
        vulkanDevice->GetValidationMessageCounts();
    EXPECT_EQ(messagesAfter.errors, messagesBefore.errors);
    EXPECT_EQ(messagesAfter.warnings, messagesBefore.warnings);
}

TEST(VulkanValidation, PairedBufferOwnershipTransferComputeToGraphics)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);

    auto* vulkanDevice = dynamic_cast<VulkanDevice*>(device.get());
    ASSERT_NE(vulkanDevice, nullptr);
    if (vulkanDevice->GetComputeQueueFamily() ==
        vulkanDevice->GetGraphicsQueueFamily())
    {
        GTEST_SKIP() << "Adapter has no dedicated Compute queue family";
    }

    const VulkanValidationMessageCounts validationBefore =
        vulkanDevice->GetValidationMessageCounts();

    std::array<uint32, 64> sourceData{};
    for (uint32 index = 0; index < sourceData.size(); ++index)
    {
        sourceData[index] = index + 1U;
    }
    RHIBufferRef source = CreateVulkanUploadBuffer(
        *device,
        RHIBufferUsage::CopySrc,
        sizeof(uint32),
        sourceData.data(),
        sizeof(sourceData),
        "QueueOwnershipSource");
    ASSERT_NE(source.Get(), nullptr);

    RHIBufferDesc destinationDesc;
    destinationDesc.size = sizeof(sourceData);
    destinationDesc.usage = RHIBufferUsage::CopyDst |
                            RHIBufferUsage::CopySrc |
                            RHIBufferUsage::Structured;
    destinationDesc.memoryType = RHIMemoryType::Default;
    destinationDesc.stride = sizeof(uint32);
    destinationDesc.debugName = "QueueOwnershipDestination";
    RHIBufferRef destination = device->CreateBuffer(destinationDesc);
    ASSERT_NE(destination.Get(), nullptr);

    const RHIAccessSnapshot computeCommon = MakeRHIAccessSnapshot(
        RHIResourceState::Common,
        RHIShaderStage::Compute,
        GPUQueueDomain::Compute,
        RHIContentValidity::Valid);
    const RHIAccessSnapshot computeCopyDestination = MakeRHIAccessSnapshot(
        RHIResourceState::CopyDest,
        RHIShaderStage::None,
        GPUQueueDomain::Compute,
        RHIContentValidity::Valid);
    const RHIAccessSnapshot graphicsCopySource = MakeRHIAccessSnapshot(
        RHIResourceState::CopySource,
        RHIShaderStage::None,
        GPUQueueDomain::Graphics,
        RHIContentValidity::Valid);
    const RHIBufferBarrier ownershipTransfer = MakeRHIBufferBarrier(
        destination.Get(),
        computeCopyDestination,
        graphicsCopySource);
    ASSERT_TRUE(HasDependencyKind(ownershipTransfer.dependencyKind,
                                  RHIDependencyKind::Ownership));

    RHICommandContextRef computeContext =
        device->CreateCommandContext(RHICommandQueueType::Compute);
    RHICommandContextRef graphicsContext =
        device->CreateCommandContext(RHICommandQueueType::Graphics);
    ASSERT_NE(computeContext.Get(), nullptr);
    ASSERT_NE(graphicsContext.Get(), nullptr);

    computeContext->Begin();
    computeContext->BufferBarrier(destination.Get(),
                                  computeCommon,
                                  computeCopyDestination);
    computeContext->CopyBuffer(source.Get(),
                               destination.Get(),
                               0,
                               0,
                               sizeof(sourceData));
    computeContext->BufferBarrier(ownershipTransfer);
    computeContext->End();

    graphicsContext->Begin();
    graphicsContext->BufferBarrier(ownershipTransfer);
    graphicsContext->End();

    RHIFenceRef completionFence = device->CreateFence(0);
    ASSERT_NE(completionFence.Get(), nullptr);
    std::array<RHICommandContext*, 2> contexts = {
        computeContext.Get(),
        graphicsContext.Get(),
    };
    const uint64 completionValue =
        device->SubmitCommandContexts(contexts, completionFence.Get());
    ASSERT_NE(completionValue, 0u);
    device->WaitForFence(completionFence.Get(), completionValue);
    device->WaitIdle();

    const VulkanValidationMessageCounts validationAfter =
        vulkanDevice->GetValidationMessageCounts();
    EXPECT_EQ(validationAfter.errors, validationBefore.errors);
    EXPECT_EQ(validationAfter.warnings, validationBefore.warnings);
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
    GPUQueueDomain computeDomain = GPUQueueDomain::Graphics;
    TryGetGPUQueueDomain(device->GetCapabilities().queueTopology,
                         RHICommandQueueType::Compute,
                         computeDomain);
    const RHIAccessSnapshot unorderedAccess = MakeRHIAccessSnapshot(
        RHIResourceState::UnorderedAccess,
        RHIShaderStage::Compute,
        computeDomain,
        RHIContentValidity::Valid);
    ctx->BufferBarrier(buffer.Get(),
                       RHIResourceState::Common,
                       RHIResourceState::UnorderedAccess);
    ctx->BufferBarrier(MakeRHIBufferBarrier(
        buffer.Get(), unorderedAccess, unorderedAccess));

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
