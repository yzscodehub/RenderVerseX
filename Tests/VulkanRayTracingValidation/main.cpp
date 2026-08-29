#include "Common/GpuTestUtils.h"
#include "RHI/RHI.h"
#include "ShaderCompiler/ShaderReflection.h"
#include "ShaderContracts/ShaderArtifact.h"
#include "ShaderRuntime/CookedShaderProgramProvider.h"
#include "VulkanCommandContext.h"
#include "VulkanDevice.h"
#include "VulkanPipeline.h"
#include "VulkanResources.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace RVX;

namespace
{
    struct ShaderFixture
    {
        RHIShaderStage stage;
        const char* entryPoint;
    };

    constexpr std::array<ShaderFixture, 6> kShaderFixtures = {{
        {RHIShaderStage::RayGeneration, "RayGenerationMain"},
        {RHIShaderStage::Miss, "MissMain"},
        {RHIShaderStage::ClosestHit, "ClosestHitMain"},
        {RHIShaderStage::AnyHit, "AnyHitMain"},
        {RHIShaderStage::Intersection, "IntersectionMain"},
        {RHIShaderStage::Callable, "CallableMain"},
    }};

    constexpr const char* kLogicalAssetId = "VulkanRayTracingFixture.hlsl";
    constexpr uint32 kHitSentinel = 0x0C105E57u;

    uint64 AlignUp(uint64 value, uint64 alignment)
    {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    std::string ReadEnvironment(const char* name)
    {
        const char* value = std::getenv(name);
        return value ? value : "";
    }

    std::filesystem::path ShaderArtifactRoot()
    {
        return ReadEnvironment("RVX_GFX20_VULKAN_RT_SHADER_ROOT");
    }

    ShaderProgramRequest MakeRequest(const ShaderFixture& fixture)
    {
        ShaderProgramRequest request;
        request.logicalAssetId = kLogicalAssetId;
        request.stage = fixture.stage;
        request.entryPoint = fixture.entryPoint;
        request.backend = RHIBackendType::Vulkan;
        return request;
    }

    bool HasResourceBinding(
        const ShaderReflection& reflection,
        uint32 binding,
        RHIBindingType type)
    {
        return std::any_of(
            reflection.resources.begin(),
            reflection.resources.end(),
            [binding, type](const ShaderReflection::ResourceBinding& resource)
            {
                return resource.set == 0 && resource.binding == binding &&
                       resource.type == type;
            });
    }

    bool ContainsSpirvLiteral(
        const std::vector<uint8>& code,
        std::string_view literal)
    {
        return std::search(
                   code.begin(),
                   code.end(),
                   literal.begin(),
                   literal.end()) != code.end();
    }

    bool HasDeviceExtension(
        VkPhysicalDevice physicalDevice,
        const char* extensionName)
    {
        uint32 extensionCount = 0;
        if (vkEnumerateDeviceExtensionProperties(
                physicalDevice, nullptr, &extensionCount, nullptr) !=
            VK_SUCCESS)
        {
            return false;
        }
        std::vector<VkExtensionProperties> extensions(extensionCount);
        if (vkEnumerateDeviceExtensionProperties(
                physicalDevice, nullptr, &extensionCount, extensions.data()) !=
            VK_SUCCESS)
        {
            return false;
        }
        return std::any_of(
            extensions.begin(), extensions.end(),
            [extensionName](const VkExtensionProperties& extension)
            {
                return std::strcmp(extension.extensionName, extensionName) == 0;
            });
    }

    std::string EscapeJson(std::string_view value)
    {
        std::string escaped;
        escaped.reserve(value.size());
        for (const char character : value)
        {
            switch (character)
            {
                case '\\': escaped += "\\\\"; break;
                case '"': escaped += "\\\""; break;
                case '\n': escaped += "\\n"; break;
                case '\r': escaped += "\\r"; break;
                case '\t': escaped += "\\t"; break;
                default: escaped += character; break;
            }
        }
        return escaped;
    }

    void WriteNativeEvidence(
        const VulkanDevice& device,
        const VulkanRayTracingDispatchRegions& regions,
        const VulkanValidationMessageCounts& before,
        const VulkanValidationMessageCounts& after,
        uint32 observedValue)
    {
        const std::filesystem::path evidencePath =
            ReadEnvironment("RVX_GFX20_VULKAN_RT_EVIDENCE");
        ASSERT_FALSE(evidencePath.empty());

        const std::string runId = ReadEnvironment("RVX_VALIDATION_RUN_ID");
        const std::string sourceCommit =
            ReadEnvironment("RVX_VALIDATION_SOURCE_COMMIT");
        const std::string preset = ReadEnvironment("RVX_VALIDATION_PRESET");
        if (!runId.empty())
        {
            ASSERT_FALSE(sourceCommit.empty());
            ASSERT_FALSE(preset.empty());
        }

        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device.GetPhysicalDevice(), &properties);
        const auto& accelerationStructure =
            device.GetAccelerationStructureProperties();
        const auto& rayTracing = device.GetRayTracingPipelineProperties();

        std::error_code directoryError;
        std::filesystem::create_directories(
            evidencePath.parent_path(), directoryError);
        ASSERT_FALSE(directoryError) << directoryError.message();
        std::ofstream file(evidencePath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file.is_open());
        file << "{\n"
             << "  \"schema\": \"RVX.VulkanRayTracingEvidence\",\n"
             << "  \"schemaVersion\": 1,\n"
             << "  \"runId\": \"" << EscapeJson(runId.empty() ? "standalone" : runId) << "\",\n"
             << "  \"sourceCommit\": \"" << EscapeJson(sourceCommit) << "\",\n"
             << "  \"preset\": \"" << EscapeJson(preset) << "\",\n"
             << "  \"adapter\": \"" << EscapeJson(properties.deviceName) << "\",\n"
             << "  \"driverVersion\": " << properties.driverVersion << ",\n"
             << "  \"extensions\": {\n"
             << "    \"VK_KHR_acceleration_structure\": true,\n"
             << "    \"VK_KHR_deferred_host_operations\": true,\n"
             << "    \"VK_KHR_ray_tracing_pipeline\": true\n"
             << "  },\n"
             << "  \"features\": {\n"
             << "    \"bufferDeviceAddress\": true,\n"
             << "    \"accelerationStructure\": true,\n"
             << "    \"rayTracingPipeline\": true\n"
             << "  },\n"
             << "  \"limits\": {\n"
             << "    \"minAccelerationStructureScratchOffsetAlignment\": "
             << accelerationStructure.minAccelerationStructureScratchOffsetAlignment << ",\n"
             << "    \"shaderGroupHandleSize\": "
             << rayTracing.shaderGroupHandleSize << ",\n"
             << "    \"shaderGroupHandleAlignment\": "
             << rayTracing.shaderGroupHandleAlignment << ",\n"
             << "    \"shaderGroupBaseAlignment\": "
             << rayTracing.shaderGroupBaseAlignment << ",\n"
             << "    \"maxRayRecursionDepth\": "
             << rayTracing.maxRayRecursionDepth << "\n"
             << "  },\n"
             << "  \"blasBuilt\": true,\n"
             << "  \"tlasBuilt\": true,\n"
             << "  \"pipelineShaderGroupCount\": 3,\n"
             << "  \"shaderBindingTable\": {\n"
             << "    \"rayGenerationAddress\": " << regions.rayGeneration.deviceAddress << ",\n"
             << "    \"rayGenerationStride\": " << regions.rayGeneration.stride << ",\n"
             << "    \"missAddress\": " << regions.miss.deviceAddress << ",\n"
             << "    \"missStride\": " << regions.miss.stride << ",\n"
             << "    \"hitGroupAddress\": " << regions.hitGroup.deviceAddress << ",\n"
             << "    \"hitGroupStride\": " << regions.hitGroup.stride << "\n"
             << "  },\n"
             << "  \"dispatch\": {\"width\": 1, \"height\": 1, \"depth\": 1},\n"
             << "  \"readback\": " << observedValue << ",\n"
             << "  \"validationErrorsBefore\": " << before.errors << ",\n"
             << "  \"validationErrorsAfter\": " << after.errors << ",\n"
             << "  \"validationWarningsBefore\": " << before.warnings << ",\n"
             << "  \"validationWarningsAfter\": " << after.warnings << "\n"
             << "}\n";
        ASSERT_TRUE(file.good());
    }

    RHIShaderRef LoadCookedShader(
        IShaderProgramProvider& provider,
        IRHIDevice& device,
        const ShaderFixture& fixture)
    {
        ShaderProgramResult result = provider.Load(device, MakeRequest(fixture));
        EXPECT_TRUE(result.Succeeded()) << result.diagnostic;
        return result.shader;
    }
} // namespace

TEST(VulkanRayTracingValidation, CookedArtifactsCoverAllSixStages)
{
    const std::filesystem::path artifactRoot = ShaderArtifactRoot();
    ASSERT_FALSE(artifactRoot.empty());

    for (const ShaderFixture& fixture : kShaderFixtures)
    {
        const ShaderProgramRequest request = MakeRequest(fixture);
        const std::filesystem::path artifactPath =
            artifactRoot / BuildShaderArtifactRelativePath(request);
        ShaderArtifact artifact;
        std::string error;
        ASSERT_TRUE(ReadShaderArtifactV2(artifactPath, artifact, error))
            << artifactPath << ": " << error;
        EXPECT_EQ(artifact.request.logicalAssetId, kLogicalAssetId);
        EXPECT_EQ(artifact.request.stage, fixture.stage);
        EXPECT_EQ(artifact.request.entryPoint, fixture.entryPoint);
        EXPECT_EQ(artifact.request.backend, RHIBackendType::Vulkan);
        EXPECT_EQ(artifact.targetProfile, "lib_6_3");
        EXPECT_EQ(artifact.runtimeEntryPoint, fixture.entryPoint);
        EXPECT_EQ(artifact.compilerAbi, RVX_SHADER_COMPILER_ABI_VERSION);
        ASSERT_GE(artifact.code.size(), sizeof(uint32));
        uint32 magic = 0;
        std::memcpy(&magic, artifact.code.data(), sizeof(magic));
        EXPECT_EQ(magic, 0x07230203u);
        EXPECT_TRUE(ContainsSpirvLiteral(
            artifact.code, "SPV_KHR_ray_tracing"));
        EXPECT_FALSE(ContainsSpirvLiteral(
            artifact.code, "SPV_KHR_ray_query"))
            << "Pipeline shaders must not force the runtime to enable the "
               "unimplemented inline ray-query feature";

        const ShaderReflection strictReflection = ReflectShader(
            RHIBackendType::Vulkan,
            fixture.stage,
            artifact.code,
            fixture.entryPoint);
        EXPECT_TRUE(strictReflection.valid)
            << "SPIR-V entry point/stage mismatch for " << fixture.entryPoint;
        if (fixture.stage == RHIShaderStage::RayGeneration)
        {
            EXPECT_TRUE(HasResourceBinding(
                strictReflection, 0, RHIBindingType::AccelerationStructure));
            EXPECT_TRUE(HasResourceBinding(
                strictReflection, 1, RHIBindingType::StorageBuffer));
        }
    }
}

TEST(VulkanRayTracingValidation, NativeDispatchProducesDeterministicEvidence)
{
    RHIDeviceDesc deviceDesc;
    deviceDesc.enableDebugLayer = true;
    deviceDesc.allowSoftwareAdapter = false;
    std::unique_ptr<IRHIDevice> device =
        RVX::Test::CreateNativeValidationDevice(
            RHIBackendType::Vulkan, deviceDesc);
    ASSERT_NE(device, nullptr)
        << "The required GFX20 suite needs a Vulkan device";
    ASSERT_TRUE(RVX::Test::IsHardwareGpuDevice(device))
        << "The required GFX20 suite forbids software adapters";

    auto* vulkanDevice = dynamic_cast<VulkanDevice*>(device.get());
    ASSERT_NE(vulkanDevice, nullptr);
    const RHICapabilities& capabilities = device->GetCapabilities();
    ASSERT_TRUE(capabilities.supportsRaytracing);
    ASSERT_TRUE(capabilities.supportsRaytracingPipeline);
    ASSERT_TRUE(capabilities.supportsAccelerationStructureUpdate);
    EXPECT_TRUE(capabilities.supportsAccelerationStructureCompaction);
    EXPECT_FALSE(capabilities.supportsRayQuery);
    ASSERT_TRUE(vulkanDevice->IsRayTracingEnabled());
    ASSERT_TRUE(vulkanDevice->GetRayTracingFunctions().IsComplete());
    ASSERT_TRUE(HasDeviceExtension(
        vulkanDevice->GetPhysicalDevice(),
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME));
    ASSERT_TRUE(HasDeviceExtension(
        vulkanDevice->GetPhysicalDevice(),
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME));
    ASSERT_TRUE(HasDeviceExtension(
        vulkanDevice->GetPhysicalDevice(),
        VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME));

    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayTracingFeatures = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationStructureFeatures = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceBufferDeviceAddressFeatures bufferDeviceAddressFeatures = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
    VkPhysicalDeviceFeatures2 features = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &bufferDeviceAddressFeatures;
    bufferDeviceAddressFeatures.pNext = &accelerationStructureFeatures;
    accelerationStructureFeatures.pNext = &rayTracingFeatures;
    vkGetPhysicalDeviceFeatures2(vulkanDevice->GetPhysicalDevice(), &features);
    ASSERT_EQ(bufferDeviceAddressFeatures.bufferDeviceAddress, VK_TRUE);
    ASSERT_EQ(accelerationStructureFeatures.accelerationStructure, VK_TRUE);
    ASSERT_EQ(rayTracingFeatures.rayTracingPipeline, VK_TRUE);

    ShaderProgramProviderFactory providerFactory =
        CreateCookedShaderProgramProviderFactory(ShaderArtifactRoot());
    std::unique_ptr<IShaderProgramProvider> provider = providerFactory();
    ASSERT_NE(provider, nullptr);
    RHIShaderRef rayGeneration = LoadCookedShader(
        *provider, *device, kShaderFixtures[0]);
    RHIShaderRef miss = LoadCookedShader(
        *provider, *device, kShaderFixtures[1]);
    RHIShaderRef closestHit = LoadCookedShader(
        *provider, *device, kShaderFixtures[2]);
    RHIShaderRef anyHit = LoadCookedShader(
        *provider, *device, kShaderFixtures[3]);
    ASSERT_NE(rayGeneration.Get(), nullptr);
    ASSERT_NE(miss.Get(), nullptr);
    ASSERT_NE(closestHit.Get(), nullptr);
    ASSERT_NE(anyHit.Get(), nullptr);

    RHIDescriptorSetLayoutDesc setLayoutDesc;
    setLayoutDesc.AddBinding(
        0, RHIBindingType::AccelerationStructure,
        RHIShaderStage::AllRayTracing);
    setLayoutDesc.AddBinding(
        1, RHIBindingType::StorageBuffer,
        RHIShaderStage::AllRayTracing);
    setLayoutDesc.debugName = "GFX20VulkanRayTracingSetLayout";
    RHIDescriptorSetLayoutRef setLayout =
        device->CreateDescriptorSetLayout(setLayoutDesc);
    ASSERT_NE(setLayout.Get(), nullptr);

    RHIPipelineLayoutDesc pipelineLayoutDesc;
    pipelineLayoutDesc.setLayouts.push_back(setLayout.Get());
    pipelineLayoutDesc.debugName = "GFX20VulkanRayTracingPipelineLayout";
    RHIPipelineLayoutRef pipelineLayout =
        device->CreatePipelineLayout(pipelineLayoutDesc);
    ASSERT_NE(pipelineLayout.Get(), nullptr);

    RHIRayTracingShaderGroupDesc rayGenerationGroup;
    rayGenerationGroup.type = RHIRayTracingShaderGroupType::General;
    rayGenerationGroup.exportName = "RayGenerationMain";
    rayGenerationGroup.generalShader = rayGeneration.Get();
    RHIRayTracingShaderGroupDesc missGroup;
    missGroup.type = RHIRayTracingShaderGroupType::General;
    missGroup.exportName = "MissMain";
    missGroup.generalShader = miss.Get();
    RHIRayTracingShaderGroupDesc hitGroup;
    hitGroup.type = RHIRayTracingShaderGroupType::TrianglesHitGroup;
    hitGroup.exportName = "PrimaryHitGroup";
    hitGroup.closestHitShader = closestHit.Get();
    hitGroup.anyHitShader = anyHit.Get();

    RHIRayTracingPipelineDesc pipelineDesc;
    pipelineDesc.pipelineLayout = pipelineLayout.Get();
    pipelineDesc.shaderGroups = {
        rayGenerationGroup, missGroup, hitGroup};
    pipelineDesc.maxRecursionDepth = 1;
    pipelineDesc.maxPayloadSize = sizeof(uint32);
    pipelineDesc.maxAttributeSize = sizeof(float) * 2;
    pipelineDesc.debugName = "GFX20VulkanRayTracingPipeline";
    RHIPipelineRef pipeline =
        device->CreateRayTracingPipeline(pipelineDesc);
    ASSERT_NE(pipeline.Get(), nullptr);
    ASSERT_TRUE(pipeline->IsRayTracing());
    ASSERT_EQ(pipeline->GetRayTracingShaderGroupCount(), 3u);

    RHIShaderTableDesc shaderTableDesc;
    shaderTableDesc.rayTracingPipelineOwner = pipeline;
    shaderTableDesc.rayGenerationRecords.push_back({0});
    shaderTableDesc.missRecords.push_back({1});
    shaderTableDesc.hitGroupRecords.push_back({2});
    shaderTableDesc.debugName = "GFX20VulkanRayTracingShaderTable";
    RHIShaderTableRef shaderTable =
        device->CreateShaderTable(shaderTableDesc);
    ASSERT_NE(shaderTable.Get(), nullptr);
    auto* vulkanShaderTable =
        dynamic_cast<VulkanShaderTable*>(shaderTable.Get());
    ASSERT_NE(vulkanShaderTable, nullptr);
    const VulkanRayTracingDispatchRegions dispatchRegions =
        vulkanShaderTable->BuildDispatchRaysRegions(1, 1, 1);
    ASSERT_NE(dispatchRegions.rayGeneration.deviceAddress, 0u);
    ASSERT_NE(dispatchRegions.miss.deviceAddress, 0u);
    ASSERT_NE(dispatchRegions.hitGroup.deviceAddress, 0u);

    constexpr float vertices[] = {
        -0.5f, -0.5f, 0.0f,
         0.5f, -0.5f, 0.0f,
         0.0f,  0.5f, 0.0f,
    };
    RHIBufferDesc vertexBufferDesc;
    vertexBufferDesc.size = sizeof(vertices);
    vertexBufferDesc.usage = RHIBufferUsage::Vertex |
        RHIBufferUsage::AccelerationStructureInput |
        RHIBufferUsage::DeviceAddress;
    vertexBufferDesc.memoryType = RHIMemoryType::Upload;
    vertexBufferDesc.stride = sizeof(float) * 3;
    vertexBufferDesc.debugName = "GFX20VulkanRayTracingVertices";
    RHIBufferRef vertexBuffer = device->CreateBuffer(vertexBufferDesc);
    ASSERT_NE(vertexBuffer.Get(), nullptr);
    void* vertexData = vertexBuffer->Map();
    ASSERT_NE(vertexData, nullptr);
    std::memcpy(vertexData, vertices, sizeof(vertices));
    vertexBuffer->Unmap();

    RHIRayTracingGeometryDesc geometry;
    geometry.type = RHIRayTracingGeometryType::Triangles;
    geometry.flags = RHIRayTracingGeometryFlags::Opaque;
    geometry.triangles.vertexBuffer = vertexBuffer.Get();
    geometry.triangles.vertexStride = sizeof(float) * 3;
    geometry.triangles.vertexFormat = RHIFormat::RGB32_FLOAT;
    geometry.triangles.vertexCount = 3;
    RHIBottomLevelASDesc blasDesc;
    blasDesc.geometries.push_back(geometry);
    blasDesc.buildFlags =
        RHIAccelerationStructureBuildFlags::AllowCompaction |
        RHIAccelerationStructureBuildFlags::PreferFastTrace;
    blasDesc.debugName = "GFX20VulkanRayTracingBLASBuild";
    const RHIAccelerationStructureBuildSizes blasSizes =
        device->GetBottomLevelASBuildSizes(blasDesc);
    ASSERT_TRUE(blasSizes.IsValid());

    RHIAccelerationStructureDesc blasResourceDesc;
    blasResourceDesc.type = RHIAccelerationStructureType::BottomLevel;
    blasResourceDesc.size = blasSizes.accelerationStructureSize;
    blasResourceDesc.debugName = "GFX20VulkanRayTracingBLAS";
    RHIAccelerationStructureRef blas =
        device->CreateAccelerationStructure(blasResourceDesc);
    ASSERT_NE(blas.Get(), nullptr);
    ASSERT_NE(blas->GetGPUVirtualAddress(), 0u);

    RHIBufferDesc blasScratchDesc;
    blasScratchDesc.size = blasSizes.buildScratchSize;
    blasScratchDesc.usage = RHIBufferUsage::UnorderedAccess |
        RHIBufferUsage::DeviceAddress;
    blasScratchDesc.memoryType = RHIMemoryType::Default;
    blasScratchDesc.debugName = "GFX20VulkanRayTracingBLASScratch";
    RHIBufferRef blasScratch = device->CreateBuffer(blasScratchDesc);
    ASSERT_NE(blasScratch.Get(), nullptr);

    RHIRayTracingInstanceDesc instance;
    instance.bottomLevel = blas.Get();
    instance.instanceMask = 0xFFu;
    const RHIRayTracingInstanceRecord instanceRecord =
        PackRHIRayTracingInstanceRecord(
            instance, blas->GetGPUVirtualAddress());
    RHIBufferDesc instanceBufferDesc;
    instanceBufferDesc.size = sizeof(instanceRecord);
    instanceBufferDesc.usage = RHIBufferUsage::AccelerationStructureInput |
        RHIBufferUsage::DeviceAddress;
    instanceBufferDesc.memoryType = RHIMemoryType::Upload;
    instanceBufferDesc.stride = sizeof(instanceRecord);
    instanceBufferDesc.debugName = "GFX20VulkanRayTracingInstance";
    RHIBufferRef instanceBuffer = device->CreateBuffer(instanceBufferDesc);
    ASSERT_NE(instanceBuffer.Get(), nullptr);
    void* instanceData = instanceBuffer->Map();
    ASSERT_NE(instanceData, nullptr);
    std::memcpy(instanceData, &instanceRecord, sizeof(instanceRecord));
    instanceBuffer->Unmap();

    RHITopLevelASDesc tlasDesc;
    tlasDesc.instanceBuffer = instanceBuffer.Get();
    tlasDesc.instanceCount = 1;
    tlasDesc.buildFlags = RHIAccelerationStructureBuildFlags::PreferFastTrace;
    tlasDesc.debugName = "GFX20VulkanRayTracingTLASBuild";
    const RHIAccelerationStructureBuildSizes tlasSizes =
        device->GetTopLevelASBuildSizes(tlasDesc);
    ASSERT_TRUE(tlasSizes.IsValid());
    RHIAccelerationStructureDesc tlasResourceDesc;
    tlasResourceDesc.type = RHIAccelerationStructureType::TopLevel;
    tlasResourceDesc.size = tlasSizes.accelerationStructureSize;
    tlasResourceDesc.debugName = "GFX20VulkanRayTracingTLAS";
    RHIAccelerationStructureRef tlas =
        device->CreateAccelerationStructure(tlasResourceDesc);
    ASSERT_NE(tlas.Get(), nullptr);

    RHIBufferDesc tlasScratchDesc;
    tlasScratchDesc.size = tlasSizes.buildScratchSize;
    tlasScratchDesc.usage = RHIBufferUsage::UnorderedAccess |
        RHIBufferUsage::DeviceAddress;
    tlasScratchDesc.memoryType = RHIMemoryType::Default;
    tlasScratchDesc.debugName = "GFX20VulkanRayTracingTLASScratch";
    RHIBufferRef tlasScratch = device->CreateBuffer(tlasScratchDesc);
    ASSERT_NE(tlasScratch.Get(), nullptr);

    RHIBufferDesc outputDesc;
    outputDesc.size = sizeof(uint32);
    outputDesc.usage = RHIBufferUsage::Structured |
        RHIBufferUsage::UnorderedAccess | RHIBufferUsage::CopySrc;
    outputDesc.memoryType = RHIMemoryType::Default;
    outputDesc.stride = sizeof(uint32);
    outputDesc.debugName = "GFX20VulkanRayTracingOutput";
    RHIBufferRef output = device->CreateBuffer(outputDesc);
    ASSERT_NE(output.Get(), nullptr);
    RHIBufferDesc readbackDesc;
    readbackDesc.size = sizeof(uint32);
    readbackDesc.usage = RHIBufferUsage::CopyDst;
    readbackDesc.memoryType = RHIMemoryType::Readback;
    readbackDesc.debugName = "GFX20VulkanRayTracingReadback";
    RHIBufferRef readback = device->CreateBuffer(readbackDesc);
    ASSERT_NE(readback.Get(), nullptr);

    RHIAccelerationStructureCompactedSizeQueryDesc compactionQueryDesc;
    compactionQueryDesc.count = 1;
    compactionQueryDesc.queueType = RHICommandQueueType::Graphics;
    compactionQueryDesc.debugName = "GFX21VulkanCompactedSizeQuery";
    RHIAccelerationStructureCompactedSizeQueryRef compactionQuery =
        device->CreateAccelerationStructureCompactedSizeQuery(
            compactionQueryDesc);
    ASSERT_NE(compactionQuery.Get(), nullptr);
    RHIBufferDesc compactionReadbackDesc;
    compactionReadbackDesc.size = sizeof(uint64);
    compactionReadbackDesc.usage = RHIBufferUsage::CopyDst;
    compactionReadbackDesc.memoryType = RHIMemoryType::Readback;
    compactionReadbackDesc.debugName = "GFX21VulkanCompactedSizeReadback";
    RHIBufferRef compactionReadback =
        device->CreateBuffer(compactionReadbackDesc);
    ASSERT_NE(compactionReadback.Get(), nullptr);

    RHIDescriptorSetDesc descriptorSetDesc;
    descriptorSetDesc.layout = setLayout.Get();
    descriptorSetDesc.debugName = "GFX20VulkanRayTracingDescriptorSet";
    descriptorSetDesc.BindAccelerationStructure(0, tlas.Get());
    descriptorSetDesc.BindBuffer(1, output.Get(), 0, sizeof(uint32));
    RHIDescriptorSetRef descriptorSet =
        device->CreateDescriptorSet(descriptorSetDesc);
    ASSERT_NE(descriptorSet.Get(), nullptr)
        << "Vulkan descriptor writes may reference a live TLAS before its "
           "build, while command binding remains build-state validated";

    auto* vulkanBlas = dynamic_cast<VulkanAccelerationStructure*>(blas.Get());
    auto* vulkanTlas = dynamic_cast<VulkanAccelerationStructure*>(tlas.Get());
    ASSERT_NE(vulkanBlas, nullptr);
    ASSERT_NE(vulkanTlas, nullptr);
    EXPECT_FALSE(vulkanBlas->HasSubmittedBuild());
    EXPECT_FALSE(vulkanTlas->HasSubmittedBuild());

    // Recording is not publication. Resetting a command buffer must discard
    // its local AS build state and leave the resources globally unbuilt.
    RHICommandContextRef discardedBuildContext =
        device->CreateCommandContext(RHICommandQueueType::Graphics);
    ASSERT_NE(discardedBuildContext.Get(), nullptr);
    auto* discardedVulkanContext =
        dynamic_cast<VulkanCommandContext*>(discardedBuildContext.Get());
    ASSERT_NE(discardedVulkanContext, nullptr);
    discardedBuildContext->Begin();
    discardedBuildContext->BufferBarrier({
        vertexBuffer.Get(), RHIResourceState::Common,
        RHIResourceState::AccelerationStructureBuildRead});
    discardedBuildContext->BufferBarrier({
        blasScratch.Get(), RHIResourceState::Common,
        RHIResourceState::UnorderedAccess});
    discardedBuildContext->BuildBottomLevelAccelerationStructure(
        blas.Get(), blasDesc, blasScratch.Get());
    discardedBuildContext->BufferBarrier({
        instanceBuffer.Get(), RHIResourceState::Common,
        RHIResourceState::AccelerationStructureBuildRead});
    discardedBuildContext->BufferBarrier({
        tlasScratch.Get(), RHIResourceState::Common,
        RHIResourceState::UnorderedAccess});
    discardedBuildContext->BuildTopLevelAccelerationStructure(
        tlas.Get(), tlasDesc, tlasScratch.Get());
    discardedBuildContext->End();
    EXPECT_TRUE(
        discardedVulkanContext
            ->HasCompatiblePendingAccelerationStructureBuild(blas.Get()));
    EXPECT_TRUE(
        discardedVulkanContext
            ->HasCompatiblePendingAccelerationStructureBuild(tlas.Get()));
    EXPECT_FALSE(vulkanBlas->HasSubmittedBuild());
    EXPECT_FALSE(vulkanTlas->HasSubmittedBuild());
    discardedBuildContext->Reset();
    EXPECT_FALSE(
        discardedVulkanContext
            ->HasCompatiblePendingAccelerationStructureBuild(blas.Get()));
    EXPECT_FALSE(
        discardedVulkanContext
            ->HasCompatiblePendingAccelerationStructureBuild(tlas.Get()));
    EXPECT_FALSE(vulkanBlas->HasSubmittedBuild());
    EXPECT_FALSE(vulkanTlas->HasSubmittedBuild());

    const VulkanValidationMessageCounts messagesBefore =
        vulkanDevice->GetValidationMessageCounts();
    ASSERT_EQ(messagesBefore.errors, 0u);
    ASSERT_EQ(messagesBefore.warnings, 0u);

    RHICommandContextRef commandContext =
        device->CreateCommandContext(RHICommandQueueType::Graphics);
    ASSERT_NE(commandContext.Get(), nullptr);
    auto* vulkanCommandContext =
        dynamic_cast<VulkanCommandContext*>(commandContext.Get());
    ASSERT_NE(vulkanCommandContext, nullptr);
    commandContext->Begin();
    commandContext->BufferBarrier({
        vertexBuffer.Get(), RHIResourceState::Common,
        RHIResourceState::AccelerationStructureBuildRead});
    commandContext->BufferBarrier({
        blasScratch.Get(), RHIResourceState::Common,
        RHIResourceState::UnorderedAccess});
    commandContext->BuildBottomLevelAccelerationStructure(
        blas.Get(), blasDesc, blasScratch.Get());
    RHIAccelerationStructure* blasHandle = blas.Get();
    commandContext->WriteAccelerationStructureCompactedSizes(
        compactionQuery.Get(),
        0,
        std::span<RHIAccelerationStructure* const>(&blasHandle, 1));
    commandContext->ResolveAccelerationStructureCompactedSizes(
        compactionQuery.Get(), 0, 1, compactionReadback.Get());
    commandContext->BufferBarrier({
        instanceBuffer.Get(), RHIResourceState::Common,
        RHIResourceState::AccelerationStructureBuildRead});
    commandContext->BufferBarrier({
        tlasScratch.Get(), RHIResourceState::Common,
        RHIResourceState::UnorderedAccess});
    commandContext->BuildTopLevelAccelerationStructure(
        tlas.Get(), tlasDesc, tlasScratch.Get());
    EXPECT_TRUE(
        vulkanCommandContext
            ->HasCompatiblePendingAccelerationStructureBuild(blas.Get()));
    EXPECT_TRUE(
        vulkanCommandContext
            ->HasCompatiblePendingAccelerationStructureBuild(tlas.Get()));
    EXPECT_FALSE(vulkanBlas->HasSubmittedBuild());
    EXPECT_FALSE(vulkanTlas->HasSubmittedBuild());

    commandContext->BufferBarrier({
        output.Get(), RHIResourceState::Common,
        RHIResourceState::UnorderedAccess});
    commandContext->SetPipeline(pipeline.Get());
    commandContext->SetDescriptorSet(0, descriptorSet.Get());
    commandContext->DispatchRays({shaderTable.Get(), 1, 1, 1});
    commandContext->BufferBarrier({
        output.Get(), RHIResourceState::UnorderedAccess,
        RHIResourceState::CopySource});
    commandContext->CopyBuffer(
        output.Get(), readback.Get(), 0, 0, sizeof(uint32));
    commandContext->End();

    RHIFenceRef fence = device->CreateFence(0);
    ASSERT_NE(fence.Get(), nullptr);
    const uint64 submittedValue =
        device->SubmitCommandContext(commandContext.Get(), fence.Get());
    ASSERT_NE(submittedValue, 0u);
    EXPECT_TRUE(vulkanBlas->HasSubmittedBuild());
    EXPECT_TRUE(vulkanTlas->HasSubmittedBuild());
    EXPECT_TRUE(vulkanBlas->SubmittedBuildWasOnQueue(
        RHICommandQueueType::Graphics));
    EXPECT_FALSE(vulkanBlas->SubmittedBuildWasOnQueue(
        RHICommandQueueType::Compute));
    EXPECT_FALSE(
        vulkanCommandContext
            ->HasCompatiblePendingAccelerationStructureBuild(blas.Get()));
    EXPECT_FALSE(
        vulkanCommandContext
            ->HasCompatiblePendingAccelerationStructureBuild(tlas.Get()));
    device->WaitForFence(fence.Get(), submittedValue);
    ASSERT_GE(fence->GetCompletedValue(), submittedValue);

    const void* mappedReadback = readback->Map();
    ASSERT_NE(mappedReadback, nullptr);
    const uint32 observedValue =
        *static_cast<const uint32*>(mappedReadback);
    readback->Unmap();
    EXPECT_EQ(observedValue, kHitSentinel);

    const void* mappedCompactionSize = compactionReadback->Map();
    ASSERT_NE(mappedCompactionSize, nullptr);
    const uint64 compactedSize =
        *static_cast<const uint64*>(mappedCompactionSize);
    compactionReadback->Unmap();
    ASSERT_GT(compactedSize, 0u);
    ASSERT_LE(compactedSize, blasSizes.accelerationStructureSize);

    RHIAccelerationStructureDesc compactedBlasDesc;
    compactedBlasDesc.type = RHIAccelerationStructureType::BottomLevel;
    compactedBlasDesc.size = AlignUp(
        compactedSize,
        RVX_RAY_TRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
    compactedBlasDesc.debugName = "GFX21VulkanCompactedBLAS";
    RHIAccelerationStructureRef compactedBlas =
        device->CreateAccelerationStructure(compactedBlasDesc);
    ASSERT_NE(compactedBlas.Get(), nullptr);

    RHIRayTracingInstanceDesc compactedInstance = instance;
    compactedInstance.bottomLevel = compactedBlas.Get();
    const RHIRayTracingInstanceRecord compactedInstanceRecord =
        PackRHIRayTracingInstanceRecord(
            compactedInstance, compactedBlas->GetGPUVirtualAddress());
    RHIBufferDesc compactedInstanceBufferDesc;
    compactedInstanceBufferDesc.size = sizeof(compactedInstanceRecord);
    compactedInstanceBufferDesc.usage =
        RHIBufferUsage::AccelerationStructureInput |
        RHIBufferUsage::DeviceAddress;
    compactedInstanceBufferDesc.memoryType = RHIMemoryType::Upload;
    compactedInstanceBufferDesc.stride = sizeof(compactedInstanceRecord);
    compactedInstanceBufferDesc.debugName = "GFX21VulkanCompactedInstance";
    RHIBufferRef compactedInstanceBuffer =
        device->CreateBuffer(compactedInstanceBufferDesc);
    ASSERT_NE(compactedInstanceBuffer.Get(), nullptr);
    void* compactedInstanceData = compactedInstanceBuffer->Map();
    ASSERT_NE(compactedInstanceData, nullptr);
    std::memcpy(compactedInstanceData,
                &compactedInstanceRecord,
                sizeof(compactedInstanceRecord));
    compactedInstanceBuffer->Unmap();

    RHITopLevelASDesc compactedTlasBuildDesc;
    compactedTlasBuildDesc.instanceBuffer = compactedInstanceBuffer.Get();
    compactedTlasBuildDesc.instanceCount = 1;
    compactedTlasBuildDesc.buildFlags =
        RHIAccelerationStructureBuildFlags::PreferFastTrace;
    compactedTlasBuildDesc.debugName = "GFX21VulkanCompactedTLASBuild";
    const RHIAccelerationStructureBuildSizes compactedTlasSizes =
        device->GetTopLevelASBuildSizes(compactedTlasBuildDesc);
    ASSERT_TRUE(compactedTlasSizes.IsValid());
    RHIAccelerationStructureDesc compactedTlasDesc;
    compactedTlasDesc.type = RHIAccelerationStructureType::TopLevel;
    compactedTlasDesc.size = compactedTlasSizes.accelerationStructureSize;
    compactedTlasDesc.debugName = "GFX21VulkanCompactedTLAS";
    RHIAccelerationStructureRef compactedTlas =
        device->CreateAccelerationStructure(compactedTlasDesc);
    ASSERT_NE(compactedTlas.Get(), nullptr);
    RHIBufferDesc compactedTlasScratchDesc;
    compactedTlasScratchDesc.size = compactedTlasSizes.buildScratchSize;
    compactedTlasScratchDesc.usage = RHIBufferUsage::UnorderedAccess |
                                     RHIBufferUsage::DeviceAddress;
    compactedTlasScratchDesc.memoryType = RHIMemoryType::Default;
    compactedTlasScratchDesc.debugName = "GFX21VulkanCompactedTLASScratch";
    RHIBufferRef compactedTlasScratch =
        device->CreateBuffer(compactedTlasScratchDesc);
    ASSERT_NE(compactedTlasScratch.Get(), nullptr);

    RHICommandContextRef compactContext =
        device->CreateCommandContext(RHICommandQueueType::Graphics);
    ASSERT_NE(compactContext.Get(), nullptr);
    compactContext->Begin();
    compactContext->CopyAccelerationStructureCompact(
        compactedBlas.Get(), blas.Get(), compactedSize);
    compactContext->BufferBarrier({
        compactedInstanceBuffer.Get(), RHIResourceState::Common,
        RHIResourceState::AccelerationStructureBuildRead});
    compactContext->BufferBarrier({
        compactedTlasScratch.Get(), RHIResourceState::Common,
        RHIResourceState::UnorderedAccess});
    compactContext->BuildTopLevelAccelerationStructure(
        compactedTlas.Get(),
        compactedTlasBuildDesc,
        compactedTlasScratch.Get());
    compactContext->End();
    const uint64 compactSubmission =
        device->SubmitCommandContext(compactContext.Get(), fence.Get());
    ASSERT_NE(compactSubmission, 0u);
    device->WaitForFence(fence.Get(), compactSubmission);
    ASSERT_GE(fence->GetCompletedValue(), compactSubmission);
    auto* vulkanCompactedTlas =
        dynamic_cast<VulkanAccelerationStructure*>(compactedTlas.Get());
    ASSERT_NE(vulkanCompactedTlas, nullptr);
    EXPECT_TRUE(vulkanCompactedTlas->HasSubmittedBuild());
    device->WaitIdle();
    const VulkanValidationMessageCounts messagesAfter =
        vulkanDevice->GetValidationMessageCounts();
    EXPECT_EQ(messagesAfter.errors, 0u);
    EXPECT_EQ(messagesAfter.warnings, 0u);

    WriteNativeEvidence(
        *vulkanDevice,
        dispatchRegions,
        messagesBefore,
        messagesAfter,
        observedValue);
}
