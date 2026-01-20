#pragma once

#include "RHI/RHIDevice.h"

namespace RVX
{
    // Factory function - creates an OpenGL device
    std::unique_ptr<IRHIDevice> CreateOpenGLDevice(const RHIDeviceDesc& desc);

} // namespace RVX
