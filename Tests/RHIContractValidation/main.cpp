#include "RHI/RHICapabilities.h"
#include "RHI/RHIBuffer.h"
#include "RHI/RHICommandContext.h"
#include "RHI/RHIDevice.h"
#include "RHI/RHIDeviceStatus.h"
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
    static_assert(static_cast<uint8>(RHIDeviceRuntimeStatus::Ready) == 0);
    static_assert(static_cast<uint8>(RHIDeviceRuntimeStatus::DeviceLost) == 1);
    static_assert(static_cast<uint8>(RHIDeviceRuntimeStatus::FatalError) == 2);
    static_assert(static_cast<uint8>(RHIDeviceFaultOperation::None) == 0);
    static_assert(static_cast<uint8>(RHIDeviceFaultOperation::Shutdown) == 10);

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

        class DescriptorContractBuffer final : public RHIBuffer
        {
        public:
            uint64 GetSize() const override { return 256; }
            RHIBufferUsage GetUsage() const override { return RHIBufferUsage::Constant; }
            RHIMemoryType GetMemoryType() const override { return RHIMemoryType::Upload; }
            uint32 GetStride() const override { return 0; }
            void* Map() override { return nullptr; }
            void Unmap() override {}
        };

        class DescriptorContractLayout final : public RHIDescriptorSetLayout
        {
        public:
            explicit DescriptorContractLayout(RHIDescriptorSetLayoutDesc desc)
                : m_entries(std::move(desc.entries))
            {
            }

            const std::vector<RHIBindingLayoutEntry>& GetEntries() const override
            {
                return m_entries;
            }

        private:
            std::vector<RHIBindingLayoutEntry> m_entries;
        };

        class DescriptorContractSet final : public RHIDescriptorSet
        {
        public:
            explicit DescriptorContractSet(const RHIDescriptorSetDesc& desc)
                : RHIDescriptorSet(desc)
            {
            }
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
            capabilities.queueTopology.logicalQueueDomains = {
                GPUQueueDomain::Graphics,
                GPUQueueDomain::Graphics,
                GPUQueueDomain::Graphics,
            };
            capabilities.queueTopology.activeDomainCount = 1;

            if (backend == RHIBackendType::DX11 || backend == RHIBackendType::OpenGL)
            {
                capabilities.queueTopology.completionMode =
                    RHIQueueCompletionMode::CompatibilityWaitIdle;
                capabilities.emulatesQueueFences = true;
            }
            else
            {
                capabilities.queueTopology.completionMode = RHIQueueCompletionMode::NativeTimeline;
            }

            if (backend == RHIBackendType::DX12)
            {
                capabilities.dx12.resourceBindingTier = 2;
                capabilities.supportsAsyncCompute = true;
                capabilities.queueTopology.logicalQueueDomains = {
                    GPUQueueDomain::Graphics,
                    GPUQueueDomain::Compute,
                    GPUQueueDomain::Copy,
                };
                capabilities.queueTopology.activeDomainCount = 3;
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

    TEST(RHIContractValidation, DescriptorSetsRequireCompleteImmutableSnapshots)
    {
        RHIDescriptorSetLayoutDesc layoutDesc;
        layoutDesc.AddBinding(0, RHIBindingType::UniformBuffer, RHIShaderStage::All, 2);
        layoutDesc.AddBinding(1, RHIBindingType::ShaderResourceBuffer);
        DescriptorContractLayout layout(layoutDesc);
        DescriptorContractBuffer buffer;

        RHIDescriptorSetDesc incompleteDesc;
        incompleteDesc.SetLayout(&layout).BindBuffer(0, &buffer, 0, 64, 0);
        const RHIDescriptorValidationResult incomplete =
            ValidateRHIDescriptorSetDesc(incompleteDesc);
        EXPECT_FALSE(incomplete);
        EXPECT_EQ(incomplete.code, RHIDescriptorValidationCode::IncompleteSnapshot);
        EXPECT_EQ(incomplete.binding, 0u);
        EXPECT_EQ(incomplete.arrayElement, 1u);

        RHIDescriptorSetDesc completeDesc;
        completeDesc.SetLayout(&layout)
            .BindBuffer(1, &buffer)
            .BindBuffer(0, &buffer, 0, 64, 1)
            .BindBuffer(0, &buffer, 0, 64, 0);
        ASSERT_TRUE(ValidateRHIDescriptorSetDesc(completeDesc));

        DescriptorContractSet descriptorSet(completeDesc);
        ASSERT_TRUE(descriptorSet.IsReadyForBinding(&layout));
        ASSERT_EQ(descriptorSet.GetDescriptorSnapshot().size(), 3u);
        EXPECT_EQ(descriptorSet.GetDescriptorSnapshot()[0].binding, 0u);
        EXPECT_EQ(descriptorSet.GetDescriptorSnapshot()[0].arrayElement, 0u);
        EXPECT_EQ(descriptorSet.GetDescriptorSnapshot()[1].binding, 0u);
        EXPECT_EQ(descriptorSet.GetDescriptorSnapshot()[1].arrayElement, 1u);
        EXPECT_EQ(descriptorSet.GetDescriptorSnapshot()[2].binding, 1u);
        EXPECT_TRUE(descriptorSet.Update({}));
        EXPECT_FALSE(descriptorSet.Update({completeDesc.bindings.front()}));
        EXPECT_FALSE(descriptorSet.HasBeenBound());
        descriptorSet.MarkBound();
        EXPECT_TRUE(descriptorSet.HasBeenBound());
        EXPECT_FALSE(descriptorSet.Update({completeDesc.bindings.front()}));

        DescriptorContractLayout differentLayout(layoutDesc);
        EXPECT_FALSE(descriptorSet.IsReadyForBinding(&differentLayout));
    }

    TEST(RHIContractValidation, DescriptorDataVolatilityIsIndependentFromSnapshotCompleteness)
    {
        RHIDescriptorSetLayoutDesc immutableReadLayout;
        immutableReadLayout.AddBinding(
            0,
            RHIBindingType::ShaderResourceBuffer,
            RHIShaderStage::Compute,
            1,
            RHIResourceDataVolatility::Immutable);
        EXPECT_TRUE(ValidateRHIDescriptorSetLayoutDesc(immutableReadLayout));

        RHIDescriptorSetLayoutDesc invalidWritableLayout;
        invalidWritableLayout.AddBinding(
            0,
            RHIBindingType::StorageBuffer,
            RHIShaderStage::Compute,
            1,
            RHIResourceDataVolatility::Immutable);
        const RHIDescriptorValidationResult validation =
            ValidateRHIDescriptorSetLayoutDesc(invalidWritableLayout);
        EXPECT_FALSE(validation);
        EXPECT_EQ(validation.code, RHIDescriptorValidationCode::InvalidLayout);

        RHICapabilities capabilities = MakeValidCapabilities(RHIBackendType::Vulkan);
        capabilities.supportsDynamicDescriptorOffsets = false;
        RHIDescriptorSetLayoutDesc dynamicLayout;
        dynamicLayout.AddDynamicBinding(0, RHIBindingType::DynamicUniformBuffer);
        EXPECT_FALSE(ValidateRHIDescriptorSetLayoutCapabilities(dynamicLayout, capabilities));

        capabilities.supportsDynamicDescriptorOffsets = true;
        capabilities.supportsRaytracing = false;
        RHIDescriptorSetLayoutDesc accelerationStructureLayout;
        accelerationStructureLayout.AddBinding(
            0,
            RHIBindingType::AccelerationStructure,
            RHIShaderStage::RayGeneration);
        EXPECT_FALSE(ValidateRHIDescriptorSetLayoutCapabilities(
            accelerationStructureLayout,
            capabilities));
    }

    TEST(RHIContractValidation, PrimaryBackendsConsumeTheSharedDescriptorSnapshotContract)
    {
        const std::string descriptorHeader = ReadSource("RHI/Include/RHI/RHIDescriptor.h");
        const std::string dx12Pipeline = ReadSource("RHI_DX12/Private/DX12Pipeline.cpp");
        const std::string vulkanPipeline = ReadSource("RHI_Vulkan/Private/VulkanPipeline.cpp");
        const std::string metalResources = ReadSource("RHI_Metal/Private/MetalResources.mm");
        const std::string metalCommands = ReadSource("RHI_Metal/Private/MetalCommandContext.mm");

        EXPECT_NE(descriptorHeader.find("RHIResourceDataVolatility"), std::string::npos);
        EXPECT_NE(descriptorHeader.find("IncompleteSnapshot"), std::string::npos);
        EXPECT_NE(dx12Pipeline.find("D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE"), std::string::npos);
        EXPECT_NE(dx12Pipeline.find("ToDX12DescriptorRangeDataFlags"), std::string::npos);
        EXPECT_NE(vulkanPipeline.find("GetDescriptorSnapshot()"), std::string::npos);
        EXPECT_NE(vulkanPipeline.find("InitializeNativeSnapshot()"), std::string::npos);
        EXPECT_NE(metalResources.find("snapshotBinding.arrayElement"), std::string::npos);
        EXPECT_NE(metalCommands.find("binding.binding + binding.arrayElement"), std::string::npos);
    }

    TEST(RHIContractValidation, ScopedAccessSeparatesMemoryLayoutDomainAndDiscard)
    {
        const RHIAccessSnapshot write = MakeRHIAccessSnapshot(
            RHIResourceState::UnorderedAccess,
            RHIShaderStage::Compute,
            GPUQueueDomain::Compute,
            RHIContentValidity::Valid);
        RHIAccessSnapshot read = write;
        read.memoryAccess = RHIMemoryAccess::ShaderRead;

        const RHIDependencyKind dependency = ClassifyRHIDependency(write, read);
        EXPECT_TRUE(HasDependencyKind(dependency, RHIDependencyKind::Memory));
        EXPECT_FALSE(HasDependencyKind(dependency, RHIDependencyKind::Transition));
        EXPECT_FALSE(HasDependencyKind(dependency, RHIDependencyKind::Ownership));
        EXPECT_EQ(ProjectRHIResourceState(write), RHIResourceState::UnorderedAccess);
        EXPECT_EQ(ProjectRHIResourceState(read), RHIResourceState::UnorderedAccess);
        EXPECT_TRUE(HasDependencyKind(
            ClassifyRHIDependency(read, write),
            RHIDependencyKind::Memory));

        RHIAccessSnapshot graphicsRead = read;
        graphicsRead.domain = GPUQueueDomain::Graphics;
        const RHIDependencyKind ownership = ClassifyRHIDependency(
            read,
            graphicsRead,
            RHIDiscardIntent::Discard);
        EXPECT_TRUE(HasDependencyKind(ownership, RHIDependencyKind::Ownership));
        EXPECT_TRUE(HasDependencyKind(ownership, RHIDependencyKind::Discard));

        RHIBufferBarrier barrier = MakeRHIBufferBarrier(
            nullptr,
            read,
            graphicsRead,
            64,
            128,
            RHIDiscardIntent::Discard);
        EXPECT_EQ(barrier.accessBefore, read);
        EXPECT_EQ(barrier.discardIntent, RHIDiscardIntent::Discard);
        EXPECT_NE(DescribeRHIAccessSnapshot(barrier.accessBefore).find("domain=Compute"),
                  std::string::npos);
    }

    TEST(RHIContractValidation, PrimaryBackendsTranslateScopedDependenciesStructurally)
    {
        const std::string accessHeader = ReadSource("RHI/Include/RHI/RHIAccess.h");
        const std::string dx12 = ReadSource("RHI_DX12/Private/DX12CommandContext.cpp");
        const std::string vulkanCommon = ReadSource("RHI_Vulkan/Private/VulkanCommon.h");
        const std::string vulkan = ReadSource("RHI_Vulkan/Private/VulkanCommandContext.cpp");
        const std::string metal = ReadSource("RHI_Metal/Private/MetalCommandContext.mm");

        EXPECT_NE(accessHeader.find("struct RHIAccessSnapshot"), std::string::npos);
        EXPECT_NE(accessHeader.find("RHIContentValidity"), std::string::npos);
        EXPECT_NE(accessHeader.find("RHIDiscardIntent"), std::string::npos);
        EXPECT_NE(dx12.find("D3D12_RESOURCE_BARRIER_TYPE_UAV"), std::string::npos);
        EXPECT_NE(dx12.find("RHIDependencyKind::Memory"), std::string::npos);
        EXPECT_NE(vulkanCommon.find("ToVkPipelineStageFlags2(RHIExecutionScope"), std::string::npos);
        EXPECT_NE(vulkanCommon.find("ToVkAccessFlags2(RHIMemoryAccess"), std::string::npos);
        EXPECT_NE(vulkan.find("GetQueueFamilyIndex(device, before.domain)"), std::string::npos);
        EXPECT_NE(vulkan.find("GetQueueFamilyIndex(device, after.domain)"), std::string::npos);
        EXPECT_NE(vulkan.find("requires paired release/acquire barriers"), std::string::npos);
        EXPECT_NE(vulkan.find("VK_QUEUE_FAMILY_IGNORED"), std::string::npos);
        EXPECT_NE(metal.find("barrier.accessBefore.executionScope"), std::string::npos);
        EXPECT_NE(metal.find("RequiresMetalBarrier"), std::string::npos);
    }

    TEST(RHIContractValidation, LifetimeOwnersCommitScopedSnapshots)
    {
        const std::string poolHeader = ReadSource(
            "Render/Include/Render/Graph/TransientResourcePool.h");
        const std::string poolSource = ReadSource(
            "Render/Private/Graph/TransientResourcePool.cpp");
        const std::string sceneRenderer = ReadSource(
            "Render/Private/Renderer/SceneRenderer.cpp");
        const std::string uploadTypes = ReadSource(
            "Render/Include/Render/GPUUploadTypes.h");
        const std::string uploadProcessor = ReadSource(
            "Render/Private/Resources/RenderUploadProcessor.cpp");
        const std::string resourceRegistry = ReadSource(
            "Render/Private/Resources/RenderResourceRegistry.h");

        EXPECT_NE(poolHeader.find("TransientTextureLease"), std::string::npos);
        EXPECT_NE(poolHeader.find("RHITextureAccessSnapshot accessSnapshot"), std::string::npos);
        EXPECT_NE(poolSource.find("pooled.accessSnapshot = finalAccess"), std::string::npos);
        EXPECT_NE(sceneRenderer.find("owner->GetAccessSnapshots()"), std::string::npos);
        EXPECT_NE(sceneRenderer.find("m_depthGPUCullingGraphHandles"), std::string::npos);
        EXPECT_NE(sceneRenderer.find("m_opaqueGPUCullingGraphHandles"), std::string::npos);
        EXPECT_NE(sceneRenderer.find("CommitGPUDrivenAccessSnapshots()"), std::string::npos);
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

    TEST(RHIContractValidation, DeviceRuntimeFaultContractIsOwnedAndStable)
    {
        RHIDeviceFault fault;
        EXPECT_EQ(fault.status, RHIDeviceRuntimeStatus::Ready);
        EXPECT_EQ(fault.operation, RHIDeviceFaultOperation::None);
        EXPECT_FALSE(fault.IsFailure());
        EXPECT_FALSE(fault.IsDeviceLost());

        fault.status = RHIDeviceRuntimeStatus::DeviceLost;
        fault.operation = RHIDeviceFaultOperation::Present;
        fault.backend = RHIBackendType::Vulkan;
        fault.nativeError = 17U;
        fault.sequence = 3U;
        fault.message = "owned fault evidence";
        EXPECT_TRUE(fault.IsFailure());
        EXPECT_TRUE(fault.IsDeviceLost());
        EXPECT_TRUE(IsDeclaredRHIDeviceRuntimeStatus(fault.status));
        EXPECT_TRUE(IsDeclaredRHIDeviceFaultOperation(fault.operation));
    }

    TEST(RHIContractValidation,
         SoftwareAdapterFallbackIsExplicitAndDefaultsOff)
    {
        const RHIDeviceDesc defaults;
        EXPECT_FALSE(defaults.allowSoftwareAdapter);

        const std::string runtimeTypes =
            ReadSource("Render/Include/Render/RenderRuntimeTypes.h");
        const std::string renderContext =
            ReadSource("Render/Private/Context/RenderContext.cpp");
        const std::string renderSubsystem =
            ReadSource("Render/Private/RenderSubsystem.cpp");
        const std::string dx12Device =
            ReadSource("RHI_DX12/Private/DX12Device.cpp");
        const std::string vulkanDevice =
            ReadSource("RHI_Vulkan/Private/VulkanDevice.cpp");

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
                      "RenderResizeResult RequestResize(const NativeSurfaceDesc& surface)"),
                  std::string::npos);
        EXPECT_NE(contextHeader.find(
                      "bool ResizeSwapChain(const NativeSurfaceDesc& surface)"),
                  std::string::npos);
        EXPECT_NE(contextHeader.find("GetSurface() const"), std::string::npos);

        const size_t classify =
            renderSubsystem.find("ClassifyNativeSurfaceUpdate(");
        const size_t resizeCase = renderSubsystem.find(
            "if (updateKind == NativeSurfaceUpdateKind::Resize)", classify);
        const size_t replaceCase = renderSubsystem.find(
            "else if (updateKind == NativeSurfaceUpdateKind::Replace", classify);
        ASSERT_NE(classify, std::string::npos);
        ASSERT_NE(resizeCase, std::string::npos);
        ASSERT_NE(replaceCase, std::string::npos);
        EXPECT_NE(renderSubsystem.find("m_context->ResizeSwapChain(surface)", resizeCase),
                  std::string::npos);
        EXPECT_NE(renderSubsystem.find("m_context->CreateSwapChain(surface)", replaceCase),
                  std::string::npos);
        EXPECT_NE(renderSubsystem.find("Surface update could not be applied", classify),
                  std::string::npos);

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
            ReadSource("Render/Include/Render/RenderSubsystem.h");
        const std::string renderSubsystem =
            ReadSource("Render/Private/RenderSubsystem.cpp");

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
        const std::string renderComposition =
            ReadSource("Engine/Private/RenderRuntimeComposition.cpp");
        const std::string showcase =
            ReadSource("Samples/Showcase/RenderingShowcase/main.cpp");

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
            engine.find("CreateEngineRenderRuntimeCompositionServices(");
        const size_t initialize =
            engine.find("return m_subsystems.InitializeAll(");
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
            "NativeSurfaceDesc surface = m_services->CaptureRenderSurface()");
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

    TEST(RHIContractValidation, OpenGLSurfaceRebindIsRejectedBeforeActiveSwapChainMutation)
    {
        const std::string deviceInterface =
            ReadSource("RHI/Include/RHI/RHIDevice.h");
        const std::string openGLHeader =
            ReadSource("RHI_OpenGL/Private/OpenGLDevice.h");
        const std::string openGLDevice =
            ReadSource("RHI_OpenGL/Private/OpenGLDevice.cpp");
        const std::string renderSubsystem =
            ReadSource("Render/Private/RenderSubsystem.cpp");
        const std::string renderContext =
            ReadSource("Render/Private/Context/RenderContext.cpp");

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
            "else if (updateKind == NativeSurfaceUpdateKind::Replace");
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
            ReadSource("RHI_OpenGL/Private/OpenGLDevice.h");
        const std::string source =
            ReadSource("RHI_OpenGL/Private/OpenGLDevice.cpp");

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

    TEST(RHIContractValidation, SurfaceRebindPolicyDoesNotDisableVulkanReplacement)
    {
        const FakeRHIDevice vulkanDevice(
            MakeValidCapabilities(RHIBackendType::Vulkan));
        NativeSurfaceDesc current = MakeSurface(NativeSurfacePlatform::Win32);
        NativeSurfaceDesc replacement = current;
        replacement.backendWindow = 0x5000;
        replacement.generation++;

        EXPECT_TRUE(vulkanDevice.SupportsSurfaceRebind(current, replacement));

        const std::string vulkanHeader =
            ReadSource("RHI_Vulkan/Private/VulkanDevice.h");
        const std::string openGLSwapChain =
            ReadSource("RHI_OpenGL/Private/OpenGLSwapChain.cpp");
        EXPECT_EQ(vulkanHeader.find("SupportsSurfaceRebind"),
                  std::string::npos);
        EXPECT_EQ(openGLSwapChain.find("glfwMakeContextCurrent"),
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

        capabilities = MakeValidCapabilities(RHIBackendType::DX11);
        capabilities.supportsExplicitResourceBarriers = false;
        capabilities.supportsExplicitAliasingBarriers = true;

        result = ValidateRHICapabilities(capabilities);

        EXPECT_FALSE(result);
        EXPECT_NE(result.message.find("aliasing barriers"), std::string::npos);
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
        capabilities.queueTopology.logicalQueueDomains[1] = GPUQueueDomain::Compute;
        capabilities.queueTopology.activeDomainCount = 2;
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
        EXPECT_NE(text.find("Schema: 4"), std::string::npos);
        EXPECT_NE(text.find("Backend: OpenGL"), std::string::npos);
        EXPECT_NE(text.find("Adapter: OpenGL Test Adapter"), std::string::npos);
        EXPECT_NE(text.find("DriverVersion: TestDriver.1"), std::string::npos);
        EXPECT_NE(text.find("Validation: Passed"), std::string::npos);
        EXPECT_NE(text.find("QueueCompletionMode: CompatibilityWaitIdle"), std::string::npos);
        EXPECT_NE(text.find("QueueDomains: Graphics=Graphics, Compute=Graphics, Copy=Graphics, Active=1"),
                  std::string::npos);
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

        EXPECT_NE(json.find("\"schemaVersion\": 4"), std::string::npos);
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
        EXPECT_NE(json.find("\"completionMode\": \"CompatibilityWaitIdle\""), std::string::npos);
        EXPECT_NE(json.find("\"logicalQueueDomains\": [\"Graphics\", \"Graphics\", \"Graphics\"]"),
                  std::string::npos);
        EXPECT_NE(json.find("\"renderGraphBaseline\": {"), std::string::npos);
        EXPECT_NE(json.find("\"supported\": true"), std::string::npos);
        EXPECT_NE(json.find("\"missingRequirements\": []"), std::string::npos);
        EXPECT_NE(json.find("\"feature\": \"ExplicitResourceBarriers\""), std::string::npos);
        EXPECT_NE(json.find("\"status\": \"Emulated\""), std::string::npos);
    }

    TEST(RHIContractValidation, QueueTopologyPublicationStaysBackendHonest)
    {
        const std::string dx12 = ReadSource("RHI_DX12/Private/DX12Device.cpp");
        EXPECT_NE(dx12.find("RHIQueueCompletionMode::NativeTimeline"), std::string::npos);
        EXPECT_NE(dx12.find("GPUQueueDomain::Compute"), std::string::npos);
        EXPECT_NE(dx12.find("GPUQueueDomain::Copy"), std::string::npos);

        const std::string vulkan = ReadSource("RHI_Vulkan/Private/VulkanDevice.cpp");
        EXPECT_NE(vulkan.find("leftFamily == rightFamily && leftQueue == rightQueue"),
                  std::string::npos);
        EXPECT_NE(vulkan.find("m_capabilities.supportsAsyncCompute = computeDomain != GPUQueueDomain::Graphics"),
                  std::string::npos);

        const std::string metal = ReadSource("RHI_Metal/Private/MetalDevice.mm");
        EXPECT_NE(metal.find("m_capabilities.supportsAsyncCompute = false"), std::string::npos);
        EXPECT_NE(metal.find("RHIQueueCompletionMode::NativeTimeline"), std::string::npos);
        EXPECT_NE(metal.find("m_capabilities.queueTopology.activeDomainCount = 1"),
                  std::string::npos);
        EXPECT_EQ(metal.find("windows.h"), std::string::npos);
        EXPECT_EQ(metal.find("d3d12.h"), std::string::npos);

        const std::string metalCommandContext =
            ReadSource("RHI_Metal/Private/MetalCommandContext.mm");
        const std::string metalSynchronization =
            ReadSource("RHI_Metal/Private/MetalSynchronization.mm");
        EXPECT_NE(metalCommandContext.find("SignalFromCommandBuffer(m_commandBuffer)"),
                  std::string::npos);
        EXPECT_NE(metalSynchronization.find("encodeSignalEvent:m_event value:newValue"),
                  std::string::npos);
        EXPECT_NE(metalSynchronization.find("return m_event.signaledValue"),
                  std::string::npos);

        for (const char* path : {
                 "RHI_DX11/Private/DX11Device.cpp",
                 "RHI_OpenGL/Private/OpenGLDevice.cpp"})
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
                 "RHI_DX11/Private/DX11CommandContext.h",
                 "RHI_DX12/Private/DX12CommandContext.h",
                 "RHI_Vulkan/Private/VulkanCommandContext.h",
                 "RHI_Metal/Private/MetalCommandContext.h",
                 "RHI_OpenGL/Private/OpenGLCommandContext.h"})
        {
            const std::string context = ReadSource(path);
            EXPECT_NE(context.find("RHICommandQueueType GetQueueType() const override"),
                      std::string::npos) << path;
        }
    }

    TEST(RHIContractValidation, RenderSubmissionTrackerRemainsPrivateAndBackendAgnostic)
    {
        const std::string frameSynchronizer =
            ReadSource("Render/Include/Render/Context/FrameSynchronizer.h");
        EXPECT_EQ(frameSynchronizer.find("RHIFence"), std::string::npos);
        EXPECT_EQ(frameSynchronizer.find("GetFrameFenceValue"), std::string::npos);
        EXPECT_NE(frameSynchronizer.find("GPUCompletionPoint"), std::string::npos);
        EXPECT_NE(frameSynchronizer.find("std::unique_ptr<RenderSubmissionTracker>"),
                  std::string::npos);

        const std::string renderContext =
            ReadSource("Render/Include/Render/Context/RenderContext.h");
        EXPECT_NE(renderContext.find("GPUCompletionPoint EndFrame();"), std::string::npos);
        EXPECT_EQ(renderContext.find("RenderSubmissionTracker"), std::string::npos);
        EXPECT_EQ(renderContext.find("m_computeFences"), std::string::npos);
        EXPECT_EQ(renderContext.find("m_computeFenceValues"), std::string::npos);

        const std::string renderContextSource =
            ReadSource("Render/Private/Context/RenderContext.cpp");
        EXPECT_NE(renderContextSource.find(
                      "submittedPoint = m_frameSynchronizer.SubmitGraphics(ctx)"),
                  std::string::npos);
        EXPECT_NE(renderContextSource.find(
                      "m_frameSynchronizer.SignalFrame(m_frameIndex, submittedPoint)"),
                  std::string::npos);
        EXPECT_NE(renderContextSource.find("return submittedPoint"), std::string::npos);

        const std::string frameSynchronizerSource =
            ReadSource("Render/Private/Context/FrameSynchronizer.cpp");
        EXPECT_EQ(frameSynchronizerSource.find("CreateFence("), std::string::npos);

        const std::string renderCmake = ReadSource("Render/CMakeLists.txt");
        EXPECT_NE(renderCmake.find("Private/Resources/RenderSubmissionTracker.cpp"),
                  std::string::npos);
        EXPECT_NE(renderCmake.find("RVX::RHI"), std::string::npos);
        EXPECT_EQ(renderCmake.find("RHI_DX12"), std::string::npos);
        EXPECT_EQ(renderCmake.find("RHI_Vulkan"), std::string::npos);
        EXPECT_EQ(renderCmake.find("RHI_Metal"), std::string::npos);
    }

    TEST(RHIContractValidation, OptimizedClearIsOptionalTypedAndPartOfTextureIdentity)
    {
        RHITextureDesc absent = RHITextureDesc::RenderTarget(
            64, 64, RHIFormat::RGBA16_FLOAT);
        EXPECT_TRUE(IsRHIOptimizedClearValueCompatible(absent));
        EXPECT_FALSE(absent.optimizedClearValue.IsPresent());

        RHITextureDesc color = absent;
        color.SetOptimizedClearColor({0.1f, 0.2f, 0.3f, 1.0f});
        EXPECT_TRUE(IsRHIOptimizedClearValueCompatible(color));
        EXPECT_FALSE(AreRHITextureDescsEquivalent(absent, color));

        RHITextureDesc sameColor = color;
        EXPECT_TRUE(AreRHITextureDescsEquivalent(color, sameColor));
        sameColor.optimizedClearValue.color.b = 0.4f;
        EXPECT_FALSE(AreRHITextureDescsEquivalent(color, sameColor));

        RHITextureDesc depth = RHITextureDesc::DepthStencil(
            64, 64, RHIFormat::D32_FLOAT);
        depth.SetOptimizedClearDepthStencil({0.0f, 0});
        EXPECT_TRUE(IsRHIOptimizedClearValueCompatible(depth));

        depth.SetOptimizedClearColor({0.0f, 0.0f, 0.0f, 1.0f});
        EXPECT_FALSE(IsRHIOptimizedClearValueCompatible(depth));

        color.SetOptimizedClearDepthStencil({1.0f, 0});
        EXPECT_FALSE(IsRHIOptimizedClearValueCompatible(color));
    }

} // namespace RVX::Tests
