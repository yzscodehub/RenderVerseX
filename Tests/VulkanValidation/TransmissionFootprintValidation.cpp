#include "Common/TransmissionFootprintNative.h"

TEST(VulkanValidation, TransmissionFootprintGuardBandCoversFilteredSupport)
{
    RVX::Test::RunTransmissionFootprintReference(RVX::RHIBackendType::Vulkan, false, true);
}

TEST(VulkanValidation, TransmissionFootprintGuardBandUsesReverseZCanonicalDepth)
{
    RVX::Test::RunTransmissionFootprintReference(RVX::RHIBackendType::Vulkan, true, true);
}

TEST(VulkanValidation, TransmissionFootprintCapturesPositiveFilteredContributor)
{
    RVX::Test::RunTransmissionFootprintReference(RVX::RHIBackendType::Vulkan, false, false);
}

TEST(VulkanValidation, TransmissionFootprintCapturesPositiveReverseZContributor)
{
    RVX::Test::RunTransmissionFootprintReference(RVX::RHIBackendType::Vulkan, true, false);
}
