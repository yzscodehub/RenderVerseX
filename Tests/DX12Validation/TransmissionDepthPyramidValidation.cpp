#include "Common/TransmissionDepthPyramidNative.h"

TEST(DX12Validation, TransmissionDepthPyramidHighStandardZMatchesRecursiveReference)
{
    RVX::Test::RunTransmissionDepthPyramidGraph(
        RVX::RHIBackendType::DX12, RVX::RenderQualityTier::High, false);
}

TEST(DX12Validation, TransmissionDepthPyramidHighReverseZMatchesRecursiveReference)
{
    RVX::Test::RunTransmissionDepthPyramidGraph(
        RVX::RHIBackendType::DX12, RVX::RenderQualityTier::High, true);
}

TEST(DX12Validation, TransmissionDepthPyramidUltraStandardZMatchesRecursiveReference)
{
    RVX::Test::RunTransmissionDepthPyramidGraph(
        RVX::RHIBackendType::DX12, RVX::RenderQualityTier::Ultra, false);
}

TEST(DX12Validation, TransmissionDepthPyramidUltraReverseZMatchesRecursiveReference)
{
    RVX::Test::RunTransmissionDepthPyramidGraph(
        RVX::RHIBackendType::DX12, RVX::RenderQualityTier::Ultra, true);
}
