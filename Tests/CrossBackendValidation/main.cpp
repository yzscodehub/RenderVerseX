#include "Core/Core.h"
#include "RHI/RHI.h"
#include "TestFramework/TestRunner.h"

using namespace RVX;
using namespace RVX::Test;

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

// Test struct for results comparison
struct BackendTestResult
{
    RHIBackendType backend;
    bool success;
    std::string message;
};

// =============================================================================
// Buffer Creation Consistency
// =============================================================================
bool Test_BufferCreationConsistency()
{
    std::vector<RHIBackendType> backends = {
        RHIBackendType::DX12,
        RHIBackendType::Vulkan
    };
    
    RHIBufferDesc bufferDesc;
    bufferDesc.size = 4096;
    bufferDesc.usage = RHIBufferUsage::Vertex | RHIBufferUsage::Index;
    bufferDesc.memoryType = RHIMemoryType::Upload;
    bufferDesc.stride = 32;
    bufferDesc.debugName = "CrossBackendBuffer";
    
    for (auto backend : backends)
    {
        auto device = CreateDeviceForBackend(backend);
        if (!device)
        {
            RVX_CORE_WARN("Backend {} not available, skipping", ToString(backend));
            continue;
        }
        
        auto buffer = device->CreateBuffer(bufferDesc);
        if (!buffer)
        {
            RVX_CORE_ERROR("Backend {} failed to create buffer", ToString(backend));
            return false;
        }
        
        // Verify properties match
        TEST_ASSERT_EQ(buffer->GetSize(), bufferDesc.size);
        TEST_ASSERT_EQ(buffer->GetStride(), bufferDesc.stride);
        TEST_ASSERT_EQ(buffer->GetMemoryType(), bufferDesc.memoryType);
        
        // Test mapping
        void* data = buffer->Map();
        TEST_ASSERT_NOT_NULL(data);
        buffer->Unmap();
        
        RVX_CORE_INFO("Backend {}: Buffer creation OK", ToString(backend));
    }
    
    return true;
}

// =============================================================================
// Texture Creation Consistency
// =============================================================================
bool Test_TextureCreationConsistency()
{
    std::vector<RHIBackendType> backends = {
        RHIBackendType::DX12,
        RHIBackendType::Vulkan
    };
    
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
        if (!device)
        {
            RVX_CORE_WARN("Backend {} not available, skipping", ToString(backend));
            continue;
        }
        
        for (auto& tc : testCases)
        {
            auto texture = device->CreateTexture(tc.desc);
            if (!texture)
            {
                RVX_CORE_ERROR("Backend {} failed to create texture: {}", ToString(backend), tc.name);
                return false;
            }
            
            TEST_ASSERT_EQ(texture->GetWidth(), tc.desc.width);
            TEST_ASSERT_EQ(texture->GetHeight(), tc.desc.height);
            TEST_ASSERT_EQ(texture->GetFormat(), tc.desc.format);
        }
        
        RVX_CORE_INFO("Backend {}: All texture types OK", ToString(backend));
    }
    
    return true;
}

// =============================================================================
// Command Context Consistency
// =============================================================================
bool Test_CommandContextConsistency()
{
    std::vector<RHIBackendType> backends = {
        RHIBackendType::DX12,
        RHIBackendType::Vulkan
    };
    
    for (auto backend : backends)
    {
        auto device = CreateDeviceForBackend(backend);
        if (!device)
        {
            RVX_CORE_WARN("Backend {} not available, skipping", ToString(backend));
            continue;
        }
        
        // Test all queue types
        auto graphicsCtx = device->CreateCommandContext(RHICommandQueueType::Graphics);
        auto computeCtx = device->CreateCommandContext(RHICommandQueueType::Compute);
        auto copyCtx = device->CreateCommandContext(RHICommandQueueType::Copy);
        
        TEST_ASSERT_NOT_NULL(graphicsCtx.Get());
        TEST_ASSERT_NOT_NULL(computeCtx.Get());
        TEST_ASSERT_NOT_NULL(copyCtx.Get());
        
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
    
    return true;
}

// =============================================================================
// Barrier Operations Consistency
// =============================================================================
bool Test_BarrierOperationsConsistency()
{
    std::vector<RHIBackendType> backends = {
        RHIBackendType::DX12,
        RHIBackendType::Vulkan
    };
    
    for (auto backend : backends)
    {
        auto device = CreateDeviceForBackend(backend);
        if (!device)
        {
            RVX_CORE_WARN("Backend {} not available, skipping", ToString(backend));
            continue;
        }
        
        // Create test resources
        auto rtDesc = RHITextureDesc::RenderTarget(512, 512, RHIFormat::RGBA8_UNORM);
        auto texture = device->CreateTexture(rtDesc);
        TEST_ASSERT_NOT_NULL(texture.Get());
        
        RHIBufferDesc bufDesc;
        bufDesc.size = 4096;
        bufDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::UnorderedAccess;
        bufDesc.memoryType = RHIMemoryType::Default;
        auto buffer = device->CreateBuffer(bufDesc);
        TEST_ASSERT_NOT_NULL(buffer.Get());
        
        auto ctx = device->CreateCommandContext(RHICommandQueueType::Graphics);
        ctx->Begin();
        
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
    
    return true;
}

// =============================================================================
// Fence Synchronization Consistency
// =============================================================================
bool Test_FenceConsistency()
{
    std::vector<RHIBackendType> backends = {
        RHIBackendType::DX12,
        RHIBackendType::Vulkan
    };
    
    for (auto backend : backends)
    {
        auto device = CreateDeviceForBackend(backend);
        if (!device)
        {
            RVX_CORE_WARN("Backend {} not available, skipping", ToString(backend));
            continue;
        }
        
        auto fence = device->CreateFence(0);
        TEST_ASSERT_NOT_NULL(fence.Get());
        TEST_ASSERT_EQ(fence->GetCompletedValue(), 0u);
        
        // CPU signal and wait
        fence->Signal(5);
        fence->Wait(5);
        TEST_ASSERT_TRUE(fence->GetCompletedValue() >= 5u);
        
        // GPU signal via command submission
        auto ctx = device->CreateCommandContext(RHICommandQueueType::Graphics);
        ctx->Begin();
        ctx->End();
        
        auto fence2 = device->CreateFence(0);
        device->SubmitCommandContext(ctx.Get(), fence2.Get());
        fence2->Wait(1);
        
        RVX_CORE_INFO("Backend {}: Fence synchronization OK", ToString(backend));
    }
    
    return true;
}

// =============================================================================
// Heap and Placed Resources Consistency
// =============================================================================
bool Test_HeapConsistency()
{
    std::vector<RHIBackendType> backends = {
        RHIBackendType::DX12,
        RHIBackendType::Vulkan
    };
    
    for (auto backend : backends)
    {
        auto device = CreateDeviceForBackend(backend);
        if (!device)
        {
            RVX_CORE_WARN("Backend {} not available, skipping", ToString(backend));
            continue;
        }
        
        // Create heap
        RHIHeapDesc heapDesc;
        heapDesc.size = 32 * 1024 * 1024;  // 32 MB
        heapDesc.type = RHIHeapType::Default;
        heapDesc.flags = RHIHeapFlags::AllowRenderTargets;
        
        auto heap = device->CreateHeap(heapDesc);
        TEST_ASSERT_NOT_NULL(heap.Get());
        TEST_ASSERT_EQ(heap->GetSize(), heapDesc.size);
        TEST_ASSERT_EQ(heap->GetType(), heapDesc.type);
        
        // Create placed texture
        auto texDesc = RHITextureDesc::RenderTarget(512, 512, RHIFormat::RGBA16_FLOAT);
        auto memReq = device->GetTextureMemoryRequirements(texDesc);
        TEST_ASSERT_TRUE(memReq.size > 0);
        TEST_ASSERT_TRUE(memReq.alignment > 0);
        
        auto texture = device->CreatePlacedTexture(heap.Get(), 0, texDesc);
        TEST_ASSERT_NOT_NULL(texture.Get());
        TEST_ASSERT_EQ(texture->GetWidth(), 512u);
        TEST_ASSERT_EQ(texture->GetHeight(), 512u);
        
        RVX_CORE_INFO("Backend {}: Heap and placed resources OK", ToString(backend));
    }
    
    return true;
}

// =============================================================================
// Sampler Creation Consistency  
// =============================================================================
bool Test_SamplerConsistency()
{
    std::vector<RHIBackendType> backends = {
        RHIBackendType::DX12,
        RHIBackendType::Vulkan
    };
    
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
        if (!device)
        {
            RVX_CORE_WARN("Backend {} not available, skipping", ToString(backend));
            continue;
        }
        
        for (auto& tc : testCases)
        {
            auto sampler = device->CreateSampler(tc.desc);
            if (!sampler)
            {
                RVX_CORE_ERROR("Backend {} failed to create sampler: {}", ToString(backend), tc.name);
                return false;
            }
        }
        
        RVX_CORE_INFO("Backend {}: All sampler types OK", ToString(backend));
    }
    
    return true;
}

int main()
{
    Log::Initialize();
    RVX_CORE_INFO("Cross-Backend Validation Tests");
    RVX_CORE_INFO("Testing consistency between DX12 and Vulkan backends");
    
    TestSuite suite;
    
    suite.AddTest("BufferCreationConsistency", Test_BufferCreationConsistency);
    suite.AddTest("TextureCreationConsistency", Test_TextureCreationConsistency);
    suite.AddTest("CommandContextConsistency", Test_CommandContextConsistency);
    suite.AddTest("BarrierOperationsConsistency", Test_BarrierOperationsConsistency);
    suite.AddTest("FenceConsistency", Test_FenceConsistency);
    suite.AddTest("HeapConsistency", Test_HeapConsistency);
    suite.AddTest("SamplerConsistency", Test_SamplerConsistency);
    
    auto results = suite.Run();
    suite.PrintResults(results);
    
    Log::Shutdown();
    
    for (const auto& result : results)
    {
        if (!result.passed)
            return 1;
    }
    
    return 0;
}
