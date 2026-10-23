#include "Common/TransmissionDepthNative.h"

TEST(VulkanValidation, TransmissionDepthTextureLoadRejectsForegroundAndAcceptsBackground)
{
    RVX::Test::RunTransmissionDepthReference(RVX::RHIBackendType::Vulkan, false);
}

TEST(VulkanValidation, TransmissionDepthTextureLoadUsesReverseZConvention)
{
    RVX::Test::RunTransmissionDepthReference(RVX::RHIBackendType::Vulkan, true);
}
