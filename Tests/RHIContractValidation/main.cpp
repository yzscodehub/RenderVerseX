#include "RHI/RHICapabilities.h"
#include "RHI/RHICommandContext.h"
#include "RHI/RHIDevice.h"
#include "RHI/RHINativeSurface.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <type_traits>

namespace RVX::Tests
{
    namespace
    {
        class FakeRHIDevice final : public IRHIDevice
        {
        public:
            explicit FakeRHIDevice(RHICapabilities capabilities)
                : m_capabilities(std::move(capabilities))
            {
            }

            RHIBufferRef CreateBuffer(const RHIBufferDesc&) override { return {}; }
            RHITextureRef CreateTexture(const RHITextureDesc&) override { return {}; }
            RHITextureViewRef CreateTextureView(RHITexture*, const RHITextureViewDesc& = {}) override { return {}; }
            RHISamplerRef CreateSampler(const RHISamplerDesc&) override { return {}; }
            RHIShaderRef CreateShader(const RHIShaderDesc&) override { return {}; }
            RHIHeapRef CreateHeap(const RHIHeapDesc&) override { return {}; }
            RHITextureRef CreatePlacedTexture(RHIHeap*, uint64, const RHITextureDesc&) override { return {}; }
            RHIBufferRef CreatePlacedBuffer(RHIHeap*, uint64, const RHIBufferDesc&) override { return {}; }
            MemoryRequirements GetTextureMemoryRequirements(const RHITextureDesc&) override { return {}; }
            MemoryRequirements GetBufferMemoryRequirements(const RHIBufferDesc&) override { return {}; }
            RHIDescriptorSetLayoutRef CreateDescriptorSetLayout(const RHIDescriptorSetLayoutDesc&) override { return {}; }
            RHIPipelineLayoutRef CreatePipelineLayout(const RHIPipelineLayoutDesc&) override { return {}; }
            RHIPipelineRef CreateGraphicsPipeline(const RHIGraphicsPipelineDesc&) override { return {}; }
            RHIPipelineRef CreateComputePipeline(const RHIComputePipelineDesc&) override { return {}; }
            RHIDescriptorSetRef CreateDescriptorSet(const RHIDescriptorSetDesc&) override { return {}; }
            RHIQueryPoolRef CreateQueryPool(const RHIQueryPoolDesc&) override { return {}; }
            RHICommandContextRef CreateCommandContext(RHICommandQueueType) override { return {}; }
            uint64 SubmitCommandContext(RHICommandContext*, RHIFence*) override { return 0; }
            uint64 SubmitCommandContexts(std::span<RHICommandContext* const>, RHIFence*) override { return 0; }
            RHISwapChainRef CreateSwapChain(const RHISwapChainDesc&) override { return {}; }
            RHIFenceRef CreateFence(uint64 = 0) override { return {}; }
            void WaitForFence(RHIFence*, uint64) override {}
            void WaitIdle() override {}
            void BeginFrame() override {}
            void EndFrame() override {}
            uint32 GetCurrentFrameIndex() const override { return 0; }
            RHIStagingBufferRef CreateStagingBuffer(const RHIStagingBufferDesc&) override { return {}; }
            RHIRingBufferRef CreateRingBuffer(const RHIRingBufferDesc&) override { return {}; }
            RHIMemoryStats GetMemoryStats() const override { return {}; }
            void BeginResourceGroup(const char*) override {}
            void EndResourceGroup() override {}
            const RHICapabilities& GetCapabilities() const override { return m_capabilities; }
            RHIBackendType GetBackendType() const override { return m_capabilities.backendType; }

        private:
            RHICapabilities m_capabilities;
        };

        RHICapabilities MakeValidCapabilities(RHIBackendType backend)
        {
            RHICapabilities capabilities;
            capabilities.backendType = backend;
            capabilities.adapterName = std::string(ToString(backend)) + " Test Adapter";
            capabilities.driverVersion = "TestDriver.1";
            capabilities.supportsComputePipeline = true;
            capabilities.supportsDescriptorSets = true;
            capabilities.supportsDynamicDescriptorOffsets = true;
            capabilities.maxDescriptorSets = 4;
            capabilities.supportsExplicitResourceBarriers = true;
            capabilities.supportsDefaultQueueFenceSignal = true;

            if (backend == RHIBackendType::DX12)
            {
                capabilities.dx12.resourceBindingTier = 2;
            }
            else if (backend == RHIBackendType::Vulkan)
            {
                capabilities.vulkan.apiVersion = 1;
            }
            else if (backend == RHIBackendType::OpenGL)
            {
                capabilities.opengl.majorVersion = 4;
                capabilities.opengl.minorVersion = 5;
            }

            return capabilities;
        }

        const RHICapabilityReportEntry* FindReportEntry(const RHICapabilityReport& report,
                                                        RHICapabilityFeature feature)
        {
            const auto it = std::find_if(report.entries.begin(),
                                         report.entries.end(),
                                         [feature](const RHICapabilityReportEntry& entry)
                                         {
                                             return entry.feature == feature;
                                         });
            return it != report.entries.end() ? &(*it) : nullptr;
        }

        bool HasMissingRequirement(const RHICapabilityReport& report, const std::string& requirement)
        {
            return std::find(report.renderGraphBaselineMissingRequirements.begin(),
                             report.renderGraphBaselineMissingRequirements.end(),
                             requirement) != report.renderGraphBaselineMissingRequirements.end();
        }

        std::string ReadSource(const std::filesystem::path& relativePath)
        {
            std::ifstream stream(std::filesystem::path(RVX_SOURCE_DIR) / relativePath,
                                 std::ios::binary);
            return {std::istreambuf_iterator<char>(stream),
                    std::istreambuf_iterator<char>()};
        }

        template <typename T>
        concept HasWindowHandleField = requires(T value) { value.windowHandle; };

        template <typename T>
        concept HasWidthField = requires(T value) { value.width; };

        template <typename T>
        concept HasHeightField = requires(T value) { value.height; };

        template <typename T>
        concept HasFormatField = requires(T value) { value.format; };

        template <typename T>
        concept HasVsyncField = requires(T value) { value.vsync; };

        NativeSurfaceDesc MakeSurface(NativeSurfacePlatform platform)
        {
            NativeSurfaceDesc surface;
            surface.platform = platform;
            surface.nativeWindow = 0x1000;
            surface.nativeDisplay = 0x2000;
            surface.nativeLayer = 0x3000;
            surface.backendWindow = 0x4000;
            surface.width = 1280;
            surface.height = 720;
            surface.contentScale = 1.0f;
            surface.generation = 1;
            return surface;
        }
    } // namespace

    TEST(RHIContractValidation, NativeSurfaceRequiresBackendSpecificHandles)
    {
        EXPECT_TRUE(MakeSurface(NativeSurfacePlatform::Win32).IsValidFor(RHIBackendType::DX11));
        EXPECT_TRUE(MakeSurface(NativeSurfacePlatform::Win32).IsValidFor(RHIBackendType::DX12));
        EXPECT_TRUE(MakeSurface(NativeSurfacePlatform::Win32).IsValidFor(RHIBackendType::Vulkan));
        EXPECT_TRUE(MakeSurface(NativeSurfacePlatform::X11).IsValidFor(RHIBackendType::Vulkan));
        EXPECT_TRUE(MakeSurface(NativeSurfacePlatform::Wayland).IsValidFor(RHIBackendType::Vulkan));
        EXPECT_TRUE(MakeSurface(NativeSurfacePlatform::Cocoa).IsValidFor(RHIBackendType::Metal));
        EXPECT_TRUE(MakeSurface(NativeSurfacePlatform::UIKit).IsValidFor(RHIBackendType::Metal));
        EXPECT_TRUE(MakeSurface(NativeSurfacePlatform::GLFW).IsValidFor(RHIBackendType::OpenGL));

        NativeSurfaceDesc surface = MakeSurface(NativeSurfacePlatform::Win32);
        surface.nativeWindow = 0;
        EXPECT_FALSE(surface.IsValidFor(RHIBackendType::DX12));

        surface = MakeSurface(NativeSurfacePlatform::X11);
        surface.backendWindow = 0;
        EXPECT_FALSE(surface.IsValidFor(RHIBackendType::Vulkan));

        surface = MakeSurface(NativeSurfacePlatform::Cocoa);
        surface.nativeLayer = 0;
        EXPECT_FALSE(surface.IsValidFor(RHIBackendType::Metal));

        surface = MakeSurface(NativeSurfacePlatform::GLFW);
        surface.backendWindow = 0;
        EXPECT_FALSE(surface.IsValidFor(RHIBackendType::OpenGL));
    }

    TEST(RHIContractValidation, NativeSurfaceRejectsInvalidValueFields)
    {
        NativeSurfaceDesc surface = MakeSurface(NativeSurfacePlatform::Win32);

        surface.width = 0;
        EXPECT_FALSE(surface.IsValidFor(RHIBackendType::DX12));
        surface = MakeSurface(NativeSurfacePlatform::Win32);
        surface.height = 0;
        EXPECT_FALSE(surface.IsValidFor(RHIBackendType::DX12));
        surface = MakeSurface(NativeSurfacePlatform::Win32);
        surface.generation = 0;
        EXPECT_FALSE(surface.IsValidFor(RHIBackendType::DX12));
        surface = MakeSurface(NativeSurfacePlatform::Win32);
        surface.contentScale = 0.0f;
        EXPECT_FALSE(surface.IsValidFor(RHIBackendType::DX12));
        surface.contentScale = -1.0f;
        EXPECT_FALSE(surface.IsValidFor(RHIBackendType::DX12));
        surface.contentScale = std::numeric_limits<float32>::infinity();
        EXPECT_FALSE(surface.IsValidFor(RHIBackendType::DX12));
        surface.contentScale = std::numeric_limits<float32>::quiet_NaN();
        EXPECT_FALSE(surface.IsValidFor(RHIBackendType::DX12));
    }

    TEST(RHIContractValidation, NativeSurfaceUpdateClassificationUsesProductionLifecycleRules)
    {
        NativeSurfaceDesc current = MakeSurface(NativeSurfacePlatform::Win32);
        current.generation = 42;
        NativeSurfaceDesc update = current;

        update.generation = 42;
        EXPECT_EQ(NativeSurfaceUpdateKind::Reject,
                  ClassifyNativeSurfaceUpdate(current, update));
        update.generation = 41;
        EXPECT_EQ(NativeSurfaceUpdateKind::Reject,
                  ClassifyNativeSurfaceUpdate(current, update));

        update = current;
        update.generation = 43;
        update.width = 1920;
        update.height = 1080;
        EXPECT_EQ(NativeSurfaceUpdateKind::Resize,
                  ClassifyNativeSurfaceUpdate(current, update));

        const auto expectReplacement = [&](NativeSurfaceDesc replacement) {
            replacement.generation = 43;
            replacement.width = 1920;
            EXPECT_EQ(NativeSurfaceUpdateKind::Replace,
                      ClassifyNativeSurfaceUpdate(current, replacement));
        };

        update = current;
        update.nativeWindow = 0x5000;
        expectReplacement(update);
        update = current;
        update.nativeDisplay = 0x5000;
        expectReplacement(update);
        update = current;
        update.nativeLayer = 0x5000;
        expectReplacement(update);
        update = current;
        update.backendWindow = 0x5000;
        expectReplacement(update);
        update = current;
        update.platform = NativeSurfacePlatform::GLFW;
        expectReplacement(update);
        update = current;
        update.preferredFormat = RHIFormat::RGBA8_UNORM;
        expectReplacement(update);
        update = current;
        update.contentScale = 2.0f;
        expectReplacement(update);
        update = current;
        update.vsync = false;
        expectReplacement(update);

        update = current;
        update.generation = 43;
        EXPECT_EQ(NativeSurfaceUpdateKind::Replace,
                  ClassifyNativeSurfaceUpdate(current, update));
    }

    TEST(RHIContractValidation, RenderSubsystemRoutesCompleteSurfaceUpdates)
    {
        const std::string renderHeader =
            ReadSource("Render/Include/Render/RenderSubsystem.h");
        const std::string contextHeader =
            ReadSource("Render/Include/Render/Context/RenderContext.h");
        const std::string renderSubsystem =
            ReadSource("Render/Private/RenderSubsystem.cpp");

        EXPECT_NE(renderHeader.find(
                      "bool SetWindow(const NativeSurfaceDesc& surface)"),
                  std::string::npos);
        EXPECT_NE(contextHeader.find(
                      "bool ResizeSwapChain(const NativeSurfaceDesc& surface)"),
                  std::string::npos);
        EXPECT_NE(contextHeader.find("GetSurface() const"), std::string::npos);

        const size_t classify =
            renderSubsystem.find("ClassifyNativeSurfaceUpdate(");
        const size_t resizeCase = renderSubsystem.find(
            "case NativeSurfaceUpdateKind::Resize:", classify);
        const size_t replaceCase = renderSubsystem.find(
            "case NativeSurfaceUpdateKind::Replace:", classify);
        ASSERT_NE(classify, std::string::npos);
        ASSERT_NE(resizeCase, std::string::npos);
        ASSERT_NE(replaceCase, std::string::npos);
        EXPECT_NE(renderSubsystem.find("ResizeSwapChain(surface)", resizeCase),
                  std::string::npos);
        EXPECT_NE(renderSubsystem.find("CreateSwapChain(surface)", replaceCase),
                  std::string::npos);
        EXPECT_NE(renderSubsystem.find("return false;", classify),
                  std::string::npos);

        const size_t onResize = renderSubsystem.find(
            "void RenderSubsystem::OnResize(uint32_t width, uint32_t height)");
        ASSERT_NE(onResize, std::string::npos);
        EXPECT_NE(renderSubsystem.find("ResizeSwapChain(width, height)", onResize),
                  std::string::npos);
    }

    TEST(RHIContractValidation, SwapChainDescriptionHasSingleSurfaceSourceOfTruth)
    {
        static_assert(std::is_same_v<decltype(RHISwapChainDesc::surface), NativeSurfaceDesc>);
        static_assert(!HasWindowHandleField<RHISwapChainDesc>);
        static_assert(!HasWidthField<RHISwapChainDesc>);
        static_assert(!HasHeightField<RHISwapChainDesc>);
        static_assert(!HasFormatField<RHISwapChainDesc>);
        static_assert(!HasVsyncField<RHISwapChainDesc>);
    }

    TEST(RHIContractValidation, NativeSurfaceCallersUseTheAtomicDescriptorContract)
    {
        const std::filesystem::path root;
        const std::string basicRHI = ReadSource(root / "Samples/Basic/BasicRHI/main.cpp");
        const std::string computeDemo = ReadSource(root / "Samples/Basic/ComputeDemo/main.cpp");
        const std::string renderContextHeader =
            ReadSource(root / "Render/Include/Render/Context/RenderContext.h");
        const std::string renderSubsystem =
            ReadSource(root / "Render/Private/RenderSubsystem.cpp");
        const std::string windowSubsystem =
            ReadSource(root / "Runtime/Private/Window/WindowSubsystem.cpp");
        const std::string editorSwapChain =
            ReadSource(root / "Editor/Private/EditorMainSwapChainService.cpp");
        const std::string editorBootstrap =
            ReadSource(root / "Editor/Private/EditorRenderBootstrapService.cpp");

        for (const std::string* source : {&basicRHI, &computeDemo, &renderSubsystem})
        {
            EXPECT_EQ(source->find("swapChainDesc.windowHandle"), std::string::npos);
            EXPECT_EQ(source->find("swapChainDesc.width"), std::string::npos);
            EXPECT_EQ(source->find("swapChainDesc.height"), std::string::npos);
            EXPECT_EQ(source->find("swapChainDesc.format"), std::string::npos);
            EXPECT_EQ(source->find("swapChainDesc.vsync"), std::string::npos);
        }
        EXPECT_NE(renderContextHeader.find("CreateSwapChain(const NativeSurfaceDesc&"),
                  std::string::npos);
        EXPECT_EQ(renderContextHeader.find("CreateSwapChain(void*"), std::string::npos);
        EXPECT_EQ(editorSwapChain.find("windowHandle"), std::string::npos);
        EXPECT_NE(editorBootstrap.find("Initialize(config, initialSurface)"),
                  std::string::npos);
        EXPECT_NE(windowSubsystem.find(
                      "surface.platform = NativeSurfacePlatform::GLFW;"),
                  std::string::npos);
    }

    TEST(RHIContractValidation, StandardEngineInjectsWindowBeforeRenderInitialization)
    {
        const std::string renderHeader =
            ReadSource("Render/Include/Render/RenderSubsystem.h");
        const std::string engine = ReadSource("Engine/Private/Engine.cpp");
        const std::string renderSubsystem =
            ReadSource("Render/Private/RenderSubsystem.cpp");
        const std::string showcase =
            ReadSource("Samples/Showcase/RenderingShowcase/main.cpp");

        EXPECT_NE(renderHeader.find("RVX_SUBSYSTEM_DEPENDENCIES(WindowSubsystem)"),
                  std::string::npos);
        const size_t inject =
            engine.find("render->SetWindowSubsystem(window)");
        const size_t initialize =
            engine.find("return m_subsystems.InitializeAll()");
        ASSERT_NE(inject, std::string::npos);
        ASSERT_NE(initialize, std::string::npos);
        EXPECT_LT(inject, initialize);

        const size_t capture = renderSubsystem.find(
            "initialSurface = m_windowSubsystem->CaptureRenderSurface()");
        const size_t release = renderSubsystem.find(
            "m_windowSubsystem->ReleaseGraphicsContextFromCurrentThread()",
            capture);
        const size_t rhiStartup = renderSubsystem.find(
            "m_renderContext->Initialize(ctxConfig, initialSurface)",
            release);
        ASSERT_NE(capture, std::string::npos);
        ASSERT_NE(release, std::string::npos);
        ASSERT_NE(rhiStartup, std::string::npos);
        EXPECT_LT(capture, release);
        EXPECT_LT(release, rhiStartup);

        EXPECT_EQ(showcase.find(
                      "renderSubsystem->SetWindowSubsystem(windowSubsystem);"),
                  std::string::npos);
    }

    TEST(RHIContractValidation, OpenGLClaimsTheBackendWindowBeforeLoadingGlad)
    {
        const std::string source = ReadSource("RHI_OpenGL/Private/OpenGLDevice.cpp");
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
            ReadSource("RHI_OpenGL/Private/OpenGLSwapChain.cpp");
        EXPECT_EQ(source.find("glfwGetFramebufferSize"), std::string::npos);
        EXPECT_NE(source.find("m_width(desc.surface.width)"),
                  std::string::npos);
        EXPECT_NE(source.find("m_height(desc.surface.height)"),
                  std::string::npos);
    }

    TEST(RHIContractValidation, MetalPresentationStaysInsideCommandBufferOwnership)
    {
        const std::string swapChain = ReadSource("RHI_Metal/Private/MetalSwapChain.mm");
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
            ReadSource("RHI_Metal/Private/MetalCommandContext.mm");
        const size_t present = commandContext.find("presentDrawable:");
        const size_t commit = commandContext.find("[m_commandBuffer commit]");
        ASSERT_NE(present, std::string::npos);
        ASSERT_NE(commit, std::string::npos);
        EXPECT_LT(present, commit);
    }

    TEST(RHIContractValidation, PlatformSurfaceBridgesRejectPartialConstruction)
    {
        const std::string vulkanHeader =
            ReadSource("RHI_Vulkan/Private/VulkanSwapChain.h");
        const std::string vulkanSwapChain =
            ReadSource("RHI_Vulkan/Private/VulkanSwapChain.cpp");
        EXPECT_NE(vulkanHeader.find("bool IsValid() const"),
                  std::string::npos);
        EXPECT_NE(vulkanSwapChain.find("if (!swapChain->IsValid())"),
                  std::string::npos);
        EXPECT_NE(vulkanSwapChain.find("return nullptr;"),
                  std::string::npos);

        const std::string appleBridge =
            ReadSource("HAL/Private/Apple/GLFWMetalLayerBridge.mm");
        EXPECT_EQ(appleBridge.find("static_cast<CAMetalLayer*>"),
                  std::string::npos);
        EXPECT_NE(appleBridge.find("(CAMetalLayer*)view.layer"),
                  std::string::npos);
    }

    TEST(RHIContractValidation, SwapChainFactoriesRejectFailedConstruction)
    {
        const std::string dx12Header =
            ReadSource("RHI_DX12/Private/DX12SwapChain.h");
        const std::string dx12Factory =
            ReadSource("RHI_DX12/Private/DX12SwapChain.cpp");
        const std::string metalHeader =
            ReadSource("RHI_Metal/Private/MetalSwapChain.h");
        const std::string metalFactory =
            ReadSource("RHI_Metal/Private/MetalDevice.mm");
        const std::string openGLHeader =
            ReadSource("RHI_OpenGL/Private/OpenGLSwapChain.h");
        const std::string openGLFactory =
            ReadSource("RHI_OpenGL/Private/OpenGLDevice.cpp");

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

    TEST(RHIContractValidation, AcceptsMinimalConcreteBackendCapabilities)
    {
        RHICapabilities capabilities = MakeValidCapabilities(RHIBackendType::DX12);

        auto result = ValidateRHICapabilities(capabilities);

        EXPECT_TRUE(result) << result.message;
        EXPECT_TRUE(result.message.empty());
    }

    TEST(RHIContractValidation, RejectsNonConcreteBackendIdentity)
    {
        RHICapabilities capabilities = MakeValidCapabilities(RHIBackendType::DX12);
        capabilities.backendType = RHIBackendType::Auto;

        auto result = ValidateRHICapabilities(capabilities);

        EXPECT_FALSE(result);
        EXPECT_NE(result.message.find("concrete backend"), std::string::npos);
    }

    TEST(RHIContractValidation, RejectsDescriptorSetContradictions)
    {
        RHICapabilities capabilities = MakeValidCapabilities(RHIBackendType::DX12);
        capabilities.maxDescriptorSets = 0;

        auto result = ValidateRHICapabilities(capabilities);

        EXPECT_FALSE(result);
        EXPECT_NE(result.message.find("maxDescriptorSets"), std::string::npos);

        capabilities = MakeValidCapabilities(RHIBackendType::DX12);
        capabilities.supportsDescriptorSets = false;

        result = ValidateRHICapabilities(capabilities);

        EXPECT_FALSE(result);
        EXPECT_NE(result.message.find("maxDescriptorSets"), std::string::npos);

        capabilities = MakeValidCapabilities(RHIBackendType::DX12);
        capabilities.supportsDescriptorSets = false;
        capabilities.supportsDynamicDescriptorOffsets = true;
        capabilities.maxDescriptorSets = 0;

        result = ValidateRHICapabilities(capabilities);

        EXPECT_FALSE(result);
        EXPECT_NE(result.message.find("dynamic descriptor offsets"), std::string::npos);
    }

    TEST(RHIContractValidation, RejectsBarrierContradictions)
    {
        RHICapabilities capabilities = MakeValidCapabilities(RHIBackendType::DX12);
        capabilities.emulatesResourceBarriers = true;

        auto result = ValidateRHICapabilities(capabilities);

        EXPECT_FALSE(result);
        EXPECT_NE(result.message.find("both explicit and emulated"), std::string::npos);

        capabilities = MakeValidCapabilities(RHIBackendType::DX11);
        capabilities.supportsExplicitResourceBarriers = false;
        capabilities.supportsSplitBarrier = true;

        result = ValidateRHICapabilities(capabilities);

        EXPECT_FALSE(result);
        EXPECT_NE(result.message.find("split barriers"), std::string::npos);
    }

    TEST(RHIContractValidation, RejectsQueueFenceContradictions)
    {
        RHICapabilities capabilities = MakeValidCapabilities(RHIBackendType::DX12);
        capabilities.supportsExplicitQueueFenceSignal = false;
        capabilities.supportsQueueFenceWait = true;

        auto result = ValidateRHICapabilities(capabilities);

        EXPECT_FALSE(result);
        EXPECT_NE(result.message.find("queue fence waits"), std::string::npos);

        capabilities = MakeValidCapabilities(RHIBackendType::DX11);
        capabilities.supportsExplicitQueueFenceSignal = true;
        capabilities.supportsQueueFenceWait = true;
        capabilities.emulatesQueueFences = true;

        result = ValidateRHICapabilities(capabilities);

        EXPECT_FALSE(result);
        EXPECT_NE(result.message.find("emulated queue fences"), std::string::npos);
    }

    TEST(RHIContractValidation, RejectsAsyncComputeWithoutComputePipeline)
    {
        RHICapabilities capabilities = MakeValidCapabilities(RHIBackendType::DX12);
        capabilities.supportsComputePipeline = false;
        capabilities.supportsAsyncCompute = true;

        auto result = ValidateRHICapabilities(capabilities);

        EXPECT_FALSE(result);
        EXPECT_NE(result.message.find("compute pipeline"), std::string::npos);
    }

    TEST(RHIContractValidation, RejectsRayTracingPipelineWithoutRequiredLimits)
    {
        RHICapabilities capabilities = MakeValidCapabilities(RHIBackendType::DX12);
        capabilities.supportsRaytracing = true;
        capabilities.supportsRaytracingPipeline = true;

        auto result = ValidateRHICapabilities(capabilities);

        EXPECT_FALSE(result);
        EXPECT_NE(result.message.find("shader table limits"), std::string::npos);

        capabilities.maxRayRecursionDepth = 31;
        capabilities.shaderGroupHandleSize = 32;
        capabilities.shaderGroupHandleAlignment = 32;
        capabilities.shaderTableBaseAlignment = 64;

        result = ValidateRHICapabilities(capabilities);

        EXPECT_TRUE(result) << result.message;
    }

    TEST(RHIContractValidation, RejectsBackendSpecificMissingMetadata)
    {
        RHICapabilities vulkan = MakeValidCapabilities(RHIBackendType::Vulkan);
        vulkan.vulkan.apiVersion = 0;

        auto result = ValidateRHICapabilities(vulkan);

        EXPECT_FALSE(result);
        EXPECT_NE(result.message.find("apiVersion"), std::string::npos);

        RHICapabilities opengl = MakeValidCapabilities(RHIBackendType::OpenGL);
        opengl.opengl.majorVersion = 0;

        result = ValidateRHICapabilities(opengl);

        EXPECT_FALSE(result);
        EXPECT_NE(result.message.find("majorVersion"), std::string::npos);
    }

    TEST(RHIContractValidation, CapabilityReportClassifiesCoreBackendContracts)
    {
        RHICapabilities capabilities = MakeValidCapabilities(RHIBackendType::DX12);
        capabilities.supportsAsyncCompute = true;
        capabilities.supportsIndirectDrawCount = true;
        capabilities.supportsTimestampQueries = true;
        capabilities.supportsMemoryBudgetQuery = true;
        capabilities.supportsExplicitHeapManagement = true;

        const RHICapabilityReport report = BuildRHICapabilityReport(capabilities);

        EXPECT_EQ(report.schemaVersion, RVX_RHI_CAPABILITY_REPORT_SCHEMA_VERSION);
        EXPECT_EQ(report.backendType, RHIBackendType::DX12);
        EXPECT_EQ(report.adapterName, capabilities.adapterName);
        EXPECT_EQ(report.driverVersion, capabilities.driverVersion);
        EXPECT_TRUE(report.validationPassed) << report.validationMessage;
        EXPECT_TRUE(report.renderGraphBaselineSupported);
        EXPECT_TRUE(report.renderGraphBaselineMissingRequirements.empty());
        EXPECT_EQ(report.entries.size(), static_cast<size_t>(11));
        EXPECT_STREQ(GetRHICapabilityFeatureName(RHICapabilityFeature::RayTracing), "RayTracing");
        EXPECT_STREQ(GetRHICapabilityStatusName(RHICapabilityStatus::Unsupported), "Unsupported");
        EXPECT_GE(report.supportedCount, 7u);
        EXPECT_EQ(report.emulatedCount, 0u);
        EXPECT_GE(report.unsupportedCount, 1u);

        const RHICapabilityReportEntry* compute =
            FindReportEntry(report, RHICapabilityFeature::ComputePipeline);
        ASSERT_NE(compute, nullptr);
        EXPECT_EQ(compute->status, RHICapabilityStatus::Supported);
        EXPECT_TRUE(compute->supported);
        EXPECT_FALSE(compute->emulated);
        EXPECT_EQ(compute->requiredCapability, "supportsComputePipeline");

        const RHICapabilityReportEntry* rayTracing =
            FindReportEntry(report, RHICapabilityFeature::RayTracing);
        ASSERT_NE(rayTracing, nullptr);
        EXPECT_EQ(rayTracing->status, RHICapabilityStatus::Unsupported);
        EXPECT_FALSE(rayTracing->supported);
        EXPECT_EQ(rayTracing->requiredCapability, "supportsRaytracing+supportsRaytracingPipeline");
        EXPECT_NE(rayTracing->diagnosticMessage.find("unavailable"), std::string::npos);
    }

    TEST(RHIContractValidation, CapabilityReportSurfacesEmulationAndValidationFailures)
    {
        RHICapabilities capabilities = MakeValidCapabilities(RHIBackendType::DX11);
        capabilities.supportsExplicitResourceBarriers = false;
        capabilities.emulatesResourceBarriers = true;
        capabilities.supportsDefaultQueueFenceSignal = true;
        capabilities.emulatesQueueFences = true;

        RHICapabilityReport report = BuildRHICapabilityReport(capabilities);
        EXPECT_TRUE(report.validationPassed) << report.validationMessage;
        EXPECT_GE(report.emulatedCount, 2u);

        const RHICapabilityReportEntry* barriers =
            FindReportEntry(report, RHICapabilityFeature::ExplicitResourceBarriers);
        ASSERT_NE(barriers, nullptr);
        EXPECT_EQ(barriers->status, RHICapabilityStatus::Emulated);
        EXPECT_TRUE(barriers->supported);
        EXPECT_TRUE(barriers->emulated);
        EXPECT_NE(barriers->diagnosticMessage.find("emulated"), std::string::npos);

        capabilities.supportsExplicitResourceBarriers = true;
        report = BuildRHICapabilityReport(capabilities);

        EXPECT_FALSE(report.validationPassed);
        EXPECT_FALSE(report.renderGraphBaselineSupported);
        EXPECT_TRUE(HasMissingRequirement(report, "ValidateRHICapabilities"));
        EXPECT_NE(report.validationMessage.find("both explicit and emulated"), std::string::npos);
    }

    TEST(RHIContractValidation, CapabilityReportExposesRenderGraphBaselineReadiness)
    {
        RHICapabilities capabilities = MakeValidCapabilities(RHIBackendType::DX12);

        RHICapabilityReport report = BuildRHICapabilityReport(capabilities);

        EXPECT_TRUE(report.validationPassed) << report.validationMessage;
        EXPECT_TRUE(report.renderGraphBaselineSupported);
        EXPECT_TRUE(report.renderGraphBaselineMissingRequirements.empty());

        capabilities.supportsDescriptorSets = false;
        capabilities.supportsDynamicDescriptorOffsets = false;
        capabilities.maxDescriptorSets = 0;

        report = BuildRHICapabilityReport(capabilities);

        EXPECT_TRUE(report.validationPassed) << report.validationMessage;
        EXPECT_FALSE(report.renderGraphBaselineSupported);
        EXPECT_TRUE(HasMissingRequirement(report, "supportsDescriptorSets+maxDescriptorSets"));
        EXPECT_FALSE(HasMissingRequirement(report, "ValidateRHICapabilities"));
    }

    TEST(RHIContractValidation, DeviceCapabilityReportUsesPublicDeviceCapabilities)
    {
        RHICapabilities capabilities = MakeValidCapabilities(RHIBackendType::Vulkan);
        capabilities.supportsAsyncCompute = true;
        capabilities.supportsIndirectDrawCount = true;
        capabilities.supportsTimestampQueries = true;

        const FakeRHIDevice device(capabilities);
        const RHICapabilityReport report = device.GetCapabilityReport();

        EXPECT_EQ(report.schemaVersion, RVX_RHI_CAPABILITY_REPORT_SCHEMA_VERSION);
        EXPECT_EQ(report.backendType, RHIBackendType::Vulkan);
        EXPECT_EQ(report.adapterName, capabilities.adapterName);
        EXPECT_EQ(report.driverVersion, capabilities.driverVersion);
        EXPECT_TRUE(report.validationPassed) << report.validationMessage;
        EXPECT_EQ(report.entries.size(), static_cast<size_t>(11));

        const RHICapabilityReportEntry* asyncCompute =
            FindReportEntry(report, RHICapabilityFeature::AsyncCompute);
        ASSERT_NE(asyncCompute, nullptr);
        EXPECT_EQ(asyncCompute->status, RHICapabilityStatus::Supported);
        EXPECT_EQ(asyncCompute->requiredCapability, "supportsAsyncCompute+supportsComputePipeline");
    }

    TEST(RHIContractValidation, CapabilityReportTextExportIsStableForTools)
    {
        RHICapabilities capabilities = MakeValidCapabilities(RHIBackendType::OpenGL);
        capabilities.supportsExplicitResourceBarriers = false;
        capabilities.emulatesResourceBarriers = true;
        capabilities.supportsDefaultQueueFenceSignal = false;
        capabilities.emulatesQueueFences = true;

        const FakeRHIDevice device(capabilities);
        const std::string text = device.ExportCapabilityReportText();

        EXPECT_NE(text.find("RHI Capability Report"), std::string::npos);
        EXPECT_NE(text.find("Schema: 3"), std::string::npos);
        EXPECT_NE(text.find("Backend: OpenGL"), std::string::npos);
        EXPECT_NE(text.find("Adapter: OpenGL Test Adapter"), std::string::npos);
        EXPECT_NE(text.find("DriverVersion: TestDriver.1"), std::string::npos);
        EXPECT_NE(text.find("Validation: Passed"), std::string::npos);
        EXPECT_NE(text.find("RenderGraphBaseline: Passed"), std::string::npos);
        EXPECT_NE(text.find("RenderGraphBaselineMissing: none"), std::string::npos);
        EXPECT_NE(text.find("ExplicitResourceBarriers: Emulated"), std::string::npos);
        EXPECT_NE(text.find("QueueSynchronization: Emulated"), std::string::npos);
        EXPECT_NE(text.find("Summary: supported="), std::string::npos);
    }

    TEST(RHIContractValidation, CapabilityReportJsonExportIsStableForTools)
    {
        RHICapabilities capabilities = MakeValidCapabilities(RHIBackendType::OpenGL);
        capabilities.supportsExplicitResourceBarriers = false;
        capabilities.emulatesResourceBarriers = true;
        capabilities.supportsDefaultQueueFenceSignal = false;
        capabilities.emulatesQueueFences = true;

        const FakeRHIDevice device(capabilities);
        const std::string json = device.ExportCapabilityReportJson();

        EXPECT_NE(json.find("\"schemaVersion\": 3"), std::string::npos);
        EXPECT_NE(json.find("\"schemaId\": \"RVX.RHI.CapabilityReport\""), std::string::npos);
        EXPECT_NE(json.find("\"id\": \"rhiCapabilityReportJson\""), std::string::npos);
        EXPECT_NE(json.find("\"kind\": \"RHICapabilityReportJson\""), std::string::npos);
        EXPECT_NE(json.find("\"contentType\": \"application/json\""), std::string::npos);
        EXPECT_NE(json.find("\"contentHash\": \"\""), std::string::npos);
        EXPECT_NE(json.find("\"relativePath\": \"\""), std::string::npos);
        EXPECT_NE(json.find("\"backend\": \"OpenGL\""), std::string::npos);
        EXPECT_NE(json.find("\"adapterName\": \"OpenGL Test Adapter\""), std::string::npos);
        EXPECT_NE(json.find("\"driverVersion\": \"TestDriver.1\""), std::string::npos);
        EXPECT_NE(json.find("\"validationPassed\": true"), std::string::npos);
        EXPECT_NE(json.find("\"renderGraphBaseline\": {"), std::string::npos);
        EXPECT_NE(json.find("\"supported\": true"), std::string::npos);
        EXPECT_NE(json.find("\"missingRequirements\": []"), std::string::npos);
        EXPECT_NE(json.find("\"feature\": \"ExplicitResourceBarriers\""), std::string::npos);
        EXPECT_NE(json.find("\"status\": \"Emulated\""), std::string::npos);
    }

} // namespace RVX::Tests
