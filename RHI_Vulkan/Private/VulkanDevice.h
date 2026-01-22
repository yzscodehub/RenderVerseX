#pragma once

#include "VulkanCommon.h"
#include "RHI/RHIDevice.h"
#include <mutex>

namespace RVX
{
    class VulkanSwapChain;

    // =============================================================================
    // Vulkan Device Implementation
    // =============================================================================
    class VulkanDevice : public IRHIDevice
    {
    public:
        VulkanDevice(const RHIDeviceDesc& desc);
        ~VulkanDevice() override;

        // =========================================================================
        // IRHIDevice Interface
        // =========================================================================
        RHIBufferRef CreateBuffer(const RHIBufferDesc& desc) override;
        RHITextureRef CreateTexture(const RHITextureDesc& desc) override;
        RHITextureViewRef CreateTextureView(RHITexture* texture, const RHITextureViewDesc& desc = {}) override;
        RHISamplerRef CreateSampler(const RHISamplerDesc& desc) override;
        RHIShaderRef CreateShader(const RHIShaderDesc& desc) override;

        // Memory Heap Management (for Placed Resources / Memory Aliasing)
        RHIHeapRef CreateHeap(const RHIHeapDesc& desc) override;
        RHITextureRef CreatePlacedTexture(RHIHeap* heap, uint64 offset, const RHITextureDesc& desc) override;
        RHIBufferRef CreatePlacedBuffer(RHIHeap* heap, uint64 offset, const RHIBufferDesc& desc) override;
        MemoryRequirements GetTextureMemoryRequirements(const RHITextureDesc& desc) override;
        MemoryRequirements GetBufferMemoryRequirements(const RHIBufferDesc& desc) override;

        RHIDescriptorSetLayoutRef CreateDescriptorSetLayout(const RHIDescriptorSetLayoutDesc& desc) override;
        RHIPipelineLayoutRef CreatePipelineLayout(const RHIPipelineLayoutDesc& desc) override;
        RHIPipelineRef CreateGraphicsPipeline(const RHIGraphicsPipelineDesc& desc) override;
        RHIPipelineRef CreateComputePipeline(const RHIComputePipelineDesc& desc) override;

        RHIDescriptorSetRef CreateDescriptorSet(const RHIDescriptorSetDesc& desc) override;

        RHIQueryPoolRef CreateQueryPool(const RHIQueryPoolDesc& desc) override;

        RHICommandContextRef CreateCommandContext(RHICommandQueueType type) override;
        void SubmitCommandContext(RHICommandContext* context, RHIFence* signalFence) override;
        void SubmitCommandContexts(std::span<RHICommandContext* const> contexts, RHIFence* signalFence) override;

        RHISwapChainRef CreateSwapChain(const RHISwapChainDesc& desc) override;

        RHIFenceRef CreateFence(uint64 initialValue) override;
        void WaitForFence(RHIFence* fence, uint64 value) override;
        void WaitIdle() override;

        void BeginFrame() override;
        void EndFrame() override;
        uint32 GetCurrentFrameIndex() const override { return m_currentFrameIndex; }

        const RHICapabilities& GetCapabilities() const override { return m_capabilities; }
        RHIBackendType GetBackendType() const override { return RHIBackendType::Vulkan; }

        // Upload Resources
        RHIStagingBufferRef CreateStagingBuffer(const RHIStagingBufferDesc& desc) override;
        RHIRingBufferRef CreateRingBuffer(const RHIRingBufferDesc& desc) override;

        // Memory Statistics
        RHIMemoryStats GetMemoryStats() const override;

        // Debug Resource Groups
        void BeginResourceGroup(const char* name) override;
        void EndResourceGroup() override;

        // =========================================================================
        // Vulkan Specific Accessors
        // =========================================================================
        VkInstance GetInstance() const { return m_instance; }
        VkPhysicalDevice GetPhysicalDevice() const { return m_physicalDevice; }
        VkDevice GetDevice() const { return m_device; }
        VmaAllocator GetAllocator() const { return m_allocator; }

        VkQueue GetGraphicsQueue() const { return m_graphicsQueue; }
        VkQueue GetComputeQueue() const { return m_computeQueue; }
        VkQueue GetTransferQueue() const { return m_transferQueue; }
        
        uint32 GetGraphicsQueueFamily() const { return m_queueFamilies.graphicsFamily.value(); }
        uint32 GetComputeQueueFamily() const { return m_queueFamilies.computeFamily.value_or(m_queueFamilies.graphicsFamily.value()); }
        uint32 GetTransferQueueFamily() const { return m_queueFamilies.transferFamily.value_or(m_queueFamilies.graphicsFamily.value()); }

        VkCommandPool GetCommandPool(RHICommandQueueType type);
        VkDescriptorPool GetDescriptorPool() const { return m_descriptorPool; }
        VkPipelineCache GetPipelineCache() const { return m_pipelineCache; }

        VkSemaphore GetImageAvailableSemaphore() const { return m_imageAvailableSemaphores[m_currentFrameIndex]; }
        VkSemaphore GetRenderFinishedSemaphore() const;
        VkFence GetCurrentFrameFence() const { return m_frameFences[m_currentFrameIndex]; }
        std::mutex& GetSubmitMutex() { return m_submitMutex; }

        void SetPrimarySwapChain(VulkanSwapChain* swapChain) { m_primarySwapChain = swapChain; }
        VulkanSwapChain* GetPrimarySwapChain() const { return m_primarySwapChain; }

        // Debug Utils function pointers
        PFN_vkCmdBeginDebugUtilsLabelEXT vkCmdBeginDebugUtilsLabel = nullptr;
        PFN_vkCmdEndDebugUtilsLabelEXT vkCmdEndDebugUtilsLabel = nullptr;
        PFN_vkCmdInsertDebugUtilsLabelEXT vkCmdInsertDebugUtilsLabel = nullptr;
        PFN_vkSetDebugUtilsObjectNameEXT vkSetDebugUtilsObjectName = nullptr;

        bool HasDebugUtils() const { return m_validationEnabled && vkCmdBeginDebugUtilsLabel != nullptr; }

        // Helper to set debug object names for RenderDoc/validation layers
        void SetObjectName(VkObjectType objectType, uint64 objectHandle, const char* name)
        {
            if (!name || !vkSetDebugUtilsObjectName)
                return;

            VkDebugUtilsObjectNameInfoEXT nameInfo = {VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
            nameInfo.objectType = objectType;
            nameInfo.objectHandle = objectHandle;
            nameInfo.pObjectName = name;
            vkSetDebugUtilsObjectName(m_device, &nameInfo);
        }

    private:
        void LoadDebugUtilsFunctions();
        bool CreateInstance(bool enableValidation);
        bool SelectPhysicalDevice();
        bool CreateLogicalDevice();
        bool CreateAllocator();
        bool CreateCommandPools();
        bool CreateDescriptorPool();
        bool CreatePipelineCache();
        void SavePipelineCache();

        QueueFamilyIndices FindQueueFamilies(VkPhysicalDevice device);
        bool CheckDeviceExtensionSupport(VkPhysicalDevice device);
        void QueryDeviceCapabilities();

        // Vulkan objects
        VkInstance m_instance = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
        VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
        VkDevice m_device = VK_NULL_HANDLE;
        VmaAllocator m_allocator = VK_NULL_HANDLE;

        // Queues
        VkQueue m_graphicsQueue = VK_NULL_HANDLE;
        VkQueue m_computeQueue = VK_NULL_HANDLE;
        VkQueue m_transferQueue = VK_NULL_HANDLE;
        QueueFamilyIndices m_queueFamilies;

        // Command pools (per queue type)
        VkCommandPool m_graphicsCommandPool = VK_NULL_HANDLE;
        VkCommandPool m_computeCommandPool = VK_NULL_HANDLE;
        VkCommandPool m_transferCommandPool = VK_NULL_HANDLE;

        // Descriptor pool
        VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;

        // Pipeline cache for faster pipeline creation
        VkPipelineCache m_pipelineCache = VK_NULL_HANDLE;

        // Per-frame synchronization
        std::array<VkFence, RVX_MAX_FRAME_COUNT> m_frameFences = {};
        std::array<VkSemaphore, RVX_MAX_FRAME_COUNT> m_imageAvailableSemaphores = {};
        std::array<VkSemaphore, RVX_MAX_FRAME_COUNT> m_renderFinishedSemaphores = {};
        uint32 m_currentFrameIndex = 0;

        // State
        RHICapabilities m_capabilities;
        bool m_validationEnabled = false;

        // Thread safety
        std::mutex m_submitMutex;

        // Optional primary swapchain for per-image sync
        VulkanSwapChain* m_primarySwapChain = nullptr;
    };

} // namespace RVX
