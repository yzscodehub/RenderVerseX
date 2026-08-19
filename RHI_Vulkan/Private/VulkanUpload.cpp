#include "VulkanUpload.h"
#include "VulkanResources.h"

namespace RVX
{
    namespace
    {
        bool FindMemoryType(
            VkPhysicalDevice physicalDevice,
            uint32 typeFilter,
            VkMemoryPropertyFlags requiredProperties,
            uint32& outMemoryTypeIndex,
            VkMemoryPropertyFlags& outMemoryProperties)
        {
            outMemoryTypeIndex = 0;
            outMemoryProperties = 0;
            VkPhysicalDeviceMemoryProperties memProperties = {};
            vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

            for (uint32 i = 0; i < memProperties.memoryTypeCount; ++i)
            {
                const bool typeMatches = (typeFilter & (1u << i)) != 0;
                const VkMemoryPropertyFlags properties =
                    memProperties.memoryTypes[i].propertyFlags;
                const bool propertiesMatch =
                    (properties & requiredProperties) == requiredProperties;
                if (typeMatches && propertiesMatch)
                {
                    outMemoryTypeIndex = i;
                    outMemoryProperties = properties;
                    return true;
                }
            }

            RVX_RHI_ERROR(
                "VulkanUpload: Failed to find memory type with required properties 0x{:X}",
                static_cast<uint32>(requiredProperties));
            return false;
        }
    } // namespace

    // =============================================================================
    // Vulkan Staging Buffer
    // =============================================================================
    VulkanStagingBuffer::VulkanStagingBuffer(VulkanDevice* device, const RHIStagingBufferDesc& desc)
        : m_device(device), m_size(desc.size)
    {
        if (m_device == nullptr || m_size == 0)
        {
            RVX_RHI_ERROR("VulkanStagingBuffer: invalid device or zero-sized allocation");
            return;
        }

        VkBufferCreateInfo bufferInfo = {};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = m_size;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkResult result = vkCreateBuffer(device->GetDevice(), &bufferInfo, nullptr, &m_buffer);
        if (result != VK_SUCCESS)
        {
            RVX_RHI_ERROR("VulkanStagingBuffer: Failed to create buffer: {}", static_cast<int32>(result));
            return;
        }

        // Staging writes are submitted without an extra flush path. Require
        // HOST_COHERENT memory rather than silently selecting a different
        // type and claiming the CPU data is GPU-visible.
        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(device->GetDevice(), m_buffer, &memRequirements);

        VkMemoryAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        const VkMemoryPropertyFlags requiredMemoryProperties =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if (!FindMemoryType(m_device->GetPhysicalDevice(),
                            memRequirements.memoryTypeBits,
                            requiredMemoryProperties,
                            allocInfo.memoryTypeIndex,
                            m_memoryProperties) ||
            (m_memoryProperties & requiredMemoryProperties) !=
                requiredMemoryProperties)
        {
            RVX_RHI_ERROR(
                "VulkanStagingBuffer: no HOST_VISIBLE|HOST_COHERENT memory type is available");
            vkDestroyBuffer(m_device->GetDevice(), m_buffer, nullptr);
            m_buffer = VK_NULL_HANDLE;
            m_memoryProperties = 0;
            return;
        }

        result = vkAllocateMemory(device->GetDevice(), &allocInfo, nullptr, &m_memory);
        if (result != VK_SUCCESS)
        {
            RVX_RHI_ERROR("VulkanStagingBuffer: Failed to allocate memory: {}", static_cast<int32>(result));
            vkDestroyBuffer(device->GetDevice(), m_buffer, nullptr);
            m_buffer = VK_NULL_HANDLE;
            return;
        }

        result = vkBindBufferMemory(device->GetDevice(), m_buffer, m_memory, 0);
        if (result != VK_SUCCESS)
        {
            RVX_RHI_ERROR("VulkanStagingBuffer: Failed to bind memory: {}", static_cast<int32>(result));
            vkFreeMemory(m_device->GetDevice(), m_memory, nullptr);
            vkDestroyBuffer(m_device->GetDevice(), m_buffer, nullptr);
            m_memory = VK_NULL_HANDLE;
            m_buffer = VK_NULL_HANDLE;
            m_memoryProperties = 0;
            return;
        }

        if (desc.debugName)
        {
            device->SetObjectName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64>(m_buffer), desc.debugName);
        }
    }

    VulkanStagingBuffer::~VulkanStagingBuffer()
    {
        m_wrapperBuffer.Reset();
        if (m_isMapped && IsValid())
        {
            vkUnmapMemory(m_device->GetDevice(), m_memory);
        }
        DiscardMappedState();
        if (m_buffer != VK_NULL_HANDLE && m_device != nullptr &&
            m_device->GetDevice() != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(m_device->GetDevice(), m_buffer, nullptr);
        }
        if (m_memory != VK_NULL_HANDLE && m_device != nullptr &&
            m_device->GetDevice() != VK_NULL_HANDLE)
        {
            vkFreeMemory(m_device->GetDevice(), m_memory, nullptr);
        }
    }

    void* VulkanStagingBuffer::Map(uint64 offset, uint64 size)
    {
        if (HasActiveMappedWriteRange())
        {
            RVX_RHI_ERROR("VulkanStagingBuffer: Cannot use legacy Map() during an active mapped-write transaction");
            return nullptr;
        }

        if (!IsValid() || m_isMapped || offset > m_size)
        {
            return nullptr;
        }

        VkDeviceSize mapSize = (size == RVX_WHOLE_SIZE) ? (m_size - offset) : size;
        if (mapSize == 0 || mapSize > m_size - offset)
        {
            return nullptr;
        }
        // Vulkan maps a VkDeviceMemory allocation once. Returning a requested
        // staging subrange therefore derives from an allocation-base mapping;
        // mapping directly at offset 1 is invalid on implementations whose
        // minMemoryMapAlignment is larger than one byte.
        void* data = nullptr;
        VkResult result = vkMapMemory(
            m_device->GetDevice(), m_memory, 0, VK_WHOLE_SIZE, 0, &data);
        if (result != VK_SUCCESS)
        {
            m_device->ReportRuntimeFailure(
                result,
                RHIDeviceFaultOperation::Context,
                "Vulkan staging buffer host mapping failed");
            RVX_RHI_ERROR("VulkanStagingBuffer: Failed to map memory: {}", static_cast<int32>(result));
            return nullptr;
        }

        m_mappedData = data;
        m_isMapped = true;
        return static_cast<uint8*>(m_mappedData) + offset;
    }

    void VulkanStagingBuffer::Unmap()
    {
        if (HasActiveMappedWriteRange())
        {
            RVX_RHI_ERROR("VulkanStagingBuffer: Cannot use legacy Unmap() during an active mapped-write transaction");
            return;
        }

        if (!m_isMapped)
        {
            return;
        }

        if (!CommitMappedWrite())
        {
            RVX_RHI_ERROR(
                "VulkanStagingBuffer: failed to commit mapped staging write");
            if (IsValid())
            {
                vkUnmapMemory(m_device->GetDevice(), m_memory);
            }
            DiscardMappedState();
        }
    }

    void VulkanStagingBuffer::DiscardMappedState() noexcept
    {
        m_mappedData = nullptr;
        m_isMapped = false;
    }

    bool VulkanStagingBuffer::CommitMappedWrite()
    {
        if (HasActiveMappedWriteRange())
        {
            RVX_RHI_ERROR("VulkanStagingBuffer: Cannot use legacy CommitMappedWrite() during an active mapped-write transaction");
            return false;
        }

        if (!IsValid() || !m_isMapped || m_mappedData == nullptr)
        {
            if (m_isMapped)
                DiscardMappedState();
            return false;
        }

        vkUnmapMemory(m_device->GetDevice(), m_memory);
        DiscardMappedState();
        return true;
    }

    RHIHostWriteReceipt VulkanStagingBuffer::CommitMappedWriteRangeImpl(
        uint64,
        uint64)
    {
        RHIHostWriteReceipt receipt;
        if (!IsValid() || !m_isMapped || m_mappedData == nullptr)
        {
            return receipt;
        }

        // Vulkan staging allocations deliberately require HOST_COHERENT
        // memory. Unmapping ends the CPU access, but no explicit cache range
        // was flushed, so reporting the requested memcpy range as synchronized
        // would be misleading.
        vkUnmapMemory(m_device->GetDevice(), m_memory);
        DiscardMappedState();
        receipt.committed = true;
        receipt.synchronization =
            RHIHostWriteSynchronization::CoherentNoExplicitSync;
        return receipt;
    }

    bool VulkanStagingBuffer::CancelMappedWriteRangeImpl(uint64, uint64)
    {
        if (!m_isMapped || m_mappedData == nullptr)
        {
            return false;
        }

        if (IsValid())
            vkUnmapMemory(m_device->GetDevice(), m_memory);
        DiscardMappedState();
        return true;
    }

    RHIBuffer* VulkanStagingBuffer::GetBuffer() const
    {
        if (!IsValid())
        {
            return nullptr;
        }
        if (!m_wrapperBuffer)
        {
            RHIBufferDesc desc = {};
            desc.size = m_size;
            desc.usage = RHIBufferUsage::CopySrc;
            // The wrapper must reference this staging allocation. Creating a
            // normal VulkanBuffer here would allocate a second, empty buffer
            // and make every staging copy read from the wrong resource.
            desc.memoryType = RHIMemoryType::Default;
            m_wrapperBuffer = Ref<VulkanBuffer>(new VulkanBuffer(
                m_device, m_buffer, m_memory, 0, desc, false));
        }
        return m_wrapperBuffer.Get();
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
            RVX_RHI_ERROR("VulkanRingBuffer: Failed to create buffer: {}", static_cast<int32>(result));
            return;
        }

        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(device->GetDevice(), m_buffer, &memRequirements);

        VkMemoryAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        VkMemoryPropertyFlags memoryProperties = 0;
        if (!FindMemoryType(
                device->GetPhysicalDevice(),
                memRequirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                allocInfo.memoryTypeIndex,
                memoryProperties))
        {
            RVX_RHI_ERROR(
                "VulkanRingBuffer: no HOST_VISIBLE|HOST_COHERENT memory type is available");
            vkDestroyBuffer(device->GetDevice(), m_buffer, nullptr);
            m_buffer = VK_NULL_HANDLE;
            return;
        }

        result = vkAllocateMemory(device->GetDevice(), &allocInfo, nullptr, &m_memory);
        if (result != VK_SUCCESS)
        {
            RVX_RHI_ERROR("VulkanRingBuffer: Failed to allocate memory: {}", static_cast<int32>(result));
            vkDestroyBuffer(device->GetDevice(), m_buffer, nullptr);
            m_buffer = VK_NULL_HANDLE;
            return;
        }

        result = vkBindBufferMemory(device->GetDevice(), m_buffer, m_memory, 0);
        if (result != VK_SUCCESS)
        {
            RVX_RHI_ERROR("VulkanRingBuffer: Failed to bind memory: {}", static_cast<int32>(result));
        }

        // Persistent mapping
        vkMapMemory(device->GetDevice(), m_memory, 0, m_totalSize, 0, &m_mappedData);

        if (desc.debugName)
        {
            device->SetObjectName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64>(m_buffer), desc.debugName);
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
        const uint64 alignment = static_cast<uint64>(m_alignment);
        uint64 alignedSize = (size + alignment - 1) & ~(alignment - 1);

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
                alloc.buffer = GetBuffer();
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
            desc.usage = RHIBufferUsage::CopySrc | RHIBufferUsage::Constant;
            desc.memoryType = RHIMemoryType::Upload;
            m_wrapperBuffer = CreateVulkanBuffer(m_device, desc);
        }
        return m_wrapperBuffer.Get();
    }

    // =============================================================================
    // Factory Functions
    // =============================================================================
    RHIStagingBufferRef CreateVulkanStagingBuffer(VulkanDevice* device, const RHIStagingBufferDesc& desc)
    {
        RHIStagingBufferRef staging(new VulkanStagingBuffer(device, desc));
        return static_cast<VulkanStagingBuffer*>(staging.Get())->IsValid()
                   ? staging
                   : RHIStagingBufferRef{};
    }

    RHIRingBufferRef CreateVulkanRingBuffer(VulkanDevice* device, const RHIRingBufferDesc& desc)
    {
        return RHIRingBufferRef(new VulkanRingBuffer(device, desc));
    }

} // namespace RVX
