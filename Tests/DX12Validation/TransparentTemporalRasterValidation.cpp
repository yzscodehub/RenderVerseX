#include "Common/TransparentTemporalRasterNative.h"
#include "Common/TransparentTemporalNative.h"

TEST(DX12Validation, TransparentTemporalRasterProducesMeshMotionCoverageAndLayers)
{
    RVX::Test::RunTransparentTemporalRaster(RVX::RHIBackendType::DX12);
}

TEST(DX12Validation, TransparentTemporalRawAdmissionUsesMaterialHistoryAndLayerProof)
{ RVX::Test::RunTransparentTemporalReference(RVX::RHIBackendType::DX12, true, true); }
TEST(DX12Validation, TransparentTemporalRawAdmissionRejectsGlobalHistoryReset)
{ RVX::Test::RunTransparentTemporalReference(RVX::RHIBackendType::DX12, false, true); }
