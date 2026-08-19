#include "Core/Assert.h"
#include "Core/Diagnostics/ArtifactMetadata.h"
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

    TEST(CoreDebugConfigValidation, ArtifactMetadataReportsIdentityAndSchemaAvailability)
    {
        RVX::Diagnostics::ArtifactMetadata metadata;
        EXPECT_FALSE(metadata.HasIdentity());
        EXPECT_FALSE(metadata.HasSchema());

        metadata.id = "frameReport";
        metadata.kind = "FrameReportJson";
        metadata.contentType = "application/json";
        EXPECT_TRUE(metadata.HasIdentity());
        EXPECT_FALSE(metadata.HasSchema());

        metadata.schemaId = "RVX.Test.FrameReport";
        metadata.schemaVersion = 1;
        EXPECT_TRUE(metadata.HasSchema());
    }
} // namespace
