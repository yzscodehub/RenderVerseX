#include "VulkanCommandBufferPool.h"
#include "VulkanDevice.h"

namespace RVX
{
    VulkanCommandBufferPool::VulkanCommandBufferPool(VulkanDevice* device, uint32 queueFamilyIndex)
        : m_device(device)
        , m_queueFamilyIndex(queueFamilyIndex)
    {
        // Create the underlying command pool
        VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | 
                         VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = queueFamilyIndex;

        VkResult result = vkCreateCommandPool(device->GetDevice(), &poolInfo, nullptr, &m_commandPool);
        if (result != VK_SUCCESS)
        {
            RVX_RHI_ERROR("Failed to create command buffer pool: {}", VkResultToString(result));
        }
    }

    VulkanCommandBufferPool::~VulkanCommandBufferPool()
    {
        if (m_commandPool != VK_NULL_HANDLE)
        {
            // All command buffers are implicitly freed when the pool is destroyed
            vkDestroyCommandPool(m_device->GetDevice(), m_commandPool, nullptr);
        }
    }

    VkCommandBuffer VulkanCommandBufferPool::Acquire()
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Try to reuse an available buffer
        if (!m_availableBuffers.empty())
        {
            VkCommandBuffer cmdBuffer = m_availableBuffers.back();
            m_availableBuffers.pop_back();
            m_activeCount++;
            return cmdBuffer;
        }

        // Need to allocate new buffers
        VkCommandBufferAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocInfo.commandPool = m_commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = s_batchAllocSize;

        std::vector<VkCommandBuffer> newBuffers(s_batchAllocSize);
        VkResult result = vkAllocateCommandBuffers(m_device->GetDevice(), &allocInfo, newBuffers.data());
        if (result != VK_SUCCESS)
        {
            RVX_RHI_ERROR("Failed to allocate command buffers: {}", VkResultToString(result));
            return VK_NULL_HANDLE;
        }

        // Track all allocated buffers
        m_allBuffers.insert(m_allBuffers.end(), newBuffers.begin(), newBuffers.end());

        // Return first buffer, add rest to available pool
        VkCommandBuffer cmdBuffer = newBuffers[0];
        for (size_t i = 1; i < newBuffers.size(); ++i)
        {
            m_availableBuffers.push_back(newBuffers[i]);
        }

        m_activeCount++;
        RVX_RHI_DEBUG("Allocated {} new command buffers (total: {})", s_batchAllocSize, m_allBuffers.size());
        return cmdBuffer;
    }

    void VulkanCommandBufferPool::Release(VkCommandBuffer cmdBuffer)
    {
        if (cmdBuffer == VK_NULL_HANDLE)
            return;

        std::lock_guard<std::mutex> lock(m_mutex);

        // Reset the command buffer before returning to pool
        vkResetCommandBuffer(cmdBuffer, 0);
        m_availableBuffers.push_back(cmdBuffer);

        if (m_activeCount > 0)
            m_activeCount--;
    }

    void VulkanCommandBufferPool::ResetAll()
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Reset the entire command pool (more efficient than individual resets)
        if (m_commandPool != VK_NULL_HANDLE)
        {
            vkResetCommandPool(m_device->GetDevice(), m_commandPool, 0);
        }

        // All buffers become available again
        m_availableBuffers = m_allBuffers;
        m_activeCount = 0;
    }

} // namespace RVX
