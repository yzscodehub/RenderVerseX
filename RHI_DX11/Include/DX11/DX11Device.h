#pragma once

#include "RHI/RHIDevice.h"

namespace RVX
{
    // Factory function - implemented in RHI_DX11
    std::unique_ptr<IRHIDevice> CreateDX11Device(const RHIDeviceDesc& desc);

    // Check if DX11 is available on this system
    bool IsDX11Available();

} // namespace RVX
