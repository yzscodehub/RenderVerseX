#pragma once

#include "RHI/RHIDevice.h"

namespace RVX
{
<<<<<<< HEAD
    // Forward declaration
||||||| (empty tree)
=======
    // Factory function to create Vulkan device
>>>>>>> 0e3344f (feat(RHI): 实现完整的 DX12 和 Vulkan 后端)
    std::unique_ptr<IRHIDevice> CreateVulkanDevice(const RHIDeviceDesc& desc);

} // namespace RVX
