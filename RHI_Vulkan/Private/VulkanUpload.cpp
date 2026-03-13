#include "VulkanUpload.h"
#include "VulkanResources.h"

namespace RVX
{
    // =============================================================================
    // Vulkan Staging Buffer
    // =============================================================================
    VulkanStagingBuffer::VulkanStagingBuffer(VulkanDevice* device, const RHIStagingBufferDesc& desc)
        : m_device(device), m_size(desc.size)
    {
        VkBufferCreateInfo bufferInfo = {};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = m_size;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkResult result = vkCreateBuffer(device->GetDevice(), &bufferInfo, nullptr, &m_buffer);
        if (result != VK_SUCCESS)
        {
            RVX_RHI_ERROR("VulkanStagingBuffer: Failed to create buffer: {}", result);
            return;
        }

        // Get memory requirements and allocate HOST_VISIBLE memory
        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(device->GetDevice(), m_buffer, &memRequirements);

        VkMemoryAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = device->FindMemoryType(
            memRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        result = vkAllocateMemory(device->GetDevice(), &allocInfo, nullptr, &m_memory);
        if (result != VK_SUCCESS)
        {
            RVX_RHI_ERROR("VulkanStagingBuffer: Failed to allocate memory: {}", result);
            vkDestroyBuffer(device->GetDevice(), m_buffer, nullptr);
            m_buffer = VK_NULL_HANDLE;
            return;
        }

        result = vkBindBufferMemory(device->GetDevice(), m_buffer, m_memory, 0);
        if (result != VK_SUCCESS)
        {
            RVX_RHI_ERROR("VulkanStagingBuffer: Failed to bind memory: {}", result);
        }

        if (desc.debugName)
        {
            device->SetDebugName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64>(m_buffer), desc.debugName);
        }
    }

    VulkanStagingBuffer::~VulkanStagingBuffer()
    {
        if (m_buffer != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(m_device->GetDevice(), m_buffer, nullptr);
        }
        if (m_memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(m_device->GetDevice(), m_memory, nullptr);
        }
    }

    void* VulkanStagingBuffer::Map(uint64 offset, uint64 size)
    {
        if (m_isMapped)
        {
            return m_mappedData;
        }

        VkDeviceSize mapSize = (size == RVX_WHOLE_SIZE) ? (m_size - offset) : size;
        void* data = nullptr;
        VkResult result = vkMapMemory(m_device->GetDevice(), m_memory, offset, mapSize, 0, &data);
        if (result != VK_SUCCESS)
        {
            RVX_RHI_ERROR("VulkanStagingBuffer: Failed to map memory: {}", result);
            return nullptr;
        }

        m_mappedData = data;
        m_isMapped = true;
        return m_mappedData;
    }

    void VulkanStagingBuffer::Unmap()
    {
        if (!m_isMapped)
        {
            return;
        }

        vkUnmapMemory(m_device->GetDevice(), m_memory);
        m_mappedData = nullptr;
        m_isMapped = false;
    }

    RHIBuffer* VulkanStagingBuffer::GetBuffer() const
    {
        if (!m_wrapperBuffer)
        {
            RHIBufferDesc desc = {};
            desc.size = m_size;
            desc.usage = RHIBufferUsage::CopySrc;
            desc.memoryType = RHIMemoryType::Upload;
            m_wrapperBuffer = CreateVulkanBuffer(m_device, desc);
        }
        return m_wrapperBuffer.get();
    }

    // =============================================================================
    // Vulkan Ring Buffer
    // =============================================================================
    VulkanRingBuffer::VulkanRingBuffer(VulkanDevice* device, const RHIRingBufferDesc& desc)
        : m_device(device), m_totalSize(desc.size), m_alignment(desc.alignment)
    {
        VkBufferCreateInfo bufferInfo = {};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = m_totalSize;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkResult result = vkCreateBuffer(device->GetDevice(), &bufferInfo, nullptr, &m_buffer);
        if (result != VK_SUCCESS)
        {
            RVX_RHI_ERROR("VulkanRingBuffer: Failed to create buffer: {}", result);
            return;
        }

        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(device->GetDevice(), m_buffer, &memRequirements);

        VkMemoryAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = device->FindMemoryType(
            memRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        result = vkAllocateMemory(device->GetDevice(), &allocInfo, nullptr, &m_memory);
        if (result != VK_SUCCESS)
        {
            RVX_RHI_ERROR("VulkanRingBuffer: Failed to allocate memory: {}", result);
            vkDestroyBuffer(device->GetDevice(), m_buffer, nullptr);
            m_buffer = VK_NULL_HANDLE;
            return;
        }

        result = vkBindBufferMemory(device->GetDevice(), m_buffer, m_memory, 0);
        if (result != VK_SUCCESS)
        {
            RVX_RHI_ERROR("VulkanRingBuffer: Failed to bind memory: {}", result);
        }

        // Persistent mapping
        vkMapMemory(device->GetDevice(), m_memory, 0, m_totalSize, 0, &m_mappedData);

        if (desc.debugName)
        {
            device->SetDebugName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64>(m_buffer), desc.debugName);
        }
    }

    VulkanRingBuffer::~VulkanRingBuffer()
    {
        if (m_mappedData)
        {
            vkUnmapMemory(m_device->GetDevice(), m_memory);
        }
        if (m_buffer != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(m_device->GetDevice(), m_buffer, nullptr);
        }
        if (m_memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(m_device->GetDevice(), m_memory, nullptr);
        }
    }

    RHIRingAllocation VulkanRingBuffer::Allocate(uint64 size)
    {
        // Align the allocation size
        uint64 alignedSize = (size + m_alignment - 1) & ~(m_alignment - 1);

        uint64 currentHead = m_head.load(std::memory_order_acquire);

        while (true)
        {
            uint64 nextHead = currentHead + alignedSize;

            // Wrap around if needed
            if (nextHead >= m_totalSize)
            {
                currentHead = 0;
                nextHead = alignedSize;
            }

            // Check if there's enough space (need to wait for GPU to complete)
            uint64 tail = m_tail.load(std::memory_order_acquire);
            if (nextHead > tail && nextHead > m_totalSize - tail)
            {
                // Not enough space, need to wait - in practice would need fence sync
                std::this_thread::yield();
                continue;
            }

            // Try to atomically update head
            if (m_head.compare_exchange_weak(currentHead, nextHead, std::memory_order_release))
            {
                RHIRingAllocation alloc;
                alloc.cpuAddress = static_cast<uint8*>(m_mappedData) + currentHead;
                alloc.gpuOffset = currentHead;
                alloc.size = alignedSize;
                alloc.buffer = m_wrapperBuffer.get();
                return alloc;
            }
        }
    }

    void VulkanRingBuffer::Reset(uint32 frameIndex)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Store fence value for this frame
        uint32 bufferIndex = frameIndex % MAX_FRAMES_IN_FLIGHT;
        m_frameFenceValues[bufferIndex] = m_head.load(std::memory_order_acquire);

        // Update tail to allow reuse of oldest frame's memory
        // Find the oldest frame that's still in flight
        uint64 minFenceValue = m_frameFenceValues[0];
        for (uint32 i = 1; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            if (m_frameFenceValues[i] < minFenceValue)
            {
                minFenceValue = m_frameFenceValues[i];
            }
        }

        // Reset head if it wrapped
        if (m_head.load(std::memory_order_acquire) >= m_totalSize)
        {
            m_head.store(0, std::memory_order_release);
        }
    }

    RHIBuffer* VulkanRingBuffer::GetBuffer() const
    {
        if (!m_wrapperBuffer)
        {
            RHIBufferDesc desc = {};
            desc.size = m_totalSize;
            desc.usage = RHIBufferUsage::CopySrc | RHIBufferUsage::Uniform;
            desc.memoryType = RHIMemoryType::Upload;
            m_wrapperBuffer = CreateVulkanBuffer(m_device, desc);
        }
        return m_wrapperBuffer.get();
    }

    // =============================================================================
    // Factory Functions
    // =============================================================================
    RHIStagingBufferRef CreateVulkanStagingBuffer(VulkanDevice* device, const RHIStagingBufferDesc& desc)
    {
        return new VulkanStagingBuffer(device, desc);
    }

    RHIRingBufferRef CreateVulkanRingBuffer(VulkanDevice* device, const RHIRingBufferDesc& desc)
    {
        return new VulkanRingBuffer(device, desc);
    }

} // namespace RVX
