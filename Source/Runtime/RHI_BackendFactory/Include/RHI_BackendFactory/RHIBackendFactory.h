#pragma once

/** @file RHIBackendFactory.h @brief Enabled RHI backend composition factory */

#include "RHI/RHIDevice.h"

namespace RVX
{
    /** @brief Create a device from the backends enabled in this engine build. */
    std::unique_ptr<IRHIDevice> CreateRHIDevice(
        RHIBackendType backend,
        const RHIDeviceDesc& desc);

} // namespace RVX
