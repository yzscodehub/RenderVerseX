#include "Common/TransparentTemporalNative.h"
#include "Common/TAAStableGridNative.h"

TEST(DX12Validation, TAAReconstructsStableGridWithResetReactiveAndMovingHistory)
{
    RVX::Test::RunTAAStableGridReference(RVX::RHIBackendType::DX12);
}

TEST(DX12Validation, TransparentTemporalResolveMatchesReferenceAndPreservesOpaqueMotion)
{
    RVX::Test::RunTransparentTemporalReference(RVX::RHIBackendType::DX12);
}

TEST(DX12Validation, TransparentTemporalResetRejectsQualifiedSurfaceHistory)
{
    RVX::Test::RunTransparentTemporalReference(RVX::RHIBackendType::DX12, false);
}
