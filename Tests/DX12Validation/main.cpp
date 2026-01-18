#include "Core/Core.h"
#include "RHI/RHI.h"
#include "TestFramework/TestRunner.h"

using namespace RVX;
using namespace RVX::Test;

// =============================================================================
// DX12 Validation Tests
// =============================================================================

bool Test_DeviceCreation()
{
    RHIDeviceDesc desc;
    desc.enableDebugLayer = true;
    
    auto device = CreateRHIDevice(RHIBackendType::DX12, desc);
    TEST_ASSERT_NOT_NULL(device.get());
    TEST_ASSERT_EQ(device->GetBackendType(), RHIBackendType::DX12);
    
    return true;
}

bool Test_BufferCreation()
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
    TEST_ASSERT_NOT_NULL(device.get());
    
    // Test Default buffer
    RHIBufferDesc bufferDesc;
    bufferDesc.size = 1024;
    bufferDesc.usage = RHIBufferUsage::Vertex;
    bufferDesc.memoryType = RHIMemoryType::Default;
    bufferDesc.debugName = "TestVertexBuffer";
    
    auto buffer = device->CreateBuffer(bufferDesc);
    TEST_ASSERT_NOT_NULL(buffer.Get());
    TEST_ASSERT_EQ(buffer->GetSize(), 1024u);
    TEST_ASSERT_EQ(buffer->GetMemoryType(), RHIMemoryType::Default);
    
    return true;
}

bool Test_UploadBuffer()
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
    TEST_ASSERT_NOT_NULL(device.get());
    
    // Test Upload buffer with data
    RHIBufferDesc bufferDesc;
    bufferDesc.size = 256;
    bufferDesc.usage = RHIBufferUsage::Constant;
    bufferDesc.memoryType = RHIMemoryType::Upload;
    bufferDesc.debugName = "TestUploadBuffer";
    
    auto buffer = device->CreateBuffer(bufferDesc);
    TEST_ASSERT_NOT_NULL(buffer.Get());
    
    // Map and write data
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
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
    TEST_ASSERT_NOT_NULL(device.get());
    
    auto textureDesc = RHITextureDesc::Texture2D(512, 512, RHIFormat::RGBA8_UNORM);
    textureDesc.debugName = "TestTexture";
    auto texture = device->CreateTexture(textureDesc);
    TEST_ASSERT_NOT_NULL(texture.Get());
    TEST_ASSERT_EQ(texture->GetWidth(), 512u);
    TEST_ASSERT_EQ(texture->GetHeight(), 512u);
    TEST_ASSERT_EQ(texture->GetFormat(), RHIFormat::RGBA8_UNORM);
    
    return true;
}

bool Test_RenderTargetTexture()
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
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
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
    TEST_ASSERT_NOT_NULL(device.get());
    
    auto textureDesc = RHITextureDesc::DepthStencil(1920, 1080, RHIFormat::D24_UNORM_S8_UINT);
    textureDesc.debugName = "TestDepthStencil";
    auto texture = device->CreateTexture(textureDesc);
    TEST_ASSERT_NOT_NULL(texture.Get());
    TEST_ASSERT_TRUE(HasFlag(texture->GetUsage(), RHITextureUsage::DepthStencil));
    TEST_ASSERT_EQ(texture->GetFormat(), RHIFormat::D24_UNORM_S8_UINT);
    
    return true;
}

bool Test_TextureView()
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
    TEST_ASSERT_NOT_NULL(device.get());
    
    auto textureDesc = RHITextureDesc::Texture2D(256, 256, RHIFormat::RGBA8_UNORM);
    auto texture = device->CreateTexture(textureDesc);
    TEST_ASSERT_NOT_NULL(texture.Get());
    
    RHITextureViewDesc viewDesc;
    viewDesc.format = RHIFormat::RGBA8_UNORM;
    auto view = device->CreateTextureView(texture.Get(), viewDesc);
    TEST_ASSERT_NOT_NULL(view.Get());
    TEST_ASSERT_EQ(view->GetTexture(), texture.Get());
    
    return true;
}

bool Test_Sampler()
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
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
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
    TEST_ASSERT_NOT_NULL(device.get());
    
    auto ctx = device->CreateCommandContext(RHICommandQueueType::Graphics);
    TEST_ASSERT_NOT_NULL(ctx.Get());
    
    ctx->Begin();
    ctx->End();
    
    device->SubmitCommandContext(ctx.Get(), nullptr);
    device->WaitIdle();
    
    return true;
}

bool Test_Fence()
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
    TEST_ASSERT_NOT_NULL(device.get());
    
    auto fence = device->CreateFence(0);
    TEST_ASSERT_NOT_NULL(fence.Get());
    TEST_ASSERT_EQ(fence->GetCompletedValue(), 0u);
    
    // Signal from CPU
    fence->Signal(1);
    fence->Wait(1);
    TEST_ASSERT_TRUE(fence->GetCompletedValue() >= 1u);
    
    return true;
}

bool Test_Heap()
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
    TEST_ASSERT_NOT_NULL(device.get());
    
    RHIHeapDesc heapDesc;
    heapDesc.size = 64 * 1024 * 1024;  // 64 MB
    heapDesc.type = RHIHeapType::Default;
    heapDesc.flags = RHIHeapFlags::AllowRenderTargets;
    heapDesc.debugName = "TestHeap";
    
    auto heap = device->CreateHeap(heapDesc);
    TEST_ASSERT_NOT_NULL(heap.Get());
    TEST_ASSERT_EQ(heap->GetSize(), 64u * 1024u * 1024u);
    TEST_ASSERT_EQ(heap->GetType(), RHIHeapType::Default);
    
    return true;
}

bool Test_PlacedTexture()
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
    TEST_ASSERT_NOT_NULL(device.get());
    
    // Create heap
    RHIHeapDesc heapDesc;
    heapDesc.size = 64 * 1024 * 1024;  // 64 MB
    heapDesc.type = RHIHeapType::Default;
    heapDesc.flags = RHIHeapFlags::AllowRenderTargets;
    
    auto heap = device->CreateHeap(heapDesc);
    TEST_ASSERT_NOT_NULL(heap.Get());
    
    // Create placed texture
    auto textureDesc = RHITextureDesc::RenderTarget(1024, 1024, RHIFormat::RGBA16_FLOAT);
    textureDesc.debugName = "PlacedRenderTarget";
    
    auto memReq = device->GetTextureMemoryRequirements(textureDesc);
    TEST_ASSERT_TRUE(memReq.size > 0);
    TEST_ASSERT_TRUE(memReq.alignment > 0);
    
    auto texture = device->CreatePlacedTexture(heap.Get(), 0, textureDesc);
    TEST_ASSERT_NOT_NULL(texture.Get());
    TEST_ASSERT_EQ(texture->GetWidth(), 1024u);
    TEST_ASSERT_EQ(texture->GetHeight(), 1024u);
    
    return true;
}

bool Test_PlacedBuffer()
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
    TEST_ASSERT_NOT_NULL(device.get());
    
    // Create heap for buffers
    RHIHeapDesc heapDesc;
    heapDesc.size = 16 * 1024 * 1024;  // 16 MB
    heapDesc.type = RHIHeapType::Default;
    heapDesc.flags = RHIHeapFlags::AllowBuffers;
    
    auto heap = device->CreateHeap(heapDesc);
    TEST_ASSERT_NOT_NULL(heap.Get());
    
    // Create placed buffer
    RHIBufferDesc bufferDesc;
    bufferDesc.size = 1024 * 1024;  // 1 MB
    bufferDesc.usage = RHIBufferUsage::UnorderedAccess | RHIBufferUsage::Structured;
    bufferDesc.memoryType = RHIMemoryType::Default;
    bufferDesc.stride = 16;
    bufferDesc.debugName = "PlacedStructuredBuffer";
    
    auto memReq = device->GetBufferMemoryRequirements(bufferDesc);
    TEST_ASSERT_TRUE(memReq.size > 0);
    
    auto buffer = device->CreatePlacedBuffer(heap.Get(), 0, bufferDesc);
    TEST_ASSERT_NOT_NULL(buffer.Get());
    TEST_ASSERT_EQ(buffer->GetSize(), 1024u * 1024u);
    
    return true;
}

bool Test_MultipleBufferTypes()
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
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
    
    // Structured buffer
    RHIBufferDesc sbDesc;
    sbDesc.size = 1024;
    sbDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
    sbDesc.memoryType = RHIMemoryType::Default;
    sbDesc.stride = 16;
    auto sb = device->CreateBuffer(sbDesc);
    TEST_ASSERT_NOT_NULL(sb.Get());
    
    return true;
}

int main()
{
    Log::Initialize();
    RVX_CORE_INFO("DX12 Validation Tests");
    
    TestSuite suite;
    
    // Device tests
    suite.AddTest("DeviceCreation", Test_DeviceCreation);
    
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
    suite.AddTest("Fence", Test_Fence);
    
    // Memory aliasing tests
    suite.AddTest("Heap", Test_Heap);
    suite.AddTest("PlacedTexture", Test_PlacedTexture);
    suite.AddTest("PlacedBuffer", Test_PlacedBuffer);
    
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
