#pragma once

#include "RHI/RHIDevice.h"

namespace RVX
{
    // Forward declaration
    std::unique_ptr<IRHIDevice> CreateDX12Device(const RHIDeviceDesc& desc);

} // namespace RVX
