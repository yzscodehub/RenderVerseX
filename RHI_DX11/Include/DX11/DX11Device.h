#pragma once

#include "RHI/RHIDevice.h"

namespace RVX
{
    // Forward declaration
    std::unique_ptr<IRHIDevice> CreateDX11Device(const RHIDeviceDesc& desc);

} // namespace RVX
