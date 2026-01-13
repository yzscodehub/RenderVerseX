#pragma once

#include "RHI/RHIDevice.h"

namespace RVX
{
    // Forward declaration
    std::unique_ptr<IRHIDevice> CreateVulkanDevice(const RHIDeviceDesc& desc);

} // namespace RVX
