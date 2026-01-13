#pragma once

#include "RHI/RHIDevice.h"

namespace RVX
{
<<<<<<< HEAD
    // Forward declaration
    std::unique_ptr<IRHIDevice> CreateDX12Device(const RHIDeviceDesc& desc);
||||||| (empty tree)
=======
    // =============================================================================
    // DX12 Backend Factory
    // =============================================================================

    // Create a DX12 RHI device
    // Returns nullptr on failure
    std::unique_ptr<IRHIDevice> CreateDX12Device(const RHIDeviceDesc& desc);

    // Check if DX12 is available on this system
    bool IsDX12Available();
>>>>>>> 0e3344f (feat(RHI): 实现完整的 DX12 和 Vulkan 后端)

} // namespace RVX
