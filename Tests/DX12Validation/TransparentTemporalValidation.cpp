#include "Common/TransparentTemporalNative.h"

TEST(DX12Validation, TransparentTemporalResolveMatchesReferenceAndPreservesOpaqueMotion)
{
    RVX::Test::RunTransparentTemporalReference(RVX::RHIBackendType::DX12);
}

TEST(DX12Validation, TransparentTemporalResetRejectsQualifiedSurfaceHistory)
{
    RVX::Test::RunTransparentTemporalReference(RVX::RHIBackendType::DX12, false);
}
