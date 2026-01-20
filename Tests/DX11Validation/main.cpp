#include "Core/Core.h"
#include "RHI/RHI.h"
#include "TestFramework/TestRunner.h"

using namespace RVX;
using namespace RVX::Test;

// =============================================================================
// DX11 Validation Tests
// =============================================================================

bool Test_DeviceCreation()
{
    RHIDeviceDesc desc;
    desc.enableDebugLayer = true;
    
    auto device = CreateRHIDevice(RHIBackendType::DX11, desc);
    TEST_ASSERT_NOT_NULL(device.get());
    TEST_ASSERT_EQ(device->GetBackendType(), RHIBackendType::DX11);
    
    return true;
}

bool Test_Capabilities()
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX11, deviceDesc);
    TEST_ASSERT_NOT_NULL(device.get());
    
    const auto& caps = device->GetCapabilities();
    
    // DX11 should NOT support bindless or raytracing
    TEST_ASSERT_FALSE(caps.supportsBindless);
    TEST_ASSERT_FALSE(caps.supportsRaytracing);
    
    // Should have an adapter name
    TEST_ASSERT_FALSE(caps.adapterName.empty());
    
    RVX_CORE_INFO("DX11 Adapter: {}", caps.adapterName);
    
    return true;
}

bool Test_BufferCreation()
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX11, deviceDesc);
    TEST_ASSERT_NOT_NULL(device.get());
    
    RHIBufferDesc bufferDesc;
    bufferDesc.size = 1024;
    bufferDesc.usage = RHIBufferUsage::Vertex;
    bufferDesc.memoryType = RHIMemoryType::Default;
    bufferDesc.debugName = "TestVertexBuffer";
    
    auto buffer = device->CreateBuffer(bufferDesc);
    TEST_ASSERT_NOT_NULL(buffer.Get());
    TEST_ASSERT_EQ(buffer->GetSize(), 1024u);
    
    return true;
}

bool Test_UploadBuffer()
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX11, deviceDesc);
    TEST_ASSERT_NOT_NULL(device.get());
    
    RHIBufferDesc bufferDesc;
    bufferDesc.size = 256;
    bufferDesc.usage = RHIBufferUsage::Constant;
    bufferDesc.memoryType = RHIMemoryType::Upload;
    bufferDesc.debugName = "TestUploadBuffer";
    
    auto buffer = device->CreateBuffer(bufferDesc);
    TEST_ASSERT_NOT_NULL(buffer.Get());
    
    void* mappedData = buffer->Map();
    TEST_ASSERT_NOT_NULL(mappedData);
    
    float testData[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    std::memcpy(mappedData, testData, sizeof(testData));
    buffer->Unmap();
    
    return true;
}

bool Test_TextureCreation()
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX11, deviceDesc);
    TEST_ASSERT_NOT_NULL(device.get());
    
    auto textureDesc = RHITextureDesc::Texture2D(512, 512, RHIFormat::RGBA8_UNORM);
    textureDesc.debugName = "TestTexture";
    auto texture = device->CreateTexture(textureDesc);
    TEST_ASSERT_NOT_NULL(texture.Get());
    TEST_ASSERT_EQ(texture->GetWidth(), 512u);
    TEST_ASSERT_EQ(texture->GetHeight(), 512u);
    
    return true;
}

bool Test_RenderTargetTexture()
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX11, deviceDesc);
    TEST_ASSERT_NOT_NULL(device.get());
    
    auto textureDesc = RHITextureDesc::RenderTarget(1920, 1080, RHIFormat::RGBA16_FLOAT);
    textureDesc.debugName = "TestRenderTarget";
    auto texture = device->CreateTexture(textureDesc);
    TEST_ASSERT_NOT_NULL(texture.Get());
    TEST_ASSERT_TRUE(HasFlag(texture->GetUsage(), RHITextureUsage::RenderTarget));
    
    return true;
}

bool Test_DepthStencilTexture()
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX11, deviceDesc);
    TEST_ASSERT_NOT_NULL(device.get());
    
    auto textureDesc = RHITextureDesc::DepthStencil(1920, 1080, RHIFormat::D24_UNORM_S8_UINT);
    textureDesc.debugName = "TestDepthStencil";
    auto texture = device->CreateTexture(textureDesc);
    TEST_ASSERT_NOT_NULL(texture.Get());
    TEST_ASSERT_TRUE(HasFlag(texture->GetUsage(), RHITextureUsage::DepthStencil));
    
    return true;
}

bool Test_TextureView()
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX11, deviceDesc);
    TEST_ASSERT_NOT_NULL(device.get());
    
    auto textureDesc = RHITextureDesc::Texture2D(256, 256, RHIFormat::RGBA8_UNORM);
    auto texture = device->CreateTexture(textureDesc);
    TEST_ASSERT_NOT_NULL(texture.Get());
    
    RHITextureViewDesc viewDesc;
    viewDesc.format = RHIFormat::RGBA8_UNORM;
    auto view = device->CreateTextureView(texture.Get(), viewDesc);
    TEST_ASSERT_NOT_NULL(view.Get());
    
    return true;
}

bool Test_Sampler()
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX11, deviceDesc);
    TEST_ASSERT_NOT_NULL(device.get());
    
    RHISamplerDesc samplerDesc;
    samplerDesc.magFilter = RHIFilterMode::Linear;
    samplerDesc.minFilter = RHIFilterMode::Linear;
    samplerDesc.mipFilter = RHIFilterMode::Linear;
    samplerDesc.addressU = RHIAddressMode::Repeat;
    samplerDesc.addressV = RHIAddressMode::Repeat;
    samplerDesc.addressW = RHIAddressMode::Repeat;
    samplerDesc.debugName = "TestSampler";
    
    auto sampler = device->CreateSampler(samplerDesc);
    TEST_ASSERT_NOT_NULL(sampler.Get());
    
    return true;
}

bool Test_CommandContext()
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX11, deviceDesc);
    TEST_ASSERT_NOT_NULL(device.get());
    
    auto ctx = device->CreateCommandContext(RHICommandQueueType::Graphics);
    TEST_ASSERT_NOT_NULL(ctx.Get());
    
    ctx->Begin();
    ctx->End();
    
    device->SubmitCommandContext(ctx.Get(), nullptr);
    device->WaitIdle();
    
    return true;
}

bool Test_MultipleBufferTypes()
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX11, deviceDesc);
    TEST_ASSERT_NOT_NULL(device.get());
    
    // Vertex buffer
    RHIBufferDesc vbDesc;
    vbDesc.size = 4096;
    vbDesc.usage = RHIBufferUsage::Vertex;
    vbDesc.memoryType = RHIMemoryType::Upload;
    vbDesc.stride = 32;
    auto vb = device->CreateBuffer(vbDesc);
    TEST_ASSERT_NOT_NULL(vb.Get());
    
    // Index buffer
    RHIBufferDesc ibDesc;
    ibDesc.size = 2048;
    ibDesc.usage = RHIBufferUsage::Index;
    ibDesc.memoryType = RHIMemoryType::Upload;
    auto ib = device->CreateBuffer(ibDesc);
    TEST_ASSERT_NOT_NULL(ib.Get());
    
    // Constant buffer
    RHIBufferDesc cbDesc;
    cbDesc.size = 256;
    cbDesc.usage = RHIBufferUsage::Constant;
    cbDesc.memoryType = RHIMemoryType::Upload;
    auto cb = device->CreateBuffer(cbDesc);
    TEST_ASSERT_NOT_NULL(cb.Get());
    
    return true;
}

bool Test_BarrierOperations()
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX11, deviceDesc);
    TEST_ASSERT_NOT_NULL(device.get());
    
    // DX11 doesn't have explicit barriers like DX12/Vulkan
    // But our abstraction should still accept them gracefully (as no-ops)
    
    auto rtDesc = RHITextureDesc::RenderTarget(512, 512, RHIFormat::RGBA8_UNORM);
    auto texture = device->CreateTexture(rtDesc);
    TEST_ASSERT_NOT_NULL(texture.Get());
    
    auto ctx = device->CreateCommandContext(RHICommandQueueType::Graphics);
    ctx->Begin();
    
    // These should be no-ops on DX11 but shouldn't crash
    ctx->TextureBarrier({texture.Get(), RHIResourceState::Undefined, RHIResourceState::RenderTarget});
    ctx->TextureBarrier({texture.Get(), RHIResourceState::RenderTarget, RHIResourceState::ShaderResource});
    
    ctx->End();
    device->SubmitCommandContext(ctx.Get(), nullptr);
    device->WaitIdle();
    
    return true;
}

int main()
{
    Log::Initialize();

#if !defined(_WIN32)
    // DX11 is only available on Windows
    RVX_CORE_INFO("DX11 Validation Tests - SKIPPED (only available on Windows)");
    Log::Shutdown();
    return 0;
#endif

    RVX_CORE_INFO("DX11 Validation Tests");
    
    TestSuite suite;
    
    // Device tests
    suite.AddTest("DeviceCreation", Test_DeviceCreation);
    suite.AddTest("Capabilities", Test_Capabilities);
    
    // Buffer tests
    suite.AddTest("BufferCreation", Test_BufferCreation);
    suite.AddTest("UploadBuffer", Test_UploadBuffer);
    suite.AddTest("MultipleBufferTypes", Test_MultipleBufferTypes);
    
    // Texture tests
    suite.AddTest("TextureCreation", Test_TextureCreation);
    suite.AddTest("RenderTargetTexture", Test_RenderTargetTexture);
    suite.AddTest("DepthStencilTexture", Test_DepthStencilTexture);
    suite.AddTest("TextureView", Test_TextureView);
    
    // Other resources
    suite.AddTest("Sampler", Test_Sampler);
    suite.AddTest("CommandContext", Test_CommandContext);
    suite.AddTest("BarrierOperations", Test_BarrierOperations);
    
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
