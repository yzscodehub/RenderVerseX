#include "Common/TransmissionDepthPyramidNative.h"

TEST(VulkanValidation, TransmissionDepthPyramidHighStandardZMatchesRecursiveReference)
{
    RVX::Test::RunTransmissionDepthPyramidGraph(
        RVX::RHIBackendType::Vulkan, RVX::RenderQualityTier::High, false);
}

TEST(VulkanValidation, TransmissionDepthPyramidHighReverseZMatchesRecursiveReference)
{
    RVX::Test::RunTransmissionDepthPyramidGraph(
        RVX::RHIBackendType::Vulkan, RVX::RenderQualityTier::High, true);
}

TEST(VulkanValidation, TransmissionDepthPyramidUltraStandardZMatchesRecursiveReference)
{
    RVX::Test::RunTransmissionDepthPyramidGraph(
        RVX::RHIBackendType::Vulkan, RVX::RenderQualityTier::Ultra, false);
}

TEST(VulkanValidation, TransmissionDepthPyramidUltraReverseZMatchesRecursiveReference)
{
    RVX::Test::RunTransmissionDepthPyramidGraph(
        RVX::RHIBackendType::Vulkan, RVX::RenderQualityTier::Ultra, true);
}
