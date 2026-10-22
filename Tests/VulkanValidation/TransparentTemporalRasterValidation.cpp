#include "Common/TransparentTemporalRasterNative.h"
#include "Common/TransparentTemporalNative.h"

TEST(VulkanValidation, TransparentTemporalRasterProducesMeshMotionCoverageAndLayers)
{
    RVX::Test::RunTransparentTemporalRaster(RVX::RHIBackendType::Vulkan);
}

TEST(VulkanValidation, TransparentTemporalRawAdmissionUsesMaterialHistoryAndLayerProof)
{ RVX::Test::RunTransparentTemporalReference(RVX::RHIBackendType::Vulkan, true, true); }
TEST(VulkanValidation, TransparentTemporalRawAdmissionRejectsGlobalHistoryReset)
{ RVX::Test::RunTransparentTemporalReference(RVX::RHIBackendType::Vulkan, false, true); }
