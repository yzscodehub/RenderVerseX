#include "Core/Core.h"
#include "RHI/RHI.h"
#include "Common/GpuTestUtils.h"

#include <gtest/gtest.h>

#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace RVX;

// =============================================================================
// Cross-Backend Validation Tests
// Tests that run the same operations on multiple backends to ensure consistency
// =============================================================================

// Helper to create device for any backend
std::unique_ptr<IRHIDevice> CreateDeviceForBackend(RHIBackendType backend)
{
    RHIDeviceDesc desc;
    desc.enableDebugLayer = true;
    return CreateRHIDevice(backend, desc);
}

bool ShouldRunBackend(const std::unique_ptr<IRHIDevice>& device, RHIBackendType backend)
{
    if (!device)
    {
        RVX_CORE_WARN("Backend {} not available, skipping", ToString(backend));
        return false;
    }

    if (!RVX::Test::IsHardwareGpuDevice(device))
    {
        RVX_CORE_WARN("Backend {} uses software adapter '{}', skipping",
                      ToString(backend),
                      device->GetCapabilities().adapterName);
        return false;
    }

    return true;
}

// Test struct for results comparison
struct BackendTestResult
{
    RHIBackendType backend;
    bool success;
    std::string message;
};

// =============================================================================
// Get Available Backends for Current Platform
// =============================================================================
std::vector<RHIBackendType> GetAvailableBackends()
{
    std::vector<RHIBackendType> backends;

#if defined(__APPLE__)
    backends.push_back(RHIBackendType::Metal);
#elif defined(_WIN32)
    backends.push_back(RHIBackendType::DX12);
    backends.push_back(RHIBackendType::DX11);
    backends.push_back(RHIBackendType::Vulkan);
#else
    backends.push_back(RHIBackendType::Vulkan);
#endif

    return backends;
}

// =============================================================================
// Capability Contract Consistency
// =============================================================================
TEST(CrossBackendValidation, CapabilityContractConsistency)
{
    std::vector<RHIBackendType> backends = GetAvailableBackends();
    uint32_t testedBackendCount = 0;

    for (auto backend : backends)
    {
        auto device = CreateDeviceForBackend(backend);
        if (!ShouldRunBackend(device, backend))
        {
            continue;
        }
        ++testedBackendCount;

        const RHICapabilities& caps = device->GetCapabilities();
        auto validation = ValidateRHICapabilities(caps);

        EXPECT_TRUE(validation)
            << "Backend " << ToString(backend)
            << " reported inconsistent capabilities: " << validation.message;
    }

    RVX_GTEST_SKIP_IF_NO_GPU_BACKENDS(testedBackendCount);
}

TEST(CrossBackendValidation, DeviceCapabilityReportRenderGraphBaselineConsistency)
{
    std::vector<RHIBackendType> backends = GetAvailableBackends();
    uint32_t testedBackendCount = 0;

    for (auto backend : backends)
    {
        auto device = CreateDeviceForBackend(backend);
        if (!ShouldRunBackend(device, backend))
        {
            continue;
        }
        ++testedBackendCount;

        const RHICapabilities& caps = device->GetCapabilities();
        const RHICapabilityReport report = device->GetCapabilityReport();

        EXPECT_EQ(report.schemaVersion, RVX_RHI_CAPABILITY_REPORT_SCHEMA_VERSION) << ToString(backend);
        EXPECT_EQ(report.backendType, backend) << ToString(backend);
        EXPECT_EQ(report.adapterName, caps.adapterName) << ToString(backend);
        EXPECT_EQ(report.driverVersion, caps.driverVersion) << ToString(backend);
        EXPECT_FALSE(report.adapterName.empty()) << ToString(backend);
        EXPECT_TRUE(report.validationPassed)
            << "Backend " << ToString(backend)
            << " reported invalid capabilities: " << report.validationMessage;
        EXPECT_TRUE(report.renderGraphBaselineSupported)
            << "Backend " << ToString(backend)
            << " is missing " << report.renderGraphBaselineMissingRequirements.size()
            << " RenderGraph baseline requirements";
        EXPECT_TRUE(report.renderGraphBaselineMissingRequirements.empty()) << ToString(backend);

        const std::string text = device->ExportCapabilityReportText();
        EXPECT_NE(text.find("RHI Capability Report"), std::string::npos) << ToString(backend);
        EXPECT_NE(text.find("Adapter: " + report.adapterName), std::string::npos) << ToString(backend);
        EXPECT_NE(text.find("DriverVersion: " + report.driverVersion), std::string::npos) << ToString(backend);
        EXPECT_NE(text.find("RenderGraphBaseline: Passed"), std::string::npos) << ToString(backend);
        EXPECT_NE(text.find("RenderGraphBaselineMissing: none"), std::string::npos) << ToString(backend);

        const std::string json = device->ExportCapabilityReportJson();
        EXPECT_NE(json.find("\"schemaId\": \"RVX.RHI.CapabilityReport\""), std::string::npos) << ToString(backend);
        EXPECT_NE(json.find("\"kind\": \"RHICapabilityReportJson\""), std::string::npos) << ToString(backend);
        EXPECT_NE(json.find("\"adapterName\": \"" + report.adapterName + "\""), std::string::npos)
            << ToString(backend);
        EXPECT_NE(json.find("\"driverVersion\": \"" + report.driverVersion + "\""), std::string::npos)
            << ToString(backend);
        EXPECT_NE(json.find("\"renderGraphBaseline\": {"), std::string::npos) << ToString(backend);
        EXPECT_NE(json.find("\"supported\": true"), std::string::npos) << ToString(backend);

        RVX_CORE_INFO("Backend {}: RHI capability report RenderGraph baseline OK", ToString(backend));
    }

    RVX_GTEST_SKIP_IF_NO_GPU_BACKENDS(testedBackendCount);
}

// =============================================================================
// Buffer Creation Consistency
// =============================================================================
TEST(CrossBackendValidation, BufferCreationConsistency)
{
    std::vector<RHIBackendType> backends = GetAvailableBackends();
    uint32_t testedBackendCount = 0;

    RHIBufferDesc bufferDesc;
    bufferDesc.size = 4096;
    bufferDesc.usage = RHIBufferUsage::Vertex | RHIBufferUsage::Index;
    bufferDesc.memoryType = RHIMemoryType::Upload;
    bufferDesc.stride = 32;
    bufferDesc.debugName = "CrossBackendBuffer";

    for (auto backend : backends)
    {
        auto device = CreateDeviceForBackend(backend);
        if (!ShouldRunBackend(device, backend))
        {
            continue;
        }
        ++testedBackendCount;

        auto buffer = device->CreateBuffer(bufferDesc);
        if (!buffer)
        {
            RVX_CORE_ERROR("Backend {} failed to create buffer", ToString(backend));
            FAIL() << "Backend " << ToString(backend) << " failed to create buffer";
        }

        // Verify properties match
        EXPECT_EQ(buffer->GetSize(), bufferDesc.size);
        EXPECT_EQ(buffer->GetStride(), bufferDesc.stride);
        EXPECT_EQ(buffer->GetMemoryType(), bufferDesc.memoryType);

        // Test mapping
        void* data = buffer->Map();
        ASSERT_NE(nullptr, data);
        buffer->Unmap();

        RVX_CORE_INFO("Backend {}: Buffer creation OK", ToString(backend));
    }

    RVX_GTEST_SKIP_IF_NO_GPU_BACKENDS(testedBackendCount);
}

// =============================================================================
// Texture Creation Consistency
// =============================================================================
TEST(CrossBackendValidation, TextureCreationConsistency)
{
    std::vector<RHIBackendType> backends = GetAvailableBackends();
    uint32_t testedBackendCount = 0;

    // Test various texture types
    struct TextureTestCase
    {
        const char* name;
        RHITextureDesc desc;
    };

    std::vector<TextureTestCase> testCases = {
        {"RGBA8 2D", RHITextureDesc::Texture2D(512, 512, RHIFormat::RGBA8_UNORM)},
        {"RGBA16F RT", RHITextureDesc::RenderTarget(1920, 1080, RHIFormat::RGBA16_FLOAT)},
        {"D24S8 DS", RHITextureDesc::DepthStencil(1920, 1080, RHIFormat::D24_UNORM_S8_UINT)},
        {"D32F DS", RHITextureDesc::DepthStencil(1920, 1080, RHIFormat::D32_FLOAT)},
    };

    for (auto backend : backends)
    {
        auto device = CreateDeviceForBackend(backend);
        if (!ShouldRunBackend(device, backend))
        {
            continue;
        }
        ++testedBackendCount;

        for (auto& tc : testCases)
        {
            auto texture = device->CreateTexture(tc.desc);
            if (!texture)
            {
                RVX_CORE_ERROR("Backend {} failed to create texture: {}", ToString(backend), tc.name);
                FAIL() << "Backend " << ToString(backend) << " failed to create texture: " << tc.name;
            }

            EXPECT_EQ(texture->GetWidth(), tc.desc.width);
            EXPECT_EQ(texture->GetHeight(), tc.desc.height);
            EXPECT_EQ(texture->GetFormat(), tc.desc.format);
        }

        RVX_CORE_INFO("Backend {}: All texture types OK", ToString(backend));
    }

    RVX_GTEST_SKIP_IF_NO_GPU_BACKENDS(testedBackendCount);
}

// =============================================================================
// Command Context Consistency
// =============================================================================
TEST(CrossBackendValidation, CommandContextConsistency)
{
    std::vector<RHIBackendType> backends = GetAvailableBackends();
    uint32_t testedBackendCount = 0;

    for (auto backend : backends)
    {
        auto device = CreateDeviceForBackend(backend);
        if (!ShouldRunBackend(device, backend))
        {
            continue;
        }
        ++testedBackendCount;

        // Test all queue types
        auto graphicsCtx = device->CreateCommandContext(RHICommandQueueType::Graphics);
        auto computeCtx = device->CreateCommandContext(RHICommandQueueType::Compute);
        auto copyCtx = device->CreateCommandContext(RHICommandQueueType::Copy);

        ASSERT_NE(nullptr, graphicsCtx.Get());
        ASSERT_NE(nullptr, computeCtx.Get());
        ASSERT_NE(nullptr, copyCtx.Get());

        // Test basic command recording
        graphicsCtx->Begin();
        graphicsCtx->End();
        device->SubmitCommandContext(graphicsCtx.Get(), nullptr);

        computeCtx->Begin();
        computeCtx->End();
        device->SubmitCommandContext(computeCtx.Get(), nullptr);

        copyCtx->Begin();
        copyCtx->End();
        device->SubmitCommandContext(copyCtx.Get(), nullptr);

        device->WaitIdle();

        RVX_CORE_INFO("Backend {}: Command contexts OK", ToString(backend));
    }

    RVX_GTEST_SKIP_IF_NO_GPU_BACKENDS(testedBackendCount);
}

// =============================================================================
// Barrier Operations Consistency
// =============================================================================
TEST(CrossBackendValidation, BarrierOperationsConsistency)
{
    std::vector<RHIBackendType> backends = GetAvailableBackends();
    uint32_t testedBackendCount = 0;

    for (auto backend : backends)
    {
        auto device = CreateDeviceForBackend(backend);
        if (!ShouldRunBackend(device, backend))
        {
            continue;
        }
        ++testedBackendCount;

        // Create test resources
        auto rtDesc = RHITextureDesc::RenderTarget(512, 512, RHIFormat::RGBA8_UNORM);
        auto texture = device->CreateTexture(rtDesc);
        ASSERT_NE(nullptr, texture.Get());

        RHIBufferDesc bufDesc;
        bufDesc.size = 4096;
        bufDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::UnorderedAccess;
        bufDesc.memoryType = RHIMemoryType::Default;
        auto buffer = device->CreateBuffer(bufDesc);
        ASSERT_NE(nullptr, buffer.Get());

        auto ctx = device->CreateCommandContext(RHICommandQueueType::Graphics);
        ctx->Begin();

        // No-work barriers must be accepted without backend calls or crashes.
        ctx->TextureBarrier({nullptr, RHIResourceState::Common, RHIResourceState::RenderTarget});
        ctx->BufferBarrier({nullptr, RHIResourceState::Common, RHIResourceState::UnorderedAccess});
        ctx->TextureBarrier({texture.Get(), RHIResourceState::Common, RHIResourceState::Common});
        ctx->BufferBarrier({buffer.Get(), RHIResourceState::Common, RHIResourceState::Common});

        // Test texture barriers
        ctx->TextureBarrier({texture.Get(), RHIResourceState::Undefined, RHIResourceState::RenderTarget});
        ctx->TextureBarrier({texture.Get(), RHIResourceState::RenderTarget, RHIResourceState::ShaderResource});
        ctx->TextureBarrier({texture.Get(), RHIResourceState::ShaderResource, RHIResourceState::CopySource});

        // Test buffer barriers
        ctx->BufferBarrier({buffer.Get(), RHIResourceState::Common, RHIResourceState::UnorderedAccess});
        ctx->BufferBarrier({buffer.Get(), RHIResourceState::UnorderedAccess, RHIResourceState::ShaderResource});

        ctx->End();
        device->SubmitCommandContext(ctx.Get(), nullptr);
        device->WaitIdle();

        RVX_CORE_INFO("Backend {}: Barrier operations OK", ToString(backend));
    }

    RVX_GTEST_SKIP_IF_NO_GPU_BACKENDS(testedBackendCount);
}

TEST(CrossBackendValidation, DescriptorContractConsistency)
{
    std::vector<RHIBackendType> backends = GetAvailableBackends();
    uint32_t testedBackendCount = 0;

    for (auto backend : backends)
    {
        auto device = CreateDeviceForBackend(backend);
        if (!ShouldRunBackend(device, backend))
        {
            continue;
        }
        ++testedBackendCount;

        const RHICapabilities& caps = device->GetCapabilities();
        EXPECT_TRUE(caps.supportsDescriptorSets) << ToString(backend);
        EXPECT_GE(caps.maxDescriptorSets, 4u) << ToString(backend);

        RHIDescriptorSetLayoutDesc duplicateLayout;
        duplicateLayout.AddBinding(0, RHIBindingType::UniformBuffer);
        duplicateLayout.AddBinding(0, RHIBindingType::Sampler);
        EXPECT_EQ(device->CreateDescriptorSetLayout(duplicateLayout).Get(), nullptr) << ToString(backend);

        RHIDescriptorSetDesc nullSetDesc;
        EXPECT_EQ(device->CreateDescriptorSet(nullSetDesc).Get(), nullptr) << ToString(backend);

        RHIDescriptorSetLayoutDesc validLayoutDesc;
        validLayoutDesc.AddBinding(0, RHIBindingType::UniformBuffer);
        auto layout = device->CreateDescriptorSetLayout(validLayoutDesc);
        ASSERT_NE(nullptr, layout.Get()) << ToString(backend);

        RHIDescriptorSetDesc validSetDesc;
        validSetDesc.SetLayout(layout.Get());
        auto set = device->CreateDescriptorSet(validSetDesc);
        ASSERT_NE(nullptr, set.Get()) << ToString(backend);
        EXPECT_TRUE(set->Update({})) << ToString(backend);

        RHIDescriptorBinding unknownBinding;
        unknownBinding.binding = 99;
        EXPECT_FALSE(set->Update(std::vector<RHIDescriptorBinding>{unknownBinding})) << ToString(backend);

        RHIPipelineLayoutDesc invalidPipelineLayout;
        invalidPipelineLayout.setLayouts.push_back(nullptr);
        EXPECT_EQ(device->CreatePipelineLayout(invalidPipelineLayout).Get(), nullptr) << ToString(backend);

        RVX_CORE_INFO("Backend {}: Descriptor contract OK", ToString(backend));
    }

    RVX_GTEST_SKIP_IF_NO_GPU_BACKENDS(testedBackendCount);
}

// =============================================================================
// Fence Synchronization Consistency
// =============================================================================
TEST(CrossBackendValidation, FenceConsistency)
{
    std::vector<RHIBackendType> backends = GetAvailableBackends();
    uint32_t testedBackendCount = 0;

    for (auto backend : backends)
    {
        auto device = CreateDeviceForBackend(backend);
        if (!ShouldRunBackend(device, backend))
        {
            continue;
        }
        ++testedBackendCount;

        auto fence = device->CreateFence(0);
        ASSERT_NE(nullptr, fence.Get());
        EXPECT_EQ(fence->GetCompletedValue(), 0u);

        // CPU signal and wait
        fence->Signal(5);
        fence->Wait(5);
        EXPECT_TRUE(fence->GetCompletedValue() >= 5u);

        // GPU signal via command submission
        auto ctx = device->CreateCommandContext(RHICommandQueueType::Graphics);
        ctx->Begin();
        ctx->End();

        auto fence2 = device->CreateFence(0);
        device->SubmitCommandContext(ctx.Get(), fence2.Get());
        fence2->Wait(1);

        RVX_CORE_INFO("Backend {}: Fence synchronization OK", ToString(backend));
    }

    RVX_GTEST_SKIP_IF_NO_GPU_BACKENDS(testedBackendCount);
}

TEST(CrossBackendValidation, SubmitFenceValueConsistency)
{
    std::vector<RHIBackendType> backends = GetAvailableBackends();
    uint32_t testedBackendCount = 0;

    for (auto backend : backends)
    {
        auto device = CreateDeviceForBackend(backend);
        if (!ShouldRunBackend(device, backend))
        {
            continue;
        }
        ++testedBackendCount;

        const RHICapabilities& caps = device->GetCapabilities();
        EXPECT_TRUE(caps.supportsDefaultQueueFenceSignal || caps.emulatesQueueFences);

        auto fence = device->CreateFence(0);
        ASSERT_NE(nullptr, fence.Get());

        const RHICommandQueueType queueType =
            caps.supportsAsyncCompute ? RHICommandQueueType::Compute : RHICommandQueueType::Graphics;

        EXPECT_EQ(device->SubmitCommandContext(nullptr, fence.Get()), 0u) << ToString(backend);
        EXPECT_EQ(fence->GetCompletedValue(), 0u) << ToString(backend);

        EXPECT_EQ(device->SubmitCommandContexts(std::span<RHICommandContext* const>(), fence.Get()), 0u)
            << ToString(backend);
        EXPECT_EQ(fence->GetCompletedValue(), 0u) << ToString(backend);

        std::vector<RHICommandContext*> nullBatchContexts = { nullptr, nullptr };
        EXPECT_EQ(device->SubmitCommandContexts(
                      std::span<RHICommandContext* const>(nullBatchContexts.data(), nullBatchContexts.size()),
                      fence.Get()),
                  0u)
            << ToString(backend);
        EXPECT_EQ(fence->GetCompletedValue(), 0u) << ToString(backend);

        auto noFenceContext = device->CreateCommandContext(queueType);
        ASSERT_NE(nullptr, noFenceContext.Get());
        noFenceContext->Begin();
        noFenceContext->End();
        EXPECT_EQ(device->SubmitCommandContext(noFenceContext.Get(), nullptr), 0u);

        auto firstContext = device->CreateCommandContext(queueType);
        ASSERT_NE(nullptr, firstContext.Get());
        firstContext->Begin();
        firstContext->End();
        uint64 firstValue = device->SubmitCommandContext(firstContext.Get(), fence.Get());

        auto secondContext = device->CreateCommandContext(queueType);
        ASSERT_NE(nullptr, secondContext.Get());
        secondContext->Begin();
        secondContext->End();
        uint64 secondValue = device->SubmitCommandContext(secondContext.Get(), fence.Get());

        EXPECT_NE(firstValue, 0u) << ToString(backend);
        EXPECT_GT(secondValue, firstValue) << ToString(backend);

        device->WaitForFence(fence.Get(), secondValue);
        EXPECT_GE(fence->GetCompletedValue(), secondValue) << ToString(backend);

        auto batchFence = device->CreateFence(0);
        ASSERT_NE(nullptr, batchFence.Get());

        auto batchContextA = device->CreateCommandContext(queueType);
        auto batchContextB = device->CreateCommandContext(queueType);
        ASSERT_NE(nullptr, batchContextA.Get());
        ASSERT_NE(nullptr, batchContextB.Get());

        batchContextA->Begin();
        batchContextA->End();
        batchContextB->Begin();
        batchContextB->End();

        std::vector<RHICommandContext*> batchContexts = { batchContextA.Get(), batchContextB.Get() };
        uint64 batchValue = device->SubmitCommandContexts(
            std::span<RHICommandContext* const>(batchContexts.data(), batchContexts.size()),
            batchFence.Get());
        EXPECT_NE(batchValue, 0u) << ToString(backend);

        device->WaitForFence(batchFence.Get(), batchValue);
        EXPECT_GE(batchFence->GetCompletedValue(), batchValue) << ToString(backend);

        RVX_CORE_INFO("Backend {}: Submit fence values OK", ToString(backend));
    }

    RVX_GTEST_SKIP_IF_NO_GPU_BACKENDS(testedBackendCount);
}

// =============================================================================
// Heap and Placed Resources Consistency
// =============================================================================
TEST(CrossBackendValidation, HeapConsistency)
{
    std::vector<RHIBackendType> backends = GetAvailableBackends();
    uint32_t testedBackendCount = 0;

    for (auto backend : backends)
    {
        auto device = CreateDeviceForBackend(backend);
        if (!ShouldRunBackend(device, backend))
        {
            continue;
        }
        ++testedBackendCount;

        const RHICapabilities& caps = device->GetCapabilities();

        // Create heap
        RHIHeapDesc heapDesc;
        heapDesc.size = 32 * 1024 * 1024;  // 32 MB
        heapDesc.type = RHIHeapType::Default;
        heapDesc.flags = RHIHeapFlags::AllowRenderTargets;

        auto heap = device->CreateHeap(heapDesc);
        if (!caps.supportsExplicitHeapManagement)
        {
            EXPECT_EQ(nullptr, heap.Get()) << ToString(backend);
            RVX_CORE_INFO("Backend {}: Explicit heaps unsupported as advertised", ToString(backend));
            continue;
        }

        ASSERT_NE(nullptr, heap.Get());
        EXPECT_EQ(heap->GetSize(), heapDesc.size);
        EXPECT_EQ(heap->GetType(), heapDesc.type);

        // Create placed texture
        auto texDesc = RHITextureDesc::RenderTarget(512, 512, RHIFormat::RGBA16_FLOAT);
        auto memReq = device->GetTextureMemoryRequirements(texDesc);
        EXPECT_TRUE(memReq.size > 0);
        EXPECT_TRUE(memReq.alignment > 0);

        auto texture = device->CreatePlacedTexture(heap.Get(), 0, texDesc);
        ASSERT_NE(nullptr, texture.Get());
        EXPECT_EQ(texture->GetWidth(), 512u);
        EXPECT_EQ(texture->GetHeight(), 512u);

        RVX_CORE_INFO("Backend {}: Heap and placed resources OK", ToString(backend));
    }

    RVX_GTEST_SKIP_IF_NO_GPU_BACKENDS(testedBackendCount);
}

// =============================================================================
// Sampler Creation Consistency
// =============================================================================
TEST(CrossBackendValidation, SamplerConsistency)
{
    std::vector<RHIBackendType> backends = GetAvailableBackends();
    uint32_t testedBackendCount = 0;

    struct SamplerTestCase
    {
        const char* name;
        RHISamplerDesc desc;
    };

    std::vector<SamplerTestCase> testCases;

    // Point sampling
    {
        RHISamplerDesc desc;
        desc.magFilter = RHIFilterMode::Nearest;
        desc.minFilter = RHIFilterMode::Nearest;
        desc.mipFilter = RHIFilterMode::Nearest;
        testCases.push_back({"Point", desc});
    }

    // Linear sampling
    {
        RHISamplerDesc desc;
        desc.magFilter = RHIFilterMode::Linear;
        desc.minFilter = RHIFilterMode::Linear;
        desc.mipFilter = RHIFilterMode::Linear;
        testCases.push_back({"Linear", desc});
    }

    // Anisotropic sampling
    {
        RHISamplerDesc desc;
        desc.magFilter = RHIFilterMode::Linear;
        desc.minFilter = RHIFilterMode::Linear;
        desc.mipFilter = RHIFilterMode::Linear;
        desc.anisotropyEnable = true;
        desc.maxAnisotropy = 16.0f;
        testCases.push_back({"Anisotropic", desc});
    }

    for (auto backend : backends)
    {
        auto device = CreateDeviceForBackend(backend);
        if (!ShouldRunBackend(device, backend))
        {
            continue;
        }
        ++testedBackendCount;

        for (auto& tc : testCases)
        {
            auto sampler = device->CreateSampler(tc.desc);
            if (!sampler)
            {
                RVX_CORE_ERROR("Backend {} failed to create sampler: {}", ToString(backend), tc.name);
                FAIL() << "Backend " << ToString(backend) << " failed to create sampler: " << tc.name;
            }
        }

        RVX_CORE_INFO("Backend {}: All sampler types OK", ToString(backend));
    }

    RVX_GTEST_SKIP_IF_NO_GPU_BACKENDS(testedBackendCount);
}
