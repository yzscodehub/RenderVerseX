#include "Common/GpuTestUtils.h"
#include "Common/RenderGraphValidationAccess.h"
#include "Core/Core.h"
#include "Render/Context/RenderContext.h"
#include "Render/Graph/RenderGraph.h"
#include "Render/GPUUploadService.h"
#include "Render/Submission/RasterInstanceStream.h"
#include "RHI/RHI.h"
#include "RHI_BackendFactory/RHIBackendFactory.h"
#include "ShaderCompiler/ShaderCompiler.h"
#include "VulkanCommon.h"
#include "VulkanDevice.h"
#include "VulkanResources.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace RVX;

namespace
{
    class LegacyMappedBuffer : public RHIBuffer
    {
    public:
        uint64 GetSize() const override { return m_storage.size(); }
        RHIBufferUsage GetUsage() const override { return RHIBufferUsage::CopyDst; }
        RHIMemoryType GetMemoryType() const override { return RHIMemoryType::Upload; }
        uint32 GetStride() const override { return sizeof(uint32); }

        void* Map() override
        {
            ++m_mapCount;
            return m_storage.data();
        }

        void Unmap() override
        {
            ++m_unmapCount;
        }

        std::array<uint8, 16> m_storage{};
        uint32 m_mapCount = 0;
        uint32 m_unmapCount = 0;
    };

    class FailingCommitBuffer final : public LegacyMappedBuffer
    {
    public:
        bool CommitMappedWrite() override
        {
            ++m_commitCount;
            if (m_failNextCommit)
            {
                m_failNextCommit = false;
                return false;
            }

            Unmap();
            return true;
        }

        uint32 m_commitCount = 0;
        bool m_failNextCommit = true;
    };

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

TEST(VulkanValidation, DriverIdentityMatchesPhysicalDeviceDriverEvidence)
{
    RHIDeviceDesc desc;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, desc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);

    auto* vulkanDevice = dynamic_cast<VulkanDevice*>(device.get());
    ASSERT_NE(vulkanDevice, nullptr);

    VkPhysicalDeviceDriverProperties driverProperties = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 properties2 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties2.pNext = &driverProperties;
    vkGetPhysicalDeviceProperties2(vulkanDevice->GetPhysicalDevice(),
                                   &properties2);
    const VkPhysicalDeviceProperties& properties = properties2.properties;
    ASSERT_GE(properties.apiVersion, VK_API_VERSION_1_3);

    std::string expectedVersion;
    const auto appendEvidence = [&expectedVersion](const char* label,
                                                    const std::string& value)
    {
        if (value.empty())
        {
            return;
        }

        if (!expectedVersion.empty())
        {
            expectedVersion += "; ";
        }
        expectedVersion += label;
        expectedVersion += "=";
        expectedVersion += value;
    };
    appendEvidence("driverName", driverProperties.driverName);
    appendEvidence("driverInfo", driverProperties.driverInfo);
    if (properties.driverVersion != 0)
    {
        std::array<char, 11> rawDriverVersion = {};
        std::snprintf(rawDriverVersion.data(),
                      rawDriverVersion.size(),
                      "0x%08x",
                      static_cast<unsigned int>(properties.driverVersion));
        appendEvidence("rawDriverVersion",
                       rawDriverVersion.data());
    }

    ASSERT_FALSE(expectedVersion.empty());
    const std::string& reportedVersion =
        device->GetCapabilities().driverVersion;
    EXPECT_FALSE(reportedVersion.empty());
    EXPECT_EQ(reportedVersion, expectedVersion);
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

    constexpr uint32 kRenderTargetWidth = 32u;
    constexpr uint32 kRenderTargetHeight = 32u;
    constexpr uint32 kReadbackRowPitch = kRenderTargetWidth * 4u;
    constexpr uint32 kTopWideSampleX = 11u;
    constexpr uint32 kTopWideSampleY = 10u;
    constexpr uint32 kMirroredBottomSampleY =
        kRenderTargetHeight - 1u - kTopWideSampleY;

    auto renderTarget = device->CreateTexture(
        RHITextureDesc::RenderTarget(kRenderTargetWidth,
                                     kRenderTargetHeight,
                                     RHIFormat::RGBA8_UNORM));
    ASSERT_NE(renderTarget.Get(), nullptr);
    auto renderTargetView = CreateVulkanRenderTargetView(*device, renderTarget.Get());
    ASSERT_NE(renderTargetView.Get(), nullptr);

    RHIBufferDesc readbackDesc;
    readbackDesc.size = static_cast<uint64>(kReadbackRowPitch) *
                        kRenderTargetHeight;
    readbackDesc.usage = RHIBufferUsage::CopyDst;
    readbackDesc.memoryType = RHIMemoryType::Readback;
    readbackDesc.debugName = "VulkanNativeIndexedIndirectReadback";
    RHIBufferRef readbackBuffer = device->CreateBuffer(readbackDesc);
    ASSERT_NE(readbackBuffer.Get(), nullptr);

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
    context->SetViewport({0.0f,
                          0.0f,
                          static_cast<float>(kRenderTargetWidth),
                          static_cast<float>(kRenderTargetHeight),
                          0.0f,
                          1.0f});
    context->SetScissor({0, 0, kRenderTargetWidth, kRenderTargetHeight});
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
    context->TextureBarrier({renderTarget.Get(), RHIResourceState::RenderTarget,
                             RHIResourceState::CopySource});
    context->BufferBarrier({readbackBuffer.Get(), RHIResourceState::Common,
                            RHIResourceState::CopyDest});
    RHIBufferTextureCopyDesc readbackCopy;
    readbackCopy.bufferRowPitch = kReadbackRowPitch;
    readbackCopy.textureRegion = {0, 0, kRenderTargetWidth, kRenderTargetHeight};
    context->CopyTextureToBuffer(renderTarget.Get(), readbackBuffer.Get(),
                                 readbackCopy);
    context->BufferBarrier({readbackBuffer.Get(), RHIResourceState::CopyDest,
                            RHIResourceState::Common});
    context->End();

    const uint64 submittedValue = device->SubmitCommandContext(context.Get(), fence.Get());
    ASSERT_NE(submittedValue, 0u);
    device->WaitForFence(fence.Get(), submittedValue);
    EXPECT_GE(fence->GetCompletedValue(), submittedValue);

    const auto* readbackPixels =
        static_cast<const uint8*>(readbackBuffer->Map());
    ASSERT_NE(readbackPixels, nullptr);
    const auto* topWidePixel = readbackPixels +
        static_cast<size_t>(kTopWideSampleY) * kReadbackRowPitch +
        static_cast<size_t>(kTopWideSampleX) * 4u;
    const auto* mirroredBottomPixel = readbackPixels +
        static_cast<size_t>(kMirroredBottomSampleY) * kReadbackRowPitch +
        static_cast<size_t>(kTopWideSampleX) * 4u;

    // Native Vulkan's positive-height viewport maps clip -Y to top raster
    // rows, and CopyTextureToBuffer preserves those rows top-to-bottom. The
    // production fullscreen helper converts the engine's upper-left texture
    // UV contract to this backend-native raster convention.
    EXPECT_GT(topWidePixel[1], 128u)
        << "Expected top-wide sample at (" << kTopWideSampleX << ", "
        << kTopWideSampleY << ") to be green; observed RGBA=("
        << static_cast<uint32>(topWidePixel[0]) << ", "
        << static_cast<uint32>(topWidePixel[1]) << ", "
        << static_cast<uint32>(topWidePixel[2]) << ", "
        << static_cast<uint32>(topWidePixel[3]) << ")";
    EXPECT_GT(topWidePixel[1], topWidePixel[0]);
    EXPECT_GT(topWidePixel[1], topWidePixel[2]);
    EXPECT_EQ(topWidePixel[3], 255u);
    EXPECT_EQ(mirroredBottomPixel[0], 0u)
        << "Expected vertically mirrored clear sample at ("
        << kTopWideSampleX << ", " << kMirroredBottomSampleY
        << "); observed RGBA=(" << static_cast<uint32>(mirroredBottomPixel[0])
        << ", " << static_cast<uint32>(mirroredBottomPixel[1]) << ", "
        << static_cast<uint32>(mirroredBottomPixel[2]) << ", "
        << static_cast<uint32>(mirroredBottomPixel[3]) << ")";
    EXPECT_EQ(mirroredBottomPixel[1], 0u);
    EXPECT_EQ(mirroredBottomPixel[2], 0u);
    EXPECT_EQ(mirroredBottomPixel[3], 255u);
    readbackBuffer->Unmap();
    device->WaitIdle();

    const VulkanValidationMessageCounts messagesAfter =
        vulkanDevice->GetValidationMessageCounts();
    EXPECT_EQ(messagesAfter.errors, messagesBefore.errors);
    EXPECT_EQ(messagesAfter.warnings, messagesBefore.warnings);
}

TEST(VulkanValidation,
     IndexedIndirectCountFirstInstanceRasterMatchesDirectInstanceInput)
{
    constexpr uint32 kWidth = 64;
    constexpr uint32 kHeight = 32;
    constexpr uint32 kRowPitch = kWidth * 4;
    constexpr uint32 kFirstInstance = 2;

    RHIDeviceDesc deviceDesc;
    deviceDesc.enableDebugLayer = true;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);
    auto* vulkanDevice = dynamic_cast<VulkanDevice*>(device.get());
    ASSERT_NE(vulkanDevice, nullptr);

    const RHIIndexedIndirectExecutionCapabilities& indirectCapabilities =
        device->GetCapabilities().indexedIndirectExecution;
    if (!indirectCapabilities.supportsFirstInstance ||
        !indirectCapabilities.supportsCountBuffer ||
        indirectCapabilities.maxDrawCount < 3u)
    {
        GTEST_SKIP() << "Vulkan drawIndirectFirstInstance/count support or its "
                     << "three-draw limit is unavailable; the test intentionally "
                     << "has no fixed-count fallback.";
    }

    constexpr const char* kVertexShaderSource = R"(
        struct VertexInput
        {
            uint instanceIndex : INSTANCEINDEX;
            uint instancePadding : INSTANCEPADDING;
            uint vertexId : SV_VertexID;
        };
        struct VertexOutput
        {
            float4 position : SV_Position;
            nointerpolation float4 color : TEXCOORD0;
        };
        VertexOutput VSMain(VertexInput input)
        {
            const float2 localPosition = input.vertexId == 0
                ? float2(-0.17, -0.28)
                : (input.vertexId == 1
                    ? float2(0.17, -0.28)
                    : float2(0.00, 0.28));
            VertexOutput output;
            if (input.instanceIndex == 2u)
            {
                output.position = float4(localPosition + float2(-0.45, 0.0),
                                         0.0,
                                         1.0);
                output.color = float4(1.0, 0.0, 0.0, 1.0);
            }
            else if (input.instanceIndex == 3u)
            {
                output.position = float4(localPosition + float2(0.45, 0.0),
                                         0.0,
                                         1.0);
                output.color = float4(0.0, 1.0, 0.0, 1.0);
            }
            else if (input.instanceIndex == 4u)
            {
                // This row belongs to the second command in both paths. Its
                // center pixel proves the indirect command stride selected the
                // intended second payload rather than an adjacent field.
                output.position = float4(localPosition, 0.0, 1.0);
                output.color = float4(0.0, 0.0, 1.0, 1.0);
            }
            else if (input.instanceIndex == 5u)
            {
                // This row belongs only to the third indirect command. A
                // correct count of two must never expose it.
                output.position = float4(localPosition + float2(0.0, 0.60),
                                         0.0,
                                         1.0);
                output.color = float4(1.0, 0.0, 1.0, 1.0);
            }
            else
            {
                // The two rows before firstInstance are deliberate sentinels.
                // A backend that ignores firstInstance therefore cannot paint
                // either required foreground sample below.
                output.position = float4(4.0, 4.0, 0.0, 1.0);
                output.color = float4(0.0, 0.0, 1.0, 1.0);
            }
            if (input.instancePadding != 0u)
            {
                // The fixture initializes this production-ABI padding to zero.
                // A non-zero value must be visibly wrong, so the compiler keeps
                // the second slot-6 attribute live and validates the full stride.
                output.position = float4(localPosition + float2(0.0, -0.60),
                                         0.0,
                                         1.0);
                output.color = float4(1.0, 1.0, 0.0, 1.0);
            }
            return output;
        }
    )";
    constexpr const char* kPixelShaderSource = R"(
        struct VertexOutput
        {
            float4 position : SV_Position;
            nointerpolation float4 color : TEXCOORD0;
        };
        float4 PSMain(VertexOutput input) : SV_Target0
        {
            return input.color * saturate(input.position.w);
        }
    )";

    RHIShaderRef vertexShader = CompileInlineVulkanGraphicsShader(
        *device, RHIShaderStage::Vertex, "VSMain", kVertexShaderSource,
        "vs_6_0", "VulkanFirstInstanceParityVS");
    RHIShaderRef pixelShader = CompileInlineVulkanGraphicsShader(
        *device, RHIShaderStage::Pixel, "PSMain", kPixelShaderSource,
        "ps_6_0", "VulkanFirstInstanceParityPS");
    ASSERT_NE(vertexShader.Get(), nullptr);
    ASSERT_NE(pixelShader.Get(), nullptr);

    RHIPipelineLayoutDesc layoutDesc;
    layoutDesc.debugName = "VulkanFirstInstanceParityLayout";
    RHIPipelineLayoutRef pipelineLayout = device->CreatePipelineLayout(layoutDesc);
    ASSERT_NE(pipelineLayout.Get(), nullptr);

    RHIGraphicsPipelineDesc pipelineDesc;
    pipelineDesc.vertexShader = vertexShader.Get();
    pipelineDesc.pixelShader = pixelShader.Get();
    pipelineDesc.pipelineLayout = pipelineLayout.Get();
    pipelineDesc.inputLayout.AddElement("INSTANCEINDEX", RHIFormat::R32_UINT, 6);
    pipelineDesc.inputLayout.elements.back().alignedByteOffset =
        static_cast<uint32>(offsetof(GPUInstanceData, sourceIndex));
    pipelineDesc.inputLayout.elements.back().perInstance = true;
    pipelineDesc.inputLayout.elements.back().instanceDataStepRate = 1;
    // Keep the Vulkan binding stride equal to the production GPUInstanceData
    // ABI instead of reducing it to sourceIndex's field end at byte 200.
    pipelineDesc.inputLayout.AddElement("INSTANCEPADDING", RHIFormat::R32_UINT, 6);
    pipelineDesc.inputLayout.elements.back().alignedByteOffset =
        static_cast<uint32>(offsetof(GPUInstanceData, padding) + sizeof(uint32));
    pipelineDesc.inputLayout.elements.back().perInstance = true;
    pipelineDesc.inputLayout.elements.back().instanceDataStepRate = 1;
    pipelineDesc.rasterizerState = RHIRasterizerState::NoCull();
    pipelineDesc.depthStencilState = RHIDepthStencilState::Disabled();
    pipelineDesc.numRenderTargets = 1;
    pipelineDesc.renderTargetFormats[0] = RHIFormat::RGBA8_UNORM;
    pipelineDesc.depthStencilFormat = RHIFormat::Unknown;
    pipelineDesc.primitiveTopology = RHIPrimitiveTopology::TriangleList;
    pipelineDesc.debugName = "VulkanFirstInstanceParityPipeline";
    RHIPipelineRef pipeline = device->CreateGraphicsPipeline(pipelineDesc);
    ASSERT_NE(pipeline.Get(), nullptr);

    const std::array<uint32, 3> indices{0u, 1u, 2u};
    const std::array<GPUInstanceData, 6> instanceRows = []
    {
        std::array<GPUInstanceData, 6> rows{};
        rows[0].sourceIndex = 0xA11CE001u;
        rows[1].sourceIndex = 0xA11CE002u;
        rows[2].sourceIndex = 2u;
        rows[3].sourceIndex = 3u;
        rows[4].sourceIndex = 4u;
        rows[5].sourceIndex = 5u;
        return rows;
    }();
    auto indexBuffer = CreateVulkanUploadBuffer(
        *device, RHIBufferUsage::Index, sizeof(uint32), indices.data(),
        sizeof(indices), "VulkanFirstInstanceParityIndices");
    auto instanceBuffer = CreateVulkanUploadBuffer(
        *device, RHIBufferUsage::Vertex, sizeof(GPUInstanceData),
        instanceRows.data(), sizeof(instanceRows),
        "VulkanFirstInstanceParityGPUInstanceData");

    struct IndirectArgumentPayload
    {
        uint32 prefix = 0u;
        IndirectDrawIndexedCommand command{3u, 2u, 0u, 0, kFirstInstance};
        IndirectDrawIndexedCommand visibleSentinel{3u, 1u, 0u, 0, 4u};
        IndirectDrawIndexedCommand overflowSentinel{3u, 1u, 0u, 0, 5u};
    };
    static_assert(offsetof(IndirectArgumentPayload, command) == sizeof(uint32));
    static_assert(offsetof(IndirectArgumentPayload, visibleSentinel) ==
                  sizeof(uint32) + sizeof(IndirectDrawIndexedCommand));
    static_assert(offsetof(IndirectArgumentPayload, overflowSentinel) ==
                  sizeof(uint32) + 2u * sizeof(IndirectDrawIndexedCommand));
    const IndirectArgumentPayload argumentPayload;
    // argumentOffset zero decodes indexCount = 0 from prefix, so it cannot
    // paint. countOffset = sizeof(uint32) selects the first two real commands;
    // ignoring it reaches overflowSentinel.
    const std::array<uint32, 2> countPayload{0u, 2u};
    auto argumentBuffer = CreateVulkanUploadBuffer(
        *device, RHIBufferUsage::IndirectArgs,
        sizeof(IndirectDrawIndexedCommand), &argumentPayload,
        sizeof(argumentPayload), "VulkanFirstInstanceParityArguments");
    auto countBuffer = CreateVulkanUploadBuffer(
        *device, RHIBufferUsage::IndirectArgs, sizeof(uint32),
        countPayload.data(), sizeof(countPayload),
        "VulkanFirstInstanceParityCount");

    const auto createTarget = [device = device.get()](const char* name)
    {
        RHITextureDesc desc = RHITextureDesc::Texture2D(
            kWidth, kHeight, RHIFormat::RGBA8_UNORM,
            RHITextureUsage::RenderTarget | RHITextureUsage::CopySrc);
        desc.debugName = name;
        return device->CreateTexture(desc);
    };
    RHITextureRef directTarget = createTarget("VulkanFirstInstanceParityDirectTarget");
    RHITextureRef indirectTarget = createTarget("VulkanFirstInstanceParityIndirectTarget");
    RHITextureViewRef directTargetView =
        CreateVulkanRenderTargetView(*device, directTarget.Get());
    RHITextureViewRef indirectTargetView =
        CreateVulkanRenderTargetView(*device, indirectTarget.Get());
    RHIBufferDesc readbackDesc;
    readbackDesc.size = static_cast<uint64>(kRowPitch) * kHeight;
    readbackDesc.usage = RHIBufferUsage::CopyDst;
    readbackDesc.memoryType = RHIMemoryType::Readback;
    readbackDesc.debugName = "VulkanFirstInstanceParityReadback";
    RHIBufferRef directReadback = device->CreateBuffer(readbackDesc);
    RHIBufferRef indirectReadback = device->CreateBuffer(readbackDesc);
    ASSERT_NE(indexBuffer.Get(), nullptr);
    ASSERT_NE(instanceBuffer.Get(), nullptr);
    ASSERT_NE(argumentBuffer.Get(), nullptr);
    ASSERT_NE(countBuffer.Get(), nullptr);
    ASSERT_NE(directTarget.Get(), nullptr);
    ASSERT_NE(indirectTarget.Get(), nullptr);
    ASSERT_NE(directTargetView.Get(), nullptr);
    ASSERT_NE(indirectTargetView.Get(), nullptr);
    ASSERT_NE(directReadback.Get(), nullptr);
    ASSERT_NE(indirectReadback.Get(), nullptr);

    auto context = device->CreateCommandContext(RHICommandQueueType::Graphics);
    auto fence = device->CreateFence(0);
    ASSERT_NE(context.Get(), nullptr);
    ASSERT_NE(fence.Get(), nullptr);

    const VulkanValidationMessageCounts messagesBefore =
        vulkanDevice->GetValidationMessageCounts();
    context->Begin();
    context->BufferBarrier(
        {instanceBuffer.Get(), RHIResourceState::Common,
         RHIResourceState::VertexBuffer});
    context->BufferBarrier(
        {indexBuffer.Get(), RHIResourceState::Common, RHIResourceState::IndexBuffer});
    context->BufferBarrier(
        {argumentBuffer.Get(), RHIResourceState::Common,
         RHIResourceState::IndirectArgument});
    context->BufferBarrier(
        {countBuffer.Get(), RHIResourceState::Common,
         RHIResourceState::IndirectArgument});

    const auto beginTarget = [&context, &pipeline, &indexBuffer, &instanceBuffer](
                                 RHITexture* target,
                                 RHITextureView* targetView)
    {
        context->TextureBarrier(
            {target, RHIResourceState::Undefined, RHIResourceState::RenderTarget});
        RHIRenderPassDesc renderPass;
        renderPass.AddColorAttachment(targetView, RHILoadOp::Clear,
                                      RHIStoreOp::Store,
                                      {0.0f, 0.0f, 0.0f, 1.0f});
        context->BeginRenderPass(renderPass);
        context->SetViewport({0.0f, 0.0f, static_cast<float>(kWidth),
                              static_cast<float>(kHeight), 0.0f, 1.0f});
        context->SetScissor({0, 0, kWidth, kHeight});
        context->SetPipeline(pipeline.Get());
        context->SetVertexBuffer(6, instanceBuffer.Get());
        context->SetIndexBuffer(indexBuffer.Get(), RHIFormat::R32_UINT);
    };

    beginTarget(directTarget.Get(), directTargetView.Get());
    context->DrawIndexed(3, 2, 0, 0, kFirstInstance);
    context->DrawIndexed(3, 1, 0, 0, 4u);
    context->EndRenderPass();
    context->TextureBarrier(
        {directTarget.Get(), RHIResourceState::RenderTarget,
         RHIResourceState::CopySource});

    beginTarget(indirectTarget.Get(), indirectTargetView.Get());
    context->DrawIndexedIndirectCount(
        argumentBuffer.Get(), offsetof(IndirectArgumentPayload, command),
        countBuffer.Get(), sizeof(uint32), 3,
        sizeof(IndirectDrawIndexedCommand));
    context->EndRenderPass();
    context->TextureBarrier(
        {indirectTarget.Get(), RHIResourceState::RenderTarget,
         RHIResourceState::CopySource});

    context->BufferBarrier(
        {directReadback.Get(), RHIResourceState::Common, RHIResourceState::CopyDest});
    context->BufferBarrier(
        {indirectReadback.Get(), RHIResourceState::Common,
         RHIResourceState::CopyDest});
    RHIBufferTextureCopyDesc copyDesc;
    copyDesc.bufferRowPitch = kRowPitch;
    copyDesc.textureRegion = {0, 0, kWidth, kHeight};
    context->CopyTextureToBuffer(directTarget.Get(), directReadback.Get(), copyDesc);
    context->CopyTextureToBuffer(indirectTarget.Get(), indirectReadback.Get(), copyDesc);
    context->BufferBarrier(
        {directReadback.Get(), RHIResourceState::CopyDest, RHIResourceState::Common});
    context->BufferBarrier(
        {indirectReadback.Get(), RHIResourceState::CopyDest, RHIResourceState::Common});
    context->End();

    const uint64 submittedValue = device->SubmitCommandContext(context.Get(), fence.Get());
    ASSERT_NE(submittedValue, 0u);
    device->WaitForFence(fence.Get(), submittedValue);
    ASSERT_GE(fence->GetCompletedValue(), submittedValue);

    const auto* directPixels =
        static_cast<const uint8*>(directReadback->Map());
    const auto* indirectPixels =
        static_cast<const uint8*>(indirectReadback->Map());
    ASSERT_NE(directPixels, nullptr);
    ASSERT_NE(indirectPixels, nullptr);
    EXPECT_EQ(std::memcmp(directPixels, indirectPixels,
                          static_cast<size_t>(readbackDesc.size)),
              0);

    const auto pixelAt = [](const uint8* pixels, uint32 x, uint32 y)
    {
        return pixels + static_cast<size_t>(y) * kRowPitch +
            static_cast<size_t>(x) * 4u;
    };
    const uint8* redForeground = pixelAt(directPixels, 18u, 16u);
    const uint8* greenForeground = pixelAt(directPixels, 46u, 16u);
    const uint8* blueSecondCommand = pixelAt(directPixels, 32u, 16u);
    const uint8* clearBackground = pixelAt(directPixels, 32u, 2u);
    EXPECT_GT(redForeground[0], 200u);
    EXPECT_LT(redForeground[1], 32u);
    EXPECT_GT(greenForeground[1], 200u);
    EXPECT_LT(greenForeground[0], 32u);
    EXPECT_LT(blueSecondCommand[0], 32u);
    EXPECT_LT(blueSecondCommand[1], 32u);
    EXPECT_GT(blueSecondCommand[2], 200u);
    EXPECT_EQ(blueSecondCommand[3], 255u);
    EXPECT_EQ(clearBackground[0], 0u);
    EXPECT_EQ(clearBackground[1], 0u);
    EXPECT_EQ(clearBackground[2], 0u);
    EXPECT_EQ(clearBackground[3], 255u);
    directReadback->Unmap();
    indirectReadback->Unmap();
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

TEST(VulkanValidation, RHIBufferUploadUsesObservableCommitAndPreflightsBounds)
{
    LegacyMappedBuffer legacyBuffer;
    const std::array<uint32, 2> source = {0x10203040U, 0x50607080U};

    EXPECT_TRUE(legacyBuffer.Upload(source.data(), source.size()));
    EXPECT_EQ(legacyBuffer.m_mapCount, 1u);
    EXPECT_EQ(legacyBuffer.m_unmapCount, 1u);
    EXPECT_EQ(0, std::memcmp(legacyBuffer.m_storage.data(), source.data(),
                             sizeof(source)));

    EXPECT_FALSE(legacyBuffer.Upload(source.data(), source.size(), 12));
    EXPECT_FALSE(legacyBuffer.Upload(source.data(),
                                     std::numeric_limits<uint64>::max()));
    EXPECT_FALSE(legacyBuffer.Upload<uint32>(nullptr, 1));
    EXPECT_TRUE(legacyBuffer.Upload<uint32>(nullptr, 0));
    EXPECT_EQ(legacyBuffer.m_mapCount, 1u);
    EXPECT_EQ(legacyBuffer.m_unmapCount, 1u);

    FailingCommitBuffer failingBuffer;
    EXPECT_FALSE(failingBuffer.Upload(source.data(), source.size()));
    EXPECT_EQ(failingBuffer.m_mapCount, 1u);
    EXPECT_EQ(failingBuffer.m_commitCount, 1u);
    EXPECT_EQ(failingBuffer.m_unmapCount, 1u);
    EXPECT_TRUE(failingBuffer.Upload(source.data(), source.size()));
    EXPECT_EQ(failingBuffer.m_mapCount, 2u);
    EXPECT_EQ(failingBuffer.m_commitCount, 2u);
    EXPECT_EQ(failingBuffer.m_unmapCount, 2u);
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

    float testData[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    EXPECT_TRUE(buffer->Upload(testData, sizeof(testData) / sizeof(testData[0])));

    void* mappedData = buffer->Map();
    ASSERT_NE(nullptr, mappedData);
    std::memcpy(mappedData, testData, sizeof(testData));
    EXPECT_TRUE(buffer->CommitMappedWrite());
    EXPECT_TRUE(buffer->CommitMappedWrite()); // A completed commit is idempotent.
}

TEST(VulkanValidation, RangeMappedWriteReportsActualHostVisibility)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);

    RHIBufferDesc bufferDesc;
    bufferDesc.size = 64;
    bufferDesc.usage = RHIBufferUsage::CopySrc;
    bufferDesc.memoryType = RHIMemoryType::Upload;
    bufferDesc.debugName = "VulkanRangeMappedWrite";
    RHIBufferRef buffer = device->CreateBuffer(bufferDesc);
    ASSERT_NE(buffer.Get(), nullptr);

    void* legacyAccess = buffer->Map();
    ASSERT_NE(legacyAccess, nullptr);
    EXPECT_FALSE(buffer->MapWriteRange(4, 4).IsValid());
    EXPECT_TRUE(buffer->CommitMappedWrite());

    EXPECT_FALSE(buffer->MapWriteRange(0, 0).IsValid());
    EXPECT_FALSE(buffer->MapWriteRange(60, 8).IsValid());
    auto access = buffer->MapWriteRange(12, 8);
    ASSERT_TRUE(access.IsValid());
    std::memset(access.GetData(), 0xA5, static_cast<size_t>(access.GetSize()));
    EXPECT_FALSE(buffer->MapWriteRange(24, 4).IsValid());
    EXPECT_EQ(buffer->Map(), nullptr);
    buffer->Unmap();
    EXPECT_FALSE(buffer->CommitMappedWrite());

    const RHIHostWriteReceipt receipt =
        buffer->CommitMappedWriteRange(std::move(access));
    EXPECT_TRUE(receipt.IsPublished());
    EXPECT_EQ(receipt.cpuWriteOffset, 12U);
    EXPECT_EQ(receipt.cpuWriteSize, 8U);
    if (receipt.synchronization ==
        RHIHostWriteSynchronization::CoherentNoExplicitSync)
    {
        EXPECT_FALSE(receipt.HasSynchronizedRange());
        EXPECT_EQ(receipt.synchronizedSize, 0U);
    }
    else
    {
        EXPECT_TRUE(receipt.synchronization ==
                        RHIHostWriteSynchronization::AtomAlignedRange ||
                    receipt.synchronization ==
                        RHIHostWriteSynchronization::WholeAllocation);
        EXPECT_TRUE(receipt.HasSynchronizedRange());
    }
    EXPECT_FALSE(buffer->CommitMappedWriteRange(std::move(access)).IsPublished());

    auto cancelledAccess = buffer->MapWriteRange(24, 4);
    ASSERT_TRUE(cancelledAccess.IsValid());
    EXPECT_TRUE(buffer->CancelMappedWriteRange(std::move(cancelledAccess)));
    auto retryAccess = buffer->MapWriteRange(24, 4);
    ASSERT_TRUE(retryAccess.IsValid());
    EXPECT_TRUE(buffer->CommitMappedWriteRange(std::move(retryAccess)).IsPublished());

    RHIStagingBufferDesc stagingDesc;
    stagingDesc.size = 64;
    stagingDesc.debugName = "VulkanRangeMappedStaging";
    RHIStagingBufferRef staging = device->CreateStagingBuffer(stagingDesc);
    ASSERT_NE(staging.Get(), nullptr);
    void* legacyStagingAccess = staging->Map(0, 4);
    ASSERT_NE(legacyStagingAccess, nullptr);
    EXPECT_FALSE(staging->MapWriteRange(4, 4).IsValid());
    EXPECT_TRUE(staging->CommitMappedWrite());
    auto stagingAccess = staging->MapWriteRange(16, 12);
    ASSERT_TRUE(stagingAccess.IsValid());
    std::memset(stagingAccess.GetData(), 0x5A,
                static_cast<size_t>(stagingAccess.GetSize()));
    EXPECT_EQ(staging->Map(0, 4), nullptr);
    staging->Unmap();
    EXPECT_FALSE(staging->CommitMappedWrite());
    const RHIHostWriteReceipt stagingReceipt =
        staging->CommitMappedWriteRange(std::move(stagingAccess));
    EXPECT_TRUE(stagingReceipt.IsPublished());
    EXPECT_EQ(stagingReceipt.synchronization,
              RHIHostWriteSynchronization::CoherentNoExplicitSync);
    EXPECT_FALSE(stagingReceipt.HasSynchronizedRange());
    EXPECT_EQ(stagingReceipt.synchronizedSize, 0U);
}

TEST(VulkanValidation, StagingCommitWriteRejectsInvalidAccessAndEndsMappedAccess)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);

    RHIStagingBufferDesc desc;
    desc.size = 64;
    desc.debugName = "VulkanStagingCommitContract";
    RHIStagingBufferRef staging = device->CreateStagingBuffer(desc);
    ASSERT_NE(staging.Get(), nullptr);

    EXPECT_FALSE(staging->CommitMappedWrite());
    void* mapped = staging->Map(0, 16);
    ASSERT_NE(mapped, nullptr);
    std::memset(mapped, 0xAB, 16);
    EXPECT_TRUE(staging->CommitMappedWrite());
    EXPECT_FALSE(staging->CommitMappedWrite());
}

TEST(VulkanValidation, StagingMapUsesAllocationBaseForUnalignedSubranges)
{
    RHIDeviceDesc deviceDesc;
    deviceDesc.enableDebugLayer = true;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);

    auto* vulkanDevice = static_cast<VulkanDevice*>(device.get());
    const VulkanValidationMessageCounts validationBefore =
        vulkanDevice->GetValidationMessageCounts();

    const std::array<uint8, 4> sourceData = {0x12U, 0x34U, 0x56U, 0x78U};
    RHIStagingBufferDesc stagingDesc;
    stagingDesc.size = 64;
    stagingDesc.debugName = "VulkanUnalignedStagingMap";
    RHIStagingBufferRef staging = device->CreateStagingBuffer(stagingDesc);
    ASSERT_NE(staging.Get(), nullptr);

    void* mappedData = staging->Map(1, sourceData.size());
    ASSERT_NE(mappedData, nullptr);
    std::memcpy(mappedData, sourceData.data(), sourceData.size());
    ASSERT_TRUE(staging->CommitMappedWrite());

    RHIBufferDesc readbackDesc;
    readbackDesc.size = stagingDesc.size;
    readbackDesc.usage = RHIBufferUsage::CopyDst;
    readbackDesc.memoryType = RHIMemoryType::Readback;
    readbackDesc.debugName = "VulkanUnalignedStagingReadback";
    RHIBufferRef readback = device->CreateBuffer(readbackDesc);
    ASSERT_NE(readback.Get(), nullptr);

    RHIBuffer* stagingBuffer = staging->GetBuffer();
    ASSERT_NE(stagingBuffer, nullptr);
    RHICommandContextRef context =
        device->CreateCommandContext(RHICommandQueueType::Copy);
    ASSERT_NE(context.Get(), nullptr);
    context->Begin();
    context->BufferBarrier(stagingBuffer,
                           RHIResourceState::Common,
                           RHIResourceState::CopySource);
    context->BufferBarrier(readback.Get(),
                           RHIResourceState::Common,
                           RHIResourceState::CopyDest);
    context->CopyBuffer(stagingBuffer,
                        readback.Get(),
                        1,
                        1,
                        sourceData.size());
    context->BufferBarrier(readback.Get(),
                           RHIResourceState::CopyDest,
                           RHIResourceState::Common);
    context->End();

    RHIFenceRef completionFence = device->CreateFence(0);
    ASSERT_NE(completionFence.Get(), nullptr);
    const uint64 completionValue =
        device->SubmitCommandContext(context.Get(), completionFence.Get());
    ASSERT_NE(completionValue, 0U);
    device->WaitForFence(completionFence.Get(), completionValue);

    const auto* readbackData = static_cast<const uint8*>(readback->Map());
    ASSERT_NE(readbackData, nullptr);
    EXPECT_EQ(0, std::memcmp(readbackData + 1,
                             sourceData.data(),
                             sourceData.size()));
    readback->Unmap();

    device->WaitIdle();
    const VulkanValidationMessageCounts validationAfter =
        vulkanDevice->GetValidationMessageCounts();
    EXPECT_EQ(validationAfter.errors, validationBefore.errors);
    EXPECT_EQ(validationAfter.warnings, validationBefore.warnings);
}

TEST(VulkanValidation, HostMemorySynchronizationContract)
{
    EXPECT_EQ(GetVulkanHostMemorySynchronization(RHIMemoryType::Default),
              VulkanHostMemorySynchronization::None);
    EXPECT_EQ(GetVulkanHostMemorySynchronization(RHIMemoryType::Upload),
              VulkanHostMemorySynchronization::Flush);
    EXPECT_EQ(GetVulkanHostMemorySynchronization(RHIMemoryType::Readback),
              VulkanHostMemorySynchronization::Invalidate);

    const VulkanMappedMemoryRange alignedRange = MakeVulkanMappedMemoryRange(
        130, 10, 1024, 64, 64);
    EXPECT_TRUE(alignedRange.valid);
    EXPECT_EQ(alignedRange.offset, 128u);
    EXPECT_EQ(alignedRange.size, 64u);
    EXPECT_EQ(alignedRange.resourceOffset, 2u);

    const VulkanMappedMemoryRange tailRange = MakeVulkanMappedMemoryRange(
        960, 40, 1000, 64, 64);
    EXPECT_TRUE(tailRange.valid);
    EXPECT_EQ(tailRange.offset, 960u);
    EXPECT_EQ(tailRange.size, VK_WHOLE_SIZE);
    EXPECT_EQ(tailRange.resourceOffset, 0u);

    const VulkanMappedMemoryRange unalignedTailRange =
        MakeVulkanMappedMemoryRange(130, 870, 1000, 64, 64);
    EXPECT_TRUE(unalignedTailRange.valid);
    EXPECT_EQ(unalignedTailRange.offset, 128u);
    EXPECT_EQ(unalignedTailRange.size, VK_WHOLE_SIZE);
    EXPECT_EQ(unalignedTailRange.resourceOffset, 2u);

    const VulkanMappedMemoryRange mapAlignedRange = MakeVulkanMappedMemoryRange(
        130, 10, 1024, 64, 256);
    EXPECT_TRUE(mapAlignedRange.valid);
    EXPECT_EQ(mapAlignedRange.offset, 0u);
    EXPECT_EQ(mapAlignedRange.size, 256u);
    EXPECT_EQ(mapAlignedRange.resourceOffset, 130u);

    EXPECT_FALSE(MakeVulkanMappedMemoryRange(1000, 1, 1000, 64, 64).valid);
    EXPECT_FALSE(MakeVulkanMappedMemoryRange(960, 41, 1000, 64, 64).valid);

    const VulkanMappedMemoryRange atomAlignedRange =
        MakeVulkanHostWriteSynchronizationRange(128, 5, 10, 1024, 64);
    EXPECT_TRUE(atomAlignedRange.valid);
    EXPECT_EQ(atomAlignedRange.offset, 128U);
    EXPECT_EQ(atomAlignedRange.size, 64U);
    EXPECT_EQ(atomAlignedRange.resourceOffset, 5U);

    const VulkanMappedMemoryRange tailSynchronizationRange =
        MakeVulkanHostWriteSynchronizationRange(960, 0, 40, 1000, 64);
    EXPECT_TRUE(tailSynchronizationRange.valid);
    EXPECT_EQ(tailSynchronizationRange.offset, 960U);
    EXPECT_EQ(tailSynchronizationRange.size, VK_WHOLE_SIZE);
    EXPECT_EQ(GetVulkanMappedMemoryRangeCoveredSize(tailSynchronizationRange, 1000),
              40U);
    EXPECT_FALSE(DoesVulkanMappedMemoryRangeCoverWholeAllocation(
        tailSynchronizationRange, 1000));

    const VulkanMappedMemoryRange fullAllocationRange =
        MakeVulkanHostWriteSynchronizationRange(0, 0, 1000, 1000, 64);
    EXPECT_TRUE(fullAllocationRange.valid);
    EXPECT_EQ(fullAllocationRange.offset, 0U);
    EXPECT_EQ(fullAllocationRange.size, VK_WHOLE_SIZE);
    EXPECT_EQ(GetVulkanMappedMemoryRangeCoveredSize(fullAllocationRange, 1000),
              1000U);
    EXPECT_TRUE(DoesVulkanMappedMemoryRangeCoverWholeAllocation(
        fullAllocationRange, 1000));
    EXPECT_FALSE(MakeVulkanHostWriteSynchronizationRange(1000, 0, 1, 1000, 64).valid);
}

TEST(VulkanValidation, HostVisibleBufferRoundTrip)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);

    auto* vulkanDevice = static_cast<VulkanDevice*>(device.get());
    const VulkanValidationMessageCounts validationBefore =
        vulkanDevice->GetValidationMessageCounts();

    const std::array<uint32, 4> sourceData = {0x1U, 0x2345U, 0x6789U, 0xABCDEFU};
    RHIBufferDesc uploadDesc;
    uploadDesc.size = sizeof(sourceData);
    uploadDesc.usage = RHIBufferUsage::CopySrc;
    uploadDesc.memoryType = RHIMemoryType::Upload;
    uploadDesc.stride = sizeof(uint32);
    uploadDesc.debugName = "HostVisibleUpload";
    RHIBufferRef upload = device->CreateBuffer(uploadDesc);
    ASSERT_NE(upload.Get(), nullptr);

    void* uploadData = upload->Map();
    ASSERT_NE(uploadData, nullptr);
    std::memcpy(uploadData, sourceData.data(), sizeof(sourceData));
    upload->Unmap();
    upload->Unmap(); // A completed persistent Map()/Unmap() pair is idempotent.

    RHIBufferDesc readbackDesc;
    readbackDesc.size = sizeof(sourceData);
    readbackDesc.usage = RHIBufferUsage::CopyDst;
    readbackDesc.memoryType = RHIMemoryType::Readback;
    readbackDesc.stride = sizeof(uint32);
    readbackDesc.debugName = "HostVisibleReadback";
    RHIBufferRef readback = device->CreateBuffer(readbackDesc);
    ASSERT_NE(readback.Get(), nullptr);

    RHICommandContextRef context =
        device->CreateCommandContext(RHICommandQueueType::Copy);
    ASSERT_NE(context.Get(), nullptr);
    context->Begin();
    context->BufferBarrier(upload.Get(),
                           RHIResourceState::Common,
                           RHIResourceState::CopySource);
    context->BufferBarrier(readback.Get(),
                           RHIResourceState::Common,
                           RHIResourceState::CopyDest);
    context->CopyBuffer(upload.Get(), readback.Get(), 0, 0, sizeof(sourceData));
    context->BufferBarrier(readback.Get(),
                           RHIResourceState::CopyDest,
                           RHIResourceState::Common);
    context->End();

    RHIFenceRef completionFence = device->CreateFence(0);
    ASSERT_NE(completionFence.Get(), nullptr);
    const uint64 completionValue =
        device->SubmitCommandContext(context.Get(), completionFence.Get());
    ASSERT_NE(completionValue, 0u);
    device->WaitForFence(completionFence.Get(), completionValue);

    const void* readbackData = readback->Map();
    ASSERT_NE(readbackData, nullptr);
    EXPECT_EQ(0, std::memcmp(readbackData, sourceData.data(), sizeof(sourceData)));
    readback->Unmap();
    readback->Unmap(); // A completed on-demand mapping must not be unmapped twice.

    device->WaitIdle();
    const VulkanValidationMessageCounts validationAfter =
        vulkanDevice->GetValidationMessageCounts();
    EXPECT_EQ(validationAfter.errors, validationBefore.errors);
    EXPECT_EQ(validationAfter.warnings, validationBefore.warnings);
}

TEST(VulkanValidation, PlacedHostVisibleBuffersShareTheirHeapMapping)
{
    RHIDeviceDesc deviceDesc;
    deviceDesc.enableDebugLayer = true;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);

    auto* vulkanDevice = static_cast<VulkanDevice*>(device.get());
    const VulkanValidationMessageCounts validationBefore =
        vulkanDevice->GetValidationMessageCounts();

    RHIBufferDesc bufferDesc;
    bufferDesc.size = 1024;
    bufferDesc.usage = RHIBufferUsage::CopySrc | RHIBufferUsage::CopyDst;
    bufferDesc.memoryType = RHIMemoryType::Upload;
    bufferDesc.stride = sizeof(uint32);
    const IRHIDevice::MemoryRequirements requirements =
        device->GetBufferMemoryRequirements(bufferDesc);
    ASSERT_NE(requirements.size, 0u);
    ASSERT_NE(requirements.alignment, 0u);

    const uint64 firstOffset = 0;
    const uint64 secondOffset =
        ((requirements.size + requirements.alignment - 1) / requirements.alignment) *
        requirements.alignment;
    ASSERT_GT(secondOffset, firstOffset);

    RHIHeapDesc uploadHeapDesc;
    uploadHeapDesc.size = secondOffset + requirements.size;
    uploadHeapDesc.type = RHIHeapType::Upload;
    uploadHeapDesc.flags = RHIHeapFlags::AllowBuffers;
    uploadHeapDesc.debugName = "PlacedUploadHeap";
    RHIHeapRef uploadHeap = device->CreateHeap(uploadHeapDesc);
    ASSERT_NE(uploadHeap.Get(), nullptr);

    RHIBufferRef firstUpload =
        device->CreatePlacedBuffer(uploadHeap.Get(), firstOffset, bufferDesc);
    RHIBufferRef secondUpload =
        device->CreatePlacedBuffer(uploadHeap.Get(), secondOffset, bufferDesc);
    ASSERT_NE(firstUpload.Get(), nullptr);
    ASSERT_NE(secondUpload.Get(), nullptr);

    auto firstUploadAccess = firstUpload->MapWriteRange(16, sizeof(uint32));
    auto secondUploadAccess = secondUpload->MapWriteRange(16, sizeof(uint32));
    ASSERT_TRUE(firstUploadAccess.IsValid());
    ASSERT_TRUE(secondUploadAccess.IsValid());
    EXPECT_NE(firstUploadAccess.GetData(), secondUploadAccess.GetData());
    EXPECT_EQ(static_cast<uint8*>(secondUploadAccess.GetData()) -
                  static_cast<uint8*>(firstUploadAccess.GetData()),
              static_cast<ptrdiff_t>(secondOffset - firstOffset));

    *static_cast<uint32*>(firstUploadAccess.GetData()) = 0x12345678U;
    *static_cast<uint32*>(secondUploadAccess.GetData()) = 0x87654321U;
    const RHIHostWriteReceipt firstUploadReceipt =
        firstUpload->CommitMappedWriteRange(std::move(firstUploadAccess));
    const RHIHostWriteReceipt secondUploadReceipt =
        secondUpload->CommitMappedWriteRange(std::move(secondUploadAccess));
    EXPECT_EQ(firstUploadReceipt.synchronization,
              RHIHostWriteSynchronization::CoherentNoExplicitSync);
    EXPECT_EQ(secondUploadReceipt.synchronization,
              RHIHostWriteSynchronization::CoherentNoExplicitSync);
    EXPECT_FALSE(firstUploadReceipt.HasSynchronizedRange());
    EXPECT_FALSE(secondUploadReceipt.HasSynchronizedRange());

    RHIHeapDesc readbackHeapDesc = uploadHeapDesc;
    readbackHeapDesc.type = RHIHeapType::Readback;
    readbackHeapDesc.debugName = "PlacedReadbackHeap";
    RHIHeapRef readbackHeap = device->CreateHeap(readbackHeapDesc);
    ASSERT_NE(readbackHeap.Get(), nullptr);

    bufferDesc.memoryType = RHIMemoryType::Readback;
    RHIBufferRef firstReadback =
        device->CreatePlacedBuffer(readbackHeap.Get(), firstOffset, bufferDesc);
    RHIBufferRef secondReadback =
        device->CreatePlacedBuffer(readbackHeap.Get(), secondOffset, bufferDesc);
    ASSERT_NE(firstReadback.Get(), nullptr);
    ASSERT_NE(secondReadback.Get(), nullptr);

    const void* firstReadbackData = firstReadback->Map();
    const void* secondReadbackData = secondReadback->Map();
    ASSERT_NE(firstReadbackData, nullptr);
    ASSERT_NE(secondReadbackData, nullptr);
    EXPECT_NE(firstReadbackData, secondReadbackData);
    EXPECT_EQ(reinterpret_cast<const uint8*>(secondReadbackData) -
                  reinterpret_cast<const uint8*>(firstReadbackData),
              static_cast<ptrdiff_t>(secondOffset - firstOffset));
    firstReadback->Unmap();
    secondReadback->Unmap();

    device->WaitIdle();
    const VulkanValidationMessageCounts validationAfter =
        vulkanDevice->GetValidationMessageCounts();
    EXPECT_EQ(validationAfter.errors, validationBefore.errors);
    EXPECT_EQ(validationAfter.warnings, validationBefore.warnings);
}

TEST(VulkanValidation, PlacedBufferRejectsMismatchedHeapAndMemoryTypes)
{
    RHIDeviceDesc deviceDesc;
    deviceDesc.enableDebugLayer = true;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);

    auto* vulkanDevice = static_cast<VulkanDevice*>(device.get());
    const VulkanValidationMessageCounts validationBefore =
        vulkanDevice->GetValidationMessageCounts();

    RHIBufferDesc bufferDesc;
    bufferDesc.size = 1024;
    bufferDesc.usage = RHIBufferUsage::CopySrc | RHIBufferUsage::CopyDst;
    bufferDesc.stride = sizeof(uint32);
    const IRHIDevice::MemoryRequirements requirements =
        device->GetBufferMemoryRequirements(bufferDesc);
    ASSERT_NE(requirements.size, 0u);

    RHIHeapDesc uploadHeapDesc;
    uploadHeapDesc.size = requirements.size;
    uploadHeapDesc.type = RHIHeapType::Upload;
    uploadHeapDesc.flags = RHIHeapFlags::AllowBuffers;
    RHIHeapRef uploadHeap = device->CreateHeap(uploadHeapDesc);
    ASSERT_NE(uploadHeap.Get(), nullptr);

    RHIHeapDesc readbackHeapDesc = uploadHeapDesc;
    readbackHeapDesc.type = RHIHeapType::Readback;
    RHIHeapRef readbackHeap = device->CreateHeap(readbackHeapDesc);
    ASSERT_NE(readbackHeap.Get(), nullptr);

    bufferDesc.memoryType = RHIMemoryType::Default;
    RHIBufferRef defaultOnUpload =
        device->CreatePlacedBuffer(uploadHeap.Get(), 0, bufferDesc);
    EXPECT_EQ(defaultOnUpload.Get(), nullptr);

    bufferDesc.memoryType = RHIMemoryType::Upload;
    RHIBufferRef uploadOnReadback =
        device->CreatePlacedBuffer(readbackHeap.Get(), 0, bufferDesc);
    EXPECT_EQ(uploadOnReadback.Get(), nullptr);

    bufferDesc.memoryType = RHIMemoryType::Readback;
    RHIBufferRef readbackOnUpload =
        device->CreatePlacedBuffer(uploadHeap.Get(), 0, bufferDesc);
    EXPECT_EQ(readbackOnUpload.Get(), nullptr);

    const VulkanValidationMessageCounts validationAfter =
        vulkanDevice->GetValidationMessageCounts();
    EXPECT_EQ(validationAfter.errors, validationBefore.errors);
    EXPECT_EQ(validationAfter.warnings, validationBefore.warnings);
}

TEST(VulkanValidation, PlacedBufferRejectsInvalidBindOffsets)
{
    RHIDeviceDesc deviceDesc;
    deviceDesc.enableDebugLayer = true;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);

    auto* vulkanDevice = static_cast<VulkanDevice*>(device.get());
    const VulkanValidationMessageCounts validationBefore =
        vulkanDevice->GetValidationMessageCounts();

    RHIBufferDesc bufferDesc;
    bufferDesc.size = 1024;
    bufferDesc.usage = RHIBufferUsage::CopySrc;
    bufferDesc.memoryType = RHIMemoryType::Upload;
    bufferDesc.stride = sizeof(uint32);
    const IRHIDevice::MemoryRequirements requirements =
        device->GetBufferMemoryRequirements(bufferDesc);
    ASSERT_NE(requirements.size, 0u);

    RHIHeapDesc heapDesc;
    heapDesc.size = requirements.size;
    heapDesc.type = RHIHeapType::Upload;
    heapDesc.flags = RHIHeapFlags::AllowBuffers;
    RHIHeapRef heap = device->CreateHeap(heapDesc);
    ASSERT_NE(heap.Get(), nullptr);

    RHIBufferRef unalignedOffset =
        device->CreatePlacedBuffer(heap.Get(), 1, bufferDesc);
    EXPECT_EQ(unalignedOffset.Get(), nullptr);

    RHIBufferRef outOfRangeOffset =
        device->CreatePlacedBuffer(heap.Get(), requirements.size, bufferDesc);
    EXPECT_EQ(outOfRangeOffset.Get(), nullptr);

    const VulkanValidationMessageCounts validationAfter =
        vulkanDevice->GetValidationMessageCounts();
    EXPECT_EQ(validationAfter.errors, validationBefore.errors);
    EXPECT_EQ(validationAfter.warnings, validationBefore.warnings);
}

TEST(VulkanValidation, HostVisibleHeapSynchronizationRejectsInvalidRange)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::Vulkan);

    RHIHeapDesc heapDesc;
    heapDesc.size = 1024;
    heapDesc.type = RHIHeapType::Upload;
    heapDesc.flags = RHIHeapFlags::AllowBuffers;
    RHIHeapRef heap = device->CreateHeap(heapDesc);
    ASSERT_NE(heap.Get(), nullptr);

    auto* vulkanHeap = static_cast<VulkanHeap*>(heap.Get());
    ASSERT_NE(vulkanHeap->GetMappedData(), nullptr);

    VulkanMappedMemoryRange invalidRange;
    invalidRange.offset = heapDesc.size;
    invalidRange.size = 1;
    invalidRange.valid = true;
    EXPECT_FALSE(vulkanHeap->SynchronizeMappedRange(
        invalidRange,
        VulkanHostMemorySynchronization::Flush));
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
    queryDesc.queueType = RHICommandQueueType::Graphics;
    queryDesc.count = 2;
    queryDesc.debugName = "UnsupportedTimestampQueries";
    EXPECT_TRUE(ValidateRHIQueryPoolDesc(queryDesc));
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
    RenderGraphValidationAccess::SetDevice(graph, device.get());
    ASSERT_TRUE(RenderGraphValidationAccess::SetQueueExecutionMode(graph,
        RenderGraph::QueueExecutionMode::MultiQueue));
    const RGBufferHandle resource = RenderGraphValidationAccess::ImportBuffer(graph,
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
    RenderGraphValidationAccess::Compile(graph);
    ASSERT_TRUE(graph.GetCompileStats().compileValid);

    RenderGraph::RecordedQueueSubmission recorded;
    ASSERT_TRUE(RenderGraphValidationAccess::RecordQueueSubmission(
        graph,
        recorded));
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
    EXPECT_TRUE(caps.supportsBufferRangeBarriers);
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
