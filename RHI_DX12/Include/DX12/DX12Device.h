#pragma once

#include "RHI/RHIDevice.h"

namespace RVX
{
    // =============================================================================
    // DX12 Backend Factory
    // =============================================================================

    // Create a DX12 RHI device
    // Returns nullptr on failure
    std::unique_ptr<IRHIDevice> CreateDX12Device(const RHIDeviceDesc& desc);

    // Check if DX12 is available on this system
    bool IsDX12Available();

} // namespace RVX
