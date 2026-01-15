#pragma once

#include "RHI/RHIDevice.h"

namespace RVX
{
    // Factory function to create Vulkan device
    std::unique_ptr<IRHIDevice> CreateVulkanDevice(const RHIDeviceDesc& desc);

} // namespace RVX
