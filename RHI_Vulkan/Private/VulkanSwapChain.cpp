#include "VulkanSwapChain.h"
#include "VulkanDevice.h"
#include "VulkanResources.h"

#include <GLFW/glfw3.h>

#include <algorithm>

namespace RVX
{
    VulkanSwapChain::VulkanSwapChain(VulkanDevice* device, const RHISwapChainDesc& desc)
        : m_device(device)
        , m_width(desc.surface.width)
        , m_height(desc.surface.height)
        , m_format(desc.surface.preferredFormat)
        , m_vsync(desc.surface.vsync)
        , m_backendWindow(
              reinterpret_cast<GLFWwindow*>(desc.surface.backendWindow))
    {
        if (!m_device || !desc.surface.IsValidFor(RHIBackendType::Vulkan))
        {
            RVX_RHI_ERROR("VulkanSwapChain: Invalid GLFW-backed native surface");
            return;
        }

        // Create surface
        const VkResult surfaceResult = glfwCreateWindowSurface(
            device->GetInstance(), m_backendWindow, nullptr, &m_surface);
        if (surfaceResult != VK_SUCCESS || m_surface == VK_NULL_HANDLE)
        {
            RVX_RHI_ERROR("glfwCreateWindowSurface failed: {} ({})",
                          VkResultToString(surfaceResult),
                          static_cast<int32>(surfaceResult));
            return;
        }

        // Verify present support
        VkBool32 presentSupport = VK_FALSE;
        const VkResult presentSupportResult = vkGetPhysicalDeviceSurfaceSupportKHR(
            device->GetPhysicalDevice(),
            device->GetGraphicsQueueFamily(),
            m_surface,
            &presentSupport);

        if (presentSupportResult != VK_SUCCESS || !presentSupport)
        {
            RVX_RHI_ERROR("Graphics queue present support query failed: {} ({})",
                          VkResultToString(presentSupportResult),
                          static_cast<int32>(presentSupportResult));
            return;
        }

        m_device->SetPrimarySwapChain(this);
        CreateSwapchain();
        CreateImageViews();

        RVX_RHI_INFO("Vulkan SwapChain created: {}x{}, {} buffers, format: {}", 
                     m_width, m_height, m_backBuffers.size(), static_cast<int>(m_format));
    }

    VulkanSwapChain::~VulkanSwapChain()
    {
        // The owning RenderContext resolves the surface generation first.
        CleanupSwapchain();

        if (m_surface)
            vkDestroySurfaceKHR(m_device->GetInstance(), m_surface, nullptr);

        if (m_device && m_device->GetPrimarySwapChain() == this)
        {
            m_device->SetPrimarySwapChain(nullptr);
        }
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
        m_renderFinishedSemaphores.clear();
        m_imagesInFlight.clear();

        // Get swapchain images
        uint32 imageCount;
        vkGetSwapchainImagesKHR(m_device->GetDevice(), m_swapchain, &imageCount, nullptr);
        std::vector<VkImage> images(imageCount);
        vkGetSwapchainImagesKHR(m_device->GetDevice(), m_swapchain, &imageCount, images.data());

        // Create texture wrappers and views
        m_renderFinishedSemaphores.resize(imageCount, VK_NULL_HANDLE);
        m_imagesInFlight.resize(imageCount, VK_NULL_HANDLE);

        VkSemaphoreCreateInfo semaphoreInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};

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
            viewDesc.type = RHITextureViewType::RenderTarget;
            VulkanTextureView* rawView = new VulkanTextureView(m_device, rawTex, viewDesc);
            m_backBufferViews.push_back(RHITextureViewRef(rawView));

            VK_CHECK(vkCreateSemaphore(m_device->GetDevice(), &semaphoreInfo, nullptr, &m_renderFinishedSemaphores[i]));
        }
    }

    void VulkanSwapChain::CleanupSwapchain()
    {
        for (auto semaphore : m_renderFinishedSemaphores)
        {
            if (semaphore != VK_NULL_HANDLE)
            {
                vkDestroySemaphore(m_device->GetDevice(), semaphore, nullptr);
            }
        }
        m_renderFinishedSemaphores.clear();
        m_imagesInFlight.clear();

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
        // Convert requested format to Vulkan format
        VkFormat requestedVkFormat = ToVkFormat(m_format);
        
        // First, try to find exact match for the requested format
        for (const auto& format : formats)
        {
            if (format.format == requestedVkFormat)
            {
                return format;
            }
        }

        // If exact match not found, prefer BGRA8 UNORM (most common for rendering)
        for (const auto& format : formats)
        {
            if (format.format == VK_FORMAT_B8G8R8A8_UNORM)
            {
                return format;
            }
        }

        // Fallback to BGRA8 SRGB
        for (const auto& format : formats)
        {
            if (format.format == VK_FORMAT_B8G8R8A8_SRGB && 
                format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
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
        VkResult result = vkAcquireNextImageKHR(m_device->GetDevice(), m_swapchain, UINT64_MAX,
            m_device->GetImageAvailableSemaphore(), VK_NULL_HANDLE, &m_currentImageIndex);

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

        if (m_currentImageIndex < m_imagesInFlight.size())
        {
            VkFence& inFlightFence = m_imagesInFlight[m_currentImageIndex];
            VkFence currentFrameFence = m_device->GetCurrentFrameFence();
            if (inFlightFence != VK_NULL_HANDLE && inFlightFence != currentFrameFence)
            {
                VK_CHECK(vkWaitForFences(m_device->GetDevice(), 1, &inFlightFence, VK_TRUE, UINT64_MAX));
            }
            inFlightFence = currentFrameFence;
        }

        m_hasAcquiredImage = true;
        return true;
    }

    void VulkanSwapChain::Present()
    {
        if (!m_hasAcquiredImage)
        {
            if (!AcquireNextImage())
                return;
        }

        VkPresentInfoKHR presentInfo = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        presentInfo.waitSemaphoreCount = 1;
        VkSemaphore waitSemaphore = GetCurrentRenderFinishedSemaphore();
        presentInfo.pWaitSemaphores = &waitSemaphore;
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

        m_hasAcquiredImage = false;
    }

    uint32 VulkanSwapChain::GetCurrentBackBufferIndex() const
    {
        if (!m_hasAcquiredImage)
        {
            const_cast<VulkanSwapChain*>(this)->AcquireNextImage();
        }
        return m_currentImageIndex;
    }

    VkSemaphore VulkanSwapChain::GetCurrentRenderFinishedSemaphore() const
    {
        if (!m_hasAcquiredImage || m_currentImageIndex >= m_renderFinishedSemaphores.size())
        {
            return VK_NULL_HANDLE;
        }
        return m_renderFinishedSemaphores[m_currentImageIndex];
    }

    void VulkanSwapChain::Resize(uint32 width, uint32 height)
    {
        // Skip resize when window is minimized (zero size)
        // This prevents creating invalid swapchains
        if (width == 0 || height == 0)
        {
            RVX_RHI_DEBUG("Vulkan SwapChain resize skipped (window minimized): {}x{}", width, height);
            return;
        }

        m_width = width;
        m_height = height;

        // RenderContext has already resolved the old surface-generation token.
        CleanupSwapchain();
        CreateSwapchain();
        CreateImageViews();

        RVX_RHI_INFO("Vulkan SwapChain resized: {}x{}", m_width, m_height);
        m_hasAcquiredImage = false;
    }

    RHITexture* VulkanSwapChain::GetCurrentBackBuffer()
    {
        if (!m_hasAcquiredImage)
        {
            if (!AcquireNextImage())
            {
                // Swapchain not ready (e.g., window minimized)
                return m_backBuffers.empty() ? nullptr : m_backBuffers[0].Get();
            }
        }
        if (m_currentImageIndex >= m_backBuffers.size())
        {
            return nullptr;
        }
        return m_backBuffers[m_currentImageIndex].Get();
    }

    RHITextureView* VulkanSwapChain::GetCurrentBackBufferView()
    {
        if (!m_hasAcquiredImage)
        {
            if (!AcquireNextImage())
            {
                // Swapchain not ready (e.g., window minimized)
                return m_backBufferViews.empty() ? nullptr : m_backBufferViews[0].Get();
            }
        }
        if (m_currentImageIndex >= m_backBufferViews.size())
        {
            return nullptr;
        }
        return m_backBufferViews[m_currentImageIndex].Get();
    }

    // Factory
    RHISwapChainRef CreateVulkanSwapChain(VulkanDevice* device, const RHISwapChainDesc& desc)
    {
        Ref<VulkanSwapChain> swapChain(new VulkanSwapChain(device, desc));
        if (!swapChain->IsValid())
        {
            return nullptr;
        }
        return swapChain;
    }

} // namespace RVX
