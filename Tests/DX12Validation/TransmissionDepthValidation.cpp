#include "Common/TransmissionDepthNative.h"

TEST(DX12Validation, TransmissionDepthTextureLoadRejectsForegroundAndAcceptsBackground)
{
    RVX::Test::RunTransmissionDepthReference(RVX::RHIBackendType::DX12, false);
}

TEST(DX12Validation, TransmissionDepthTextureLoadUsesReverseZConvention)
{
    RVX::Test::RunTransmissionDepthReference(RVX::RHIBackendType::DX12, true);
}
