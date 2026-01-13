#pragma once

#include "VulkanCommon.h"
#include "RHI/RHISwapChain.h"

namespace RVX
{
    class VulkanDevice;
    class VulkanTexture;
    class VulkanTextureView;

    // =============================================================================
    // Vulkan SwapChain
    // =============================================================================
    class VulkanSwapChain : public RHISwapChain
    {
    public:
        VulkanSwapChain(VulkanDevice* device, const RHISwapChainDesc& desc);
        ~VulkanSwapChain() override;

        void Present() override;
        void Resize(uint32 width, uint32 height) override;

        uint32 GetWidth() const override { return m_width; }
        uint32 GetHeight() const override { return m_height; }
        RHIFormat GetFormat() const override { return m_format; }
        uint32 GetBufferCount() const override { return static_cast<uint32>(m_backBuffers.size()); }
        uint32 GetCurrentBackBufferIndex() const override { return m_currentImageIndex; }

        RHITexture* GetCurrentBackBuffer() override;
        RHITextureView* GetCurrentBackBufferView() override;

        VkSwapchainKHR GetSwapchain() const { return m_swapchain; }
        VkSemaphore GetImageAvailableSemaphore() const { return m_imageAvailableSemaphore; }
        VkSemaphore GetRenderFinishedSemaphore() const { return m_renderFinishedSemaphore; }

        bool AcquireNextImage();

    private:
        void CreateSwapchain();
        void CreateImageViews();
        void CleanupSwapchain();

        VkSurfaceFormatKHR ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats);
        VkPresentModeKHR ChoosePresentMode(const std::vector<VkPresentModeKHR>& modes);
        VkExtent2D ChooseExtent(const VkSurfaceCapabilitiesKHR& capabilities);

        VulkanDevice* m_device;
        VkSurfaceKHR m_surface = VK_NULL_HANDLE;
        VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;

        uint32 m_width = 0;
        uint32 m_height = 0;
        RHIFormat m_format = RHIFormat::BGRA8_UNORM;
        bool m_vsync = false;

        std::vector<RHITextureRef> m_backBuffers;
        std::vector<RHITextureViewRef> m_backBufferViews;
        uint32 m_currentImageIndex = 0;

        VkSemaphore m_imageAvailableSemaphore = VK_NULL_HANDLE;
        VkSemaphore m_renderFinishedSemaphore = VK_NULL_HANDLE;
        VkFence m_inFlightFence = VK_NULL_HANDLE;

        void* m_windowHandle = nullptr;
    };

    // Factory
    RHISwapChainRef CreateVulkanSwapChain(VulkanDevice* device, const RHISwapChainDesc& desc);

} // namespace RVX
