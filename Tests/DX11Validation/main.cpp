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

bool Test_BindlessCapability()
{
    RHIDeviceDesc deviceDesc;
    auto device = CreateRHIDevice(RHIBackendType::DX11, deviceDesc);
    TEST_ASSERT_NOT_NULL(device.get());
    
    // DX11 should NOT support bindless
    const auto& caps = device->GetCapabilities();
    TEST_ASSERT_FALSE(caps.supportsBindless);
    
    return true;
}

int main()
{
    Log::Initialize();
    RVX_CORE_INFO("DX11 Validation Tests");
    
    TestSuite suite;
    suite.AddTest("DeviceCreation", Test_DeviceCreation);
    suite.AddTest("BindlessCapability", Test_BindlessCapability);
    
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
