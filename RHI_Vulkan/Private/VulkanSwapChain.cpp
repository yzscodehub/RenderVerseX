#include "VulkanSwapChain.h"
#include "VulkanDevice.h"
#include "VulkanResources.h"

#include <algorithm>

namespace RVX
{
    VulkanSwapChain::VulkanSwapChain(VulkanDevice* device, const RHISwapChainDesc& desc)
        : m_device(device)
        , m_width(desc.width)
        , m_height(desc.height)
        , m_format(desc.format)
        , m_vsync(desc.vsync)
        , m_windowHandle(desc.windowHandle)
    {
        // Create surface
#ifdef _WIN32
        VkWin32SurfaceCreateInfoKHR surfaceInfo = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        surfaceInfo.hwnd = static_cast<HWND>(desc.windowHandle);
        surfaceInfo.hinstance = GetModuleHandle(nullptr);
        VK_CHECK(vkCreateWin32SurfaceKHR(device->GetInstance(), &surfaceInfo, nullptr, &m_surface));
#endif

        // Verify present support
        VkBool32 presentSupport = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device->GetPhysicalDevice(), 
            device->GetGraphicsQueueFamily(), m_surface, &presentSupport);
        
        if (!presentSupport)
        {
            RVX_RHI_ERROR("Graphics queue does not support present");
            return;
        }

        // Create semaphores
        VkSemaphoreCreateInfo semaphoreInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(device->GetDevice(), &semaphoreInfo, nullptr, &m_imageAvailableSemaphore));
        VK_CHECK(vkCreateSemaphore(device->GetDevice(), &semaphoreInfo, nullptr, &m_renderFinishedSemaphore));

        VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(device->GetDevice(), &fenceInfo, nullptr, &m_inFlightFence));

        CreateSwapchain();
        CreateImageViews();

        RVX_RHI_INFO("Vulkan SwapChain created: {}x{}, {} buffers", m_width, m_height, m_backBuffers.size());
    }

    VulkanSwapChain::~VulkanSwapChain()
    {
        m_device->WaitIdle();

        CleanupSwapchain();

        if (m_imageAvailableSemaphore)
            vkDestroySemaphore(m_device->GetDevice(), m_imageAvailableSemaphore, nullptr);
        if (m_renderFinishedSemaphore)
            vkDestroySemaphore(m_device->GetDevice(), m_renderFinishedSemaphore, nullptr);
        if (m_inFlightFence)
            vkDestroyFence(m_device->GetDevice(), m_inFlightFence, nullptr);

        if (m_surface)
            vkDestroySurfaceKHR(m_device->GetInstance(), m_surface, nullptr);
    }

    void VulkanSwapChain::CreateSwapchain()
    {
        VkPhysicalDevice physicalDevice = m_device->GetPhysicalDevice();

        // Query swapchain support
        VkSurfaceCapabilitiesKHR capabilities;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, m_surface, &capabilities);

        uint32 formatCount;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, m_surface, &formatCount, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, m_surface, &formatCount, formats.data());

        uint32 presentModeCount;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, m_surface, &presentModeCount, nullptr);
        std::vector<VkPresentModeKHR> presentModes(presentModeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, m_surface, &presentModeCount, presentModes.data());

        VkSurfaceFormatKHR surfaceFormat = ChooseSurfaceFormat(formats);
        VkPresentModeKHR presentMode = ChoosePresentMode(presentModes);
        VkExtent2D extent = ChooseExtent(capabilities);

        m_width = extent.width;
        m_height = extent.height;
        m_format = FromVkFormat(surfaceFormat.format);

        uint32 imageCount = capabilities.minImageCount + 1;
        if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount)
        {
            imageCount = capabilities.maxImageCount;
        }

        VkSwapchainCreateInfoKHR createInfo = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        createInfo.surface = m_surface;
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = surfaceFormat.format;
        createInfo.imageColorSpace = surfaceFormat.colorSpace;
        createInfo.imageExtent = extent;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        createInfo.preTransform = capabilities.currentTransform;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        createInfo.presentMode = presentMode;
        createInfo.clipped = VK_TRUE;
        createInfo.oldSwapchain = m_swapchain;

        VK_CHECK(vkCreateSwapchainKHR(m_device->GetDevice(), &createInfo, nullptr, &m_swapchain));

        // If we had an old swapchain, destroy it
        if (createInfo.oldSwapchain != VK_NULL_HANDLE)
        {
            vkDestroySwapchainKHR(m_device->GetDevice(), createInfo.oldSwapchain, nullptr);
        }
    }

    void VulkanSwapChain::CreateImageViews()
    {
        m_backBuffers.clear();
        m_backBufferViews.clear();

        // Get swapchain images
        uint32 imageCount;
        vkGetSwapchainImagesKHR(m_device->GetDevice(), m_swapchain, &imageCount, nullptr);
        std::vector<VkImage> images(imageCount);
        vkGetSwapchainImagesKHR(m_device->GetDevice(), m_swapchain, &imageCount, images.data());

        // Create texture wrappers and views
        for (uint32 i = 0; i < imageCount; ++i)
        {
            RHITextureDesc texDesc;
            texDesc.width = m_width;
            texDesc.height = m_height;
            texDesc.depth = 1;
            texDesc.mipLevels = 1;
            texDesc.arraySize = 1;
            texDesc.format = m_format;
            texDesc.dimension = RHITextureDimension::Texture2D;
            texDesc.usage = RHITextureUsage::RenderTarget;
            texDesc.debugName = "SwapChain BackBuffer";

            VulkanTexture* rawTex = new VulkanTexture(m_device, images[i], texDesc);
            m_backBuffers.push_back(RHITextureRef(rawTex));

            RHITextureViewDesc viewDesc;
            viewDesc.format = m_format;
            VulkanTextureView* rawView = new VulkanTextureView(m_device, rawTex, viewDesc);
            m_backBufferViews.push_back(RHITextureViewRef(rawView));
        }
    }

    void VulkanSwapChain::CleanupSwapchain()
    {
        m_backBufferViews.clear();
        m_backBuffers.clear();

        if (m_swapchain)
        {
            vkDestroySwapchainKHR(m_device->GetDevice(), m_swapchain, nullptr);
            m_swapchain = VK_NULL_HANDLE;
        }
    }

    VkSurfaceFormatKHR VulkanSwapChain::ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats)
    {
        // Prefer BGRA8 SRGB
        for (const auto& format : formats)
        {
            if (format.format == VK_FORMAT_B8G8R8A8_SRGB && 
                format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            {
                return format;
            }
        }

        // Prefer BGRA8 UNORM
        for (const auto& format : formats)
        {
            if (format.format == VK_FORMAT_B8G8R8A8_UNORM)
            {
                return format;
            }
        }

        return formats[0];
    }

    VkPresentModeKHR VulkanSwapChain::ChoosePresentMode(const std::vector<VkPresentModeKHR>& modes)
    {
        if (m_vsync)
        {
            return VK_PRESENT_MODE_FIFO_KHR;  // Guaranteed available, vsync
        }

        // Prefer mailbox (triple buffering) for no vsync
        for (const auto& mode : modes)
        {
            if (mode == VK_PRESENT_MODE_MAILBOX_KHR)
            {
                return mode;
            }
        }

        // Immediate mode for no vsync
        for (const auto& mode : modes)
        {
            if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR)
            {
                return mode;
            }
        }

        return VK_PRESENT_MODE_FIFO_KHR;
    }

    VkExtent2D VulkanSwapChain::ChooseExtent(const VkSurfaceCapabilitiesKHR& capabilities)
    {
        if (capabilities.currentExtent.width != UINT32_MAX)
        {
            return capabilities.currentExtent;
        }

        VkExtent2D actualExtent = {m_width, m_height};
        actualExtent.width = std::clamp(actualExtent.width, 
            capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        actualExtent.height = std::clamp(actualExtent.height,
            capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
        return actualExtent;
    }

    bool VulkanSwapChain::AcquireNextImage()
    {
        VK_CHECK(vkWaitForFences(m_device->GetDevice(), 1, &m_inFlightFence, VK_TRUE, UINT64_MAX));

        VkResult result = vkAcquireNextImageKHR(m_device->GetDevice(), m_swapchain, UINT64_MAX,
            m_imageAvailableSemaphore, VK_NULL_HANDLE, &m_currentImageIndex);

        if (result == VK_ERROR_OUT_OF_DATE_KHR)
        {
            Resize(m_width, m_height);
            return false;
        }
        else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        {
            RVX_RHI_ERROR("Failed to acquire swapchain image: {}", VkResultToString(result));
            return false;
        }

        VK_CHECK(vkResetFences(m_device->GetDevice(), 1, &m_inFlightFence));
        return true;
    }

    void VulkanSwapChain::Present()
    {
        VkPresentInfoKHR presentInfo = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &m_renderFinishedSemaphore;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &m_swapchain;
        presentInfo.pImageIndices = &m_currentImageIndex;

        VkResult result = vkQueuePresentKHR(m_device->GetGraphicsQueue(), &presentInfo);

        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        {
            Resize(m_width, m_height);
        }
        else if (result != VK_SUCCESS)
        {
            RVX_RHI_ERROR("Failed to present: {}", VkResultToString(result));
        }
    }

    void VulkanSwapChain::Resize(uint32 width, uint32 height)
    {
        m_width = width;
        m_height = height;

        m_device->WaitIdle();
        CleanupSwapchain();
        CreateSwapchain();
        CreateImageViews();

        RVX_RHI_INFO("Vulkan SwapChain resized: {}x{}", m_width, m_height);
    }

    RHITexture* VulkanSwapChain::GetCurrentBackBuffer()
    {
        return m_backBuffers[m_currentImageIndex].Get();
    }

    RHITextureView* VulkanSwapChain::GetCurrentBackBufferView()
    {
        return m_backBufferViews[m_currentImageIndex].Get();
    }

    // Factory
    RHISwapChainRef CreateVulkanSwapChain(VulkanDevice* device, const RHISwapChainDesc& desc)
    {
        return Ref<VulkanSwapChain>(new VulkanSwapChain(device, desc));
    }

} // namespace RVX
