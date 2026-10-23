#include "Common/TransmissionFootprintNative.h"

TEST(DX12Validation, TransmissionFootprintGuardBandCoversFilteredSupport)
{
    RVX::Test::RunTransmissionFootprintReference(RVX::RHIBackendType::DX12, false, true);
}

TEST(DX12Validation, TransmissionFootprintGuardBandUsesReverseZCanonicalDepth)
{
    RVX::Test::RunTransmissionFootprintReference(RVX::RHIBackendType::DX12, true, true);
}

TEST(DX12Validation, TransmissionFootprintCapturesPositiveFilteredContributor)
{
    RVX::Test::RunTransmissionFootprintReference(RVX::RHIBackendType::DX12, false, false);
}

TEST(DX12Validation, TransmissionFootprintCapturesPositiveReverseZContributor)
{
    RVX::Test::RunTransmissionFootprintReference(RVX::RHIBackendType::DX12, true, false);
}
