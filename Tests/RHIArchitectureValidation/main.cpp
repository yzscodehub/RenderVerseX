#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace RVX::Tests
{
    namespace
    {
        std::string ReadSource(const std::filesystem::path& relativePath)
        {
            const std::filesystem::path resolvedPath =
                std::filesystem::path(RVX_SOURCE_DIR) / relativePath;
            std::ifstream stream(resolvedPath,
                                 std::ios::binary);
            EXPECT_TRUE(stream.is_open())
                << "Failed to read source file: " << resolvedPath.string();
            std::string source{std::istreambuf_iterator<char>(stream),
                               std::istreambuf_iterator<char>()};
            source.erase(std::remove(source.begin(), source.end(), '\r'),
                         source.end());
            return source;
        }
    } // namespace

    TEST(RHIContractValidation, PrimaryBackendsConsumeTheSharedDescriptorSnapshotContract)
    {
        const std::string descriptorHeader = ReadSource("Source/Runtime/RHI/Include/RHI/RHIDescriptor.h");
        const std::string dx12Pipeline = ReadSource("Source/Runtime/RHI_DX12/Private/DX12Pipeline.cpp");
        const std::string vulkanPipeline = ReadSource("Source/Runtime/RHI_Vulkan/Private/VulkanPipeline.cpp");
        const std::string metalResources = ReadSource("Source/Runtime/RHI_Metal/Private/MetalResources.mm");
        const std::string metalCommands = ReadSource("Source/Runtime/RHI_Metal/Private/MetalCommandContext.mm");
        const std::string metalBindingABI = ReadSource(
            "Source/Runtime/ShaderContracts/Include/ShaderContracts/MetalBindingABI.h");

        EXPECT_NE(descriptorHeader.find("RHIResourceDataVolatility"), std::string::npos);
        EXPECT_NE(descriptorHeader.find("IncompleteSnapshot"), std::string::npos);
        EXPECT_NE(dx12Pipeline.find("D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE"), std::string::npos);
        EXPECT_NE(dx12Pipeline.find("ToDX12DescriptorRangeDataFlags"), std::string::npos);
        EXPECT_NE(vulkanPipeline.find("GetDescriptorSnapshot()"), std::string::npos);
        EXPECT_NE(vulkanPipeline.find("InitializeNativeSnapshot()"), std::string::npos);
        EXPECT_NE(metalResources.find("snapshotBinding.arrayElement"), std::string::npos);
        EXPECT_NE(metalCommands.find("ShaderContracts/MetalBindingABI.h"), std::string::npos);
        EXPECT_NE(metalCommands.find("MetalBindingABI::TryGetBufferIndex("), std::string::npos);
        EXPECT_NE(metalCommands.find("MetalBindingABI::TryGetTextureIndex("), std::string::npos);
        EXPECT_NE(metalCommands.find("MetalBindingABI::TryGetSamplerIndex("), std::string::npos);
        EXPECT_NE(metalCommands.find("outside the direct Metal binding ABI"), std::string::npos);
        EXPECT_EQ(metalCommands.find("binding.binding + binding.arrayElement"), std::string::npos);
        EXPECT_NE(metalBindingABI.find("RVX_METAL_FIRST_MATERIAL_BUFFER_INDEX"),
                  std::string::npos);
        EXPECT_NE(metalBindingABI.find("set == 2 && binding == 27 && arrayElement == 0"),
                  std::string::npos);
        EXPECT_NE(metalBindingABI.find("binding <= 23"),
                  std::string::npos);
    }

    TEST(RHIContractValidation, PrimaryBackendsTranslateScopedDependenciesStructurally)
    {
        const std::string accessHeader = ReadSource("Source/Runtime/RHI/Include/RHI/RHIAccess.h");
        const std::string dx12 = ReadSource("Source/Runtime/RHI_DX12/Private/DX12CommandContext.cpp");
        const std::string vulkanCommon = ReadSource("Source/Runtime/RHI_Vulkan/Private/VulkanCommon.h");
        const std::string vulkan = ReadSource("Source/Runtime/RHI_Vulkan/Private/VulkanCommandContext.cpp");
        const std::string metal = ReadSource("Source/Runtime/RHI_Metal/Private/MetalCommandContext.mm");

        EXPECT_NE(accessHeader.find("struct RHIAccessSnapshot"), std::string::npos);
        EXPECT_NE(accessHeader.find("RHIContentValidity"), std::string::npos);
        EXPECT_NE(accessHeader.find("RHIDiscardIntent"), std::string::npos);
        EXPECT_NE(dx12.find("D3D12_RESOURCE_BARRIER_TYPE_UAV"), std::string::npos);
        EXPECT_NE(dx12.find("RHIDependencyKind::Memory"), std::string::npos);
        EXPECT_NE(dx12.find("nativeBarrier.Offset = barrier.offset"),
                  std::string::npos);
        EXPECT_NE(dx12.find("nativeBarrier.Size = requestedSize"),
                  std::string::npos);
        EXPECT_NE(vulkanCommon.find("struct VulkanPipelineStageSupport"),
                  std::string::npos);
        EXPECT_NE(vulkanCommon.find("ToVkPipelineStageFlags2("),
                  std::string::npos);
        EXPECT_NE(vulkanCommon.find("ToVkAccessFlags2(RHIMemoryAccess"), std::string::npos);
        EXPECT_NE(vulkan.find("GetQueueFamilyIndex(device, before.domain)"), std::string::npos);
        EXPECT_NE(vulkan.find("GetQueueFamilyIndex(device, after.domain)"), std::string::npos);
        EXPECT_NE(vulkan.find("QueueFamilyTransferRole::Release"), std::string::npos);
        EXPECT_NE(vulkan.find("QueueFamilyTransferRole::Acquire"), std::string::npos);
        EXPECT_NE(vulkan.find("VK_PIPELINE_STAGE_2_NONE"), std::string::npos);
        EXPECT_NE(vulkan.find("VK_QUEUE_FAMILY_IGNORED"), std::string::npos);
        EXPECT_NE(metal.find("barrier.accessBefore.executionScope"), std::string::npos);
        EXPECT_NE(metal.find("RequiresMetalBarrier"), std::string::npos);
    }

    TEST(RHIContractValidation, LifetimeOwnersCommitScopedSnapshots)
    {
        const std::string poolHeader = ReadSource(
            "Source/Runtime/Render/Include/Render/Graph/TransientResourcePool.h");
        const std::string poolSource = ReadSource(
            "Source/Runtime/Render/Private/Graph/TransientResourcePool.cpp");
        const std::string sceneRenderer = ReadSource(
            "Source/Runtime/Render/Private/Renderer/SceneRenderer.cpp");
        const std::string uploadTypes = ReadSource(
            "Source/Runtime/Render/Include/Render/GPUUploadTypes.h");
        const std::string uploadProcessor = ReadSource(
            "Source/Runtime/Render/Private/Resources/RenderUploadProcessor.cpp");
        const std::string resourceRegistry = ReadSource(
            "Source/Runtime/Render/Private/Resources/RenderResourceRegistry.h");

        EXPECT_NE(poolHeader.find("TransientTextureLease"), std::string::npos);
        EXPECT_NE(poolHeader.find("RHITextureAccessSnapshot accessSnapshot"), std::string::npos);
        EXPECT_NE(poolSource.find("pooled.accessSnapshot = finalAccess"), std::string::npos);
        const size_t addCullingPassPos = sceneRenderer.find("void SceneRenderer::AddGPUDrivenCullingPass(");
        const size_t commitSnapshotsPos =
            sceneRenderer.find("void SceneRenderer::CommitGPUDrivenAccessSnapshots()");
        const size_t buildRenderGraphPos =
            sceneRenderer.find("void SceneRenderer::BuildRenderGraph()");
        ASSERT_NE(addCullingPassPos, std::string::npos);
        ASSERT_NE(commitSnapshotsPos, std::string::npos);
        ASSERT_NE(buildRenderGraphPos, std::string::npos);
        ASSERT_LT(addCullingPassPos, commitSnapshotsPos);
        ASSERT_LT(commitSnapshotsPos, buildRenderGraphPos);
        EXPECT_NE(sceneRenderer.find("recordedState->GetAccessSnapshots()"), std::string::npos);
        EXPECT_NE(sceneRenderer.find("recordedState->CommitAccessSnapshots("), std::string::npos);
        EXPECT_NE(sceneRenderer.find("m_depthGPUCullingGraphHandles"), std::string::npos);
        EXPECT_NE(sceneRenderer.find("m_opaqueGPUCullingGraphHandles"), std::string::npos);
        EXPECT_NE(sceneRenderer.find("CommitGPUDrivenAccessSnapshots()"), std::string::npos);

        const std::string addPassBlock =
            sceneRenderer.substr(addCullingPassPos,
                                 commitSnapshotsPos - addCullingPassPos);
        const std::string commitBlock =
            sceneRenderer.substr(commitSnapshotsPos,
                                 buildRenderGraphPos - commitSnapshotsPos);
        EXPECT_EQ(addPassBlock.find("owner->GetAccessSnapshots()"),
                  std::string::npos);
        EXPECT_EQ(addPassBlock.find("owner->CommitAccessSnapshots("),
                  std::string::npos);
        EXPECT_EQ(commitBlock.find("owner->GetAccessSnapshots()"),
                  std::string::npos);
        EXPECT_EQ(commitBlock.find("owner->CommitAccessSnapshots("),
                  std::string::npos);
        EXPECT_EQ(sceneRenderer.find(
                      "ImportBuffer(visibilityBuffer, RHIResourceState::Common)"),
                  std::string::npos);
        EXPECT_NE(uploadTypes.find("RHIAccessSnapshot finalAccess"), std::string::npos);
        EXPECT_NE(uploadProcessor.find("result.finalAccess = MakeRHIAccessSnapshot"),
                  std::string::npos);
        EXPECT_NE(uploadProcessor.find("GetPhysicalUploadDomain"), std::string::npos);
        EXPECT_NE(resourceRegistry.find("RHIBufferAccessSnapshot accessSnapshot"),
                  std::string::npos);
        EXPECT_NE(resourceRegistry.find("constantsAccessSnapshot"), std::string::npos);
    }

    TEST(RHIArchitectureValidation,
         SoftwareAdapterFallbackImplementationIsExplicit)
    {
        const std::string runtimeTypes =
            ReadSource("Source/Runtime/Render/Include/Render/RenderRuntimeTypes.h");
        const std::string renderContext =
            ReadSource("Source/Runtime/Render/Private/Context/RenderContext.cpp");
        const std::string renderSubsystem =
            ReadSource("Source/Runtime/Render/Private/RenderSubsystem.cpp");
        const std::string dx12Device =
            ReadSource("Source/Runtime/RHI_DX12/Private/DX12Device.cpp");
        const std::string vulkanDevice =
            ReadSource("Source/Runtime/RHI_Vulkan/Private/VulkanDevice.cpp");

        EXPECT_NE(runtimeTypes.find(
                      "bool allowSoftwareAdapter = false;"),
                  std::string::npos);
        EXPECT_NE(renderContext.find(
                      "deviceDesc.allowSoftwareAdapter = "
                      "config.allowSoftwareAdapter;"),
                  std::string::npos);
        EXPECT_NE(renderSubsystem.find(
                      "contextConfig.allowSoftwareAdapter"),
                  std::string::npos);
        EXPECT_NE(dx12Device.find("EnumWarpAdapter"),
                  std::string::npos);
        EXPECT_NE(dx12Device.find(
                      "adapters.empty() && allowSoftwareAdapter"),
                  std::string::npos);
        EXPECT_NE(vulkanDevice.find(
                      "VK_PHYSICAL_DEVICE_TYPE_CPU &&"),
                  std::string::npos);
        EXPECT_NE(vulkanDevice.find(
                      "!allowSoftwareAdapter"),
                  std::string::npos);
    }

    TEST(RHIContractValidation, RenderSubsystemRoutesCompleteSurfaceUpdates)
    {
        const std::string renderHeader =
            ReadSource("Source/Runtime/Render/Include/Render/RenderSubsystem.h");
        const std::string contextHeader =
            ReadSource("Source/Runtime/Render/Include/Render/Context/RenderContext.h");
        const std::string renderSubsystem =
            ReadSource("Source/Runtime/Render/Private/RenderSubsystem.cpp");

        EXPECT_NE(renderHeader.find(
                      "RenderResizeResult RequestResize(const HAL::NativeSurfaceDesc& surface)"),
                  std::string::npos);
        EXPECT_NE(contextHeader.find(
                      "bool ResizeSwapChain(const HAL::NativeSurfaceDesc& surface)"),
                  std::string::npos);
        EXPECT_NE(contextHeader.find("GetSurface() const"), std::string::npos);

        const size_t classify =
            renderSubsystem.find("ClassifyNativeSurfaceUpdate(");
        const size_t resizeCase = renderSubsystem.find(
            "if (updateKind == HAL::NativeSurfaceUpdateKind::Resize)", classify);
        const size_t replaceCase = renderSubsystem.find(
            "else if (updateKind == HAL::NativeSurfaceUpdateKind::Replace", classify);
        ASSERT_NE(classify, std::string::npos);
        ASSERT_NE(resizeCase, std::string::npos);
        ASSERT_NE(replaceCase, std::string::npos);
        EXPECT_NE(renderSubsystem.find("m_context->ResizeSwapChain(surface)", resizeCase),
                  std::string::npos);
        EXPECT_NE(renderSubsystem.find("m_context->CreateSwapChain(surface)", replaceCase),
                  std::string::npos);
        EXPECT_NE(renderSubsystem.find("Surface update could not be applied", classify),
                  std::string::npos);

        const size_t applySurface = renderSubsystem.find("RenderRuntimeResult ApplySurface(");
        const size_t processRelease = renderSubsystem.find("void ProcessRelease", applySurface);
        ASSERT_NE(applySurface, std::string::npos);
        ASSERT_NE(processRelease, std::string::npos);
        const std::string applySurfaceBody =
            renderSubsystem.substr(applySurface, processRelease - applySurface);
        EXPECT_EQ(applySurfaceBody.find("PresentAcceptedFrame("), std::string::npos);
        constexpr std::string_view clearCall =
            "PresentDeterministicClear(surface.generation)";
        const size_t clearCallPosition = applySurfaceBody.find(clearCall);
        ASSERT_NE(clearCallPosition, std::string::npos);
        EXPECT_EQ(applySurfaceBody.find(clearCall,
                                        clearCallPosition + clearCall.size()),
                  std::string::npos);
        EXPECT_NE(renderSubsystem.find(
                      "commandContext->TextureBarrier(backBuffer,\n"
                      "                                           RHIResourceState::Undefined,\n"
                      "                                           RHIResourceState::RenderTarget)"),
                  std::string::npos);

        const size_t presentAccepted = renderSubsystem.find(
            "RenderRuntimeResult PresentAcceptedFrame(");
        const size_t presentClear = renderSubsystem.find(
            "RenderRuntimeResult PresentDeterministicClear(", presentAccepted);
        const size_t deviceStatus = renderSubsystem.find(
            "RenderRuntimeResult MakeDeviceRuntimeResult() const", presentClear);
        ASSERT_NE(presentAccepted, std::string::npos);
        ASSERT_NE(presentClear, std::string::npos);
        ASSERT_NE(deviceStatus, std::string::npos);
        const std::string acceptedFrameBody = renderSubsystem.substr(
            presentAccepted, presentClear - presentAccepted);
        const std::string deterministicClearBody = renderSubsystem.substr(
            presentClear, deviceStatus - presentClear);
        for (const std::string* frameBody : {
                 &acceptedFrameBody,
                 &deterministicClearBody})
        {
            EXPECT_NE(frameBody->find("MakeDeviceRuntimeResult()"),
                      std::string::npos);
            EXPECT_NE(frameBody->find(
                          "result.code = RenderRuntimeCode::RenderGraphValidationFailed"),
                      std::string::npos);
            EXPECT_EQ(frameBody->find(
                          "result.code = RenderRuntimeCode::DeviceLost"),
                      std::string::npos);
        }

        const size_t requestResize = renderSubsystem.find(
            "RenderResizeResult RenderSubsystem::RequestResize(");
        ASSERT_NE(requestResize, std::string::npos);
        EXPECT_NE(renderSubsystem.find("m_runtime->RequestResize(surface)", requestResize),
                  std::string::npos);
    }

    TEST(RHIContractValidation,
         RenderSubsystemReadinessDelegatesWithoutPublicTestPrivilege)
    {
        const std::string renderHeader =
            ReadSource("Source/Runtime/Render/Include/Render/RenderSubsystem.h");
        const std::string renderSubsystem =
            ReadSource("Source/Runtime/Render/Private/RenderSubsystem.cpp");

        EXPECT_EQ(renderHeader.find("friend struct RenderSubsystemTestAccess"),
                  std::string::npos);
        const size_t isReady = renderSubsystem.find(
            "bool RenderSubsystem::IsReady() const");
        ASSERT_NE(isReady, std::string::npos);
        const size_t functionEnd = renderSubsystem.find("\n}", isReady);
        ASSERT_NE(functionEnd, std::string::npos);
        const std::string body =
            renderSubsystem.substr(isReady, functionEnd - isReady);
        EXPECT_NE(body.find("m_runtime->IsReady()"), std::string::npos);
    }

    TEST(RHIContractValidation, NativeSurfaceCallersUseTheAtomicDescriptorContract)
    {
        const std::filesystem::path root;
        const std::string sampleRunner =
            ReadSource(root / "Samples/Common/Private/SampleRunner.cpp");
        const std::string renderContextHeader =
            ReadSource(root / "Source/Runtime/Render/Include/Render/Context/RenderContext.h");
        const std::string renderSubsystem =
            ReadSource(root / "Source/Runtime/Render/Private/RenderSubsystem.cpp");
        const std::string windowSubsystem =
            ReadSource(root / "Source/Runtime/RuntimeServices/Private/Window/WindowSubsystem.cpp");
        const std::string editorSwapChain =
            ReadSource(root / "Editor/Private/EditorMainSwapChainService.cpp");
        const std::string editorBootstrap =
            ReadSource(root / "Editor/Private/EditorRenderBootstrapService.cpp");

        for (const std::string* source : {&sampleRunner, &renderSubsystem})
        {
            EXPECT_EQ(source->find("swapChainDesc.windowHandle"), std::string::npos);
            EXPECT_EQ(source->find("swapChainDesc.width"), std::string::npos);
            EXPECT_EQ(source->find("swapChainDesc.height"), std::string::npos);
            EXPECT_EQ(source->find("swapChainDesc.format"), std::string::npos);
            EXPECT_EQ(source->find("swapChainDesc.vsync"), std::string::npos);
        }
        EXPECT_NE(renderContextHeader.find("CreateSwapChain(const HAL::NativeSurfaceDesc&"),
                  std::string::npos);
        EXPECT_EQ(renderContextHeader.find("CreateSwapChain(void*"), std::string::npos);
        EXPECT_EQ(editorSwapChain.find("windowHandle"), std::string::npos);
        EXPECT_NE(editorBootstrap.find("Initialize(config, initialSurface)"),
                  std::string::npos);
        EXPECT_NE(windowSubsystem.find(
                      "surface.platform = HAL::NativeSurfacePlatform::GLFW;"),
                  std::string::npos);
    }

    TEST(RHIContractValidation, StandardEngineInjectsWindowBeforeRenderInitialization)
    {
        const std::string renderHeader =
            ReadSource("Source/Runtime/Render/Include/Render/RenderSubsystem.h");
        const std::string engine = ReadSource("Source/Runtime/Engine/Private/Engine.cpp");
        const std::string renderComposition =
            ReadSource("Source/Runtime/Engine/Private/ECS/EcsRenderRuntimeComposition.cpp");
        const std::string showcase =
            ReadSource("Samples/RenderVerseSamples/main.cpp");

        EXPECT_NE(renderHeader.find("void Configure(const RenderRuntimeConfig& config,"),
                  std::string::npos);
        const size_t windowDependency = engine.find(
            "const auto windowDependency");
        const size_t windowPrerequisite =
            engine.find("WindowSubsystem>()", windowDependency);
        const size_t resourceDependency = engine.find(
            "const auto resourceDependency",
            windowPrerequisite);
        const size_t resourcePrerequisite = engine.find(
            "Resource::ResourceSubsystem>()",
            resourceDependency);
        const size_t inject =
            engine.find("CreateEngineEcsRenderRuntimeCompositionServices(");
        const size_t initialize =
            engine.find("const bool initialized = m_subsystems.InitializeAll(");
        ASSERT_NE(windowDependency, std::string::npos);
        ASSERT_NE(windowPrerequisite, std::string::npos);
        ASSERT_NE(resourceDependency, std::string::npos);
        ASSERT_NE(resourcePrerequisite, std::string::npos);
        ASSERT_NE(inject, std::string::npos);
        ASSERT_NE(initialize, std::string::npos);
        EXPECT_LT(windowPrerequisite, inject);
        EXPECT_LT(resourcePrerequisite, inject);
        EXPECT_LT(inject, initialize);

        const size_t capture = renderComposition.find(
            "HAL::NativeSurfaceDesc surface = m_services->CaptureRenderSurface()");
        const size_t release = renderComposition.find(
            "m_services->ReleaseGraphicsContextFromUpdateThread()",
            capture);
        const size_t rhiStartup = renderComposition.find(
            "m_services->ConfigureRender(m_config, surface)",
            release);
        ASSERT_NE(capture, std::string::npos);
        ASSERT_NE(release, std::string::npos);
        ASSERT_NE(rhiStartup, std::string::npos);
        EXPECT_LT(capture, release);
        EXPECT_LT(release, rhiStartup);

        EXPECT_EQ(renderHeader.find("SetWindowSubsystem"),
                  std::string::npos);
        EXPECT_EQ(engine.find("SetWindowSubsystem"),
                  std::string::npos);
        EXPECT_EQ(showcase.find("SetWindowSubsystem"),
                  std::string::npos);
        EXPECT_EQ(renderHeader.find("Runtime/Window/WindowSubsystem.h"),
                  std::string::npos);
        EXPECT_EQ(renderHeader.find("WindowSubsystem*"),
                  std::string::npos);
    }

    TEST(RHIContractValidation, OpenGLClaimsTheBackendWindowBeforeLoadingGlad)
    {
        const std::string source = ReadSource("Source/Runtime/RHI_OpenGL/Private/OpenGLDevice.cpp");
        const size_t backendWindow = source.find("desc.initialSurface.backendWindow");
        const size_t makeCurrent = source.find("glfwMakeContextCurrent");
        const size_t loadGlad = source.find("gladLoadGLLoader");

        ASSERT_NE(backendWindow, std::string::npos);
        ASSERT_NE(makeCurrent, std::string::npos);
        ASSERT_NE(loadGlad, std::string::npos);
        EXPECT_LT(backendWindow, makeCurrent);
        EXPECT_LT(makeCurrent, loadGlad);
    }

    TEST(RHIContractValidation, OpenGLSwapChainRetainsCapturedSurfaceExtent)
    {
        const std::string source =
            ReadSource("Source/Runtime/RHI_OpenGL/Private/OpenGLSwapChain.cpp");
        EXPECT_EQ(source.find("glfwGetFramebufferSize"), std::string::npos);
        EXPECT_NE(source.find("m_width(desc.surface.width)"),
                  std::string::npos);
        EXPECT_NE(source.find("m_height(desc.surface.height)"),
                  std::string::npos);
    }

    TEST(RHIContractValidation, OpenGLSurfaceRebindIsRejectedBeforeActiveSwapChainMutation)
    {
        const std::string deviceInterface =
            ReadSource("Source/Runtime/RHI/Include/RHI/RHIDevice.h");
        const std::string openGLHeader =
            ReadSource("Source/Runtime/RHI_OpenGL/Private/OpenGLDevice.h");
        const std::string openGLDevice =
            ReadSource("Source/Runtime/RHI_OpenGL/Private/OpenGLDevice.cpp");
        const std::string renderSubsystem =
            ReadSource("Source/Runtime/Render/Private/RenderSubsystem.cpp");
        const std::string renderContext =
            ReadSource("Source/Runtime/Render/Private/Context/RenderContext.cpp");

        EXPECT_NE(deviceInterface.find("virtual bool SupportsSurfaceRebind("),
                  std::string::npos);
        EXPECT_NE(openGLHeader.find("bool SupportsSurfaceRebind("),
                  std::string::npos);

        const size_t openGLPolicy = openGLDevice.find(
            "bool OpenGLDevice::SupportsSurfaceRebind(");
        ASSERT_NE(openGLPolicy, std::string::npos);
        EXPECT_NE(openGLDevice.find("currentSurface.backendWindow ==",
                                    openGLPolicy),
                  std::string::npos);
        EXPECT_NE(openGLDevice.find(
                      "IsSurfaceContextCurrent(replacementSurface)",
                      openGLPolicy),
                  std::string::npos);

        const size_t create = renderContext.find(
            "bool RenderContext::CreateSwapChain(");
        const size_t policy = renderContext.find(
            "m_device->SupportsSurfaceRebind(m_surface, surface)", create);
        const size_t rejection = renderContext.find("return false;", policy);
        const size_t destroy = renderContext.find("m_swapChain.Reset()", create);
        const size_t clearSnapshot = renderContext.find("m_surface = {};", create);
        ASSERT_NE(create, std::string::npos);
        ASSERT_NE(policy, std::string::npos);
        ASSERT_NE(rejection, std::string::npos);
        ASSERT_NE(destroy, std::string::npos);
        ASSERT_NE(clearSnapshot, std::string::npos);
        EXPECT_LT(policy, rejection);
        EXPECT_LT(rejection, destroy);
        EXPECT_LT(rejection, clearSnapshot);

        const size_t replaceCase = renderSubsystem.find(
            "else if (updateKind == HAL::NativeSurfaceUpdateKind::Replace");
        const size_t subsystemPolicy = renderSubsystem.find(
            "SupportsSurfaceRebind(", replaceCase);
        const size_t subsystemRejection = renderSubsystem.find(
            "Surface update could not be applied", subsystemPolicy);
        const size_t waitForSurfaceGeneration = renderSubsystem.find(
            "m_context->WaitForSurfaceGeneration()", replaceCase);
        const size_t prepare = renderSubsystem.find(
            "m_sceneRenderer->PrepareForSwapChainResize()", replaceCase);
        ASSERT_NE(replaceCase, std::string::npos);
        ASSERT_NE(subsystemPolicy, std::string::npos);
        ASSERT_NE(subsystemRejection, std::string::npos);
        ASSERT_NE(waitForSurfaceGeneration, std::string::npos);
        ASSERT_NE(prepare, std::string::npos);
        EXPECT_LT(subsystemPolicy, waitForSurfaceGeneration);
        EXPECT_LT(waitForSurfaceGeneration, prepare);
        EXPECT_LT(prepare, subsystemRejection);
    }

    TEST(RHIContractValidation, OpenGLFactoryRequiresItsOwnedCurrentContext)
    {
        const std::string header =
            ReadSource("Source/Runtime/RHI_OpenGL/Private/OpenGLDevice.h");
        const std::string source =
            ReadSource("Source/Runtime/RHI_OpenGL/Private/OpenGLDevice.cpp");

        EXPECT_NE(header.find("bool IsSurfaceContextCurrent("),
                  std::string::npos);
        EXPECT_NE(header.find("GLFWwindow* m_contextWindow"),
                  std::string::npos);

        const size_t create = source.find(
            "RHISwapChainRef OpenGLDevice::CreateSwapChain(");
        const size_t validate = source.find(
            "IsSurfaceContextCurrent(desc.surface)", create);
        const size_t construct = source.find(
            "MakeRef<OpenGLSwapChain>", create);
        ASSERT_NE(create, std::string::npos);
        ASSERT_NE(validate, std::string::npos);
        ASSERT_NE(construct, std::string::npos);
        EXPECT_LT(validate, construct);

        const size_t validator = source.find(
            "bool OpenGLDevice::IsSurfaceContextCurrent(");
        ASSERT_NE(validator, std::string::npos);
        EXPECT_NE(source.find("surface.backendWindow", validator),
                  std::string::npos);
        EXPECT_NE(source.find("IsOnGLThread()", validator),
                  std::string::npos);
        EXPECT_NE(source.find("targetWindow == m_contextWindow", validator),
                  std::string::npos);
        EXPECT_NE(source.find("glfwGetCurrentContext() == m_contextWindow",
                              validator),
                  std::string::npos);
    }

    TEST(RHIArchitectureValidation,
         SurfaceRebindPolicyImplementationKeepsVulkanReplacementEnabled)
    {
        const std::string vulkanHeader =
            ReadSource("Source/Runtime/RHI_Vulkan/Private/VulkanDevice.h");
        const std::string openGLSwapChain =
            ReadSource("Source/Runtime/RHI_OpenGL/Private/OpenGLSwapChain.cpp");
        EXPECT_EQ(vulkanHeader.find("SupportsSurfaceRebind"),
                  std::string::npos);
        EXPECT_EQ(openGLSwapChain.find("glfwMakeContextCurrent"),
                  std::string::npos);
    }

    TEST(RHIContractValidation, MetalPresentationStaysInsideCommandBufferOwnership)
    {
        const std::string swapChain = ReadSource("Source/Runtime/RHI_Metal/Private/MetalSwapChain.mm");
        for (const char* forbidden : {"NSWindow", "NSView", "UIView", "contentView",
                                      "setWantsLayer:", "setLayer:",
                                      "[m_currentDrawable present]"})
        {
            EXPECT_EQ(swapChain.find(forbidden), std::string::npos) << forbidden;
        }
        EXPECT_NE(swapChain.find("SetPresentationDrawable"), std::string::npos);
        EXPECT_NE(swapChain.find("maximumDrawableCount = m_bufferCount"),
                  std::string::npos);

        const std::string commandContext =
            ReadSource("Source/Runtime/RHI_Metal/Private/MetalCommandContext.mm");
        const size_t present = commandContext.find("presentDrawable:");
        const size_t commit = commandContext.find("[m_commandBuffer commit]");
        ASSERT_NE(present, std::string::npos);
        ASSERT_NE(commit, std::string::npos);
        EXPECT_LT(present, commit);
    }

    TEST(RHIContractValidation, PlatformSurfaceBridgesRejectPartialConstruction)
    {
        const std::string vulkanHeader =
            ReadSource("Source/Runtime/RHI_Vulkan/Private/VulkanSwapChain.h");
        const std::string vulkanSwapChain =
            ReadSource("Source/Runtime/RHI_Vulkan/Private/VulkanSwapChain.cpp");
        EXPECT_NE(vulkanHeader.find("bool IsValid() const"),
                  std::string::npos);
        EXPECT_NE(vulkanSwapChain.find("if (!swapChain->IsValid())"),
                  std::string::npos);
        EXPECT_NE(vulkanSwapChain.find("return nullptr;"),
                  std::string::npos);

        const std::string appleBridge =
            ReadSource("Source/Runtime/HAL/Private/Apple/GLFWMetalLayerBridge.mm");
        EXPECT_EQ(appleBridge.find("static_cast<CAMetalLayer*>"),
                  std::string::npos);
        EXPECT_NE(appleBridge.find("(CAMetalLayer*)view.layer"),
                  std::string::npos);
    }

    TEST(RHIContractValidation, SwapChainFactoriesRejectFailedConstruction)
    {
        const std::string dx12Header =
            ReadSource("Source/Runtime/RHI_DX12/Private/DX12SwapChain.h");
        const std::string dx12Factory =
            ReadSource("Source/Runtime/RHI_DX12/Private/DX12SwapChain.cpp");
        const std::string metalHeader =
            ReadSource("Source/Runtime/RHI_Metal/Private/MetalSwapChain.h");
        const std::string metalFactory =
            ReadSource("Source/Runtime/RHI_Metal/Private/MetalDevice.mm");
        const std::string openGLHeader =
            ReadSource("Source/Runtime/RHI_OpenGL/Private/OpenGLSwapChain.h");
        const std::string openGLFactory =
            ReadSource("Source/Runtime/RHI_OpenGL/Private/OpenGLDevice.cpp");

        for (const std::string* header :
             {&dx12Header, &metalHeader, &openGLHeader})
        {
            EXPECT_NE(header->find("bool IsValid() const"),
                      std::string::npos);
        }
        for (const std::string* factory :
             {&dx12Factory, &metalFactory, &openGLFactory})
        {
            EXPECT_NE(factory->find("if (!swapChain->IsValid())"),
                      std::string::npos);
            EXPECT_NE(factory->find("return nullptr;"),
                      std::string::npos);
        }
    }

    TEST(RHIContractValidation, IndexedIndirectBackendLimitsUseNativeContracts)
    {
        const std::string dx12 = ReadSource("Source/Runtime/RHI_DX12/Private/DX12Device.cpp");
        const std::string vulkan = ReadSource("Source/Runtime/RHI_Vulkan/Private/VulkanDevice.cpp");
        const std::string dx11 = ReadSource("Source/Runtime/RHI_DX11/Private/DX11Device.cpp");
        const std::string openGL = ReadSource("Source/Runtime/RHI_OpenGL/Private/OpenGLDevice.cpp");

        EXPECT_NE(dx12.find("sizeof(D3D12_DRAW_INDEXED_ARGUMENTS)"),
                  std::string::npos);
        EXPECT_NE(vulkan.find("sizeof(VkDrawIndexedIndirectCommand)"),
                  std::string::npos);
        EXPECT_NE(dx11.find("sizeof(D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS)"),
                  std::string::npos);
        EXPECT_NE(openGL.find("std::numeric_limits<GLsizei>::max()"),
                  std::string::npos);
        EXPECT_NE(openGL.find("openGLMaxDrawCount"), std::string::npos);
        EXPECT_NE(vulkan.find("m_enabledDrawIndirectFirstInstance ="),
                  std::string::npos);
        EXPECT_NE(vulkan.find("props.limits.maxDrawIndirectCount"),
                  std::string::npos);
    }

    TEST(RHIContractValidation, QueueTopologyPublicationStaysBackendHonest)
    {
        const std::string dx12 = ReadSource("Source/Runtime/RHI_DX12/Private/DX12Device.cpp");
        EXPECT_NE(dx12.find("RHIQueueCompletionMode::NativeTimeline"), std::string::npos);
        EXPECT_NE(dx12.find("GPUQueueDomain::Compute"), std::string::npos);
        EXPECT_NE(dx12.find("GPUQueueDomain::Copy"), std::string::npos);

        const std::string vulkan = ReadSource("Source/Runtime/RHI_Vulkan/Private/VulkanDevice.cpp");
        EXPECT_NE(vulkan.find("const bool hasDedicatedComputeQueue"),
                  std::string::npos);
        EXPECT_NE(vulkan.find("m_capabilities.supportsAsyncCompute = hasDedicatedComputeQueue"),
                  std::string::npos);
        const std::string vulkanDeviceHeader =
            ReadSource("Source/Runtime/RHI_Vulkan/Private/VulkanDevice.h");
        EXPECT_NE(vulkanDeviceHeader.find(
                      "uint32 GetComputeQueueFamily() const { return m_queueFamilies.computeFamily.value(); }"),
                  std::string::npos);
        EXPECT_NE(vulkanDeviceHeader.find(
                      "uint32 GetTransferQueueFamily() const { return m_queueFamilies.transferFamily.value(); }"),
                  std::string::npos);
        EXPECT_NE(vulkan.find("GPUQueueDomain copyDomain = GPUQueueDomain::Copy"),
                  std::string::npos);
        EXPECT_NE(dx12.find("supportsMultiQueueBatchSubmit = true"),
                  std::string::npos);

        const std::string metal = ReadSource("Source/Runtime/RHI_Metal/Private/MetalDevice.mm");
        EXPECT_NE(metal.find("m_capabilities.supportsAsyncCompute = false"), std::string::npos);
        EXPECT_NE(metal.find("RHIQueueCompletionMode::NativeTimeline"), std::string::npos);
        EXPECT_NE(metal.find("m_capabilities.queueTopology.activeDomainCount = 1"),
                  std::string::npos);
        EXPECT_EQ(metal.find("windows.h"), std::string::npos);
        EXPECT_EQ(metal.find("d3d12.h"), std::string::npos);

        const std::string metalCommandContext =
            ReadSource("Source/Runtime/RHI_Metal/Private/MetalCommandContext.mm");
        const std::string metalSynchronization =
            ReadSource("Source/Runtime/RHI_Metal/Private/MetalSynchronization.mm");
        EXPECT_NE(metalCommandContext.find("SignalFromCommandBuffer(m_commandBuffer)"),
                  std::string::npos);
        EXPECT_NE(metalSynchronization.find("encodeSignalEvent:m_event value:newValue"),
                  std::string::npos);
        EXPECT_NE(metalSynchronization.find("return m_event.signaledValue"),
                  std::string::npos);

        for (const char* path : {
                 "Source/Runtime/RHI_DX11/Private/DX11Device.cpp",
                 "Source/Runtime/RHI_OpenGL/Private/OpenGLDevice.cpp"})
        {
            const std::string compatibility = ReadSource(path);
            EXPECT_NE(compatibility.find("RHIQueueCompletionMode::CompatibilityWaitIdle"),
                      std::string::npos);
            EXPECT_NE(compatibility.find("m_capabilities.supportsQueueFenceWait = false"),
                      std::string::npos);
            EXPECT_NE(compatibility.find("m_capabilities.emulatesQueueFences = true"),
                      std::string::npos);
            EXPECT_NE(compatibility.find("m_capabilities.queueTopology.activeDomainCount = 1"),
                      std::string::npos);
        }

        for (const char* path : {
                 "Source/Runtime/RHI_DX11/Private/DX11CommandContext.h",
                 "Source/Runtime/RHI_DX12/Private/DX12CommandContext.h",
                 "Source/Runtime/RHI_Vulkan/Private/VulkanCommandContext.h",
                 "Source/Runtime/RHI_Metal/Private/MetalCommandContext.h",
                 "Source/Runtime/RHI_OpenGL/Private/OpenGLCommandContext.h"})
        {
            const std::string context = ReadSource(path);
            EXPECT_NE(context.find("RHICommandQueueType GetQueueType() const override"),
                      std::string::npos) << path;
        }
    }

    TEST(RHIContractValidation, RenderSubmissionTrackerRemainsPrivateAndBackendAgnostic)
    {
        const std::string frameSynchronizer =
            ReadSource("Source/Runtime/Render/Include/Render/Context/FrameSynchronizer.h");
        EXPECT_EQ(frameSynchronizer.find("RHIFence"), std::string::npos);
        EXPECT_EQ(frameSynchronizer.find("GetFrameFenceValue"), std::string::npos);
        EXPECT_NE(frameSynchronizer.find("GPUCompletionPoint"), std::string::npos);
        EXPECT_NE(frameSynchronizer.find("std::unique_ptr<RenderSubmissionTracker>"),
                  std::string::npos);

        const std::string renderContext =
            ReadSource("Source/Runtime/Render/Include/Render/Context/RenderContext.h");
        EXPECT_NE(renderContext.find("GPUCompletionPoint EndFrame();"), std::string::npos);
        EXPECT_EQ(renderContext.find("RenderSubmissionTracker"), std::string::npos);
        EXPECT_EQ(renderContext.find("m_computeFences"), std::string::npos);
        EXPECT_EQ(renderContext.find("m_computeFenceValues"), std::string::npos);

        const std::string renderContextSource =
            ReadSource("Source/Runtime/Render/Private/Context/RenderContext.cpp");
        EXPECT_NE(renderContextSource.find(
                      "submittedPoint = m_frameSynchronizer.SubmitGraphics(ctx)"),
                  std::string::npos);
        EXPECT_NE(renderContextSource.find(
                      "m_frameSynchronizer.SignalFrame(m_frameIndex, submittedPoint)"),
                  std::string::npos);
        EXPECT_NE(renderContextSource.find("return submittedPoint"), std::string::npos);

        const std::string frameSynchronizerSource =
            ReadSource("Source/Runtime/Render/Private/Context/FrameSynchronizer.cpp");
        EXPECT_EQ(frameSynchronizerSource.find("CreateFence("), std::string::npos);

        const std::string renderCmake = ReadSource("Source/Runtime/Render/CMakeLists.txt");
        EXPECT_NE(renderCmake.find("Private/Resources/RenderSubmissionTracker.cpp"),
                  std::string::npos);
        EXPECT_NE(renderCmake.find("RVX::RHI"), std::string::npos);
        EXPECT_EQ(renderCmake.find("RHI_DX12"), std::string::npos);
        EXPECT_EQ(renderCmake.find("RHI_Vulkan"), std::string::npos);
        EXPECT_EQ(renderCmake.find("RHI_Metal"), std::string::npos);
    }
} // namespace RVX::Tests
