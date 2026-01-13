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

            default:
                RVX_RHI_ERROR("Unsupported or disabled backend: {}", ToString(backend));
                return nullptr;
        }
    }

} // namespace RVX
