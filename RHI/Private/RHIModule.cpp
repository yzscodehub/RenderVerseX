#include "RHI/RHIDevice.h"
#include "Core/Log.h"

#if RVX_ENABLE_DX12
// Forward declaration - implemented in RHI_DX12
namespace RVX { std::unique_ptr<IRHIDevice> CreateDX12Device(const RHIDeviceDesc& desc); }
#endif

#if RVX_ENABLE_VULKAN
// Forward declaration - implemented in RHI_Vulkan
namespace RVX { std::unique_ptr<IRHIDevice> CreateVulkanDevice(const RHIDeviceDesc& desc); }
#endif

#if RVX_ENABLE_DX11
// Forward declaration - implemented in RHI_DX11
namespace RVX { std::unique_ptr<IRHIDevice> CreateDX11Device(const RHIDeviceDesc& desc); }
#endif

#if RVX_ENABLE_METAL
// Forward declaration - implemented in RHI_Metal
namespace RVX { std::unique_ptr<IRHIDevice> CreateMetalDevice(const RHIDeviceDesc& desc); }
#endif

#if RVX_ENABLE_OPENGL
// Forward declaration - implemented in RHI_OpenGL
namespace RVX { std::unique_ptr<IRHIDevice> CreateOpenGLDevice(const RHIDeviceDesc& desc); }
#endif

namespace RVX
{
    std::unique_ptr<IRHIDevice> CreateRHIDevice(RHIBackendType backend, const RHIDeviceDesc& desc)
    {
        RVX_RHI_INFO("Creating RHI Device with backend: {}", ToString(backend));

        switch (backend)
        {
#if RVX_ENABLE_DX12
            case RHIBackendType::DX12:
                return CreateDX12Device(desc);
#endif

#if RVX_ENABLE_VULKAN
            case RHIBackendType::Vulkan:
                return CreateVulkanDevice(desc);
#endif

#if RVX_ENABLE_DX11
            case RHIBackendType::DX11:
                return CreateDX11Device(desc);
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
                RVX_RHI_ERROR("Unsupported or disabled backend: {}", ToString(backend));
                return nullptr;
        }
    }

} // namespace RVX
