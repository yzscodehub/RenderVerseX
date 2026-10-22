#include "Common/TransparentTemporalNative.h"
#include "Common/TAAStableGridNative.h"

TEST(VulkanValidation, TAAReconstructsStableGridWithResetReactiveAndMovingHistory)
{
    RVX::Test::RunTAAStableGridReference(RVX::RHIBackendType::Vulkan);
}

TEST(VulkanValidation, TransparentTemporalResolveMatchesReferenceAndPreservesOpaqueMotion)
{
    RVX::Test::RunTransparentTemporalReference(RVX::RHIBackendType::Vulkan);
}

TEST(VulkanValidation, TransparentTemporalResetRejectsQualifiedSurfaceHistory)
{
    RVX::Test::RunTransparentTemporalReference(RVX::RHIBackendType::Vulkan, false);
}
