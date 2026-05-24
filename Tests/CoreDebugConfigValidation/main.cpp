#include "Core/Assert.h"
#include "Core/Log.h"

#if defined(RVX_EXPECT_DEBUG_CONFIG) && !defined(RVX_DEBUG)
    #error "RVX_DEBUG must be defined for Debug configuration targets"
#endif

int main()
{
    RVX::Log::Initialize();
    RVX_CORE_INFO("Core debug configuration validation passed");
    RVX::Log::Shutdown();
    return 0;
}
