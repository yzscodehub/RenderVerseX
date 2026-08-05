#pragma once

#include "VulkanCommon.h"
#include "RHI/RHIDevice.h"
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace RVX
{
    class VulkanSwapChain;

    /** @brief Native Vulkan entry point selected for indexed indirect-count draws. */
    enum class VulkanIndexedIndirectCountDispatch : uint8
    {
        None = 0,
        Core12,
        KHR,
    };

    /** @brief Inputs used to select the already-enabled native count path. */
    struct VulkanIndexedIndirectCountDispatchInput
    {
        uint32 apiVersion = 0;
        bool drawIndirectCountFeatureEnabled = false;
        bool khrExtensionEnabled = false;
        bool coreEntryPointLoaded = false;
        bool khrEntryPointLoaded = false;
    };

    /**
     * @brief Select a count-dispatch path from logical-device state only.
     *
     * A Vulkan 1.2+ device must use its explicitly enabled core feature and
     * entry point.  Do not use the KHR extension to bypass a disabled core
     * feature on such a device.
     */
    inline VulkanIndexedIndirectCountDispatch SelectVulkanIndexedIndirectCountDispatch(
        const VulkanIndexedIndirectCountDispatchInput& input)
    {
        if (input.apiVersion >= VK_API_VERSION_1_2)
        {
            return input.drawIndirectCountFeatureEnabled && input.coreEntryPointLoaded
                ? VulkanIndexedIndirectCountDispatch::Core12
                : VulkanIndexedIndirectCountDispatch::None;
        }

        return input.khrExtensionEnabled && input.khrEntryPointLoaded
            ? VulkanIndexedIndirectCountDispatch::KHR
            : VulkanIndexedIndirectCountDispatch::None;
    }

    /** @brief Physical-device requirements imposed by the Vulkan backend implementation. */
    struct VulkanRequiredDeviceFeatureSupport
    {
        uint32 apiVersion = 0;
        bool timelineSemaphore = false;
        bool dynamicRendering = false;
        bool synchronization2 = false;
    };

    /**
     * @brief Whether a physical device can run the backend's unconditional Vulkan 1.3 paths.
     *
     * Timeline semaphores, dynamic rendering, and synchronization2 are used
     * throughout the backend without an emulation path, so selection must
     * fail closed before creating a logical device.
     */
    inline bool IsVulkanRequiredDeviceConfigurationSupported(
        const VulkanRequiredDeviceFeatureSupport& support)
    {
        return support.apiVersion >= VK_API_VERSION_1_3 &&
               support.timelineSemaphore &&
               support.dynamicRendering &&
               support.synchronization2;
    }

    /** @brief Select the optional memory-budget extension from enumerated device availability. */
    inline bool SelectVulkanMemoryBudgetExtensionEnabled(bool extensionAvailable)
    {
        return extensionAvailable;
    }

    /** @brief Logical-device inputs controlling indexed indirect execution publication. */
    struct VulkanIndexedIndirectExecutionSelectionInput
    {
        bool multiDrawIndirectEnabled = false;
        VulkanIndexedIndirectCountDispatch countDispatch =
            VulkanIndexedIndirectCountDispatch::None;
        uint32 hardwareMaxDrawCount = 1;
    };

    /** @brief Backend-neutral indexed-indirect capabilities derived from enabled Vulkan state. */
    struct VulkanIndexedIndirectExecutionSelection
    {
        bool supportsFixedCount = true;
        bool supportsCountBuffer = false;
        uint32 maxDrawCount = 1;
    };

    /**
     * @brief Publish multi-command indirect support only when multiDrawIndirect was enabled.
     *
     * A Vulkan device can always execute one fixed indirect command.  Count
     * buffer dispatch and hardware draw-count limits describe multi-command
     * semantics, so they must not escape a disabled multiDrawIndirect feature.
     */
    inline VulkanIndexedIndirectExecutionSelection
    SelectVulkanIndexedIndirectExecutionSelection(
        const VulkanIndexedIndirectExecutionSelectionInput& input)
    {
        VulkanIndexedIndirectExecutionSelection selection;
        selection.supportsCountBuffer =
            input.multiDrawIndirectEnabled &&
            input.countDispatch != VulkanIndexedIndirectCountDispatch::None;
        selection.maxDrawCount = input.multiDrawIndirectEnabled
            ? input.hardwareMaxDrawCount
            : 1;
        return selection;
    }

    /** @brief Validation-layer messages observed by this logical device. */
    struct VulkanValidationMessageCounts
    {
        uint32 errors = 0;
        uint32 warnings = 0;
    };

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
        uint64 SubmitCommandContext(RHICommandContext* context, RHIFence* signalFence) override;
        uint64 SubmitCommandContexts(std::span<RHICommandContext* const> contexts, RHIFence* signalFence) override;

        RHISwapChainRef CreateSwapChain(const RHISwapChainDesc& desc) override;

        RHIFenceRef CreateFence(uint64 initialValue) override;
        void WaitForFence(RHIFence* fence, uint64 value) override;
        void WaitIdle() override;

        void BeginFrame() override;
        void EndFrame() override;
        uint32 GetCurrentFrameIndex() const override { return m_currentFrameIndex; }

        const RHICapabilities& GetCapabilities() const override { return m_capabilities; }
        RHIBackendType GetBackendType() const override { return RHIBackendType::Vulkan; }
        RHIDeviceRuntimeStatus QueryRuntimeStatus() const noexcept override;
        RHIDeviceFault GetLastDeviceFault() const override;

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
        // Queue-family ownership release/acquire pairs are not implemented.
        // Keep all logical domains on graphics until that contract exists.
        uint32 GetComputeQueueFamily() const { return GetGraphicsQueueFamily(); }
        uint32 GetTransferQueueFamily() const { return GetGraphicsQueueFamily(); }

        VkCommandPool GetCommandPool(RHICommandQueueType type);
        VkDescriptorPool GetDescriptorPool() const { return m_descriptorPool; }
        VkPipelineCache GetPipelineCache() const { return m_pipelineCache; }

        VulkanIndexedIndirectCountDispatch GetIndexedIndirectCountDispatch() const
        {
            return m_indexedIndirectCountDispatch;
        }
        PFN_vkCmdDrawIndexedIndirectCount GetCmdDrawIndexedIndirectCount() const
        {
            return m_vkCmdDrawIndexedIndirectCount;
        }
        PFN_vkCmdDrawIndexedIndirectCountKHR GetCmdDrawIndexedIndirectCountKHR() const
        {
            return m_vkCmdDrawIndexedIndirectCountKHR;
        }
        bool IsBufferDeviceAddressEnabled() const
        {
            return m_enabledBufferDeviceAddress;
        }
        bool IsMemoryBudgetEnabled() const
        {
            return m_enabledMemoryBudget;
        }
        bool IsMultiDrawIndirectEnabled() const
        {
            return m_enabledMultiDrawIndirect;
        }
        VulkanPipelineStageSupport GetEnabledPipelineStageSupport() const
        {
            return {
                m_enabledGeometryShader,
                m_enabledTessellationShader,
                m_capabilities.supportsMeshShaders,
                m_capabilities.supportsRaytracingPipeline};
        }

        VulkanValidationMessageCounts GetValidationMessageCounts() const
        {
            return {
                m_validationErrorCount.load(std::memory_order_acquire),
                m_validationWarningCount.load(std::memory_order_acquire)};
        }
        void RecordValidationMessage(
            VkDebugUtilsMessageSeverityFlagBitsEXT severity) noexcept;

        VkSemaphore GetImageAvailableSemaphore() const { return m_imageAvailableSemaphores[m_currentFrameIndex]; }
        VkSemaphore GetRenderFinishedSemaphore() const;
        VkFence GetCurrentFrameFence() const { return m_frameFences[m_currentFrameIndex]; }
        /** @brief Serializes host operations submitted to the graphics-aliased queue. */
        std::mutex& GetGraphicsQueueMutex() { return m_graphicsQueueMutex; }

        /**
         * @brief Returns the current frame fence only while VulkanDevice::BeginFrame owns it.
         *
         * Callers must hold GetGraphicsQueueMutex() and call
         * MarkArmedFrameFenceSubmitted() after a successful graphics-queue
         * submission using the returned fence. Raw RHI submissions outside a
         * device frame always receive VK_NULL_HANDLE.
         */
        VkFence GetArmedFrameFenceForSubmission() const;
        void MarkArmedFrameFenceSubmitted(VkFence fence);

        /**
         * @brief Retire binary semaphores after the caller's queue submission.
         *
         * Callers must hold GetGraphicsQueueMutex() because this method appends
         * an internal submission to the graphics-aliased queue.
         */
        void EnqueueDeferredSemaphoreDestroy(std::vector<VkSemaphore> semaphores, VkQueue signalQueue);
        void ReportRuntimeFailure(VkResult result,
                                  RHIDeviceFaultOperation operation,
                                  const char* message) noexcept;

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
        bool CreateInstance(const RHIDeviceDesc& desc);
        bool SelectPhysicalDevice(bool allowSoftwareAdapter);
        bool CreateLogicalDevice();
        bool CreateAllocator();
        bool CreateCommandPools();
        bool CreateDescriptorPool();
        bool CreatePipelineCache();
        void SavePipelineCache();
        void ProcessDeferredSemaphoreDestroys(bool force);

        QueueFamilyIndices FindQueueFamilies(VkPhysicalDevice device);
        bool CheckDeviceExtensionSupport(VkPhysicalDevice device);
        void QueryDeviceCapabilities();
        [[nodiscard]] std::string CaptureDeviceFaultDescription() const;

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
        std::array<bool, RVX_MAX_FRAME_COUNT> m_frameFenceArmed = {};
        uint32 m_currentFrameIndex = 0;

        // State
        RHICapabilities m_capabilities;
        bool m_enabledDrawIndirectFirstInstance = false;
        bool m_enabledTimelineSemaphore = false;
        bool m_enabledMultiDrawIndirect = false;
        bool m_enabledDrawIndirectCount = false;
        bool m_enabledDrawIndirectCountKHR = false;
        bool m_enabledDescriptorIndexing = false;
        bool m_enabledBufferDeviceAddress = false;
        bool m_enabledGeometryShader = false;
        bool m_enabledTessellationShader = false;
        bool m_enabledSynchronization2 = false;
        bool m_enabledDynamicRendering = false;
        bool m_enabledMemoryBudget = false;
        VulkanIndexedIndirectCountDispatch m_indexedIndirectCountDispatch =
            VulkanIndexedIndirectCountDispatch::None;
        PFN_vkCmdDrawIndexedIndirectCount m_vkCmdDrawIndexedIndirectCount = nullptr;
        PFN_vkCmdDrawIndexedIndirectCountKHR m_vkCmdDrawIndexedIndirectCountKHR = nullptr;
        bool m_validationEnabled = false;
        std::atomic<uint32> m_validationErrorCount{0};
        std::atomic<uint32> m_validationWarningCount{0};
        std::atomic<RHIDeviceRuntimeStatus> m_runtimeStatus{
            RHIDeviceRuntimeStatus::Ready};
        std::atomic<bool> m_faultClaimed{false};
        std::atomic<uint32> m_lastFaultNativeError{0};
        std::atomic<RHIDeviceFaultOperation> m_lastFaultOperation{
            RHIDeviceFaultOperation::None};
        std::atomic<uint64> m_faultSequence{0};
        mutable std::mutex m_deviceFaultMutex;
        std::string m_deviceFaultMessage;
        bool m_deviceFaultEnabled = false;

        // Thread safety
        std::mutex m_graphicsQueueMutex;

        struct DeferredSemaphoreDestroy
        {
            std::vector<VkSemaphore> semaphores;
            VkFence fence = VK_NULL_HANDLE;
        };
        std::vector<DeferredSemaphoreDestroy> m_deferredSemaphoreDestroys;
        std::mutex m_deferredSemaphoreMutex;

        // Optional primary swapchain for per-image sync
        VulkanSwapChain* m_primarySwapChain = nullptr;
    };

} // namespace RVX
