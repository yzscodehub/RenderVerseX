#include "Common/TransparentTemporalNative.h"

TEST(VulkanValidation, TransparentTemporalResolveMatchesReferenceAndPreservesOpaqueMotion)
{
    RVX::Test::RunTransparentTemporalReference(RVX::RHIBackendType::Vulkan);
}

TEST(VulkanValidation, TransparentTemporalResetRejectsQualifiedSurfaceHistory)
{
    RVX::Test::RunTransparentTemporalReference(RVX::RHIBackendType::Vulkan, false);
}
