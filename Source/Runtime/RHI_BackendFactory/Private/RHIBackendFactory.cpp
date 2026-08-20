#include "RHI_BackendFactory/RHIBackendFactory.h"

#include "Core/Log.h"

#if RVX_ENABLE_DX11
#include "DX11/DX11Device.h"
#endif

#if RVX_ENABLE_DX12
#include "DX12/DX12Device.h"
#endif

#if RVX_ENABLE_METAL
#include "Metal/MetalDevice.h"
#endif

#if RVX_ENABLE_OPENGL
#include "OpenGL/OpenGLDevice.h"
#endif

#if RVX_ENABLE_VULKAN
#include "Vulkan/VulkanDevice.h"
#endif

namespace RVX
{
    std::unique_ptr<IRHIDevice> CreateRHIDevice(
        RHIBackendType backend,
        const RHIDeviceDesc& desc)
    {
        RVX_RHI_INFO("Creating RHI Device with backend: {}", ToString(backend));

        switch (backend)
        {
#if RVX_ENABLE_DX11
            case RHIBackendType::DX11:
                return CreateDX11Device(desc);
#endif

#if RVX_ENABLE_DX12
            case RHIBackendType::DX12:
                return CreateDX12Device(desc);
#endif

#if RVX_ENABLE_VULKAN
            case RHIBackendType::Vulkan:
                return CreateVulkanDevice(desc);
#endif

#if RVX_ENABLE_METAL
            case RHIBackendType::Metal:
                return CreateMetalDevice(desc);
#endif

#if RVX_ENABLE_OPENGL
            case RHIBackendType::OpenGL:
                return CreateOpenGLDevice(desc);
#endif

            default:
                RVX_RHI_ERROR(
                    "Unsupported or disabled backend: {}",
                    ToString(backend));
                return nullptr;
        }
    }

} // namespace RVX
