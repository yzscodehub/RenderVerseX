#include "Core/Core.h"
#include "RHI/RHI.h"
#include "TestFramework/TestRunner.h"

using namespace RVX;
using namespace RVX::Test;

// =============================================================================
// Vulkan Validation Tests
// =============================================================================

bool Test_DeviceCreation()
{
    RHIDeviceDesc desc;
    desc.enableDebugLayer = true;
    
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, desc);
    TEST_ASSERT_NOT_NULL(device.get());
    TEST_ASSERT_EQ(device->GetBackendType(), RHIBackendType::Vulkan);
    
    return true;
}

bool Test_BufferCreation()
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::Vulkan, deviceDesc);
    TEST_ASSERT_NOT_NULL(device.get());
    
    RHIBufferDesc bufferDesc;
    bufferDesc.size = 1024;
    bufferDesc.usage = RHIBufferUsage::Vertex;
    bufferDesc.memoryType = RHIMemoryType::Default;
    
    auto buffer = device->CreateBuffer(bufferDesc);
    TEST_ASSERT_NOT_NULL(buffer.Get());
    
    return true;
}

int main()
{
    Log::Initialize();
    RVX_CORE_INFO("Vulkan Validation Tests");
    
    TestSuite suite;
    suite.AddTest("DeviceCreation", Test_DeviceCreation);
    suite.AddTest("BufferCreation", Test_BufferCreation);
    
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
