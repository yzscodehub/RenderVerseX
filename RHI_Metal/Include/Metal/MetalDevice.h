#pragma once

// Public header for Metal backend
// Include this to use Metal-specific functionality

#include "RHI/RHIDevice.h"

namespace RVX
{
    // Factory function declaration
    std::unique_ptr<IRHIDevice> CreateMetalDevice(const RHIDeviceDesc& desc);

} // namespace RVX
