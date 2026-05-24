#include "Core/Assert.h"
#include "Core/Log.h"

#include <gtest/gtest.h>

#if defined(RVX_EXPECT_DEBUG_CONFIG) && !defined(RVX_DEBUG)
    #error "RVX_DEBUG must be defined for Debug configuration targets"
#endif

namespace
{
    TEST(CoreDebugConfigValidation, DebugConfigurationDefinesRVXDebug)
    {
        RVX::Log::Initialize();
        SUCCEED();
        RVX::Log::Shutdown();
    }
} // namespace
