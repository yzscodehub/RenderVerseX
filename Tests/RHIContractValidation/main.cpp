#include "RHI/RHICapabilities.h"
#include "RHI/RHICommandContext.h"
#include "RHI/RHIDevice.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

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
    } // namespace

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
