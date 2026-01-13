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
    
    RHIBufferDesc bufferDesc;
    bufferDesc.size = 1024;
    bufferDesc.usage = RHIBufferUsage::Vertex;
    bufferDesc.memoryType = RHIMemoryType::Default;
    
    auto buffer = device->CreateBuffer(bufferDesc);
    TEST_ASSERT_NOT_NULL(buffer.Get());
    TEST_ASSERT_EQ(buffer->GetSize(), 1024u);
    
    return true;
}

bool Test_TextureCreation()
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX12, deviceDesc);
    TEST_ASSERT_NOT_NULL(device.get());
    
    auto textureDesc = RHITextureDesc::Texture2D(512, 512, RHIFormat::RGBA8_UNORM);
    auto texture = device->CreateTexture(textureDesc);
    TEST_ASSERT_NOT_NULL(texture.Get());
    TEST_ASSERT_EQ(texture->GetWidth(), 512u);
    TEST_ASSERT_EQ(texture->GetHeight(), 512u);
    
    return true;
}

int main()
{
    Log::Initialize();
    RVX_CORE_INFO("DX12 Validation Tests");
    
    TestSuite suite;
    suite.AddTest("DeviceCreation", Test_DeviceCreation);
    suite.AddTest("BufferCreation", Test_BufferCreation);
    suite.AddTest("TextureCreation", Test_TextureCreation);
    
    auto results = suite.Run();
    suite.PrintResults(results);
    
    Log::Shutdown();
    
    // Return non-zero if any test failed
    for (const auto& result : results)
    {
        if (!result.passed)
            return 1;
    }
    
    return 0;
}
