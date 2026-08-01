#include "Common/GpuTestUtils.h"
#include "Core/Core.h"
#include "DX12Resources.h"
#include "Render/Context/RenderContext.h"
#include "Render/PipelineCache.h"
#include "Render/RayTracing/RayTracingResourceBindings.h"
#include "RHI/RHI.h"
#include "RHI_BackendFactory/RHIBackendFactory.h"
#include "ShaderCompiler/ShaderCompiler.h"

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

using namespace RVX;

namespace
{
    namespace RTShadowBindings = RayTracingResourceBindings::Shadow;
    namespace RTReflectionBindings = RayTracingResourceBindings::Reflection;

    std::filesystem::path FindRenderShaderDirectory()
    {
        std::filesystem::path current = std::filesystem::current_path();
        for (uint32 i = 0; i < 8; ++i)
        {
            const std::filesystem::path candidate = current / "Render" / "Shaders";
            if (std::filesystem::exists(candidate / "DefaultLit.hlsl"))
            {
                return candidate;
            }

            if (!current.has_parent_path() || current.parent_path() == current)
            {
                break;
            }
            current = current.parent_path();
        }

        return {};
    }

    const RHIBindingLayoutEntry* FindLayoutEntry(const RHIDescriptorSetLayout* layout, uint32 binding)
    {
        if (!layout)
            return nullptr;

        for (const RHIBindingLayoutEntry& entry : layout->GetEntries())
        {
            if (entry.binding == binding)
            {
                return &entry;
            }
        }

        return nullptr;
    }

    RHIShaderRef CompileInlineDXRShader(IRHIDevice& device,
                                        RHIShaderStage stage,
                                        const char* entryPoint,
                                        const char* source,
                                        const char* debugName)
    {
        std::unique_ptr<IShaderCompiler> compiler = CreateShaderCompiler();
        if (!compiler)
        {
            ADD_FAILURE() << "Shader compiler is unavailable";
            return {};
        }

        ShaderCompileOptions options;
        options.stage = stage;
        options.entryPoint = entryPoint;
        options.sourceCode = source;
        options.sourcePath = debugName;
        options.targetProfile = "lib_6_3";
        options.targetBackend = RHIBackendType::DX12;
        options.enableDebugInfo = true;

        ShaderCompileResult result = compiler->Compile(options);
        if (!result.success)
        {
            ADD_FAILURE() << "Failed to compile inline DXR shader: " << result.errorMessage;
            return {};
        }

        RHIShaderDesc shaderDesc;
        shaderDesc.stage = stage;
        shaderDesc.bytecode = result.bytecode.data();
        shaderDesc.bytecodeSize = static_cast<uint64>(result.bytecode.size());
        shaderDesc.entryPoint = entryPoint;
        shaderDesc.debugName = debugName;
        return device.CreateShader(shaderDesc);
    }

    uint64 AlignUp(uint64 value, uint64 alignment)
    {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    struct RayTracingTestAlphaMetadata
    {
        uint32 flags = 0;
        uint32 baseColorUVSet = 0;
        float alphaCutoff = 0.5f;
        float baseColorAlpha = 1.0f;
        uint32 baseColorTextureIdLow = 0;
        uint32 baseColorTextureIdHigh = 0;
        uint32 baseColorTextureTableIndex = RVX_INVALID_INDEX;
        uint32 indexBufferTableIndex = RVX_INVALID_INDEX;
        uint32 uvBufferTableIndex = RVX_INVALID_INDEX;
        uint32 indexElementOffset = 0;
        uint32 baseVertex = 0;
        uint32 baseColorSamplerFlags = 0;
        uint32 normalBufferTableIndex = RVX_INVALID_INDEX;
        uint32 tangentBufferTableIndex = RVX_INVALID_INDEX;
        Vec2 baseColorUVOffset{0.0f, 0.0f};
        Vec2 baseColorUVScale{1.0f, 1.0f};
        float baseColorUVRotation = 0.0f;
        float reserved2 = 0.0f;
    };

    static_assert(sizeof(RayTracingTestAlphaMetadata) == 80,
                  "RayTracingTestAlphaMetadata must match RayTracedShadow.hlsl alpha metadata layout");

    struct RayTracingTestTextureSamplingMetadata
    {
        Vec2 uvOffset{0.0f, 0.0f};
        Vec2 uvScale{1.0f, 1.0f};
        float uvRotation = 0.0f;
        uint32 uvSet = 0;
        uint32 samplerFlags = 0;
        uint32 reserved = 0;
    };

    static_assert(sizeof(RayTracingTestTextureSamplingMetadata) == 32,
                  "RayTracingTestTextureSamplingMetadata must match RayTracedShadow.hlsl sampling layout");

    struct RayTracingTestMaterialMetadata
    {
        Vec4 baseColorFactor{1.0f, 1.0f, 1.0f, 1.0f};
        Vec4 emissiveFactor{0.0f, 0.0f, 0.0f, 1.0f};
        Vec4 materialFactors{1.0f, 1.0f, 0.5f, 1.0f};
        uint32 flags = 0;
        uint32 materialIdLow = 0;
        uint32 materialIdHigh = 0;
        uint32 workflow = 0;
        uint32 baseColorTextureTableIndex = RVX_INVALID_INDEX;
        uint32 metallicRoughnessTextureTableIndex = RVX_INVALID_INDEX;
        uint32 normalTextureTableIndex = RVX_INVALID_INDEX;
        uint32 emissiveTextureTableIndex = RVX_INVALID_INDEX;
        RayTracingTestTextureSamplingMetadata baseColorTextureSampling;
        RayTracingTestTextureSamplingMetadata metallicRoughnessTextureSampling;
        RayTracingTestTextureSamplingMetadata normalTextureSampling;
        RayTracingTestTextureSamplingMetadata emissiveTextureSampling;
    };

    static_assert(sizeof(RayTracingTestMaterialMetadata) == 208,
                  "RayTracingTestMaterialMetadata must match RayTracedShadow.hlsl material metadata layout");

    RHIBufferRef CreateUploadBufferWithData(IRHIDevice& device,
                                            uint64 bufferSize,
                                            RHIBufferUsage usage,
                                            uint32 stride,
                                            const void* initialData,
                                            uint64 initialDataSize,
                                            const char* debugName)
    {
        if (bufferSize < initialDataSize)
        {
            ADD_FAILURE() << "Upload buffer is smaller than the initial payload: " << debugName;
            return {};
        }

        RHIBufferDesc bufferDesc;
        bufferDesc.size = bufferSize;
        bufferDesc.usage = usage;
        bufferDesc.memoryType = RHIMemoryType::Upload;
        bufferDesc.stride = stride;
        bufferDesc.debugName = debugName;
        RHIBufferRef buffer = device.CreateBuffer(bufferDesc);
        if (!buffer)
        {
            ADD_FAILURE() << "Failed to create upload buffer: " << debugName;
            return {};
        }

        void* mapped = buffer->Map();
        if (!mapped)
        {
            ADD_FAILURE() << "Failed to map upload buffer: " << debugName;
            return {};
        }

        std::memset(mapped, 0, static_cast<size_t>(bufferSize));
        if (initialData && initialDataSize > 0)
        {
            std::memcpy(mapped, initialData, static_cast<size_t>(initialDataSize));
        }
        buffer->Unmap();
        return buffer;
    }

    RHITextureViewRef CreateTestTextureView(IRHIDevice& device,
                                            RHITexture* texture,
                                            RHITextureViewType type,
                                            RHIFormat format,
                                            const char* debugName)
    {
        RHITextureViewDesc viewDesc;
        viewDesc.type = type;
        viewDesc.format = format;
        viewDesc.dimension = RHITextureDimension::Texture2D;
        viewDesc.debugName = debugName;
        RHITextureViewRef view = device.CreateTextureView(texture, viewDesc);
        if (!view)
        {
            ADD_FAILURE() << "Failed to create texture view: " << debugName;
        }
        return view;
    }

    RHIBufferRef CreateTextureUploadBuffer2D(IRHIDevice& device,
                                             const void* pixels,
                                             uint32 width,
                                             uint32 height,
                                             uint32 bytesPerPixel,
                                             uint32 rowPitch,
                                             const char* debugName)
    {
        const uint32 rowBytes = width * bytesPerPixel;
        if (rowPitch < rowBytes)
        {
            ADD_FAILURE() << "Texture upload row pitch is smaller than a source row: " << debugName;
            return {};
        }

        std::vector<uint8> uploadBytes(static_cast<size_t>(rowPitch) * height, 0u);
        const uint8* srcBytes = static_cast<const uint8*>(pixels);
        for (uint32 y = 0; y < height; ++y)
        {
            std::memcpy(uploadBytes.data() + static_cast<size_t>(y) * rowPitch,
                        srcBytes + static_cast<size_t>(y) * rowBytes,
                        rowBytes);
        }

        return CreateUploadBufferWithData(device,
                                          static_cast<uint64>(uploadBytes.size()),
                                          RHIBufferUsage::CopySrc,
                                          0,
                                          uploadBytes.data(),
                                          static_cast<uint64>(uploadBytes.size()),
                                          debugName);
    }
} // namespace

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
    bufferDesc.usage = RHIBufferUsage::Vertex | RHIBufferUsage::UnorderedAccess;
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
    EXPECT_EQ(nullptr, device->CreateDescriptorSet(setDesc).Get());

    RHIDescriptorSetDesc immutableSetDesc;
    immutableSetDesc.SetLayout(layout.Get()).BindTexture(0, storageUAV.Get());
    auto set = device->CreateDescriptorSet(immutableSetDesc);
    ASSERT_NE(nullptr, set.Get());
    EXPECT_TRUE(set->Update({}));
    EXPECT_FALSE(set->Update({RHIDescriptorBinding{0, nullptr, 0, 0, storageUAV.Get(), nullptr}}));
    EXPECT_FALSE(set->Update({RHIDescriptorBinding{0, nullptr, 0, 0, storageSRV.Get(), nullptr}}));

    RHIDescriptorSetDesc validInitialSetDesc;
    validInitialSetDesc.SetLayout(layout.Get()).BindTexture(0, storageUAV.Get());
    EXPECT_NE(nullptr, device->CreateDescriptorSet(validInitialSetDesc).Get());

    RHIDescriptorSetDesc invalidInitialSetDesc;
    invalidInitialSetDesc.SetLayout(layout.Get()).BindTexture(0, storageSRV.Get());
    EXPECT_EQ(nullptr, device->CreateDescriptorSet(invalidInitialSetDesc).Get());
}

TEST(DX12Validation, Texture2DArrayLayerViewsCreateNativeDescriptors)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX12);

    auto depthArrayDesc = RHITextureDesc::DepthStencil(64, 64, RHIFormat::D32_FLOAT);
    depthArrayDesc.arraySize = 3;
    depthArrayDesc.debugName = "TestDepthArray";
    auto depthArray = device->CreateTexture(depthArrayDesc);
    ASSERT_NE(nullptr, depthArray.Get());

    RHITextureViewDesc dsvDesc;
    dsvDesc.format = RHIFormat::D32_FLOAT;
    dsvDesc.type = RHITextureViewType::DepthStencil;
    dsvDesc.subresourceRange = RHISubresourceRange{0, 1, 2, 1, RHITextureAspect::Depth};
    auto dsv = device->CreateTextureView(depthArray.Get(), dsvDesc);
    ASSERT_NE(nullptr, dsv.Get());

    auto* dx12Dsv = dynamic_cast<DX12TextureView*>(dsv.Get());
    ASSERT_NE(nullptr, dx12Dsv);
    EXPECT_TRUE(dx12Dsv->GetDSVHandle().IsValid());

    RHITextureViewDesc srvDesc = dsvDesc;
    srvDesc.type = RHITextureViewType::ShaderResource;
    auto srv = device->CreateTextureView(depthArray.Get(), srvDesc);
    ASSERT_NE(nullptr, srv.Get());

    auto* dx12Srv = dynamic_cast<DX12TextureView*>(srv.Get());
    ASSERT_NE(nullptr, dx12Srv);
    EXPECT_TRUE(dx12Srv->GetSRVHandle().IsValid());
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
    EXPECT_EQ(caps.queueTopology.completionMode, RHIQueueCompletionMode::NativeTimeline);
    EXPECT_EQ(caps.queueTopology.activeDomainCount, 3);
    EXPECT_EQ(caps.queueTopology.logicalQueueDomains[0], GPUQueueDomain::Graphics);
    EXPECT_EQ(caps.queueTopology.logicalQueueDomains[1], GPUQueueDomain::Compute);
    EXPECT_EQ(caps.queueTopology.logicalQueueDomains[2], GPUQueueDomain::Copy);
    EXPECT_TRUE(ValidateRHICapabilities(caps));
}

TEST(DX12Validation, RenderContextSubmitsTrackedGraphicsFrameWithoutSurface)
{
    RenderContextConfig config;
    config.backendType = RHIBackendType::DX12;
    config.enableValidation = false;
    config.frameBuffering = 2;
    config.appName = "DX12RenderContextValidation";

    RenderContext context;
    if (!context.Initialize(config))
    {
        GTEST_SKIP() << "DX12 RenderContext is not available";
    }
    if (RVX::Test::IsSoftwareAdapterName(
            context.GetDevice()->GetCapabilities().adapterName))
    {
        context.Shutdown();
        GTEST_SKIP() << "DX12 RenderContext uses a software adapter";
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
    bufferDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::UnorderedAccess;
    bufferDesc.memoryType = RHIMemoryType::Default;
    bufferDesc.stride = sizeof(float);
    auto buffer = device->CreateBuffer(bufferDesc);
    ASSERT_NE(nullptr, buffer.Get());
    ctx->BufferBarrier({buffer.Get(), RHIResourceState::Common, RHIResourceState::Common});
    ctx->BeginBarrier({buffer.Get(), RHIResourceState::Common, RHIResourceState::Common});
    ctx->EndBarrier({buffer.Get(), RHIResourceState::Common, RHIResourceState::Common});
    const RHIAccessSnapshot unorderedAccess = MakeRHIAccessSnapshot(
        RHIResourceState::UnorderedAccess,
        RHIShaderStage::Compute,
        GPUQueueDomain::Graphics,
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

TEST(DX12Validation, RayTracingCapabilitiesExposeOnlyImplementedDXRContracts)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX12);

    const RHICapabilities& caps = device->GetCapabilities();
    EXPECT_FALSE(caps.supportsAccelerationStructureCompaction);

    if (!caps.supportsRaytracing)
    {
        EXPECT_FALSE(caps.supportsRaytracingPipeline);
        EXPECT_FALSE(caps.supportsRayQuery);
        EXPECT_FALSE(caps.supportsAccelerationStructureUpdate);
        EXPECT_EQ(caps.maxRayRecursionDepth, 0u);
        EXPECT_EQ(caps.shaderGroupHandleSize, 0u);
        EXPECT_EQ(caps.shaderGroupHandleAlignment, 0u);
        EXPECT_EQ(caps.shaderTableBaseAlignment, 0u);
        return;
    }

    EXPECT_TRUE(caps.supportsRaytracingPipeline);
    EXPECT_TRUE(caps.supportsAccelerationStructureUpdate);
    EXPECT_GT(caps.maxRayRecursionDepth, 0u);
    EXPECT_GT(caps.shaderGroupHandleSize, 0u);
    EXPECT_GT(caps.shaderGroupHandleAlignment, 0u);
    EXPECT_GT(caps.shaderTableBaseAlignment, 0u);

    RHIAccelerationStructureDesc zeroSizeASDesc;
    zeroSizeASDesc.type = RHIAccelerationStructureType::BottomLevel;
    zeroSizeASDesc.debugName = "InvalidZeroSizeAS";
    EXPECT_EQ(device->CreateAccelerationStructure(zeroSizeASDesc).Get(), nullptr);

    RHIAccelerationStructureDesc invalidTypeASDesc;
    invalidTypeASDesc.type = static_cast<RHIAccelerationStructureType>(0xFFu);
    invalidTypeASDesc.size = 4096;
    invalidTypeASDesc.debugName = "InvalidTypeAS";
    EXPECT_EQ(device->CreateAccelerationStructure(invalidTypeASDesc).Get(), nullptr);

    RHIBufferDesc vertexBufferDesc;
    vertexBufferDesc.size = sizeof(float) * 9;
    vertexBufferDesc.usage = RHIBufferUsage::Vertex |
                             RHIBufferUsage::AccelerationStructureInput |
                             RHIBufferUsage::DeviceAddress;
    vertexBufferDesc.memoryType = RHIMemoryType::Upload;
    vertexBufferDesc.debugName = "CompactionUnsupportedBLASVertices";
    auto vertexBuffer = device->CreateBuffer(vertexBufferDesc);
    ASSERT_NE(vertexBuffer.Get(), nullptr);

    RHIRayTracingGeometryDesc geometryDesc;
    geometryDesc.type = RHIRayTracingGeometryType::Triangles;
    geometryDesc.triangles.vertexBuffer = vertexBuffer.Get();
    geometryDesc.triangles.vertexCount = 3;
    geometryDesc.triangles.vertexStride = sizeof(float) * 3;
    geometryDesc.triangles.vertexFormat = RHIFormat::RGB32_FLOAT;

    RHIBottomLevelASDesc blasDesc;
    blasDesc.geometries.push_back(geometryDesc);
    blasDesc.buildFlags = RHIAccelerationStructureBuildFlags::AllowCompaction |
                          RHIAccelerationStructureBuildFlags::PreferFastTrace;
    EXPECT_FALSE(device->GetBottomLevelASBuildSizes(blasDesc).IsValid());

    RHIBufferDesc instanceBufferDesc;
    instanceBufferDesc.size = sizeof(RHIRayTracingInstanceRecord);
    instanceBufferDesc.usage = RHIBufferUsage::ShaderResource |
                               RHIBufferUsage::AccelerationStructureInput |
                               RHIBufferUsage::DeviceAddress;
    instanceBufferDesc.memoryType = RHIMemoryType::Upload;
    instanceBufferDesc.debugName = "CompactionUnsupportedTLASInstances";
    auto instanceBuffer = device->CreateBuffer(instanceBufferDesc);
    ASSERT_NE(instanceBuffer.Get(), nullptr);

    RHITopLevelASDesc tlasDesc;
    tlasDesc.instanceBuffer = instanceBuffer.Get();
    tlasDesc.instanceCount = 1;
    tlasDesc.buildFlags = RHIAccelerationStructureBuildFlags::AllowCompaction |
                          RHIAccelerationStructureBuildFlags::PreferFastTrace;
    EXPECT_FALSE(device->GetTopLevelASBuildSizes(tlasDesc).IsValid());
}

TEST(DX12Validation, RayTracedShadowPipelineCacheResources)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX12);

    if (!device->GetCapabilities().supportsRaytracingPipeline)
    {
        GTEST_SKIP() << "DXR ray tracing pipelines are not supported on this device";
    }

    const std::filesystem::path shaderDir = FindRenderShaderDirectory();
    ASSERT_FALSE(shaderDir.empty()) << "Render/Shaders directory not found";

    PipelineCache cache;
    ASSERT_TRUE(cache.Initialize(device.get(), shaderDir.string())) << cache.GetLastError();
    EXPECT_NE(cache.GetRayTracedShadowSetLayout(), nullptr);
    EXPECT_NE(cache.GetRayTracedShadowLayout(), nullptr);
    EXPECT_NE(cache.GetRayTracedShadowPipeline(), nullptr);
    EXPECT_NE(cache.GetRayTracedShadowShaderTable(), nullptr);
    EXPECT_NE(cache.GetRayTracedReflectionSetLayout(), nullptr);
    EXPECT_NE(cache.GetRayTracedReflectionLayout(), nullptr);
    EXPECT_NE(cache.GetRayTracedReflectionPipeline(), nullptr);
    EXPECT_NE(cache.GetRayTracedReflectionShaderTable(), nullptr);
    EXPECT_NE(cache.GetRayTracedReflectionCompositePipeline(RHIFormat::RGBA16_FLOAT), nullptr);
    EXPECT_NE(cache.GetRayTracedReflectionDenoiseSetLayout(), nullptr);
    EXPECT_NE(cache.GetRayTracedReflectionDenoiseLayout(), nullptr);
    EXPECT_NE(cache.GetRayTracedReflectionDenoisePipeline(RHIFormat::RGBA16_FLOAT), nullptr);

    const RHIDescriptorSetLayout* setLayout = cache.GetRayTracedShadowSetLayout();
    ASSERT_NE(setLayout, nullptr);
    ASSERT_EQ(setLayout->GetEntries().size(), 16u);
    const RHIBindingLayoutEntry* tlasBinding = FindLayoutEntry(setLayout, RTShadowBindings::RVX_RT_SHADOW_TLAS_BINDING);
    const RHIBindingLayoutEntry* outputMaskBinding = FindLayoutEntry(setLayout, RTShadowBindings::RVX_RT_SHADOW_OUTPUT_MASK_BINDING);
    const RHIBindingLayoutEntry* sceneDepthBinding = FindLayoutEntry(setLayout, RTShadowBindings::RVX_RT_SHADOW_SCENE_DEPTH_BINDING);
    const RHIBindingLayoutEntry* constantsBinding = FindLayoutEntry(setLayout, RTShadowBindings::RVX_RT_SHADOW_CONSTANTS_BINDING);
    const RHIBindingLayoutEntry* previousMaskBinding = FindLayoutEntry(setLayout, RTShadowBindings::RVX_RT_SHADOW_PREVIOUS_MASK_BINDING);
    const RHIBindingLayoutEntry* previousDepthBinding = FindLayoutEntry(setLayout, RTShadowBindings::RVX_RT_SHADOW_PREVIOUS_DEPTH_BINDING);
    const RHIBindingLayoutEntry* outputDepthBinding = FindLayoutEntry(setLayout, RTShadowBindings::RVX_RT_SHADOW_OUTPUT_DEPTH_BINDING);
    const RHIBindingLayoutEntry* previousNormalBinding = FindLayoutEntry(setLayout, RTShadowBindings::RVX_RT_SHADOW_PREVIOUS_NORMAL_BINDING);
    const RHIBindingLayoutEntry* outputNormalBinding = FindLayoutEntry(setLayout, RTShadowBindings::RVX_RT_SHADOW_OUTPUT_NORMAL_BINDING);
    const RHIBindingLayoutEntry* alphaMetadataBinding = FindLayoutEntry(setLayout, RTShadowBindings::RVX_RT_SHADOW_ALPHA_METADATA_BINDING);
    const RHIBindingLayoutEntry* alphaTexturesBinding = FindLayoutEntry(setLayout, RTShadowBindings::RVX_RT_SHADOW_ALPHA_TEXTURES_BINDING);
    const RHIBindingLayoutEntry* alphaIndexBuffersBinding = FindLayoutEntry(setLayout, RTShadowBindings::RVX_RT_SHADOW_ALPHA_INDEX_BUFFERS_BINDING);
    const RHIBindingLayoutEntry* alphaUVBuffersBinding = FindLayoutEntry(setLayout, RTShadowBindings::RVX_RT_SHADOW_ALPHA_UV_BUFFERS_BINDING);
    const RHIBindingLayoutEntry* materialMetadataBinding = FindLayoutEntry(setLayout, RTShadowBindings::RVX_RT_SHADOW_MATERIAL_METADATA_BINDING);
    const RHIBindingLayoutEntry* materialTexturesBinding = FindLayoutEntry(setLayout, RTShadowBindings::RVX_RT_SHADOW_MATERIAL_TEXTURES_BINDING);
    const RHIBindingLayoutEntry* sceneVelocityBinding = FindLayoutEntry(setLayout, RTShadowBindings::RVX_RT_SHADOW_SCENE_VELOCITY_BINDING);
    ASSERT_NE(tlasBinding, nullptr);
    ASSERT_NE(outputMaskBinding, nullptr);
    ASSERT_NE(sceneDepthBinding, nullptr);
    ASSERT_NE(constantsBinding, nullptr);
    ASSERT_NE(previousMaskBinding, nullptr);
    ASSERT_NE(previousDepthBinding, nullptr);
    ASSERT_NE(outputDepthBinding, nullptr);
    ASSERT_NE(previousNormalBinding, nullptr);
    ASSERT_NE(outputNormalBinding, nullptr);
    ASSERT_NE(alphaMetadataBinding, nullptr);
    ASSERT_NE(alphaTexturesBinding, nullptr);
    ASSERT_NE(alphaIndexBuffersBinding, nullptr);
    ASSERT_NE(alphaUVBuffersBinding, nullptr);
    ASSERT_NE(materialMetadataBinding, nullptr);
    ASSERT_NE(materialTexturesBinding, nullptr);
    ASSERT_NE(sceneVelocityBinding, nullptr);
    EXPECT_EQ(tlasBinding->type, RHIBindingType::AccelerationStructure);
    EXPECT_EQ(outputMaskBinding->type, RHIBindingType::StorageTexture);
    EXPECT_EQ(sceneDepthBinding->type, RHIBindingType::SampledTexture);
    EXPECT_EQ(constantsBinding->type, RHIBindingType::UniformBuffer);
    EXPECT_EQ(previousMaskBinding->type, RHIBindingType::SampledTexture);
    EXPECT_EQ(previousDepthBinding->type, RHIBindingType::SampledTexture);
    EXPECT_EQ(outputDepthBinding->type, RHIBindingType::StorageTexture);
    EXPECT_EQ(previousNormalBinding->type, RHIBindingType::SampledTexture);
    EXPECT_EQ(outputNormalBinding->type, RHIBindingType::StorageTexture);
    EXPECT_EQ(alphaMetadataBinding->type, RHIBindingType::ShaderResourceBuffer);
    EXPECT_EQ(alphaTexturesBinding->type, RHIBindingType::SampledTexture);
    EXPECT_EQ(alphaTexturesBinding->count, RTShadowBindings::RVX_RT_SHADOW_MAX_ALPHA_TEXTURES);
    EXPECT_EQ(alphaIndexBuffersBinding->type, RHIBindingType::ShaderResourceBuffer);
    EXPECT_EQ(alphaIndexBuffersBinding->count, RTShadowBindings::RVX_RT_SHADOW_MAX_ALPHA_GEOMETRY_BUFFERS);
    EXPECT_EQ(alphaUVBuffersBinding->type, RHIBindingType::ShaderResourceBuffer);
    EXPECT_EQ(alphaUVBuffersBinding->count, RTShadowBindings::RVX_RT_SHADOW_MAX_ALPHA_GEOMETRY_BUFFERS);
    EXPECT_EQ(materialMetadataBinding->type, RHIBindingType::ShaderResourceBuffer);
    EXPECT_EQ(materialTexturesBinding->type, RHIBindingType::SampledTexture);
    EXPECT_EQ(materialTexturesBinding->count, RTShadowBindings::RVX_RT_SHADOW_MAX_MATERIAL_TEXTURES);
    EXPECT_EQ(sceneVelocityBinding->type, RHIBindingType::SampledTexture);

    const RHIDescriptorSetLayout* reflectionSetLayout = cache.GetRayTracedReflectionSetLayout();
    ASSERT_NE(reflectionSetLayout, nullptr);
    ASSERT_EQ(reflectionSetLayout->GetEntries().size(), 18u);
    const RHIBindingLayoutEntry* reflectionTlasBinding = FindLayoutEntry(reflectionSetLayout, RTReflectionBindings::RVX_RT_REFLECTION_TLAS_BINDING);
    const RHIBindingLayoutEntry* reflectionOutputBinding = FindLayoutEntry(reflectionSetLayout, RTReflectionBindings::RVX_RT_REFLECTION_OUTPUT_BINDING);
    const RHIBindingLayoutEntry* reflectionSceneColorBinding = FindLayoutEntry(reflectionSetLayout, RTReflectionBindings::RVX_RT_REFLECTION_SCENE_COLOR_BINDING);
    const RHIBindingLayoutEntry* reflectionDepthBinding = FindLayoutEntry(reflectionSetLayout, RTReflectionBindings::RVX_RT_REFLECTION_SCENE_DEPTH_BINDING);
    const RHIBindingLayoutEntry* reflectionConstantsBinding = FindLayoutEntry(reflectionSetLayout, RTReflectionBindings::RVX_RT_REFLECTION_CONSTANTS_BINDING);
    const RHIBindingLayoutEntry* reflectionMaterialBinding = FindLayoutEntry(reflectionSetLayout, RTReflectionBindings::RVX_RT_REFLECTION_MATERIAL_METADATA_BINDING);
    const RHIBindingLayoutEntry* reflectionMaterialTexturesBinding = FindLayoutEntry(reflectionSetLayout, RTReflectionBindings::RVX_RT_REFLECTION_MATERIAL_TEXTURES_BINDING);
    const RHIBindingLayoutEntry* reflectionPreviousHistoryBinding = FindLayoutEntry(reflectionSetLayout, RTReflectionBindings::RVX_RT_REFLECTION_PREVIOUS_HISTORY_BINDING);
    const RHIBindingLayoutEntry* reflectionPreviousDepthHistoryBinding = FindLayoutEntry(reflectionSetLayout, RTReflectionBindings::RVX_RT_REFLECTION_PREVIOUS_DEPTH_BINDING);
    const RHIBindingLayoutEntry* reflectionCurrentDepthHistoryBinding = FindLayoutEntry(reflectionSetLayout, RTReflectionBindings::RVX_RT_REFLECTION_OUTPUT_DEPTH_BINDING);
    const RHIBindingLayoutEntry* reflectionPreviousNormalHistoryBinding = FindLayoutEntry(reflectionSetLayout, RTReflectionBindings::RVX_RT_REFLECTION_PREVIOUS_NORMAL_BINDING);
    const RHIBindingLayoutEntry* reflectionCurrentNormalHistoryBinding = FindLayoutEntry(reflectionSetLayout, RTReflectionBindings::RVX_RT_REFLECTION_OUTPUT_NORMAL_BINDING);
    const RHIBindingLayoutEntry* reflectionGeometryMetadataBinding = FindLayoutEntry(reflectionSetLayout, RTReflectionBindings::RVX_RT_REFLECTION_GEOMETRY_METADATA_BINDING);
    const RHIBindingLayoutEntry* reflectionIndexBuffersBinding = FindLayoutEntry(reflectionSetLayout, RTReflectionBindings::RVX_RT_REFLECTION_INDEX_BUFFERS_BINDING);
    const RHIBindingLayoutEntry* reflectionUVBuffersBinding = FindLayoutEntry(reflectionSetLayout, RTReflectionBindings::RVX_RT_REFLECTION_UV_BUFFERS_BINDING);
    const RHIBindingLayoutEntry* reflectionNormalBuffersBinding = FindLayoutEntry(reflectionSetLayout, RTReflectionBindings::RVX_RT_REFLECTION_NORMAL_BUFFERS_BINDING);
    const RHIBindingLayoutEntry* reflectionTangentBuffersBinding = FindLayoutEntry(reflectionSetLayout, RTReflectionBindings::RVX_RT_REFLECTION_TANGENT_BUFFERS_BINDING);
    const RHIBindingLayoutEntry* reflectionVelocityBinding = FindLayoutEntry(reflectionSetLayout, RTReflectionBindings::RVX_RT_REFLECTION_SCENE_VELOCITY_BINDING);
    ASSERT_NE(reflectionTlasBinding, nullptr);
    ASSERT_NE(reflectionOutputBinding, nullptr);
    ASSERT_NE(reflectionSceneColorBinding, nullptr);
    ASSERT_NE(reflectionDepthBinding, nullptr);
    ASSERT_NE(reflectionConstantsBinding, nullptr);
    ASSERT_NE(reflectionMaterialBinding, nullptr);
    ASSERT_NE(reflectionMaterialTexturesBinding, nullptr);
    ASSERT_NE(reflectionPreviousHistoryBinding, nullptr);
    ASSERT_NE(reflectionPreviousDepthHistoryBinding, nullptr);
    ASSERT_NE(reflectionCurrentDepthHistoryBinding, nullptr);
    ASSERT_NE(reflectionPreviousNormalHistoryBinding, nullptr);
    ASSERT_NE(reflectionCurrentNormalHistoryBinding, nullptr);
    ASSERT_NE(reflectionGeometryMetadataBinding, nullptr);
    ASSERT_NE(reflectionIndexBuffersBinding, nullptr);
    ASSERT_NE(reflectionUVBuffersBinding, nullptr);
    ASSERT_NE(reflectionNormalBuffersBinding, nullptr);
    ASSERT_NE(reflectionTangentBuffersBinding, nullptr);
    ASSERT_NE(reflectionVelocityBinding, nullptr);
    EXPECT_EQ(reflectionTlasBinding->type, RHIBindingType::AccelerationStructure);
    EXPECT_EQ(reflectionOutputBinding->type, RHIBindingType::StorageTexture);
    EXPECT_EQ(reflectionSceneColorBinding->type, RHIBindingType::SampledTexture);
    EXPECT_EQ(reflectionDepthBinding->type, RHIBindingType::SampledTexture);
    EXPECT_EQ(reflectionConstantsBinding->type, RHIBindingType::UniformBuffer);
    EXPECT_EQ(reflectionMaterialBinding->type, RHIBindingType::ShaderResourceBuffer);
    EXPECT_EQ(reflectionMaterialTexturesBinding->type, RHIBindingType::SampledTexture);
    EXPECT_EQ(reflectionMaterialTexturesBinding->count, RTReflectionBindings::RVX_RT_REFLECTION_MAX_MATERIAL_TEXTURES);
    EXPECT_EQ(reflectionPreviousHistoryBinding->type, RHIBindingType::SampledTexture);
    EXPECT_EQ(reflectionPreviousDepthHistoryBinding->type, RHIBindingType::SampledTexture);
    EXPECT_EQ(reflectionCurrentDepthHistoryBinding->type, RHIBindingType::StorageTexture);
    EXPECT_EQ(reflectionPreviousNormalHistoryBinding->type, RHIBindingType::SampledTexture);
    EXPECT_EQ(reflectionCurrentNormalHistoryBinding->type, RHIBindingType::StorageTexture);
    EXPECT_EQ(reflectionGeometryMetadataBinding->type, RHIBindingType::ShaderResourceBuffer);
    EXPECT_EQ(reflectionIndexBuffersBinding->type, RHIBindingType::ShaderResourceBuffer);
    EXPECT_EQ(reflectionIndexBuffersBinding->count, RTReflectionBindings::RVX_RT_REFLECTION_MAX_GEOMETRY_BUFFERS);
    EXPECT_EQ(reflectionUVBuffersBinding->type, RHIBindingType::ShaderResourceBuffer);
    EXPECT_EQ(reflectionUVBuffersBinding->count, RTReflectionBindings::RVX_RT_REFLECTION_MAX_GEOMETRY_BUFFERS);
    EXPECT_EQ(reflectionNormalBuffersBinding->type, RHIBindingType::ShaderResourceBuffer);
    EXPECT_EQ(reflectionNormalBuffersBinding->count, RTReflectionBindings::RVX_RT_REFLECTION_MAX_GEOMETRY_BUFFERS);
    EXPECT_EQ(reflectionTangentBuffersBinding->type, RHIBindingType::ShaderResourceBuffer);
    EXPECT_EQ(reflectionTangentBuffersBinding->count, RTReflectionBindings::RVX_RT_REFLECTION_MAX_GEOMETRY_BUFFERS);
    EXPECT_EQ(reflectionVelocityBinding->type, RHIBindingType::SampledTexture);

    const RHIDescriptorSetLayout* reflectionDenoiseSetLayout = cache.GetRayTracedReflectionDenoiseSetLayout();
    ASSERT_NE(reflectionDenoiseSetLayout, nullptr);
    ASSERT_EQ(reflectionDenoiseSetLayout->GetEntries().size(), 4u);
    const RHIBindingLayoutEntry* denoiseConstantsBinding = FindLayoutEntry(reflectionDenoiseSetLayout, 0);
    const RHIBindingLayoutEntry* denoiseReflectionBinding = FindLayoutEntry(reflectionDenoiseSetLayout, 1);
    const RHIBindingLayoutEntry* denoiseDepthBinding = FindLayoutEntry(reflectionDenoiseSetLayout, 2);
    const RHIBindingLayoutEntry* denoiseNormalGuideBinding = FindLayoutEntry(reflectionDenoiseSetLayout, 3);
    ASSERT_NE(denoiseConstantsBinding, nullptr);
    ASSERT_NE(denoiseReflectionBinding, nullptr);
    ASSERT_NE(denoiseDepthBinding, nullptr);
    ASSERT_NE(denoiseNormalGuideBinding, nullptr);
    EXPECT_EQ(denoiseConstantsBinding->type, RHIBindingType::UniformBuffer);
    EXPECT_EQ(denoiseReflectionBinding->type, RHIBindingType::SampledTexture);
    EXPECT_EQ(denoiseDepthBinding->type, RHIBindingType::SampledTexture);
    EXPECT_EQ(denoiseNormalGuideBinding->type, RHIBindingType::SampledTexture);

    cache.Shutdown();
}

TEST(DX12Validation, RayTracingPipelineRejectsRecursionDepthAboveDeviceLimit)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX12);

    const RHICapabilities& caps = device->GetCapabilities();
    if (!caps.supportsRaytracingPipeline)
    {
        GTEST_SKIP() << "DXR ray tracing pipelines are not supported on this device";
    }

    ASSERT_GT(caps.maxRayRecursionDepth, 0u);

    constexpr const char* kRayGen = R"(
        [shader("raygeneration")]
        void RayGen()
        {
        }
    )";

    RHIShaderRef rayGenShader = CompileInlineDXRShader(*device,
                                                       RHIShaderStage::RayGeneration,
                                                       "RayGen",
                                                       kRayGen,
                                                       "RecursionLimitDXRRayGen");
    ASSERT_NE(rayGenShader.Get(), nullptr);

    RHIPipelineLayoutDesc pipelineLayoutDesc;
    pipelineLayoutDesc.debugName = "RecursionLimitDXRPipelineLayout";
    RHIPipelineLayoutRef pipelineLayout = device->CreatePipelineLayout(pipelineLayoutDesc);
    ASSERT_NE(pipelineLayout.Get(), nullptr);

    RHIRayTracingShaderGroupDesc rayGenGroup;
    rayGenGroup.type = RHIRayTracingShaderGroupType::General;
    rayGenGroup.exportName = "RayGen";
    rayGenGroup.generalShader = rayGenShader.Get();

    RHIRayTracingPipelineDesc pipelineDesc;
    pipelineDesc.pipelineLayout = pipelineLayout.Get();
    pipelineDesc.shaderGroups.push_back(rayGenGroup);
    pipelineDesc.maxRecursionDepth = caps.maxRayRecursionDepth + 1u;
    pipelineDesc.maxPayloadSize = sizeof(uint32);
    pipelineDesc.maxAttributeSize = sizeof(float) * 2;
    pipelineDesc.debugName = "RecursionLimitDXRPipeline";

    EXPECT_EQ(device->CreateRayTracingPipeline(pipelineDesc).Get(), nullptr);
}

TEST(DX12Validation, RayTracingMinimalPipelineBuildsASAndSubmitsDispatch)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX12);

    if (!device->GetCapabilities().supportsRaytracingPipeline)
    {
        GTEST_SKIP() << "DXR ray tracing pipelines are not supported on this device";
    }

    constexpr const char* kMinimalRayGen = R"(
        [shader("raygeneration")]
        void RayGen()
        {
        }
    )";

    RHIShaderRef rayGenShader = CompileInlineDXRShader(*device,
                                                       RHIShaderStage::RayGeneration,
                                                       "RayGen",
                                                       kMinimalRayGen,
                                                       "MinimalDXRRayGen");
    ASSERT_NE(rayGenShader.Get(), nullptr);

    RHIPipelineLayoutDesc pipelineLayoutDesc;
    pipelineLayoutDesc.debugName = "MinimalDXRPipelineLayout";
    RHIPipelineLayoutRef pipelineLayout = device->CreatePipelineLayout(pipelineLayoutDesc);
    ASSERT_NE(pipelineLayout.Get(), nullptr);

    RHIRayTracingShaderGroupDesc rayGenGroup;
    rayGenGroup.type = RHIRayTracingShaderGroupType::General;
    rayGenGroup.exportName = "RayGen";
    rayGenGroup.generalShader = rayGenShader.Get();

    RHIRayTracingPipelineDesc pipelineDesc;
    pipelineDesc.pipelineLayout = pipelineLayout.Get();
    pipelineDesc.shaderGroups.push_back(rayGenGroup);
    pipelineDesc.maxRecursionDepth = 1;
    pipelineDesc.maxPayloadSize = 4;
    pipelineDesc.maxAttributeSize = 8;
    pipelineDesc.debugName = "MinimalDXRPipeline";
    RHIPipelineRef pipeline = device->CreateRayTracingPipeline(pipelineDesc);
    ASSERT_NE(pipeline.Get(), nullptr);
    EXPECT_TRUE(pipeline->IsRayTracing());
    EXPECT_EQ(pipeline->GetRayTracingShaderGroupCount(), 1u);

    RHIShaderTableDesc shaderTableDesc;
    shaderTableDesc.rayTracingPipelineOwner = pipeline;
    shaderTableDesc.rayGenerationRecords.push_back(RHIShaderTableRecord{0});
    shaderTableDesc.debugName = "MinimalDXRShaderTable";
    RHIShaderTableRef shaderTable = device->CreateShaderTable(shaderTableDesc);
    ASSERT_NE(shaderTable.Get(), nullptr);
    EXPECT_EQ(shaderTable->GetRayGenerationRecordCount(), 1u);

    const float vertices[9] =
    {
        0.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f
    };

    RHIBufferDesc vertexBufferDesc;
    vertexBufferDesc.size = sizeof(vertices);
    vertexBufferDesc.usage = RHIBufferUsage::Vertex | RHIBufferUsage::ShaderResource | RHIBufferUsage::AccelerationStructureInput | RHIBufferUsage::DeviceAddress;
    vertexBufferDesc.memoryType = RHIMemoryType::Upload;
    vertexBufferDesc.stride = sizeof(float) * 3;
    vertexBufferDesc.debugName = "MinimalDXRVertices";
    RHIBufferRef vertexBuffer = device->CreateBuffer(vertexBufferDesc);
    ASSERT_NE(vertexBuffer.Get(), nullptr);
    void* vertexData = vertexBuffer->Map();
    ASSERT_NE(vertexData, nullptr);
    std::memcpy(vertexData, vertices, sizeof(vertices));
    vertexBuffer->Unmap();

    RHIRayTracingGeometryDesc geometryDesc;
    geometryDesc.type = RHIRayTracingGeometryType::Triangles;
    geometryDesc.flags = RHIRayTracingGeometryFlags::Opaque;
    geometryDesc.triangles.vertexBuffer = vertexBuffer.Get();
    geometryDesc.triangles.vertexStride = sizeof(float) * 3;
    geometryDesc.triangles.vertexFormat = RHIFormat::RGB32_FLOAT;
    geometryDesc.triangles.vertexCount = 3;

    RHIBottomLevelASDesc blasDesc;
    blasDesc.geometries.push_back(geometryDesc);
    blasDesc.buildFlags = RHIAccelerationStructureBuildFlags::PreferFastTrace;
    blasDesc.debugName = "MinimalDXRBLASBuild";
    RHIAccelerationStructureBuildSizes blasSizes = device->GetBottomLevelASBuildSizes(blasDesc);
    ASSERT_TRUE(blasSizes.IsValid());

    RHIAccelerationStructureDesc blasResourceDesc;
    blasResourceDesc.type = RHIAccelerationStructureType::BottomLevel;
    blasResourceDesc.size = blasSizes.accelerationStructureSize;
    blasResourceDesc.debugName = "MinimalDXRBLAS";
    RHIAccelerationStructureRef blas = device->CreateAccelerationStructure(blasResourceDesc);
    ASSERT_NE(blas.Get(), nullptr);
    ASSERT_NE(blas->GetGPUVirtualAddress(), 0u);

    RHIBufferDesc blasScratchDesc;
    blasScratchDesc.size = blasSizes.buildScratchSize;
    blasScratchDesc.usage = RHIBufferUsage::UnorderedAccess | RHIBufferUsage::DeviceAddress;
    blasScratchDesc.memoryType = RHIMemoryType::Default;
    blasScratchDesc.debugName = "MinimalDXRBLASScratch";
    RHIBufferRef blasScratch = device->CreateBuffer(blasScratchDesc);
    ASSERT_NE(blasScratch.Get(), nullptr);

    RHIRayTracingInstanceDesc instanceDesc;
    instanceDesc.bottomLevel = blas.Get();
    instanceDesc.instanceId = 0;
    instanceDesc.instanceMask = 0xFF;
    instanceDesc.instanceContributionToHitGroupIndex = 0;
    RHIRayTracingInstanceRecord instanceRecord = PackRHIRayTracingInstanceRecord(
        instanceDesc,
        blas->GetGPUVirtualAddress());

    RHIBufferDesc instanceBufferDesc;
    instanceBufferDesc.size = sizeof(instanceRecord);
    instanceBufferDesc.usage = RHIBufferUsage::ShaderResource | RHIBufferUsage::AccelerationStructureInput | RHIBufferUsage::DeviceAddress;
    instanceBufferDesc.memoryType = RHIMemoryType::Upload;
    instanceBufferDesc.stride = sizeof(instanceRecord);
    instanceBufferDesc.debugName = "MinimalDXRInstanceBuffer";
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
    tlasDesc.debugName = "MinimalDXRTLASBuild";
    RHIAccelerationStructureBuildSizes tlasSizes = device->GetTopLevelASBuildSizes(tlasDesc);
    ASSERT_TRUE(tlasSizes.IsValid());

    RHIAccelerationStructureDesc tlasResourceDesc;
    tlasResourceDesc.type = RHIAccelerationStructureType::TopLevel;
    tlasResourceDesc.size = tlasSizes.accelerationStructureSize;
    tlasResourceDesc.debugName = "MinimalDXRTLAS";
    RHIAccelerationStructureRef tlas = device->CreateAccelerationStructure(tlasResourceDesc);
    ASSERT_NE(tlas.Get(), nullptr);
    ASSERT_NE(tlas->GetGPUVirtualAddress(), 0u);

    RHIBufferDesc tlasScratchDesc;
    tlasScratchDesc.size = tlasSizes.buildScratchSize;
    tlasScratchDesc.usage = RHIBufferUsage::UnorderedAccess | RHIBufferUsage::DeviceAddress;
    tlasScratchDesc.memoryType = RHIMemoryType::Default;
    tlasScratchDesc.debugName = "MinimalDXRTLASScratch";
    RHIBufferRef tlasScratch = device->CreateBuffer(tlasScratchDesc);
    ASSERT_NE(tlasScratch.Get(), nullptr);

    RHICommandContextRef ctx = device->CreateCommandContext(RHICommandQueueType::Graphics);
    ASSERT_NE(ctx.Get(), nullptr);
    ctx->Begin();
    ctx->BufferBarrier({blasScratch.Get(), RHIResourceState::Common, RHIResourceState::UnorderedAccess});
    ctx->BuildBottomLevelAccelerationStructure(blas.Get(), blasDesc, blasScratch.Get());
    ctx->BufferBarrier({tlasScratch.Get(), RHIResourceState::Common, RHIResourceState::UnorderedAccess});
    ctx->BuildTopLevelAccelerationStructure(tlas.Get(), tlasDesc, tlasScratch.Get());
    ctx->SetPipeline(pipeline.Get());
    ctx->DispatchRays({shaderTable.Get(), 1, 1, 1});
    ctx->End();

    RHIFenceRef fence = device->CreateFence(0);
    ASSERT_NE(fence.Get(), nullptr);
    const uint64 submittedValue = device->SubmitCommandContext(ctx.Get(), fence.Get());
    ASSERT_NE(submittedValue, 0u);
    device->WaitForFence(fence.Get(), submittedValue);
    EXPECT_GE(fence->GetCompletedValue(), submittedValue);
    device->WaitIdle();
}

TEST(DX12Validation, RayTracingDescriptorSetWritesStorageTexture)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX12);

    if (!device->GetCapabilities().supportsRaytracingPipeline)
    {
        GTEST_SKIP() << "DXR ray tracing pipelines are not supported on this device";
    }

    constexpr uint32 kExpectedPixel = 0x5A17C0DEu;
    constexpr uint32 kReadbackRowPitch = 256u;
    constexpr const char* kStorageTextureRayGen = R"(
        RWTexture2D<uint> gOutput : register(u0, space0);

        [shader("raygeneration")]
        void RayGen()
        {
            gOutput[DispatchRaysIndex().xy] = 0x5A17C0DEu;
        }
    )";

    RHIShaderRef rayGenShader = CompileInlineDXRShader(*device,
                                                       RHIShaderStage::RayGeneration,
                                                       "RayGen",
                                                       kStorageTextureRayGen,
                                                       "StorageTextureDXRRayGen");
    ASSERT_NE(rayGenShader.Get(), nullptr);

    RHIDescriptorSetLayoutDesc setLayoutDesc;
    setLayoutDesc.AddBinding(0, RHIBindingType::StorageTexture, RHIShaderStage::AllRayTracing);
    setLayoutDesc.debugName = "StorageTextureDXRSetLayout";
    RHIDescriptorSetLayoutRef setLayout = device->CreateDescriptorSetLayout(setLayoutDesc);
    ASSERT_NE(setLayout.Get(), nullptr);

    RHIPipelineLayoutDesc pipelineLayoutDesc;
    pipelineLayoutDesc.setLayouts.push_back(setLayout.Get());
    pipelineLayoutDesc.debugName = "StorageTextureDXRPipelineLayout";
    RHIPipelineLayoutRef pipelineLayout = device->CreatePipelineLayout(pipelineLayoutDesc);
    ASSERT_NE(pipelineLayout.Get(), nullptr);

    RHIRayTracingShaderGroupDesc rayGenGroup;
    rayGenGroup.type = RHIRayTracingShaderGroupType::General;
    rayGenGroup.exportName = "RayGen";
    rayGenGroup.generalShader = rayGenShader.Get();

    RHIRayTracingPipelineDesc pipelineDesc;
    pipelineDesc.pipelineLayout = pipelineLayout.Get();
    pipelineDesc.shaderGroups.push_back(rayGenGroup);
    pipelineDesc.maxRecursionDepth = 1;
    pipelineDesc.maxPayloadSize = 4;
    pipelineDesc.maxAttributeSize = 8;
    pipelineDesc.debugName = "StorageTextureDXRPipeline";
    RHIPipelineRef pipeline = device->CreateRayTracingPipeline(pipelineDesc);
    ASSERT_NE(pipeline.Get(), nullptr);
    ASSERT_TRUE(pipeline->IsRayTracing());

    RHIShaderTableDesc shaderTableDesc;
    shaderTableDesc.rayTracingPipelineOwner = pipeline;
    shaderTableDesc.rayGenerationRecords.push_back(RHIShaderTableRecord{0});
    shaderTableDesc.debugName = "StorageTextureDXRShaderTable";
    RHIShaderTableRef shaderTable = device->CreateShaderTable(shaderTableDesc);
    ASSERT_NE(shaderTable.Get(), nullptr);

    RHITextureDesc outputDesc = RHITextureDesc::Texture2D(
        1,
        1,
        RHIFormat::R32_UINT,
        RHITextureUsage::UnorderedAccess | RHITextureUsage::CopySrc);
    outputDesc.debugName = "StorageTextureDXROutput";
    RHITextureRef outputTexture = device->CreateTexture(outputDesc);
    ASSERT_NE(outputTexture.Get(), nullptr);

    RHITextureViewDesc outputViewDesc;
    outputViewDesc.type = RHITextureViewType::UnorderedAccess;
    outputViewDesc.format = RHIFormat::R32_UINT;
    outputViewDesc.dimension = RHITextureDimension::Texture2D;
    outputViewDesc.debugName = "StorageTextureDXROutputUAV";
    RHITextureViewRef outputView = device->CreateTextureView(outputTexture.Get(), outputViewDesc);
    ASSERT_NE(outputView.Get(), nullptr);

    RHIDescriptorSetDesc descriptorDesc;
    descriptorDesc.layout = setLayout.Get();
    descriptorDesc.debugName = "StorageTextureDXRDescriptorSet";
    descriptorDesc.BindTexture(0, outputView.Get());
    RHIDescriptorSetRef descriptorSet = device->CreateDescriptorSet(descriptorDesc);
    ASSERT_NE(descriptorSet.Get(), nullptr);

    RHIBufferDesc readbackDesc;
    readbackDesc.size = kReadbackRowPitch;
    readbackDesc.usage = RHIBufferUsage::CopyDst;
    readbackDesc.memoryType = RHIMemoryType::Readback;
    readbackDesc.debugName = "StorageTextureDXRReadback";
    RHIBufferRef readbackBuffer = device->CreateBuffer(readbackDesc);
    ASSERT_NE(readbackBuffer.Get(), nullptr);

    RHICommandContextRef ctx = device->CreateCommandContext(RHICommandQueueType::Graphics);
    ASSERT_NE(ctx.Get(), nullptr);
    ctx->Begin();
    ctx->TextureBarrier(outputTexture.Get(), RHIResourceState::Common, RHIResourceState::UnorderedAccess);
    ctx->SetPipeline(pipeline.Get());
    ctx->SetDescriptorSet(0, descriptorSet.Get());
    ctx->DispatchRays({shaderTable.Get(), 1, 1, 1});
    ctx->TextureBarrier(outputTexture.Get(), RHIResourceState::UnorderedAccess, RHIResourceState::CopySource);

    RHIBufferTextureCopyDesc copyDesc;
    copyDesc.bufferRowPitch = kReadbackRowPitch;
    copyDesc.textureRegion = RHIRect{0, 0, 1, 1};
    ctx->CopyTextureToBuffer(outputTexture.Get(), readbackBuffer.Get(), copyDesc);
    ctx->End();

    RHIFenceRef fence = device->CreateFence(0);
    ASSERT_NE(fence.Get(), nullptr);
    const uint64 submittedValue = device->SubmitCommandContext(ctx.Get(), fence.Get());
    ASSERT_NE(submittedValue, 0u);
    device->WaitForFence(fence.Get(), submittedValue);
    EXPECT_GE(fence->GetCompletedValue(), submittedValue);

    void* mappedReadback = readbackBuffer->Map();
    ASSERT_NE(mappedReadback, nullptr);
    const uint32 observedPixel = *static_cast<const uint32*>(mappedReadback);
    readbackBuffer->Unmap();
    EXPECT_EQ(observedPixel, kExpectedPixel);
    device->WaitIdle();
}

TEST(DX12Validation, RayTracingTraceRayHitsTriangleAndWritesClosestHitPayload)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX12);

    if (!device->GetCapabilities().supportsRaytracingPipeline)
    {
        GTEST_SKIP() << "DXR ray tracing pipelines are not supported on this device";
    }

    constexpr uint32 kClosestHitValue = 0xA11CE123u;
    constexpr uint32 kReadbackRowPitch = 256u;
    constexpr const char* kTraceRayShader = R"(
        RaytracingAccelerationStructure gScene : register(t0, space0);
        RWTexture2D<uint> gOutput : register(u1, space0);

        struct Payload
        {
            uint value;
        };

        [shader("raygeneration")]
        void RayGen()
        {
            RayDesc ray;
            ray.Origin = float3(0.25f, 0.25f, -1.0f);
            ray.Direction = float3(0.0f, 0.0f, 1.0f);
            ray.TMin = 0.0f;
            ray.TMax = 10.0f;

            Payload payload;
            payload.value = 0u;
            TraceRay(gScene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ray, payload);
            gOutput[DispatchRaysIndex().xy] = payload.value;
        }

        [shader("miss")]
        void Miss(inout Payload payload)
        {
            payload.value = 0xBAD00000u;
        }

        [shader("closesthit")]
        void ClosestHit(inout Payload payload, BuiltInTriangleIntersectionAttributes attributes)
        {
            payload.value = 0xA11CE123u;
        }
    )";

    RHIShaderRef rayGenShader = CompileInlineDXRShader(*device,
                                                       RHIShaderStage::RayGeneration,
                                                       "RayGen",
                                                       kTraceRayShader,
                                                       "TraceRayHitRayGen");
    ASSERT_NE(rayGenShader.Get(), nullptr);
    RHIShaderRef missShader = CompileInlineDXRShader(*device,
                                                     RHIShaderStage::Miss,
                                                     "Miss",
                                                     kTraceRayShader,
                                                     "TraceRayHitMiss");
    ASSERT_NE(missShader.Get(), nullptr);
    RHIShaderRef closestHitShader = CompileInlineDXRShader(*device,
                                                           RHIShaderStage::ClosestHit,
                                                           "ClosestHit",
                                                           kTraceRayShader,
                                                           "TraceRayHitClosestHit");
    ASSERT_NE(closestHitShader.Get(), nullptr);

    RHIDescriptorSetLayoutDesc setLayoutDesc;
    setLayoutDesc.AddBinding(0, RHIBindingType::AccelerationStructure, RHIShaderStage::AllRayTracing);
    setLayoutDesc.AddBinding(1, RHIBindingType::StorageTexture, RHIShaderStage::AllRayTracing);
    setLayoutDesc.debugName = "TraceRayHitSetLayout";
    RHIDescriptorSetLayoutRef setLayout = device->CreateDescriptorSetLayout(setLayoutDesc);
    ASSERT_NE(setLayout.Get(), nullptr);

    RHIPipelineLayoutDesc pipelineLayoutDesc;
    pipelineLayoutDesc.setLayouts.push_back(setLayout.Get());
    pipelineLayoutDesc.debugName = "TraceRayHitPipelineLayout";
    RHIPipelineLayoutRef pipelineLayout = device->CreatePipelineLayout(pipelineLayoutDesc);
    ASSERT_NE(pipelineLayout.Get(), nullptr);

    RHIRayTracingShaderGroupDesc rayGenGroup;
    rayGenGroup.type = RHIRayTracingShaderGroupType::General;
    rayGenGroup.exportName = "RayGen";
    rayGenGroup.generalShader = rayGenShader.Get();

    RHIRayTracingShaderGroupDesc missGroup;
    missGroup.type = RHIRayTracingShaderGroupType::General;
    missGroup.exportName = "Miss";
    missGroup.generalShader = missShader.Get();

    RHIRayTracingShaderGroupDesc hitGroup;
    hitGroup.type = RHIRayTracingShaderGroupType::TrianglesHitGroup;
    hitGroup.exportName = "PrimaryHitGroup";
    hitGroup.closestHitShader = closestHitShader.Get();

    RHIRayTracingPipelineDesc pipelineDesc;
    pipelineDesc.pipelineLayout = pipelineLayout.Get();
    pipelineDesc.shaderGroups.push_back(rayGenGroup);
    pipelineDesc.shaderGroups.push_back(missGroup);
    pipelineDesc.shaderGroups.push_back(hitGroup);
    pipelineDesc.maxRecursionDepth = 1;
    pipelineDesc.maxPayloadSize = sizeof(uint32);
    pipelineDesc.maxAttributeSize = sizeof(float) * 2;
    pipelineDesc.debugName = "TraceRayHitPipeline";
    RHIPipelineRef pipeline = device->CreateRayTracingPipeline(pipelineDesc);
    ASSERT_NE(pipeline.Get(), nullptr);
    ASSERT_TRUE(pipeline->IsRayTracing());
    ASSERT_EQ(pipeline->GetRayTracingShaderGroupCount(), 3u);
    EXPECT_FALSE(pipeline->IsRayTracingHitGroup(0));
    EXPECT_FALSE(pipeline->IsRayTracingHitGroup(1));
    EXPECT_TRUE(pipeline->IsRayTracingHitGroup(2));

    RHIShaderTableDesc shaderTableDesc;
    shaderTableDesc.rayTracingPipelineOwner = pipeline;
    shaderTableDesc.rayGenerationRecords.push_back(RHIShaderTableRecord{0});
    shaderTableDesc.missRecords.push_back(RHIShaderTableRecord{1});
    shaderTableDesc.hitGroupRecords.push_back(RHIShaderTableRecord{2});
    shaderTableDesc.debugName = "TraceRayHitShaderTable";
    RHIShaderTableRef shaderTable = device->CreateShaderTable(shaderTableDesc);
    ASSERT_NE(shaderTable.Get(), nullptr);
    EXPECT_EQ(shaderTable->GetRayGenerationRecordCount(), 1u);
    EXPECT_EQ(shaderTable->GetMissRecordCount(), 1u);
    EXPECT_EQ(shaderTable->GetHitGroupRecordCount(), 1u);

    const float vertices[9] =
    {
        0.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f
    };

    RHIBufferDesc vertexBufferDesc;
    vertexBufferDesc.size = sizeof(vertices);
    vertexBufferDesc.usage = RHIBufferUsage::Vertex | RHIBufferUsage::ShaderResource | RHIBufferUsage::AccelerationStructureInput | RHIBufferUsage::DeviceAddress;
    vertexBufferDesc.memoryType = RHIMemoryType::Upload;
    vertexBufferDesc.stride = sizeof(float) * 3;
    vertexBufferDesc.debugName = "TraceRayHitVertices";
    RHIBufferRef vertexBuffer = device->CreateBuffer(vertexBufferDesc);
    ASSERT_NE(vertexBuffer.Get(), nullptr);
    void* vertexData = vertexBuffer->Map();
    ASSERT_NE(vertexData, nullptr);
    std::memcpy(vertexData, vertices, sizeof(vertices));
    vertexBuffer->Unmap();

    RHIRayTracingGeometryDesc geometryDesc;
    geometryDesc.type = RHIRayTracingGeometryType::Triangles;
    geometryDesc.flags = RHIRayTracingGeometryFlags::Opaque;
    geometryDesc.triangles.vertexBuffer = vertexBuffer.Get();
    geometryDesc.triangles.vertexStride = sizeof(float) * 3;
    geometryDesc.triangles.vertexFormat = RHIFormat::RGB32_FLOAT;
    geometryDesc.triangles.vertexCount = 3;

    RHIBottomLevelASDesc blasDesc;
    blasDesc.geometries.push_back(geometryDesc);
    blasDesc.buildFlags = RHIAccelerationStructureBuildFlags::PreferFastTrace;
    blasDesc.debugName = "TraceRayHitBLASBuild";
    RHIAccelerationStructureBuildSizes blasSizes = device->GetBottomLevelASBuildSizes(blasDesc);
    ASSERT_TRUE(blasSizes.IsValid());

    RHIAccelerationStructureDesc blasResourceDesc;
    blasResourceDesc.type = RHIAccelerationStructureType::BottomLevel;
    blasResourceDesc.size = blasSizes.accelerationStructureSize;
    blasResourceDesc.debugName = "TraceRayHitBLAS";
    RHIAccelerationStructureRef blas = device->CreateAccelerationStructure(blasResourceDesc);
    ASSERT_NE(blas.Get(), nullptr);
    ASSERT_NE(blas->GetGPUVirtualAddress(), 0u);

    RHIBufferDesc blasScratchDesc;
    blasScratchDesc.size = blasSizes.buildScratchSize;
    blasScratchDesc.usage = RHIBufferUsage::UnorderedAccess | RHIBufferUsage::DeviceAddress;
    blasScratchDesc.memoryType = RHIMemoryType::Default;
    blasScratchDesc.debugName = "TraceRayHitBLASScratch";
    RHIBufferRef blasScratch = device->CreateBuffer(blasScratchDesc);
    ASSERT_NE(blasScratch.Get(), nullptr);

    RHIRayTracingInstanceDesc instanceDesc;
    instanceDesc.bottomLevel = blas.Get();
    instanceDesc.instanceId = 0;
    instanceDesc.instanceMask = 0xFF;
    instanceDesc.instanceContributionToHitGroupIndex = 0;
    RHIRayTracingInstanceRecord instanceRecord = PackRHIRayTracingInstanceRecord(
        instanceDesc,
        blas->GetGPUVirtualAddress());

    RHIBufferDesc instanceBufferDesc;
    instanceBufferDesc.size = sizeof(instanceRecord);
    instanceBufferDesc.usage = RHIBufferUsage::ShaderResource | RHIBufferUsage::AccelerationStructureInput | RHIBufferUsage::DeviceAddress;
    instanceBufferDesc.memoryType = RHIMemoryType::Upload;
    instanceBufferDesc.stride = sizeof(instanceRecord);
    instanceBufferDesc.debugName = "TraceRayHitInstanceBuffer";
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
    tlasDesc.debugName = "TraceRayHitTLASBuild";
    RHIAccelerationStructureBuildSizes tlasSizes = device->GetTopLevelASBuildSizes(tlasDesc);
    ASSERT_TRUE(tlasSizes.IsValid());

    RHIAccelerationStructureDesc tlasResourceDesc;
    tlasResourceDesc.type = RHIAccelerationStructureType::TopLevel;
    tlasResourceDesc.size = tlasSizes.accelerationStructureSize;
    tlasResourceDesc.debugName = "TraceRayHitTLAS";
    RHIAccelerationStructureRef tlas = device->CreateAccelerationStructure(tlasResourceDesc);
    ASSERT_NE(tlas.Get(), nullptr);
    ASSERT_NE(tlas->GetGPUVirtualAddress(), 0u);

    RHIBufferDesc tlasScratchDesc;
    tlasScratchDesc.size = tlasSizes.buildScratchSize;
    tlasScratchDesc.usage = RHIBufferUsage::UnorderedAccess | RHIBufferUsage::DeviceAddress;
    tlasScratchDesc.memoryType = RHIMemoryType::Default;
    tlasScratchDesc.debugName = "TraceRayHitTLASScratch";
    RHIBufferRef tlasScratch = device->CreateBuffer(tlasScratchDesc);
    ASSERT_NE(tlasScratch.Get(), nullptr);

    RHITextureDesc outputDesc = RHITextureDesc::Texture2D(
        1,
        1,
        RHIFormat::R32_UINT,
        RHITextureUsage::UnorderedAccess | RHITextureUsage::CopySrc);
    outputDesc.debugName = "TraceRayHitOutput";
    RHITextureRef outputTexture = device->CreateTexture(outputDesc);
    ASSERT_NE(outputTexture.Get(), nullptr);

    RHITextureViewDesc outputViewDesc;
    outputViewDesc.type = RHITextureViewType::UnorderedAccess;
    outputViewDesc.format = RHIFormat::R32_UINT;
    outputViewDesc.dimension = RHITextureDimension::Texture2D;
    outputViewDesc.debugName = "TraceRayHitOutputUAV";
    RHITextureViewRef outputView = device->CreateTextureView(outputTexture.Get(), outputViewDesc);
    ASSERT_NE(outputView.Get(), nullptr);

    RHIDescriptorSetDesc descriptorDesc;
    descriptorDesc.layout = setLayout.Get();
    descriptorDesc.debugName = "TraceRayHitDescriptorSet";
    descriptorDesc.BindAccelerationStructure(0, tlas.Get());
    descriptorDesc.BindTexture(1, outputView.Get());
    RHIDescriptorSetRef descriptorSet = device->CreateDescriptorSet(descriptorDesc);
    ASSERT_NE(descriptorSet.Get(), nullptr);

    RHIBufferDesc readbackDesc;
    readbackDesc.size = kReadbackRowPitch;
    readbackDesc.usage = RHIBufferUsage::CopyDst;
    readbackDesc.memoryType = RHIMemoryType::Readback;
    readbackDesc.debugName = "TraceRayHitReadback";
    RHIBufferRef readbackBuffer = device->CreateBuffer(readbackDesc);
    ASSERT_NE(readbackBuffer.Get(), nullptr);

    RHICommandContextRef ctx = device->CreateCommandContext(RHICommandQueueType::Graphics);
    ASSERT_NE(ctx.Get(), nullptr);
    ctx->Begin();
    ctx->BufferBarrier({blasScratch.Get(), RHIResourceState::Common, RHIResourceState::UnorderedAccess});
    ctx->BuildBottomLevelAccelerationStructure(blas.Get(), blasDesc, blasScratch.Get());
    ctx->BufferBarrier({tlasScratch.Get(), RHIResourceState::Common, RHIResourceState::UnorderedAccess});
    ctx->BuildTopLevelAccelerationStructure(tlas.Get(), tlasDesc, tlasScratch.Get());
    ctx->TextureBarrier(outputTexture.Get(), RHIResourceState::Common, RHIResourceState::UnorderedAccess);
    ctx->SetPipeline(pipeline.Get());
    ctx->SetDescriptorSet(0, descriptorSet.Get());
    ctx->DispatchRays({shaderTable.Get(), 1, 1, 1});
    ctx->TextureBarrier(outputTexture.Get(), RHIResourceState::UnorderedAccess, RHIResourceState::CopySource);

    RHIBufferTextureCopyDesc copyDesc;
    copyDesc.bufferRowPitch = kReadbackRowPitch;
    copyDesc.textureRegion = RHIRect{0, 0, 1, 1};
    ctx->CopyTextureToBuffer(outputTexture.Get(), readbackBuffer.Get(), copyDesc);
    ctx->End();

    RHIFenceRef fence = device->CreateFence(0);
    ASSERT_NE(fence.Get(), nullptr);
    const uint64 submittedValue = device->SubmitCommandContext(ctx.Get(), fence.Get());
    ASSERT_NE(submittedValue, 0u);
    device->WaitForFence(fence.Get(), submittedValue);
    EXPECT_GE(fence->GetCompletedValue(), submittedValue);

    void* mappedReadback = readbackBuffer->Map();
    ASSERT_NE(mappedReadback, nullptr);
    const uint32 observedPixel = *static_cast<const uint32*>(mappedReadback);
    readbackBuffer->Unmap();
    EXPECT_EQ(observedPixel, kClosestHitValue);
    device->WaitIdle();
}

TEST(DX12Validation, RayTracedShadowProductionShaderTracesTriangleAndWritesMask)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX12);

    if (!device->GetCapabilities().supportsRaytracingPipeline)
    {
        GTEST_SKIP() << "DXR ray tracing pipelines are not supported on this device";
    }

    constexpr uint32 kReadbackRowPitch = 256u;
    constexpr uint64 kConstantBufferAlignment = 256u;
    constexpr uint32 kMaterialAlphaTestFlag = 1u << 0;
    constexpr uint32 kMaterialShadowCasterFlag = 1u << 8;
    constexpr uint32 kAlphaTestEnabledFlag = 1u << 0;
    constexpr uint32 kAlphaHasResolvedBaseColorTextureFlag = 1u << 2;
    constexpr uint32 kAlphaHasUVBufferFlag = 1u << 3;
    constexpr uint32 kAlphaHasIndexBufferFlag = 1u << 4;
    constexpr uint32 kAlphaIndexFormatUInt32Flag = 1u << 5;
    constexpr uint32 kAlphaMagNearestFlag = 1u << 2;

    struct RayTracedShadowTestConstants
    {
        Mat4 inverseViewProjection = Mat4Identity();
        Mat4 previousViewProjection = Mat4Identity();
        Vec4 lightDirectionAndTMax{0.0f, 0.0f, 1.0f, 10.0f};
        Vec4 viewportSizeAndInvSize{1.0f, 1.0f, 1.0f, 1.0f};
        Vec4 depthAndBiasParams{0.0f, 0.001f, 0.0f, 0.0f};
        Vec4 historyReprojectionParams{0.01f, 0.85f, 0.0f, 0.0f};
        Vec4 softShadowParams{0.0f, 0.0f, 1.0f, 0.0f};
        Vec4 rayOptions{255.0f, 0.0f, 0.0f, 0.0f};
    };
    static_assert(sizeof(RayTracedShadowTestConstants) == 224,
                  "RayTracedShadowTestConstants must match RayTracedShadow.hlsl cbuffer layout");

    const std::filesystem::path shaderDir = FindRenderShaderDirectory();
    ASSERT_FALSE(shaderDir.empty()) << "Render/Shaders directory not found";

    PipelineCache cache;
    ASSERT_TRUE(cache.Initialize(device.get(), shaderDir.string())) << cache.GetLastError();
    RHIPipeline* pipeline = cache.GetRayTracedShadowPipeline();
    RHIShaderTable* shaderTable = cache.GetRayTracedShadowShaderTable();
    RHIDescriptorSetLayout* setLayout = cache.GetRayTracedShadowSetLayout();
    ASSERT_NE(pipeline, nullptr);
    ASSERT_NE(shaderTable, nullptr);
    ASSERT_NE(setLayout, nullptr);

    RayTracedShadowTestConstants constants;
    const uint64 constantBufferSize = AlignUp(static_cast<uint64>(sizeof(constants)), kConstantBufferAlignment);
    RHIBufferRef constantBuffer = CreateUploadBufferWithData(
        *device,
        constantBufferSize,
        RHIBufferUsage::Constant,
        0,
        &constants,
        sizeof(constants),
        "ProductionShadowConstants");
    ASSERT_NE(constantBuffer.Get(), nullptr);

    RayTracingTestAlphaMetadata alphaMetadata;
    RHIBufferRef alphaMetadataBuffer = CreateUploadBufferWithData(
        *device,
        sizeof(alphaMetadata),
        RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource,
        sizeof(RayTracingTestAlphaMetadata),
        &alphaMetadata,
        sizeof(alphaMetadata),
        "ProductionShadowAlphaMetadata");
    ASSERT_NE(alphaMetadataBuffer.Get(), nullptr);

    RayTracingTestMaterialMetadata materialMetadata;
    materialMetadata.baseColorFactor = Vec4(1.0f, 1.0f, 1.0f, 1.0f);
    materialMetadata.materialFactors = Vec4(1.0f, 1.0f, 0.5f, 1.0f);
    materialMetadata.flags = kMaterialAlphaTestFlag | kMaterialShadowCasterFlag;
    RHIBufferRef materialMetadataBuffer = CreateUploadBufferWithData(
        *device,
        sizeof(materialMetadata),
        RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource,
        sizeof(RayTracingTestMaterialMetadata),
        &materialMetadata,
        sizeof(materialMetadata),
        "ProductionShadowMaterialMetadata");
    ASSERT_NE(materialMetadataBuffer.Get(), nullptr);

    const uint32 fallbackRawBytes[64] = {};
    RHIBufferRef fallbackRawBuffer = CreateUploadBufferWithData(
        *device,
        sizeof(fallbackRawBytes),
        RHIBufferUsage::ShaderResource,
        0,
        fallbackRawBytes,
        sizeof(fallbackRawBytes),
        "ProductionShadowFallbackRawBuffer");
    ASSERT_NE(fallbackRawBuffer.Get(), nullptr);

    const uint32 alphaIndices[3] = {0u, 1u, 2u};
    RHIBufferRef alphaIndexBuffer = CreateUploadBufferWithData(
        *device,
        sizeof(alphaIndices),
        RHIBufferUsage::ShaderResource,
        0,
        alphaIndices,
        sizeof(alphaIndices),
        "ProductionShadowAlphaIndexBuffer");
    ASSERT_NE(alphaIndexBuffer.Get(), nullptr);

    const float alphaUVs[6] =
    {
        0.0f, 0.0f,
        0.0f, 0.0f,
        0.0f, 0.0f
    };
    RHIBufferRef alphaUVBuffer = CreateUploadBufferWithData(
        *device,
        sizeof(alphaUVs),
        RHIBufferUsage::ShaderResource,
        sizeof(float) * 2u,
        alphaUVs,
        sizeof(alphaUVs),
        "ProductionShadowAlphaUVBuffer");
    ASSERT_NE(alphaUVBuffer.Get(), nullptr);
    const float vertices[9] =
    {
        -1.0f, -1.0f, 2.0f,
         1.0f, -1.0f, 2.0f,
         0.0f,  1.0f, 2.0f
    };

    RHIBufferRef vertexBuffer = CreateUploadBufferWithData(
        *device,
        sizeof(vertices),
        RHIBufferUsage::Vertex | RHIBufferUsage::ShaderResource | RHIBufferUsage::AccelerationStructureInput | RHIBufferUsage::DeviceAddress,
        sizeof(float) * 3,
        vertices,
        sizeof(vertices),
        "ProductionShadowVertices");
    ASSERT_NE(vertexBuffer.Get(), nullptr);

    RHIRayTracingGeometryDesc geometryDesc;
    geometryDesc.type = RHIRayTracingGeometryType::Triangles;
    geometryDesc.flags = RHIRayTracingGeometryFlags::None;
    geometryDesc.triangles.vertexBuffer = vertexBuffer.Get();
    geometryDesc.triangles.vertexStride = sizeof(float) * 3;
    geometryDesc.triangles.vertexFormat = RHIFormat::RGB32_FLOAT;
    geometryDesc.triangles.vertexCount = 3;

    RHIBottomLevelASDesc blasDesc;
    blasDesc.geometries.push_back(geometryDesc);
    blasDesc.buildFlags = RHIAccelerationStructureBuildFlags::PreferFastTrace;
    blasDesc.debugName = "ProductionShadowBLASBuild";
    RHIAccelerationStructureBuildSizes blasSizes = device->GetBottomLevelASBuildSizes(blasDesc);
    ASSERT_TRUE(blasSizes.IsValid());

    RHIAccelerationStructureDesc blasResourceDesc;
    blasResourceDesc.type = RHIAccelerationStructureType::BottomLevel;
    blasResourceDesc.size = blasSizes.accelerationStructureSize;
    blasResourceDesc.debugName = "ProductionShadowBLAS";
    RHIAccelerationStructureRef blas = device->CreateAccelerationStructure(blasResourceDesc);
    ASSERT_NE(blas.Get(), nullptr);
    ASSERT_NE(blas->GetGPUVirtualAddress(), 0u);

    RHIBufferDesc blasScratchDesc;
    blasScratchDesc.size = blasSizes.buildScratchSize;
    blasScratchDesc.usage = RHIBufferUsage::UnorderedAccess | RHIBufferUsage::DeviceAddress;
    blasScratchDesc.memoryType = RHIMemoryType::Default;
    blasScratchDesc.debugName = "ProductionShadowBLASScratch";
    RHIBufferRef blasScratch = device->CreateBuffer(blasScratchDesc);
    ASSERT_NE(blasScratch.Get(), nullptr);

    RHIRayTracingInstanceDesc instanceDesc;
    instanceDesc.bottomLevel = blas.Get();
    instanceDesc.instanceId = 0;
    instanceDesc.instanceMask = 0xFF;
    instanceDesc.instanceContributionToHitGroupIndex = 0;
    instanceDesc.flags = RHIRayTracingInstanceFlags::TriangleCullDisable |
                         RHIRayTracingInstanceFlags::ForceNoOpaque;
    RHIRayTracingInstanceRecord instanceRecord = PackRHIRayTracingInstanceRecord(
        instanceDesc,
        blas->GetGPUVirtualAddress());

    RHIBufferRef instanceBuffer = CreateUploadBufferWithData(
        *device,
        sizeof(instanceRecord),
        RHIBufferUsage::ShaderResource | RHIBufferUsage::AccelerationStructureInput | RHIBufferUsage::DeviceAddress,
        sizeof(instanceRecord),
        &instanceRecord,
        sizeof(instanceRecord),
        "ProductionShadowInstanceBuffer");
    ASSERT_NE(instanceBuffer.Get(), nullptr);

    RHITopLevelASDesc tlasDesc;
    tlasDesc.instanceBuffer = instanceBuffer.Get();
    tlasDesc.instanceCount = 1;
    tlasDesc.buildFlags = RHIAccelerationStructureBuildFlags::PreferFastTrace;
    tlasDesc.debugName = "ProductionShadowTLASBuild";
    RHIAccelerationStructureBuildSizes tlasSizes = device->GetTopLevelASBuildSizes(tlasDesc);
    ASSERT_TRUE(tlasSizes.IsValid());

    RHIAccelerationStructureDesc tlasResourceDesc;
    tlasResourceDesc.type = RHIAccelerationStructureType::TopLevel;
    tlasResourceDesc.size = tlasSizes.accelerationStructureSize;
    tlasResourceDesc.debugName = "ProductionShadowTLAS";
    RHIAccelerationStructureRef tlas = device->CreateAccelerationStructure(tlasResourceDesc);
    ASSERT_NE(tlas.Get(), nullptr);
    ASSERT_NE(tlas->GetGPUVirtualAddress(), 0u);

    RHIBufferDesc tlasScratchDesc;
    tlasScratchDesc.size = tlasSizes.buildScratchSize;
    tlasScratchDesc.usage = RHIBufferUsage::UnorderedAccess | RHIBufferUsage::DeviceAddress;
    tlasScratchDesc.memoryType = RHIMemoryType::Default;
    tlasScratchDesc.debugName = "ProductionShadowTLASScratch";
    RHIBufferRef tlasScratch = device->CreateBuffer(tlasScratchDesc);
    ASSERT_NE(tlasScratch.Get(), nullptr);

    auto createTexture = [&](RHIFormat format, RHITextureUsage usage, const char* debugName) -> RHITextureRef
    {
        RHITextureDesc textureDesc = RHITextureDesc::Texture2D(1, 1, format, usage);
        textureDesc.debugName = debugName;
        RHITextureRef texture = device->CreateTexture(textureDesc);
        if (!texture)
        {
            ADD_FAILURE() << "Failed to create texture: " << debugName;
        }
        return texture;
    };

    RHITextureRef shadowMaskTexture = createTexture(
        RHIFormat::R32_FLOAT,
        RHITextureUsage::UnorderedAccess | RHITextureUsage::CopySrc,
        "ProductionShadowMask");
    RHITextureRef sceneDepthTexture = createTexture(
        RHIFormat::R32_FLOAT,
        RHITextureUsage::ShaderResource | RHITextureUsage::CopyDst,
        "ProductionShadowSceneDepth");
    RHITextureRef previousShadowTexture = createTexture(
        RHIFormat::R32_FLOAT,
        RHITextureUsage::ShaderResource | RHITextureUsage::CopyDst,
        "ProductionShadowPreviousMask");
    RHITextureRef previousDepthTexture = createTexture(
        RHIFormat::R32_FLOAT,
        RHITextureUsage::ShaderResource | RHITextureUsage::CopyDst,
        "ProductionShadowPreviousDepth");
    RHITextureRef currentDepthTexture = createTexture(
        RHIFormat::R32_FLOAT,
        RHITextureUsage::UnorderedAccess,
        "ProductionShadowCurrentDepth");
    RHITextureRef previousNormalTexture = createTexture(
        RHIFormat::RGBA32_FLOAT,
        RHITextureUsage::ShaderResource | RHITextureUsage::CopyDst,
        "ProductionShadowPreviousNormal");
    RHITextureRef currentNormalTexture = createTexture(
        RHIFormat::RGBA32_FLOAT,
        RHITextureUsage::UnorderedAccess,
        "ProductionShadowCurrentNormal");
    RHITextureRef velocityTexture = createTexture(
        RHIFormat::RG32_FLOAT,
        RHITextureUsage::ShaderResource | RHITextureUsage::CopyDst,
        "ProductionShadowVelocity");
    RHITextureRef fallbackTexture = createTexture(
        RHIFormat::RGBA32_FLOAT,
        RHITextureUsage::ShaderResource | RHITextureUsage::CopyDst,
        "ProductionShadowFallbackTexture");
    RHITextureRef alphaTexture = createTexture(
        RHIFormat::RGBA32_FLOAT,
        RHITextureUsage::ShaderResource | RHITextureUsage::CopyDst,
        "ProductionShadowAlphaTexture");
    ASSERT_NE(shadowMaskTexture.Get(), nullptr);
    ASSERT_NE(sceneDepthTexture.Get(), nullptr);
    ASSERT_NE(previousShadowTexture.Get(), nullptr);
    ASSERT_NE(previousDepthTexture.Get(), nullptr);
    ASSERT_NE(currentDepthTexture.Get(), nullptr);
    ASSERT_NE(previousNormalTexture.Get(), nullptr);
    ASSERT_NE(currentNormalTexture.Get(), nullptr);
    ASSERT_NE(velocityTexture.Get(), nullptr);
    ASSERT_NE(fallbackTexture.Get(), nullptr);
    ASSERT_NE(alphaTexture.Get(), nullptr);

    RHITextureViewRef shadowMaskUAV = CreateTestTextureView(
        *device,
        shadowMaskTexture.Get(),
        RHITextureViewType::UnorderedAccess,
        RHIFormat::R32_FLOAT,
        "ProductionShadowMaskUAV");
    RHITextureViewRef sceneDepthSRV = CreateTestTextureView(
        *device,
        sceneDepthTexture.Get(),
        RHITextureViewType::ShaderResource,
        RHIFormat::R32_FLOAT,
        "ProductionShadowSceneDepthSRV");
    RHITextureViewRef previousShadowSRV = CreateTestTextureView(
        *device,
        previousShadowTexture.Get(),
        RHITextureViewType::ShaderResource,
        RHIFormat::R32_FLOAT,
        "ProductionShadowPreviousMaskSRV");
    RHITextureViewRef previousDepthSRV = CreateTestTextureView(
        *device,
        previousDepthTexture.Get(),
        RHITextureViewType::ShaderResource,
        RHIFormat::R32_FLOAT,
        "ProductionShadowPreviousDepthSRV");
    RHITextureViewRef currentDepthUAV = CreateTestTextureView(
        *device,
        currentDepthTexture.Get(),
        RHITextureViewType::UnorderedAccess,
        RHIFormat::R32_FLOAT,
        "ProductionShadowCurrentDepthUAV");
    RHITextureViewRef previousNormalSRV = CreateTestTextureView(
        *device,
        previousNormalTexture.Get(),
        RHITextureViewType::ShaderResource,
        RHIFormat::RGBA32_FLOAT,
        "ProductionShadowPreviousNormalSRV");
    RHITextureViewRef currentNormalUAV = CreateTestTextureView(
        *device,
        currentNormalTexture.Get(),
        RHITextureViewType::UnorderedAccess,
        RHIFormat::RGBA32_FLOAT,
        "ProductionShadowCurrentNormalUAV");
    RHITextureViewRef velocitySRV = CreateTestTextureView(
        *device,
        velocityTexture.Get(),
        RHITextureViewType::ShaderResource,
        RHIFormat::RG32_FLOAT,
        "ProductionShadowVelocitySRV");
    RHITextureViewRef fallbackTextureSRV = CreateTestTextureView(
        *device,
        fallbackTexture.Get(),
        RHITextureViewType::ShaderResource,
        RHIFormat::RGBA32_FLOAT,
        "ProductionShadowFallbackTextureSRV");
    RHITextureViewRef alphaTextureSRV = CreateTestTextureView(
        *device,
        alphaTexture.Get(),
        RHITextureViewType::ShaderResource,
        RHIFormat::RGBA32_FLOAT,
        "ProductionShadowAlphaTextureSRV");
    ASSERT_NE(shadowMaskUAV.Get(), nullptr);
    ASSERT_NE(sceneDepthSRV.Get(), nullptr);
    ASSERT_NE(previousShadowSRV.Get(), nullptr);
    ASSERT_NE(previousDepthSRV.Get(), nullptr);
    ASSERT_NE(currentDepthUAV.Get(), nullptr);
    ASSERT_NE(previousNormalSRV.Get(), nullptr);
    ASSERT_NE(currentNormalUAV.Get(), nullptr);
    ASSERT_NE(velocitySRV.Get(), nullptr);
    ASSERT_NE(fallbackTextureSRV.Get(), nullptr);
    ASSERT_NE(alphaTextureSRV.Get(), nullptr);

    const float sceneDepth = 0.5f;
    const float previousShadow = 1.0f;
    const float previousDepth = 0.5f;
    const float previousNormal[4] = {0.5f, 0.5f, 1.0f, 1.0f};
    const float velocity[2] = {0.0f, 0.0f};
    const float fallbackTexel[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    const float alphaCutoutTexel[4] = {1.0f, 1.0f, 1.0f, 0.0f};
    RHIBufferRef sceneDepthUpload = CreateUploadBufferWithData(
        *device,
        kReadbackRowPitch,
        RHIBufferUsage::CopySrc,
        0,
        &sceneDepth,
        sizeof(sceneDepth),
        "ProductionShadowSceneDepthUpload");
    RHIBufferRef previousShadowUpload = CreateUploadBufferWithData(
        *device,
        kReadbackRowPitch,
        RHIBufferUsage::CopySrc,
        0,
        &previousShadow,
        sizeof(previousShadow),
        "ProductionShadowPreviousMaskUpload");
    RHIBufferRef previousDepthUpload = CreateUploadBufferWithData(
        *device,
        kReadbackRowPitch,
        RHIBufferUsage::CopySrc,
        0,
        &previousDepth,
        sizeof(previousDepth),
        "ProductionShadowPreviousDepthUpload");
    RHIBufferRef previousNormalUpload = CreateUploadBufferWithData(
        *device,
        kReadbackRowPitch,
        RHIBufferUsage::CopySrc,
        0,
        previousNormal,
        sizeof(previousNormal),
        "ProductionShadowPreviousNormalUpload");
    RHIBufferRef velocityUpload = CreateUploadBufferWithData(
        *device,
        kReadbackRowPitch,
        RHIBufferUsage::CopySrc,
        0,
        velocity,
        sizeof(velocity),
        "ProductionShadowVelocityUpload");
    RHIBufferRef fallbackTextureUpload = CreateUploadBufferWithData(
        *device,
        kReadbackRowPitch,
        RHIBufferUsage::CopySrc,
        0,
        fallbackTexel,
        sizeof(fallbackTexel),
        "ProductionShadowFallbackTextureUpload");
    RHIBufferRef alphaTextureUpload = CreateUploadBufferWithData(
        *device,
        kReadbackRowPitch,
        RHIBufferUsage::CopySrc,
        0,
        alphaCutoutTexel,
        sizeof(alphaCutoutTexel),
        "ProductionShadowAlphaTextureUpload");
    ASSERT_NE(sceneDepthUpload.Get(), nullptr);
    ASSERT_NE(previousShadowUpload.Get(), nullptr);
    ASSERT_NE(previousDepthUpload.Get(), nullptr);
    ASSERT_NE(previousNormalUpload.Get(), nullptr);
    ASSERT_NE(velocityUpload.Get(), nullptr);
    ASSERT_NE(fallbackTextureUpload.Get(), nullptr);
    ASSERT_NE(alphaTextureUpload.Get(), nullptr);

    RHIDescriptorSetDesc descriptorDesc;
    descriptorDesc.layout = setLayout;
    descriptorDesc.debugName = "ProductionShadowDescriptorSet";
    descriptorDesc.BindAccelerationStructure(RTShadowBindings::RVX_RT_SHADOW_TLAS_BINDING, tlas.Get());
    descriptorDesc.BindTexture(RTShadowBindings::RVX_RT_SHADOW_OUTPUT_MASK_BINDING, shadowMaskUAV.Get());
    descriptorDesc.BindTexture(RTShadowBindings::RVX_RT_SHADOW_SCENE_DEPTH_BINDING, sceneDepthSRV.Get());
    descriptorDesc.BindBuffer(RTShadowBindings::RVX_RT_SHADOW_CONSTANTS_BINDING, constantBuffer.Get(), 0, constantBufferSize);
    descriptorDesc.BindTexture(RTShadowBindings::RVX_RT_SHADOW_PREVIOUS_MASK_BINDING, previousShadowSRV.Get());
    descriptorDesc.BindTexture(RTShadowBindings::RVX_RT_SHADOW_PREVIOUS_DEPTH_BINDING, previousDepthSRV.Get());
    descriptorDesc.BindTexture(RTShadowBindings::RVX_RT_SHADOW_OUTPUT_DEPTH_BINDING, currentDepthUAV.Get());
    descriptorDesc.BindTexture(RTShadowBindings::RVX_RT_SHADOW_PREVIOUS_NORMAL_BINDING, previousNormalSRV.Get());
    descriptorDesc.BindTexture(RTShadowBindings::RVX_RT_SHADOW_OUTPUT_NORMAL_BINDING, currentNormalUAV.Get());
    descriptorDesc.BindBuffer(RTShadowBindings::RVX_RT_SHADOW_ALPHA_METADATA_BINDING, alphaMetadataBuffer.Get());
    for (uint32 textureIndex = 0; textureIndex < RTShadowBindings::RVX_RT_SHADOW_MAX_ALPHA_TEXTURES; ++textureIndex)
    {
        descriptorDesc.BindTexture(RTShadowBindings::RVX_RT_SHADOW_ALPHA_TEXTURES_BINDING,
                                   textureIndex == 0u ? alphaTextureSRV.Get() : fallbackTextureSRV.Get(),
                                   textureIndex);
    }
    for (uint32 bufferIndex = 0; bufferIndex < RTShadowBindings::RVX_RT_SHADOW_MAX_ALPHA_GEOMETRY_BUFFERS; ++bufferIndex)
    {
        descriptorDesc.BindBuffer(RTShadowBindings::RVX_RT_SHADOW_ALPHA_INDEX_BUFFERS_BINDING,
                                  bufferIndex == 0u ? alphaIndexBuffer.Get() : fallbackRawBuffer.Get(),
                                  0,
                                  RVX_WHOLE_SIZE,
                                  bufferIndex);
        descriptorDesc.BindBuffer(RTShadowBindings::RVX_RT_SHADOW_ALPHA_UV_BUFFERS_BINDING,
                                  bufferIndex == 0u ? alphaUVBuffer.Get() : fallbackRawBuffer.Get(),
                                  0,
                                  RVX_WHOLE_SIZE,
                                  bufferIndex);
    }
    descriptorDesc.BindBuffer(RTShadowBindings::RVX_RT_SHADOW_MATERIAL_METADATA_BINDING, materialMetadataBuffer.Get());
    for (uint32 textureIndex = 0; textureIndex < RTShadowBindings::RVX_RT_SHADOW_MAX_MATERIAL_TEXTURES; ++textureIndex)
    {
        descriptorDesc.BindTexture(RTShadowBindings::RVX_RT_SHADOW_MATERIAL_TEXTURES_BINDING, fallbackTextureSRV.Get(), textureIndex);
    }
    descriptorDesc.BindTexture(RTShadowBindings::RVX_RT_SHADOW_SCENE_VELOCITY_BINDING, velocitySRV.Get());
    RHIDescriptorSetRef descriptorSet = device->CreateDescriptorSet(descriptorDesc);
    ASSERT_NE(descriptorSet.Get(), nullptr);

    RHIBufferDesc readbackDesc;
    readbackDesc.size = kReadbackRowPitch;
    readbackDesc.usage = RHIBufferUsage::CopyDst;
    readbackDesc.memoryType = RHIMemoryType::Readback;
    readbackDesc.debugName = "ProductionShadowReadback";
    RHIBufferRef readbackBuffer = device->CreateBuffer(readbackDesc);
    ASSERT_NE(readbackBuffer.Get(), nullptr);

    auto writeMaterialMetadata = [&](float alpha, const char* debugName) -> bool
    {
        RayTracingTestMaterialMetadata updatedMaterialMetadata = materialMetadata;
        updatedMaterialMetadata.baseColorFactor = Vec4(1.0f, 1.0f, 1.0f, alpha);
        updatedMaterialMetadata.materialFactors = Vec4(1.0f, 1.0f, 0.5f, 1.0f);
        updatedMaterialMetadata.flags = kMaterialAlphaTestFlag | kMaterialShadowCasterFlag;

        void* mappedMaterial = materialMetadataBuffer->Map();
        if (!mappedMaterial)
        {
            ADD_FAILURE() << "Failed to map production shadow material metadata for " << debugName;
            return false;
        }

        std::memcpy(mappedMaterial, &updatedMaterialMetadata, sizeof(updatedMaterialMetadata));
        materialMetadataBuffer->Unmap();
        return true;
    };

    auto writeAlphaTextureMetadata = [&](bool enabled, const char* debugName) -> bool
    {
        RayTracingTestAlphaMetadata updatedAlphaMetadata = alphaMetadata;
        if (enabled)
        {
            updatedAlphaMetadata.flags = kAlphaTestEnabledFlag |
                                         kAlphaHasResolvedBaseColorTextureFlag |
                                         kAlphaHasUVBufferFlag |
                                         kAlphaHasIndexBufferFlag |
                                         kAlphaIndexFormatUInt32Flag;
            updatedAlphaMetadata.alphaCutoff = 0.5f;
            updatedAlphaMetadata.baseColorAlpha = 1.0f;
            updatedAlphaMetadata.baseColorTextureTableIndex = 0u;
            updatedAlphaMetadata.indexBufferTableIndex = 0u;
            updatedAlphaMetadata.uvBufferTableIndex = 0u;
            updatedAlphaMetadata.baseColorSamplerFlags = kAlphaMagNearestFlag;
        }

        void* mappedAlpha = alphaMetadataBuffer->Map();
        if (!mappedAlpha)
        {
            ADD_FAILURE() << "Failed to map production shadow alpha metadata for " << debugName;
            return false;
        }

        std::memcpy(mappedAlpha, &updatedAlphaMetadata, sizeof(updatedAlphaMetadata));
        alphaMetadataBuffer->Unmap();
        return true;
    };
    bool accelerationStructuresBuilt = false;
    RHIResourceState shadowMaskState = RHIResourceState::Common;
    auto dispatchAndReadVisibility = [&](const char* debugName) -> float
    {
        RHICommandContextRef ctx = device->CreateCommandContext(RHICommandQueueType::Graphics);
        if (!ctx)
        {
            ADD_FAILURE() << "Failed to create production shadow command context for " << debugName;
            return -1.0f;
        }

        auto copyUploadToTexture = [&](RHIBuffer* uploadBuffer, RHITexture* texture)
        {
            ctx->TextureBarrier(texture, RHIResourceState::Common, RHIResourceState::CopyDest);
            RHIBufferTextureCopyDesc copyDesc;
            copyDesc.bufferRowPitch = kReadbackRowPitch;
            copyDesc.textureRegion = RHIRect{0, 0, 1, 1};
            ctx->CopyBufferToTexture(uploadBuffer, texture, copyDesc);
            ctx->TextureBarrier(texture, RHIResourceState::CopyDest, RHIResourceState::ShaderResource);
        };

        ctx->Begin();
        if (!accelerationStructuresBuilt)
        {
            copyUploadToTexture(sceneDepthUpload.Get(), sceneDepthTexture.Get());
            copyUploadToTexture(previousShadowUpload.Get(), previousShadowTexture.Get());
            copyUploadToTexture(previousDepthUpload.Get(), previousDepthTexture.Get());
            copyUploadToTexture(previousNormalUpload.Get(), previousNormalTexture.Get());
            copyUploadToTexture(velocityUpload.Get(), velocityTexture.Get());
            copyUploadToTexture(fallbackTextureUpload.Get(), fallbackTexture.Get());
            copyUploadToTexture(alphaTextureUpload.Get(), alphaTexture.Get());
            ctx->TextureBarrier(shadowMaskTexture.Get(), shadowMaskState, RHIResourceState::UnorderedAccess);
            ctx->TextureBarrier(currentDepthTexture.Get(), RHIResourceState::Common, RHIResourceState::UnorderedAccess);
            ctx->TextureBarrier(currentNormalTexture.Get(), RHIResourceState::Common, RHIResourceState::UnorderedAccess);
            ctx->BufferBarrier({blasScratch.Get(), RHIResourceState::Common, RHIResourceState::UnorderedAccess});
            ctx->BuildBottomLevelAccelerationStructure(blas.Get(), blasDesc, blasScratch.Get());
            ctx->BufferBarrier({tlasScratch.Get(), RHIResourceState::Common, RHIResourceState::UnorderedAccess});
            ctx->BuildTopLevelAccelerationStructure(tlas.Get(), tlasDesc, tlasScratch.Get());
            accelerationStructuresBuilt = true;
        }
        else
        {
            ctx->TextureBarrier(shadowMaskTexture.Get(), shadowMaskState, RHIResourceState::UnorderedAccess);
        }

        ctx->SetPipeline(pipeline);
        ctx->SetDescriptorSet(0, descriptorSet.Get());
        ctx->DispatchRays({shaderTable, 1, 1, 1});
        ctx->TextureBarrier(shadowMaskTexture.Get(), RHIResourceState::UnorderedAccess, RHIResourceState::CopySource);
        shadowMaskState = RHIResourceState::CopySource;

        RHIBufferTextureCopyDesc readbackCopy;
        readbackCopy.bufferRowPitch = kReadbackRowPitch;
        readbackCopy.textureRegion = RHIRect{0, 0, 1, 1};
        ctx->CopyTextureToBuffer(shadowMaskTexture.Get(), readbackBuffer.Get(), readbackCopy);
        ctx->End();

        RHIFenceRef fence = device->CreateFence(0);
        if (!fence)
        {
            ADD_FAILURE() << "Failed to create production shadow fence for " << debugName;
            return -1.0f;
        }

        const uint64 submittedValue = device->SubmitCommandContext(ctx.Get(), fence.Get());
        if (submittedValue == 0u)
        {
            ADD_FAILURE() << "Failed to submit production shadow command context for " << debugName;
            return -1.0f;
        }

        device->WaitForFence(fence.Get(), submittedValue);
        EXPECT_GE(fence->GetCompletedValue(), submittedValue);

        void* mappedReadback = readbackBuffer->Map();
        if (!mappedReadback)
        {
            ADD_FAILURE() << "Failed to map production shadow readback for " << debugName;
            return -1.0f;
        }

        const float observedVisibility = *static_cast<const float*>(mappedReadback);
        readbackBuffer->Unmap();
        return observedVisibility;
    };

    ASSERT_TRUE(writeMaterialMetadata(1.0f, "alpha-pass"));
    EXPECT_NEAR(dispatchAndReadVisibility("alpha-pass"), 0.0f, 1.0e-4f);

    ASSERT_TRUE(writeMaterialMetadata(0.0f, "alpha-cutout"));
    EXPECT_NEAR(dispatchAndReadVisibility("alpha-cutout"), 1.0f, 1.0e-4f);

    ASSERT_TRUE(writeMaterialMetadata(1.0f, "texture-alpha-cutout"));
    ASSERT_TRUE(writeAlphaTextureMetadata(true, "texture-alpha-cutout"));
    EXPECT_NEAR(dispatchAndReadVisibility("texture-alpha-cutout"), 1.0f, 1.0e-4f);

    device->WaitIdle();
    cache.Shutdown();
}
TEST(DX12Validation, RayTracedReflectionProductionShaderTracesTriangleAndWritesMaterialResponse)
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX12);

    if (!device->GetCapabilities().supportsRaytracingPipeline)
    {
        GTEST_SKIP() << "DXR ray tracing pipelines are not supported on this device";
    }

    constexpr uint32 kUploadRowPitch = 256u;
    constexpr uint32 kSceneWidth = 3u;
    constexpr uint32 kSceneHeight = 3u;
    constexpr uint32 kMaterialAlphaTestFlag = 1u << 0;
    constexpr uint32 kMaterialBaseColorTextureFlag = 1u << 3;
    constexpr uint32 kMaterialMetallicRoughnessTextureFlag = 1u << 4;
    constexpr uint32 kMaterialNormalTextureFlag = 1u << 5;
    constexpr uint32 kMaterialEmissiveTextureFlag = 1u << 6;
    constexpr uint32 kAlphaHasUVBufferFlag = 1u << 3;
    constexpr uint32 kAlphaHasIndexBufferFlag = 1u << 4;
    constexpr uint32 kAlphaIndexFormatUInt32Flag = 1u << 5;
    constexpr uint32 kAlphaHasNormalBufferFlag = 1u << 7;
    constexpr uint32 kAlphaHasTangentBufferFlag = 1u << 8;
    constexpr uint32 kAlphaMagNearestFlag = 1u << 2;
    constexpr uint32 kPBRBaseColorTextureIndex = 0u;
    constexpr uint32 kPBRMetallicRoughnessTextureIndex = 1u;
    constexpr uint32 kPBRNormalTextureIndex = 2u;
    constexpr uint32 kPBREmissiveTextureIndex = 3u;
    constexpr uint32 kAlphaCutoutTextureIndex = 4u;
    constexpr float kBaseColorR = 0.8f;
    constexpr float kBaseColorG = 0.4f;
    constexpr float kBaseColorB = 0.2f;
    constexpr float kRoughness = 0.04f;
    constexpr float kSpecularVisibility = (1.0f - kRoughness) * (1.0f - kRoughness);
    constexpr float kExpectedSpecular = 0.04f * kSpecularVisibility;
    constexpr float kExpectedR = kBaseColorR * 0.96f + kExpectedSpecular;
    constexpr float kExpectedG = kBaseColorG * 0.96f + kExpectedSpecular;
    constexpr float kExpectedB = kBaseColorB * 0.96f + kExpectedSpecular;

    struct RayTracedReflectionTestConstants
    {
        Mat4 inverseViewProjection = Mat4Identity();
        Mat4 previousViewProjection = Mat4Identity();
        Vec4 cameraPositionAndTMax{0.0f, 0.0f, 1.5f, 10.0f};
        Vec4 outputSizeAndInvSize{1.0f, 1.0f, 1.0f, 1.0f};
        Vec4 sceneSizeAndInvSize{3.0f, 3.0f, 1.0f / 3.0f, 1.0f / 3.0f};
        Vec4 reflectionOptions{1.0f, 1.0f, 255.0f, 1.0f};
        Vec4 historyParams{0.0f, 0.85f, 0.01f, 0.85f};
        Vec4 stochasticParams{1.0f, 0.0f, 0.0f, 0.0f};
        Vec4 rayBiasParams{0.001f, 0.001f, 64.0f, 0.0f};
        Vec4 historyClampParams{4.0f, 0.05f, 0.0f, 8.0f};
    };
    static_assert(sizeof(RayTracedReflectionTestConstants) == 256,
                  "RayTracedReflectionTestConstants must match RayTracedReflection.hlsl cbuffer layout");

    const std::filesystem::path shaderDir = FindRenderShaderDirectory();
    ASSERT_FALSE(shaderDir.empty()) << "Render/Shaders directory not found";

    PipelineCache cache;
    ASSERT_TRUE(cache.Initialize(device.get(), shaderDir.string())) << cache.GetLastError();
    RHIPipeline* pipeline = cache.GetRayTracedReflectionPipeline();
    RHIShaderTable* shaderTable = cache.GetRayTracedReflectionShaderTable();
    RHIDescriptorSetLayout* setLayout = cache.GetRayTracedReflectionSetLayout();
    ASSERT_NE(pipeline, nullptr);
    ASSERT_NE(shaderTable, nullptr);
    ASSERT_NE(setLayout, nullptr);

    RayTracedReflectionTestConstants constants;
    RHIBufferRef constantBuffer = CreateUploadBufferWithData(
        *device,
        sizeof(constants),
        RHIBufferUsage::Constant,
        0,
        &constants,
        sizeof(constants),
        "ProductionReflectionConstants");
    ASSERT_NE(constantBuffer.Get(), nullptr);

    RayTracingTestMaterialMetadata materialMetadata;
    materialMetadata.baseColorFactor = Vec4(kBaseColorR, kBaseColorG, kBaseColorB, 1.0f);
    materialMetadata.materialFactors = Vec4(0.0f, kRoughness, 0.5f, 1.0f);
    materialMetadata.flags = kMaterialAlphaTestFlag;
    RHIBufferRef materialMetadataBuffer = CreateUploadBufferWithData(
        *device,
        sizeof(materialMetadata),
        RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource,
        sizeof(RayTracingTestMaterialMetadata),
        &materialMetadata,
        sizeof(materialMetadata),
        "ProductionReflectionMaterialMetadata");
    ASSERT_NE(materialMetadataBuffer.Get(), nullptr);

    RayTracingTestAlphaMetadata geometryMetadata;
    RHIBufferRef geometryMetadataBuffer = CreateUploadBufferWithData(
        *device,
        sizeof(geometryMetadata),
        RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource,
        sizeof(RayTracingTestAlphaMetadata),
        &geometryMetadata,
        sizeof(geometryMetadata),
        "ProductionReflectionGeometryMetadata");
    ASSERT_NE(geometryMetadataBuffer.Get(), nullptr);

    const uint32 fallbackRawBytes[64] = {};
    RHIBufferRef fallbackRawBuffer = CreateUploadBufferWithData(
        *device,
        sizeof(fallbackRawBytes),
        RHIBufferUsage::ShaderResource,
        0,
        fallbackRawBytes,
        sizeof(fallbackRawBytes),
        "ProductionReflectionFallbackRawBuffer");
    ASSERT_NE(fallbackRawBuffer.Get(), nullptr);

    const uint32 reflectionIndices[3] = {0u, 1u, 2u};
    RHIBufferRef reflectionIndexBuffer = CreateUploadBufferWithData(
        *device,
        sizeof(reflectionIndices),
        RHIBufferUsage::ShaderResource,
        0,
        reflectionIndices,
        sizeof(reflectionIndices),
        "ProductionReflectionIndexBuffer");
    ASSERT_NE(reflectionIndexBuffer.Get(), nullptr);

    const float reflectionUVs[6] =
    {
        0.0f, 0.0f,
        0.0f, 0.0f,
        0.0f, 0.0f
    };
    RHIBufferRef reflectionUVBuffer = CreateUploadBufferWithData(
        *device,
        sizeof(reflectionUVs),
        RHIBufferUsage::ShaderResource,
        sizeof(float) * 2u,
        reflectionUVs,
        sizeof(reflectionUVs),
        "ProductionReflectionUVBuffer");
    ASSERT_NE(reflectionUVBuffer.Get(), nullptr);

    const float reflectionNormals[9] =
    {
        0.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 1.0f
    };
    RHIBufferRef reflectionNormalBuffer = CreateUploadBufferWithData(
        *device,
        sizeof(reflectionNormals),
        RHIBufferUsage::ShaderResource,
        sizeof(float) * 3u,
        reflectionNormals,
        sizeof(reflectionNormals),
        "ProductionReflectionNormalBuffer");
    ASSERT_NE(reflectionNormalBuffer.Get(), nullptr);

    const float reflectionTangents[12] =
    {
        1.0f, 0.0f, 0.0f, 1.0f,
        1.0f, 0.0f, 0.0f, 1.0f,
        1.0f, 0.0f, 0.0f, 1.0f
    };
    RHIBufferRef reflectionTangentBuffer = CreateUploadBufferWithData(
        *device,
        sizeof(reflectionTangents),
        RHIBufferUsage::ShaderResource,
        sizeof(float) * 4u,
        reflectionTangents,
        sizeof(reflectionTangents),
        "ProductionReflectionTangentBuffer");
    ASSERT_NE(reflectionTangentBuffer.Get(), nullptr);
    const float vertices[9] =
    {
        -1.0f, -1.0f, 2.0f,
         1.0f, -1.0f, 2.0f,
         0.0f,  1.0f, 2.0f
    };

    RHIBufferRef vertexBuffer = CreateUploadBufferWithData(
        *device,
        sizeof(vertices),
        RHIBufferUsage::Vertex | RHIBufferUsage::ShaderResource | RHIBufferUsage::AccelerationStructureInput | RHIBufferUsage::DeviceAddress,
        sizeof(float) * 3,
        vertices,
        sizeof(vertices),
        "ProductionReflectionVertices");
    ASSERT_NE(vertexBuffer.Get(), nullptr);

    RHIRayTracingGeometryDesc geometryDesc;
    geometryDesc.type = RHIRayTracingGeometryType::Triangles;
    geometryDesc.flags = RHIRayTracingGeometryFlags::None;
    geometryDesc.triangles.vertexBuffer = vertexBuffer.Get();
    geometryDesc.triangles.vertexStride = sizeof(float) * 3;
    geometryDesc.triangles.vertexFormat = RHIFormat::RGB32_FLOAT;
    geometryDesc.triangles.vertexCount = 3;

    RHIBottomLevelASDesc blasDesc;
    blasDesc.geometries.push_back(geometryDesc);
    blasDesc.buildFlags = RHIAccelerationStructureBuildFlags::PreferFastTrace;
    blasDesc.debugName = "ProductionReflectionBLASBuild";
    RHIAccelerationStructureBuildSizes blasSizes = device->GetBottomLevelASBuildSizes(blasDesc);
    ASSERT_TRUE(blasSizes.IsValid());

    RHIAccelerationStructureDesc blasResourceDesc;
    blasResourceDesc.type = RHIAccelerationStructureType::BottomLevel;
    blasResourceDesc.size = blasSizes.accelerationStructureSize;
    blasResourceDesc.debugName = "ProductionReflectionBLAS";
    RHIAccelerationStructureRef blas = device->CreateAccelerationStructure(blasResourceDesc);
    ASSERT_NE(blas.Get(), nullptr);
    ASSERT_NE(blas->GetGPUVirtualAddress(), 0u);

    RHIBufferDesc blasScratchDesc;
    blasScratchDesc.size = blasSizes.buildScratchSize;
    blasScratchDesc.usage = RHIBufferUsage::UnorderedAccess | RHIBufferUsage::DeviceAddress;
    blasScratchDesc.memoryType = RHIMemoryType::Default;
    blasScratchDesc.debugName = "ProductionReflectionBLASScratch";
    RHIBufferRef blasScratch = device->CreateBuffer(blasScratchDesc);
    ASSERT_NE(blasScratch.Get(), nullptr);

    RHIRayTracingInstanceDesc instanceDesc;
    instanceDesc.bottomLevel = blas.Get();
    instanceDesc.instanceId = 0;
    instanceDesc.instanceMask = 0xFF;
    instanceDesc.instanceContributionToHitGroupIndex = 0;
    instanceDesc.flags = RHIRayTracingInstanceFlags::TriangleCullDisable |
                         RHIRayTracingInstanceFlags::ForceNoOpaque;
    RHIRayTracingInstanceRecord instanceRecord = PackRHIRayTracingInstanceRecord(
        instanceDesc,
        blas->GetGPUVirtualAddress());

    RHIBufferRef instanceBuffer = CreateUploadBufferWithData(
        *device,
        sizeof(instanceRecord),
        RHIBufferUsage::ShaderResource | RHIBufferUsage::AccelerationStructureInput | RHIBufferUsage::DeviceAddress,
        sizeof(instanceRecord),
        &instanceRecord,
        sizeof(instanceRecord),
        "ProductionReflectionInstanceBuffer");
    ASSERT_NE(instanceBuffer.Get(), nullptr);

    RHITopLevelASDesc tlasDesc;
    tlasDesc.instanceBuffer = instanceBuffer.Get();
    tlasDesc.instanceCount = 1;
    tlasDesc.buildFlags = RHIAccelerationStructureBuildFlags::PreferFastTrace;
    tlasDesc.debugName = "ProductionReflectionTLASBuild";
    RHIAccelerationStructureBuildSizes tlasSizes = device->GetTopLevelASBuildSizes(tlasDesc);
    ASSERT_TRUE(tlasSizes.IsValid());

    RHIAccelerationStructureDesc tlasResourceDesc;
    tlasResourceDesc.type = RHIAccelerationStructureType::TopLevel;
    tlasResourceDesc.size = tlasSizes.accelerationStructureSize;
    tlasResourceDesc.debugName = "ProductionReflectionTLAS";
    RHIAccelerationStructureRef tlas = device->CreateAccelerationStructure(tlasResourceDesc);
    ASSERT_NE(tlas.Get(), nullptr);
    ASSERT_NE(tlas->GetGPUVirtualAddress(), 0u);

    RHIBufferDesc tlasScratchDesc;
    tlasScratchDesc.size = tlasSizes.buildScratchSize;
    tlasScratchDesc.usage = RHIBufferUsage::UnorderedAccess | RHIBufferUsage::DeviceAddress;
    tlasScratchDesc.memoryType = RHIMemoryType::Default;
    tlasScratchDesc.debugName = "ProductionReflectionTLASScratch";
    RHIBufferRef tlasScratch = device->CreateBuffer(tlasScratchDesc);
    ASSERT_NE(tlasScratch.Get(), nullptr);

    auto createTexture = [&](uint32 width,
                             uint32 height,
                             RHIFormat format,
                             RHITextureUsage usage,
                             const char* debugName) -> RHITextureRef
    {
        RHITextureDesc textureDesc = RHITextureDesc::Texture2D(width, height, format, usage);
        textureDesc.debugName = debugName;
        RHITextureRef texture = device->CreateTexture(textureDesc);
        if (!texture)
        {
            ADD_FAILURE() << "Failed to create texture: " << debugName;
        }
        return texture;
    };

    RHITextureRef reflectionTexture = createTexture(
        1,
        1,
        RHIFormat::RGBA32_FLOAT,
        RHITextureUsage::UnorderedAccess | RHITextureUsage::CopySrc,
        "ProductionReflectionOutput");
    RHITextureRef sceneColorTexture = createTexture(
        kSceneWidth,
        kSceneHeight,
        RHIFormat::RGBA32_FLOAT,
        RHITextureUsage::ShaderResource | RHITextureUsage::CopyDst,
        "ProductionReflectionSceneColor");
    RHITextureRef sceneDepthTexture = createTexture(
        kSceneWidth,
        kSceneHeight,
        RHIFormat::R32_FLOAT,
        RHITextureUsage::ShaderResource | RHITextureUsage::CopyDst,
        "ProductionReflectionSceneDepth");
    RHITextureRef previousReflectionTexture = createTexture(
        1,
        1,
        RHIFormat::RGBA32_FLOAT,
        RHITextureUsage::ShaderResource | RHITextureUsage::CopyDst,
        "ProductionReflectionPreviousHistory");
    RHITextureRef previousDepthTexture = createTexture(
        1,
        1,
        RHIFormat::R32_FLOAT,
        RHITextureUsage::ShaderResource | RHITextureUsage::CopyDst,
        "ProductionReflectionPreviousDepth");
    RHITextureRef currentDepthTexture = createTexture(
        1,
        1,
        RHIFormat::R32_FLOAT,
        RHITextureUsage::UnorderedAccess,
        "ProductionReflectionCurrentDepth");
    RHITextureRef previousNormalTexture = createTexture(
        1,
        1,
        RHIFormat::RGBA32_FLOAT,
        RHITextureUsage::ShaderResource | RHITextureUsage::CopyDst,
        "ProductionReflectionPreviousNormal");
    RHITextureRef currentNormalTexture = createTexture(
        1,
        1,
        RHIFormat::RGBA32_FLOAT,
        RHITextureUsage::UnorderedAccess,
        "ProductionReflectionCurrentNormal");
    RHITextureRef velocityTexture = createTexture(
        kSceneWidth,
        kSceneHeight,
        RHIFormat::RG32_FLOAT,
        RHITextureUsage::ShaderResource | RHITextureUsage::CopyDst,
        "ProductionReflectionVelocity");
    RHITextureRef fallbackTexture = createTexture(
        1,
        1,
        RHIFormat::RGBA32_FLOAT,
        RHITextureUsage::ShaderResource | RHITextureUsage::CopyDst,
        "ProductionReflectionFallbackTexture");
    RHITextureRef pbrBaseColorTexture = createTexture(
        1,
        1,
        RHIFormat::RGBA32_FLOAT,
        RHITextureUsage::ShaderResource | RHITextureUsage::CopyDst,
        "ProductionReflectionPBRBaseColorTexture");
    RHITextureRef pbrMetallicRoughnessTexture = createTexture(
        1,
        1,
        RHIFormat::RGBA32_FLOAT,
        RHITextureUsage::ShaderResource | RHITextureUsage::CopyDst,
        "ProductionReflectionPBRMetallicRoughnessTexture");
    RHITextureRef pbrNormalTexture = createTexture(
        1,
        1,
        RHIFormat::RGBA32_FLOAT,
        RHITextureUsage::ShaderResource | RHITextureUsage::CopyDst,
        "ProductionReflectionPBRNormalTexture");
    RHITextureRef pbrEmissiveTexture = createTexture(
        1,
        1,
        RHIFormat::RGBA32_FLOAT,
        RHITextureUsage::ShaderResource | RHITextureUsage::CopyDst,
        "ProductionReflectionPBREmissiveTexture");
    RHITextureRef alphaBaseColorTexture = createTexture(
        1,
        1,
        RHIFormat::RGBA32_FLOAT,
        RHITextureUsage::ShaderResource | RHITextureUsage::CopyDst,
        "ProductionReflectionAlphaBaseColorTexture");
    ASSERT_NE(reflectionTexture.Get(), nullptr);
    ASSERT_NE(sceneColorTexture.Get(), nullptr);
    ASSERT_NE(sceneDepthTexture.Get(), nullptr);
    ASSERT_NE(previousReflectionTexture.Get(), nullptr);
    ASSERT_NE(previousDepthTexture.Get(), nullptr);
    ASSERT_NE(currentDepthTexture.Get(), nullptr);
    ASSERT_NE(previousNormalTexture.Get(), nullptr);
    ASSERT_NE(currentNormalTexture.Get(), nullptr);
    ASSERT_NE(velocityTexture.Get(), nullptr);
    ASSERT_NE(fallbackTexture.Get(), nullptr);
    ASSERT_NE(pbrBaseColorTexture.Get(), nullptr);
    ASSERT_NE(pbrMetallicRoughnessTexture.Get(), nullptr);
    ASSERT_NE(pbrNormalTexture.Get(), nullptr);
    ASSERT_NE(pbrEmissiveTexture.Get(), nullptr);
    ASSERT_NE(alphaBaseColorTexture.Get(), nullptr);

    RHITextureViewRef reflectionUAV = CreateTestTextureView(
        *device,
        reflectionTexture.Get(),
        RHITextureViewType::UnorderedAccess,
        RHIFormat::RGBA32_FLOAT,
        "ProductionReflectionOutputUAV");
    RHITextureViewRef sceneColorSRV = CreateTestTextureView(
        *device,
        sceneColorTexture.Get(),
        RHITextureViewType::ShaderResource,
        RHIFormat::RGBA32_FLOAT,
        "ProductionReflectionSceneColorSRV");
    RHITextureViewRef sceneDepthSRV = CreateTestTextureView(
        *device,
        sceneDepthTexture.Get(),
        RHITextureViewType::ShaderResource,
        RHIFormat::R32_FLOAT,
        "ProductionReflectionSceneDepthSRV");
    RHITextureViewRef previousReflectionSRV = CreateTestTextureView(
        *device,
        previousReflectionTexture.Get(),
        RHITextureViewType::ShaderResource,
        RHIFormat::RGBA32_FLOAT,
        "ProductionReflectionPreviousHistorySRV");
    RHITextureViewRef previousDepthSRV = CreateTestTextureView(
        *device,
        previousDepthTexture.Get(),
        RHITextureViewType::ShaderResource,
        RHIFormat::R32_FLOAT,
        "ProductionReflectionPreviousDepthSRV");
    RHITextureViewRef currentDepthUAV = CreateTestTextureView(
        *device,
        currentDepthTexture.Get(),
        RHITextureViewType::UnorderedAccess,
        RHIFormat::R32_FLOAT,
        "ProductionReflectionCurrentDepthUAV");
    RHITextureViewRef previousNormalSRV = CreateTestTextureView(
        *device,
        previousNormalTexture.Get(),
        RHITextureViewType::ShaderResource,
        RHIFormat::RGBA32_FLOAT,
        "ProductionReflectionPreviousNormalSRV");
    RHITextureViewRef currentNormalUAV = CreateTestTextureView(
        *device,
        currentNormalTexture.Get(),
        RHITextureViewType::UnorderedAccess,
        RHIFormat::RGBA32_FLOAT,
        "ProductionReflectionCurrentNormalUAV");
    RHITextureViewRef velocitySRV = CreateTestTextureView(
        *device,
        velocityTexture.Get(),
        RHITextureViewType::ShaderResource,
        RHIFormat::RG32_FLOAT,
        "ProductionReflectionVelocitySRV");
    RHITextureViewRef fallbackTextureSRV = CreateTestTextureView(
        *device,
        fallbackTexture.Get(),
        RHITextureViewType::ShaderResource,
        RHIFormat::RGBA32_FLOAT,
        "ProductionReflectionFallbackTextureSRV");
    RHITextureViewRef pbrBaseColorTextureSRV = CreateTestTextureView(
        *device,
        pbrBaseColorTexture.Get(),
        RHITextureViewType::ShaderResource,
        RHIFormat::RGBA32_FLOAT,
        "ProductionReflectionPBRBaseColorTextureSRV");
    RHITextureViewRef pbrMetallicRoughnessTextureSRV = CreateTestTextureView(
        *device,
        pbrMetallicRoughnessTexture.Get(),
        RHITextureViewType::ShaderResource,
        RHIFormat::RGBA32_FLOAT,
        "ProductionReflectionPBRMetallicRoughnessTextureSRV");
    RHITextureViewRef pbrNormalTextureSRV = CreateTestTextureView(
        *device,
        pbrNormalTexture.Get(),
        RHITextureViewType::ShaderResource,
        RHIFormat::RGBA32_FLOAT,
        "ProductionReflectionPBRNormalTextureSRV");
    RHITextureViewRef pbrEmissiveTextureSRV = CreateTestTextureView(
        *device,
        pbrEmissiveTexture.Get(),
        RHITextureViewType::ShaderResource,
        RHIFormat::RGBA32_FLOAT,
        "ProductionReflectionPBREmissiveTextureSRV");
    RHITextureViewRef alphaBaseColorTextureSRV = CreateTestTextureView(
        *device,
        alphaBaseColorTexture.Get(),
        RHITextureViewType::ShaderResource,
        RHIFormat::RGBA32_FLOAT,
        "ProductionReflectionAlphaBaseColorTextureSRV");
    ASSERT_NE(reflectionUAV.Get(), nullptr);
    ASSERT_NE(sceneColorSRV.Get(), nullptr);
    ASSERT_NE(sceneDepthSRV.Get(), nullptr);
    ASSERT_NE(previousReflectionSRV.Get(), nullptr);
    ASSERT_NE(previousDepthSRV.Get(), nullptr);
    ASSERT_NE(currentDepthUAV.Get(), nullptr);
    ASSERT_NE(previousNormalSRV.Get(), nullptr);
    ASSERT_NE(currentNormalUAV.Get(), nullptr);
    ASSERT_NE(velocitySRV.Get(), nullptr);
    ASSERT_NE(fallbackTextureSRV.Get(), nullptr);
    ASSERT_NE(pbrBaseColorTextureSRV.Get(), nullptr);
    ASSERT_NE(pbrMetallicRoughnessTextureSRV.Get(), nullptr);
    ASSERT_NE(pbrNormalTextureSRV.Get(), nullptr);
    ASSERT_NE(pbrEmissiveTextureSRV.Get(), nullptr);
    ASSERT_NE(alphaBaseColorTextureSRV.Get(), nullptr);

    float sceneDepth[kSceneWidth * kSceneHeight] = {};
    for (float& depthValue : sceneDepth)
    {
        depthValue = 0.5f;
    }

    float sceneColor[kSceneWidth * kSceneHeight * 4u] = {};
    for (uint32 texel = 0; texel < kSceneWidth * kSceneHeight; ++texel)
    {
        sceneColor[texel * 4u + 0u] = 0.05f;
        sceneColor[texel * 4u + 1u] = 0.06f;
        sceneColor[texel * 4u + 2u] = 0.07f;
        sceneColor[texel * 4u + 3u] = 1.0f;
    }

    float velocity[kSceneWidth * kSceneHeight * 2u] = {};
    const float previousReflection[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const float previousDepth = 0.5f;
    const float previousNormal[4] = {0.5f, 0.5f, 1.0f, 1.0f};
    const float fallbackTexel[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    const float pbrBaseColorTexel[4] = {0.5f, 0.25f, 0.75f, 1.0f};
    const float pbrMetallicRoughnessTexel[4] = {1.0f, 0.5f, 0.5f, 1.0f};
    const float pbrNormalTexel[4] = {1.0f, 0.5f, 0.5f, 1.0f};
    const float pbrEmissiveTexel[4] = {0.5f, 0.25f, 0.1f, 1.0f};
    const float alphaCutoutTexel[4] = {1.0f, 1.0f, 1.0f, 0.0f};

    RHIBufferRef sceneColorUpload = CreateTextureUploadBuffer2D(
        *device,
        sceneColor,
        kSceneWidth,
        kSceneHeight,
        sizeof(float) * 4u,
        kUploadRowPitch,
        "ProductionReflectionSceneColorUpload");
    RHIBufferRef sceneDepthUpload = CreateTextureUploadBuffer2D(
        *device,
        sceneDepth,
        kSceneWidth,
        kSceneHeight,
        sizeof(float),
        kUploadRowPitch,
        "ProductionReflectionSceneDepthUpload");
    RHIBufferRef previousReflectionUpload = CreateTextureUploadBuffer2D(
        *device,
        previousReflection,
        1,
        1,
        sizeof(float) * 4u,
        kUploadRowPitch,
        "ProductionReflectionPreviousHistoryUpload");
    RHIBufferRef previousDepthUpload = CreateTextureUploadBuffer2D(
        *device,
        &previousDepth,
        1,
        1,
        sizeof(float),
        kUploadRowPitch,
        "ProductionReflectionPreviousDepthUpload");
    RHIBufferRef previousNormalUpload = CreateTextureUploadBuffer2D(
        *device,
        previousNormal,
        1,
        1,
        sizeof(float) * 4u,
        kUploadRowPitch,
        "ProductionReflectionPreviousNormalUpload");
    RHIBufferRef velocityUpload = CreateTextureUploadBuffer2D(
        *device,
        velocity,
        kSceneWidth,
        kSceneHeight,
        sizeof(float) * 2u,
        kUploadRowPitch,
        "ProductionReflectionVelocityUpload");
    RHIBufferRef fallbackTextureUpload = CreateTextureUploadBuffer2D(
        *device,
        fallbackTexel,
        1,
        1,
        sizeof(float) * 4u,
        kUploadRowPitch,
        "ProductionReflectionFallbackTextureUpload");
    RHIBufferRef pbrBaseColorTextureUpload = CreateTextureUploadBuffer2D(
        *device,
        pbrBaseColorTexel,
        1,
        1,
        sizeof(float) * 4u,
        kUploadRowPitch,
        "ProductionReflectionPBRBaseColorTextureUpload");
    RHIBufferRef pbrMetallicRoughnessTextureUpload = CreateTextureUploadBuffer2D(
        *device,
        pbrMetallicRoughnessTexel,
        1,
        1,
        sizeof(float) * 4u,
        kUploadRowPitch,
        "ProductionReflectionPBRMetallicRoughnessTextureUpload");
    RHIBufferRef pbrNormalTextureUpload = CreateTextureUploadBuffer2D(
        *device,
        pbrNormalTexel,
        1,
        1,
        sizeof(float) * 4u,
        kUploadRowPitch,
        "ProductionReflectionPBRNormalTextureUpload");
    RHIBufferRef pbrEmissiveTextureUpload = CreateTextureUploadBuffer2D(
        *device,
        pbrEmissiveTexel,
        1,
        1,
        sizeof(float) * 4u,
        kUploadRowPitch,
        "ProductionReflectionPBREmissiveTextureUpload");
    RHIBufferRef alphaBaseColorTextureUpload = CreateTextureUploadBuffer2D(
        *device,
        alphaCutoutTexel,
        1,
        1,
        sizeof(float) * 4u,
        kUploadRowPitch,
        "ProductionReflectionAlphaBaseColorTextureUpload");
    ASSERT_NE(sceneColorUpload.Get(), nullptr);
    ASSERT_NE(sceneDepthUpload.Get(), nullptr);
    ASSERT_NE(previousReflectionUpload.Get(), nullptr);
    ASSERT_NE(previousDepthUpload.Get(), nullptr);
    ASSERT_NE(previousNormalUpload.Get(), nullptr);
    ASSERT_NE(velocityUpload.Get(), nullptr);
    ASSERT_NE(fallbackTextureUpload.Get(), nullptr);
    ASSERT_NE(pbrBaseColorTextureUpload.Get(), nullptr);
    ASSERT_NE(pbrMetallicRoughnessTextureUpload.Get(), nullptr);
    ASSERT_NE(pbrNormalTextureUpload.Get(), nullptr);
    ASSERT_NE(pbrEmissiveTextureUpload.Get(), nullptr);
    ASSERT_NE(alphaBaseColorTextureUpload.Get(), nullptr);

    RHIDescriptorSetDesc descriptorDesc;
    descriptorDesc.layout = setLayout;
    descriptorDesc.debugName = "ProductionReflectionDescriptorSet";
    descriptorDesc.BindAccelerationStructure(RTReflectionBindings::RVX_RT_REFLECTION_TLAS_BINDING, tlas.Get());
    descriptorDesc.BindTexture(RTReflectionBindings::RVX_RT_REFLECTION_OUTPUT_BINDING, reflectionUAV.Get());
    descriptorDesc.BindTexture(RTReflectionBindings::RVX_RT_REFLECTION_SCENE_COLOR_BINDING, sceneColorSRV.Get());
    descriptorDesc.BindTexture(RTReflectionBindings::RVX_RT_REFLECTION_SCENE_DEPTH_BINDING, sceneDepthSRV.Get());
    descriptorDesc.BindBuffer(RTReflectionBindings::RVX_RT_REFLECTION_CONSTANTS_BINDING, constantBuffer.Get(), 0, sizeof(constants));
    descriptorDesc.BindBuffer(RTReflectionBindings::RVX_RT_REFLECTION_MATERIAL_METADATA_BINDING, materialMetadataBuffer.Get());
    auto resolveMaterialTextureView = [&](uint32 textureIndex) -> RHITextureView*
    {
        switch (textureIndex)
        {
            case kPBRBaseColorTextureIndex:
                return pbrBaseColorTextureSRV.Get();
            case kPBRMetallicRoughnessTextureIndex:
                return pbrMetallicRoughnessTextureSRV.Get();
            case kPBRNormalTextureIndex:
                return pbrNormalTextureSRV.Get();
            case kPBREmissiveTextureIndex:
                return pbrEmissiveTextureSRV.Get();
            case kAlphaCutoutTextureIndex:
                return alphaBaseColorTextureSRV.Get();
            default:
                return fallbackTextureSRV.Get();
        }
    };

    for (uint32 textureIndex = 0; textureIndex < RTReflectionBindings::RVX_RT_REFLECTION_MAX_MATERIAL_TEXTURES; ++textureIndex)
    {
        descriptorDesc.BindTexture(RTReflectionBindings::RVX_RT_REFLECTION_MATERIAL_TEXTURES_BINDING, resolveMaterialTextureView(textureIndex), textureIndex);
    }
    descriptorDesc.BindTexture(RTReflectionBindings::RVX_RT_REFLECTION_PREVIOUS_HISTORY_BINDING, previousReflectionSRV.Get());
    descriptorDesc.BindTexture(RTReflectionBindings::RVX_RT_REFLECTION_PREVIOUS_DEPTH_BINDING, previousDepthSRV.Get());
    descriptorDesc.BindTexture(RTReflectionBindings::RVX_RT_REFLECTION_OUTPUT_DEPTH_BINDING, currentDepthUAV.Get());
    descriptorDesc.BindTexture(RTReflectionBindings::RVX_RT_REFLECTION_PREVIOUS_NORMAL_BINDING, previousNormalSRV.Get());
    descriptorDesc.BindTexture(RTReflectionBindings::RVX_RT_REFLECTION_OUTPUT_NORMAL_BINDING, currentNormalUAV.Get());
    descriptorDesc.BindBuffer(RTReflectionBindings::RVX_RT_REFLECTION_GEOMETRY_METADATA_BINDING, geometryMetadataBuffer.Get());
    for (uint32 bufferIndex = 0; bufferIndex < RTReflectionBindings::RVX_RT_REFLECTION_MAX_GEOMETRY_BUFFERS; ++bufferIndex)
    {
        descriptorDesc.BindBuffer(RTReflectionBindings::RVX_RT_REFLECTION_INDEX_BUFFERS_BINDING,
                                  bufferIndex == 0u ? reflectionIndexBuffer.Get() : fallbackRawBuffer.Get(),
                                  0,
                                  RVX_WHOLE_SIZE,
                                  bufferIndex);
        descriptorDesc.BindBuffer(RTReflectionBindings::RVX_RT_REFLECTION_UV_BUFFERS_BINDING,
                                  bufferIndex == 0u ? reflectionUVBuffer.Get() : fallbackRawBuffer.Get(),
                                  0,
                                  RVX_WHOLE_SIZE,
                                  bufferIndex);
        descriptorDesc.BindBuffer(RTReflectionBindings::RVX_RT_REFLECTION_NORMAL_BUFFERS_BINDING,
                                  bufferIndex == 0u ? reflectionNormalBuffer.Get() : fallbackRawBuffer.Get(),
                                  0,
                                  RVX_WHOLE_SIZE,
                                  bufferIndex);
        descriptorDesc.BindBuffer(RTReflectionBindings::RVX_RT_REFLECTION_TANGENT_BUFFERS_BINDING,
                                  bufferIndex == 0u ? reflectionTangentBuffer.Get() : fallbackRawBuffer.Get(),
                                  0,
                                  RVX_WHOLE_SIZE,
                                  bufferIndex);
    }
    descriptorDesc.BindTexture(RTReflectionBindings::RVX_RT_REFLECTION_SCENE_VELOCITY_BINDING, velocitySRV.Get());
    RHIDescriptorSetRef descriptorSet = device->CreateDescriptorSet(descriptorDesc);
    ASSERT_NE(descriptorSet.Get(), nullptr);

    RHIBufferDesc readbackDesc;
    readbackDesc.size = kUploadRowPitch;
    readbackDesc.usage = RHIBufferUsage::CopyDst;
    readbackDesc.memoryType = RHIMemoryType::Readback;
    readbackDesc.debugName = "ProductionReflectionReadback";
    RHIBufferRef readbackBuffer = device->CreateBuffer(readbackDesc);
    ASSERT_NE(readbackBuffer.Get(), nullptr);

    auto writeGeometryMetadata = [&](bool enableUV,
                                     bool enableNormalAndTangent,
                                     const char* debugName) -> bool
    {
        RayTracingTestAlphaMetadata updatedGeometryMetadata = geometryMetadata;
        if (enableUV || enableNormalAndTangent)
        {
            updatedGeometryMetadata.flags = kAlphaHasIndexBufferFlag | kAlphaIndexFormatUInt32Flag;
            updatedGeometryMetadata.indexBufferTableIndex = 0u;
        }
        if (enableUV)
        {
            updatedGeometryMetadata.flags |= kAlphaHasUVBufferFlag;
            updatedGeometryMetadata.uvBufferTableIndex = 0u;
            updatedGeometryMetadata.baseColorSamplerFlags = kAlphaMagNearestFlag;
        }
        if (enableNormalAndTangent)
        {
            updatedGeometryMetadata.flags |= kAlphaHasNormalBufferFlag | kAlphaHasTangentBufferFlag;
            updatedGeometryMetadata.normalBufferTableIndex = 0u;
            updatedGeometryMetadata.tangentBufferTableIndex = 0u;
        }

        void* mappedGeometry = geometryMetadataBuffer->Map();
        if (!mappedGeometry)
        {
            ADD_FAILURE() << "Failed to map production reflection geometry metadata for " << debugName;
            return false;
        }

        std::memcpy(mappedGeometry, &updatedGeometryMetadata, sizeof(updatedGeometryMetadata));
        geometryMetadataBuffer->Unmap();
        return true;
    };

    auto writeMaterialMetadataDetailed = [&](float alpha,
                                             uint32 textureFlags,
                                             uint32 baseColorTextureTableIndex,
                                             uint32 metallicRoughnessTextureTableIndex,
                                             uint32 normalTextureTableIndex,
                                             uint32 emissiveTextureTableIndex,
                                             float metallic,
                                             float roughness,
                                             float normalScale,
                                             Vec4 emissiveFactor,
                                             const char* debugName) -> bool
    {
        RayTracingTestMaterialMetadata updatedMaterialMetadata = materialMetadata;
        updatedMaterialMetadata.baseColorFactor = Vec4(kBaseColorR, kBaseColorG, kBaseColorB, alpha);
        updatedMaterialMetadata.emissiveFactor = emissiveFactor;
        updatedMaterialMetadata.materialFactors = Vec4(metallic, roughness, 0.5f, normalScale);
        updatedMaterialMetadata.flags = kMaterialAlphaTestFlag | textureFlags;
        if ((textureFlags & kMaterialBaseColorTextureFlag) != 0u)
        {
            updatedMaterialMetadata.baseColorTextureTableIndex = baseColorTextureTableIndex;
            updatedMaterialMetadata.baseColorTextureSampling.samplerFlags = kAlphaMagNearestFlag;
        }
        if ((textureFlags & kMaterialMetallicRoughnessTextureFlag) != 0u)
        {
            updatedMaterialMetadata.metallicRoughnessTextureTableIndex = metallicRoughnessTextureTableIndex;
            updatedMaterialMetadata.metallicRoughnessTextureSampling.samplerFlags = kAlphaMagNearestFlag;
        }
        if ((textureFlags & kMaterialNormalTextureFlag) != 0u)
        {
            updatedMaterialMetadata.normalTextureTableIndex = normalTextureTableIndex;
            updatedMaterialMetadata.normalTextureSampling.samplerFlags = kAlphaMagNearestFlag;
        }
        if ((textureFlags & kMaterialEmissiveTextureFlag) != 0u)
        {
            updatedMaterialMetadata.emissiveTextureTableIndex = emissiveTextureTableIndex;
            updatedMaterialMetadata.emissiveTextureSampling.samplerFlags = kAlphaMagNearestFlag;
        }

        void* mappedMaterial = materialMetadataBuffer->Map();
        if (!mappedMaterial)
        {
            ADD_FAILURE() << "Failed to map production reflection material metadata for " << debugName;
            return false;
        }

        std::memcpy(mappedMaterial, &updatedMaterialMetadata, sizeof(updatedMaterialMetadata));
        materialMetadataBuffer->Unmap();
        return true;
    };

    auto writeMaterialMetadata = [&](float alpha, bool useBaseColorTexture, const char* debugName) -> bool
    {
        return writeMaterialMetadataDetailed(
            alpha,
            useBaseColorTexture ? kMaterialBaseColorTextureFlag : 0u,
            useBaseColorTexture ? kAlphaCutoutTextureIndex : RVX_INVALID_INDEX,
            RVX_INVALID_INDEX,
            RVX_INVALID_INDEX,
            RVX_INVALID_INDEX,
            0.0f,
            kRoughness,
            1.0f,
            Vec4(0.0f, 0.0f, 0.0f, 1.0f),
            debugName);
    };

    bool accelerationStructuresBuilt = false;
    RHIResourceState reflectionState = RHIResourceState::Common;
    auto dispatchAndReadReflection = [&](const char* debugName, float* outObserved) -> bool
    {
        RHICommandContextRef ctx = device->CreateCommandContext(RHICommandQueueType::Graphics);
        if (!ctx)
        {
            ADD_FAILURE() << "Failed to create production reflection command context for " << debugName;
            return false;
        }

        auto copyUploadToTexture = [&](RHIBuffer* uploadBuffer, RHITexture* texture, uint32 width, uint32 height)
        {
            ctx->TextureBarrier(texture, RHIResourceState::Common, RHIResourceState::CopyDest);
            RHIBufferTextureCopyDesc copyDesc;
            copyDesc.bufferRowPitch = kUploadRowPitch;
            copyDesc.textureRegion = RHIRect{0, 0, width, height};
            ctx->CopyBufferToTexture(uploadBuffer, texture, copyDesc);
            ctx->TextureBarrier(texture, RHIResourceState::CopyDest, RHIResourceState::ShaderResource);
        };

        ctx->Begin();
        if (!accelerationStructuresBuilt)
        {
            copyUploadToTexture(sceneColorUpload.Get(), sceneColorTexture.Get(), kSceneWidth, kSceneHeight);
            copyUploadToTexture(sceneDepthUpload.Get(), sceneDepthTexture.Get(), kSceneWidth, kSceneHeight);
            copyUploadToTexture(previousReflectionUpload.Get(), previousReflectionTexture.Get(), 1, 1);
            copyUploadToTexture(previousDepthUpload.Get(), previousDepthTexture.Get(), 1, 1);
            copyUploadToTexture(previousNormalUpload.Get(), previousNormalTexture.Get(), 1, 1);
            copyUploadToTexture(velocityUpload.Get(), velocityTexture.Get(), kSceneWidth, kSceneHeight);
            copyUploadToTexture(fallbackTextureUpload.Get(), fallbackTexture.Get(), 1, 1);
            copyUploadToTexture(pbrBaseColorTextureUpload.Get(), pbrBaseColorTexture.Get(), 1, 1);
            copyUploadToTexture(pbrMetallicRoughnessTextureUpload.Get(), pbrMetallicRoughnessTexture.Get(), 1, 1);
            copyUploadToTexture(pbrNormalTextureUpload.Get(), pbrNormalTexture.Get(), 1, 1);
            copyUploadToTexture(pbrEmissiveTextureUpload.Get(), pbrEmissiveTexture.Get(), 1, 1);
            copyUploadToTexture(alphaBaseColorTextureUpload.Get(), alphaBaseColorTexture.Get(), 1, 1);
            ctx->TextureBarrier(reflectionTexture.Get(), reflectionState, RHIResourceState::UnorderedAccess);
            ctx->TextureBarrier(currentDepthTexture.Get(), RHIResourceState::Common, RHIResourceState::UnorderedAccess);
            ctx->TextureBarrier(currentNormalTexture.Get(), RHIResourceState::Common, RHIResourceState::UnorderedAccess);
            ctx->BufferBarrier({blasScratch.Get(), RHIResourceState::Common, RHIResourceState::UnorderedAccess});
            ctx->BuildBottomLevelAccelerationStructure(blas.Get(), blasDesc, blasScratch.Get());
            ctx->BufferBarrier({tlasScratch.Get(), RHIResourceState::Common, RHIResourceState::UnorderedAccess});
            ctx->BuildTopLevelAccelerationStructure(tlas.Get(), tlasDesc, tlasScratch.Get());
            accelerationStructuresBuilt = true;
        }
        else
        {
            ctx->TextureBarrier(reflectionTexture.Get(), reflectionState, RHIResourceState::UnorderedAccess);
        }

        ctx->SetPipeline(pipeline);
        ctx->SetDescriptorSet(0, descriptorSet.Get());
        ctx->DispatchRays({shaderTable, 1, 1, 1});
        ctx->TextureBarrier(reflectionTexture.Get(), RHIResourceState::UnorderedAccess, RHIResourceState::CopySource);
        reflectionState = RHIResourceState::CopySource;

        RHIBufferTextureCopyDesc readbackCopy;
        readbackCopy.bufferRowPitch = kUploadRowPitch;
        readbackCopy.textureRegion = RHIRect{0, 0, 1, 1};
        ctx->CopyTextureToBuffer(reflectionTexture.Get(), readbackBuffer.Get(), readbackCopy);
        ctx->End();

        RHIFenceRef fence = device->CreateFence(0);
        if (!fence)
        {
            ADD_FAILURE() << "Failed to create production reflection fence for " << debugName;
            return false;
        }

        const uint64 submittedValue = device->SubmitCommandContext(ctx.Get(), fence.Get());
        if (submittedValue == 0u)
        {
            ADD_FAILURE() << "Failed to submit production reflection command context for " << debugName;
            return false;
        }

        device->WaitForFence(fence.Get(), submittedValue);
        EXPECT_GE(fence->GetCompletedValue(), submittedValue);

        void* mappedReadback = readbackBuffer->Map();
        if (!mappedReadback)
        {
            ADD_FAILURE() << "Failed to map production reflection readback for " << debugName;
            return false;
        }

        const float* observed = static_cast<const float*>(mappedReadback);
        std::memcpy(outObserved, observed, sizeof(float) * 4u);
        readbackBuffer->Unmap();
        return true;
    };

    float observedHit[4] = {};
    ASSERT_TRUE(writeMaterialMetadata(1.0f, false, "alpha-pass"));
    ASSERT_TRUE(dispatchAndReadReflection("alpha-pass", observedHit));
    EXPECT_NEAR(observedHit[0], kExpectedR, 2.0e-3f);
    EXPECT_NEAR(observedHit[1], kExpectedG, 2.0e-3f);
    EXPECT_NEAR(observedHit[2], kExpectedB, 2.0e-3f);
    EXPECT_NEAR(observedHit[3], 1.0f, 1.0e-4f);

    float observedCutout[4] = {};
    ASSERT_TRUE(writeMaterialMetadata(0.0f, false, "alpha-cutout"));
    ASSERT_TRUE(dispatchAndReadReflection("alpha-cutout", observedCutout));
    EXPECT_NEAR(observedCutout[0], 0.0f, 1.0e-4f);
    EXPECT_NEAR(observedCutout[1], 0.0f, 1.0e-4f);
    EXPECT_NEAR(observedCutout[2], 0.0f, 1.0e-4f);
    EXPECT_NEAR(observedCutout[3], 0.0f, 1.0e-4f);

    float observedTextureCutout[4] = {};
    ASSERT_TRUE(writeGeometryMetadata(true, false, "texture-alpha-cutout"));
    ASSERT_TRUE(writeMaterialMetadata(1.0f, true, "texture-alpha-cutout"));
    ASSERT_TRUE(dispatchAndReadReflection("texture-alpha-cutout", observedTextureCutout));
    EXPECT_NEAR(observedTextureCutout[0], 0.0f, 1.0e-4f);
    EXPECT_NEAR(observedTextureCutout[1], 0.0f, 1.0e-4f);
    EXPECT_NEAR(observedTextureCutout[2], 0.0f, 1.0e-4f);
    EXPECT_NEAR(observedTextureCutout[3], 0.0f, 1.0e-4f);

    float observedPBRTextures[4] = {};
    ASSERT_TRUE(writeGeometryMetadata(true, false, "pbr-textures"));
    ASSERT_TRUE(writeMaterialMetadataDetailed(
        1.0f,
        kMaterialBaseColorTextureFlag |
            kMaterialMetallicRoughnessTextureFlag |
            kMaterialEmissiveTextureFlag,
        kPBRBaseColorTextureIndex,
        kPBRMetallicRoughnessTextureIndex,
        RVX_INVALID_INDEX,
        kPBREmissiveTextureIndex,
        1.0f,
        0.8f,
        1.0f,
        Vec4(0.2f, 0.1f, 0.05f, 2.0f),
        "pbr-textures"));
    ASSERT_TRUE(dispatchAndReadReflection("pbr-textures", observedPBRTextures));
    EXPECT_NEAR(observedPBRTextures[0], 0.4352f, 2.0e-3f);
    EXPECT_NEAR(observedPBRTextures[1], 0.1217f, 2.0e-3f);
    EXPECT_NEAR(observedPBRTextures[2], 0.112075f, 2.0e-3f);
    EXPECT_NEAR(observedPBRTextures[3], 1.0f, 1.0e-4f);

    float observedNormalMap[4] = {};
    ASSERT_TRUE(writeGeometryMetadata(true, true, "normal-map"));
    ASSERT_TRUE(writeMaterialMetadataDetailed(
        1.0f,
        kMaterialNormalTextureFlag,
        RVX_INVALID_INDEX,
        RVX_INVALID_INDEX,
        kPBRNormalTextureIndex,
        RVX_INVALID_INDEX,
        0.0f,
        kRoughness,
        1.0f,
        Vec4(0.0f, 0.0f, 0.0f, 1.0f),
        "normal-map"));
    ASSERT_TRUE(dispatchAndReadReflection("normal-map", observedNormalMap));
    EXPECT_NEAR(observedNormalMap[0], 0.916235f, 3.0e-3f);
    EXPECT_NEAR(observedNormalMap[1], 0.898399f, 3.0e-3f);
    EXPECT_NEAR(observedNormalMap[2], 0.889481f, 3.0e-3f);
    EXPECT_NEAR(observedNormalMap[3], 1.0f, 1.0e-4f);

    device->WaitIdle();
    cache.Shutdown();
}
