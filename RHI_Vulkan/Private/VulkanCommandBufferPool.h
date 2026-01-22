#pragma once

#include "VulkanCommon.h"
#include <vector>
#include <mutex>

namespace RVX
{
    class VulkanDevice;

    // =============================================================================
    // VulkanCommandBufferPool - Per-Queue Command Buffer Pooling
    //
    // Manages reusable command buffers per queue family, reducing allocation
    // overhead for frequently created command contexts.
    // =============================================================================
    class VulkanCommandBufferPool
    {
    public:
        VulkanCommandBufferPool(VulkanDevice* device, uint32 queueFamilyIndex);
        ~VulkanCommandBufferPool();

        // Acquire a command buffer (either from pool or newly allocated)
        VkCommandBuffer Acquire();

        // Return a command buffer to the pool for reuse
        void Release(VkCommandBuffer cmdBuffer);

        // Reset all command buffers in the pool (call at frame boundaries)
        void ResetAll();

        // Get the underlying VkCommandPool
        VkCommandPool GetCommandPool() const { return m_commandPool; }

        // Statistics
        uint32 GetActiveCount() const { return m_activeCount; }
        uint32 GetPooledCount() const { return static_cast<uint32>(m_availableBuffers.size()); }

    private:
        VulkanDevice* m_device = nullptr;
        VkCommandPool m_commandPool = VK_NULL_HANDLE;
        uint32 m_queueFamilyIndex = 0;

        // Available command buffers for reuse
        std::vector<VkCommandBuffer> m_availableBuffers;

        // All allocated command buffers (for cleanup)
        std::vector<VkCommandBuffer> m_allBuffers;

        // Thread safety
        std::mutex m_mutex;

        // Statistics
        uint32 m_activeCount = 0;

        // Batch allocation size
        static constexpr uint32 s_batchAllocSize = 8;
    };

} // namespace RVX
