#include "VulkanResources.h"
#include "VulkanDevice.h"
#include <cmath>
#include <mutex>

namespace RVX
{
    namespace
    {
        const char* GetVulkanHeapTypeName(RHIHeapType type)
        {
            switch (type)
            {
                case RHIHeapType::Default:
                    return "Default";
                case RHIHeapType::Upload:
                    return "Upload";
                case RHIHeapType::Readback:
                    return "Readback";
                default:
                    return "Unknown";
            }
        }

        const char* GetRHIMemoryTypeName(RHIMemoryType type)
        {
            switch (type)
            {
                case RHIMemoryType::Default:
                    return "Default";
                case RHIMemoryType::Upload:
                    return "Upload";
                case RHIMemoryType::Readback:
                    return "Readback";
                default:
                    return "Unknown";
            }
        }

        bool IsVulkanPlacedBufferHeapTypeCompatible(
            RHIHeapType heapType,
            RHIMemoryType memoryType)
        {
            switch (memoryType)
            {
                case RHIMemoryType::Default:
                    return heapType == RHIHeapType::Default;
                case RHIMemoryType::Upload:
                    return heapType == RHIHeapType::Upload;
                case RHIMemoryType::Readback:
                    return heapType == RHIHeapType::Readback;
                default:
                    return false;
            }
        }
    } // namespace

    // =============================================================================
    // Vulkan Buffer
    // =============================================================================
    VulkanBuffer::VulkanBuffer(VulkanDevice* device, const RHIBufferDesc& desc)
        : m_device(device)
        , m_desc(desc)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        VkBufferCreateInfo bufferInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = desc.size;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        bufferInfo.usage = ToVkBufferUsage(desc.usage);
        if (RequiresVulkanBufferDeviceAddress(desc.usage) &&
            !device->IsBufferDeviceAddressEnabled())
        {
            RVX_RHI_ERROR(
                "Vulkan buffer '{}' requests device-address usage without an enabled logical-device feature",
                desc.debugName ? desc.debugName : "<unnamed>");
            return;
        }

        VmaAllocationCreateInfo allocInfo = {};
        switch (desc.memoryType)
        {
            case RHIMemoryType::Default:
                allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
                break;
            case RHIMemoryType::Upload:
                allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
                allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
                break;
            case RHIMemoryType::Readback:
                allocInfo.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
                break;
        }

        VmaAllocationInfo allocationInfo = {};
        VK_CHECK(vmaCreateBuffer(device->GetAllocator(), &bufferInfo, &allocInfo,
            &m_buffer, &m_allocation, &allocationInfo));

        // Upload buffers stay persistently mapped. Readback buffers are mapped
        // on demand so every vmaMapMemory call has a matching vmaUnmapMemory.
        if (desc.memoryType == RHIMemoryType::Upload)
        {
            m_mappedData = allocationInfo.pMappedData;
            m_mappedMemoryBase = m_mappedData;
            m_isPersistentMapping = m_mappedData != nullptr;
        }

        if (RequiresVulkanBufferDeviceAddress(desc.usage))
        {
            VkBufferDeviceAddressInfo addressInfo = {
                VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            addressInfo.buffer = m_buffer;
            m_deviceAddress =
                vkGetBufferDeviceAddress(device->GetDevice(), &addressInfo);
        }

        // Set debug name for RenderDoc/validation layers
        if (desc.debugName)
        {
            m_device->SetObjectName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64>(m_buffer), desc.debugName);
        }
    }

    VulkanBuffer::VulkanBuffer(VulkanDevice* device, VkBuffer buffer, VkDeviceMemory memory,
                               uint64 memoryOffset,
                               const RHIBufferDesc& desc, bool ownsBuffer)
        : m_device(device)
        , m_desc(desc)
        , m_buffer(buffer)
        , m_allocation(VK_NULL_HANDLE)
        , m_ownsBuffer(ownsBuffer)
        , m_boundMemory(memory)
        , m_memoryOffset(memoryOffset)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }
    }

    VulkanBuffer::VulkanBuffer(VulkanDevice* device, VkBuffer buffer, VulkanHeap* heap,
                               uint64 memoryOffset,
                               const RHIBufferDesc& desc, bool ownsBuffer)
        : m_device(device)
        , m_desc(desc)
        , m_buffer(buffer)
        , m_allocation(VK_NULL_HANDLE)  // Placed resource - no VMA allocation
        , m_ownsBuffer(ownsBuffer)
        , m_placedHeap(heap)
        , m_boundMemory(heap ? heap->GetMemory() : VK_NULL_HANDLE)
        , m_memoryOffset(memoryOffset)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        // Host-visible placed heaps own exactly one mapping for their complete
        // VkDeviceMemory allocation. Every placed subresource derives its
        // pointer from that mapping; a second vkMapMemory on the same memory is
        // invalid, even when the byte ranges do not overlap.
        if (desc.memoryType != RHIMemoryType::Default &&
            (!m_placedHeap || !m_placedHeap->GetMappedData()))
        {
            RVX_RHI_ERROR("Placed host-visible Vulkan buffer has no heap-owned mapping");
        }

        if (RequiresVulkanBufferDeviceAddress(desc.usage))
        {
            VkBufferDeviceAddressInfo addressInfo = {
                VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            addressInfo.buffer = m_buffer;
            m_deviceAddress =
                vkGetBufferDeviceAddress(device->GetDevice(), &addressInfo);
        }
    }

    VulkanBuffer::~VulkanBuffer()
    {
        if (m_buffer && m_device)
        {
            if (m_allocation && m_device->GetAllocator())
            {
                // VMA-allocated buffer
                vmaDestroyBuffer(m_device->GetAllocator(), m_buffer, m_allocation);
            }
            else if (m_ownsBuffer && m_device->GetDevice() != VK_NULL_HANDLE)
            {
                // Placed resource: destroy VkBuffer object, memory is owned by Heap
                // IMPORTANT: Caller MUST ensure correct destruction order:
                //   1. First: Release all Placed Buffers (this destructor)
                //   2. Then: Destroy the Heap (VkDeviceMemory)
                vkDestroyBuffer(m_device->GetDevice(), m_buffer, nullptr);
            }
            // If !m_ownsBuffer, the buffer is managed externally (e.g., swapchain)
        }
    }

    bool VulkanBuffer::IsHostAccessReady() const
    {
        return m_device &&
               m_device->GetDevice() != VK_NULL_HANDLE &&
               m_buffer != VK_NULL_HANDLE &&
               m_device->QueryRuntimeStatus() == RHIDeviceRuntimeStatus::Ready;
    }

    bool VulkanBuffer::IsHostCoherentMemory() const
    {
        if (m_placedHeap)
        {
            return m_placedHeap->IsHostCoherent();
        }

        if (!m_allocation || !m_device || !m_device->GetAllocator())
        {
            return false;
        }

        VkMemoryPropertyFlags memoryProperties = 0;
        vmaGetAllocationMemoryProperties(
            m_device->GetAllocator(), m_allocation, &memoryProperties);
        return (memoryProperties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    }

    bool VulkanBuffer::GetPlacedMappedMemoryRange(
        uint64 resourceOffset,
        uint64 resourceSize,
        VulkanMappedMemoryRange& range) const
    {
        range = {};
        if (!m_device ||
            !m_placedHeap ||
            m_boundMemory == VK_NULL_HANDLE)
        {
            return false;
        }

        const VkPhysicalDevice physicalDevice = m_device->GetPhysicalDevice();
        if (physicalDevice == VK_NULL_HANDLE)
            return false;

        VkPhysicalDeviceProperties properties = {};
        vkGetPhysicalDeviceProperties(physicalDevice, &properties);
        range = MakeVulkanHostWriteSynchronizationRange(
            m_memoryOffset,
            resourceOffset,
            resourceSize,
            m_placedHeap->GetSize(),
            properties.limits.nonCoherentAtomSize);
        return range.valid;
    }

    bool VulkanBuffer::SynchronizeMappedMemory(
        VulkanHostMemorySynchronization synchronization,
        uint64 resourceOffset,
        uint64 resourceSize,
        VulkanMappedMemoryRange* synchronizedRange,
        VkDeviceSize* synchronizedAllocationSize)
    {
        if (synchronizedRange)
        {
            *synchronizedRange = {};
        }
        if (synchronizedAllocationSize)
        {
            *synchronizedAllocationSize = 0;
        }

        if (synchronization == VulkanHostMemorySynchronization::None)
            return true;

        if (!IsHostAccessReady() || !m_mappedData)
        {
            RVX_RHI_ERROR("Cannot synchronize an unavailable Vulkan mapped buffer");
            return false;
        }

        if (IsHostCoherentMemory())
        {
            return true;
        }

        VkResult result = VK_ERROR_INITIALIZATION_FAILED;
        if (m_allocation)
        {
            const VmaAllocator allocator = m_device->GetAllocator();
            if (!allocator)
            {
                RVX_RHI_ERROR("Cannot synchronize Vulkan buffer without a VMA allocator");
                return false;
            }

            VmaAllocationInfo allocationInfo = {};
            vmaGetAllocationInfo(allocator, m_allocation, &allocationInfo);
            VkPhysicalDeviceProperties properties = {};
            vkGetPhysicalDeviceProperties(m_device->GetPhysicalDevice(), &properties);
            VulkanMappedMemoryRange range =
                MakeVulkanHostWriteSynchronizationRange(
                    0,
                    resourceOffset,
                    resourceSize,
                    allocationInfo.size,
                    properties.limits.nonCoherentAtomSize);
            if (!range.valid)
            {
                RVX_RHI_ERROR("Cannot synchronize Vulkan buffer with an invalid VMA range");
                return false;
            }

            // The offset and size passed to VMA are deliberately the already
            // atom-aligned range. This avoids turning a small transaction into
            // an allocation-wide cache operation.
            result = synchronization == VulkanHostMemorySynchronization::Flush
                ? vmaFlushAllocation(allocator, m_allocation, range.offset, range.size)
                : vmaInvalidateAllocation(allocator, m_allocation, range.offset, range.size);
            if (synchronizedRange)
            {
                *synchronizedRange = range;
            }
            if (synchronizedAllocationSize)
            {
                *synchronizedAllocationSize = allocationInfo.size;
            }
        }
        else if (m_placedHeap)
        {
            VulkanMappedMemoryRange range;
            if (!GetPlacedMappedMemoryRange(resourceOffset, resourceSize, range))
            {
                RVX_RHI_ERROR("Cannot synchronize placed Vulkan buffer with an invalid memory range");
                return false;
            }

            const bool synchronized =
                m_placedHeap->SynchronizeMappedRange(range, synchronization);
            if (synchronized && synchronizedRange)
            {
                *synchronizedRange = range;
            }
            if (synchronized && synchronizedAllocationSize)
            {
                *synchronizedAllocationSize = m_placedHeap->GetSize();
            }
            return synchronized;
        }
        else
        {
            RVX_RHI_ERROR("Cannot synchronize Vulkan buffer without a valid allocation");
            return false;
        }

        if (result != VK_SUCCESS)
        {
            m_device->ReportRuntimeFailure(
                result,
                RHIDeviceFaultOperation::Context,
                synchronization == VulkanHostMemorySynchronization::Flush
                    ? "Vulkan mapped buffer flush failed"
                    : "Vulkan mapped buffer invalidate failed");
            RVX_RHI_ERROR("Failed to {} Vulkan buffer memory: {}",
                          synchronization == VulkanHostMemorySynchronization::Flush
                              ? "flush"
                              : "invalidate",
                          VkResultToString(result));
            return false;
        }

        return true;
    }

    void VulkanBuffer::ReleaseTransientMapping()
    {
        if (m_isPersistentMapping || !m_mappedMemoryBase || !m_device)
            return;

        if (m_allocation &&
            m_device->QueryRuntimeStatus() == RHIDeviceRuntimeStatus::Ready)
        {
            if (const VmaAllocator allocator = m_device->GetAllocator())
            {
                vmaUnmapMemory(allocator, m_allocation);
            }
        }
        m_mappedMemoryBase = nullptr;
        m_mappedData = nullptr;
    }

    void* VulkanBuffer::Map()
    {
        if (HasActiveMappedWriteRange())
        {
            RVX_RHI_ERROR("Cannot use legacy Map() during an active Vulkan mapped-write transaction");
            return nullptr;
        }

        if (!IsHostAccessReady())
        {
            RVX_RHI_ERROR("Cannot map an unavailable Vulkan buffer");
            return nullptr;
        }

        if (m_desc.memoryType == RHIMemoryType::Default)
        {
            RVX_RHI_ERROR("Cannot map GPU-only buffer");
            return nullptr;
        }

        if (m_isMappedForAccess)
            return m_mappedData;

        if (m_placedHeap)
        {
            void* heapMappedData = m_placedHeap->GetMappedData();
            if (!heapMappedData)
            {
                RVX_RHI_ERROR("Placed Vulkan buffer heap mapping is unavailable");
                return nullptr;
            }

            m_mappedData = static_cast<uint8*>(heapMappedData) + m_memoryOffset;
            if (GetVulkanHostMemorySynchronization(m_desc.memoryType) ==
                    VulkanHostMemorySynchronization::Invalidate &&
                !SynchronizeMappedMemory(VulkanHostMemorySynchronization::Invalidate,
                                         0,
                                         m_desc.size))
            {
                m_mappedData = nullptr;
                return nullptr;
            }

            m_isMappedForAccess = true;
            return m_mappedData;
        }

        if (m_isPersistentMapping)
        {
            if (!m_mappedData)
            {
                RVX_RHI_ERROR("Vulkan persistent buffer mapping is unavailable");
                return nullptr;
            }

            m_isMappedForAccess = true;
            return m_mappedData;
        }

        void* mappedMemoryBase = nullptr;
        if (m_allocation)
        {
            // VMA-allocated buffer
            const VmaAllocator allocator = m_device->GetAllocator();
            if (!allocator)
            {
                RVX_RHI_ERROR("Cannot map Vulkan buffer without a VMA allocator");
                return nullptr;
            }

            const VkResult result = vmaMapMemory(allocator,
                                                 m_allocation,
                                                 &mappedMemoryBase);
            if (result != VK_SUCCESS)
            {
                m_device->ReportRuntimeFailure(
                    result,
                    RHIDeviceFaultOperation::Context,
                    "Vulkan buffer host mapping failed");
                RVX_RHI_ERROR("Failed to map Vulkan buffer: {}", VkResultToString(result));
                return nullptr;
            }
        }
        else
        {
            RVX_RHI_ERROR("Cannot map Vulkan buffer without a valid allocation");
            return nullptr;
        }

        if (!mappedMemoryBase)
        {
            RVX_RHI_ERROR("Vulkan buffer map returned no host pointer");
            if (m_allocation)
            {
                vmaUnmapMemory(m_device->GetAllocator(), m_allocation);
            }
            return nullptr;
        }

        m_mappedMemoryBase = mappedMemoryBase;
        m_mappedData = mappedMemoryBase;
        if (GetVulkanHostMemorySynchronization(m_desc.memoryType) ==
                VulkanHostMemorySynchronization::Invalidate &&
            !SynchronizeMappedMemory(VulkanHostMemorySynchronization::Invalidate,
                                     0,
                                     m_desc.size))
        {
            ReleaseTransientMapping();
            return nullptr;
        }

        m_isMappedForAccess = true;
        return m_mappedData;
    }

    void* VulkanBuffer::MapWriteRangeImpl(uint64 offset, uint64)
    {
        // Legacy Map() also supports readback invalidation. A transaction that
        // promises CPU-to-GPU publication must only expose upload memory.
        if (m_desc.memoryType != RHIMemoryType::Upload)
        {
            RVX_RHI_ERROR("Cannot begin a mapped write on a non-upload Vulkan buffer");
            return nullptr;
        }

        if (m_isMappedForAccess)
        {
            RVX_RHI_ERROR("Cannot begin a Vulkan range write during a legacy mapped access");
            return nullptr;
        }

        void* mappedData = Map();
        if (!mappedData)
        {
            return nullptr;
        }
        return static_cast<uint8*>(mappedData) + static_cast<size_t>(offset);
    }

    RHIHostWriteReceipt VulkanBuffer::CommitMappedWriteRangeImpl(
        uint64 offset,
        uint64 size)
    {
        RHIHostWriteReceipt receipt;
        if (!m_isMappedForAccess || !m_mappedData ||
            m_desc.memoryType != RHIMemoryType::Upload)
        {
            return receipt;
        }

        if (!IsHostAccessReady())
        {
            RVX_RHI_ERROR("Cannot commit writes to an unavailable Vulkan buffer");
            ReleaseTransientMapping();
            m_isMappedForAccess = false;
            return receipt;
        }

        if (IsHostCoherentMemory())
        {
            receipt.committed = true;
            receipt.synchronization =
                RHIHostWriteSynchronization::CoherentNoExplicitSync;
        }
        else
        {
            VulkanMappedMemoryRange synchronizedRange;
            VkDeviceSize synchronizedAllocationSize = 0;
            if (!SynchronizeMappedMemory(VulkanHostMemorySynchronization::Flush,
                                         offset,
                                         size,
                                         &synchronizedRange,
                                         &synchronizedAllocationSize))
            {
                ReleaseTransientMapping();
                m_isMappedForAccess = false;
                return receipt;
            }

            const VkDeviceSize synchronizedSize =
                GetVulkanMappedMemoryRangeCoveredSize(
                    synchronizedRange, synchronizedAllocationSize);
            if (synchronizedSize == 0)
            {
                RVX_RHI_ERROR("Vulkan mapped-write synchronization returned no covered bytes");
                ReleaseTransientMapping();
                m_isMappedForAccess = false;
                return receipt;
            }

            receipt.synchronization =
                DoesVulkanMappedMemoryRangeCoverWholeAllocation(
                    synchronizedRange, synchronizedAllocationSize)
                    ? RHIHostWriteSynchronization::WholeAllocation
                    : RHIHostWriteSynchronization::AtomAlignedRange;
            receipt.committed = true;
            receipt.synchronizedOffset = synchronizedRange.offset;
            receipt.synchronizedSize = synchronizedSize;
            receipt.synchronizedRangeAvailable = true;
        }

        ReleaseTransientMapping();
        m_isMappedForAccess = false;
        return receipt;
    }

    bool VulkanBuffer::CancelMappedWriteRangeImpl(uint64, uint64)
    {
        if (!m_isMappedForAccess || !m_mappedData)
        {
            return false;
        }

        ReleaseTransientMapping();
        m_isMappedForAccess = false;
        return true;
    }

    bool VulkanBuffer::CommitMappedWrite()
    {
        if (HasActiveMappedWriteRange())
        {
            RVX_RHI_ERROR("Cannot use legacy CommitMappedWrite() during an active Vulkan mapped-write transaction");
            return false;
        }

        if (!m_isMappedForAccess || !m_mappedData)
            return true;

        bool committed = true;
        if (!IsHostAccessReady())
        {
            // Device loss and invalid allocations must not trigger another host
            // operation. Do not publish this write; preserve the persistent
            // pointer only for destruction.
            RVX_RHI_ERROR("Cannot commit writes to an unavailable Vulkan buffer");
            committed = false;
        }
        else if (GetVulkanHostMemorySynchronization(m_desc.memoryType) ==
                 VulkanHostMemorySynchronization::Flush)
        {
            committed = SynchronizeMappedMemory(
                VulkanHostMemorySynchronization::Flush,
                0,
                m_desc.size);
        }

        ReleaseTransientMapping();
        m_isMappedForAccess = false;
        return committed;
    }

    void VulkanBuffer::Unmap()
    {
        if (HasActiveMappedWriteRange())
        {
            RVX_RHI_ERROR("Cannot use legacy Unmap() during an active Vulkan mapped-write transaction");
            return;
        }

        if (!CommitMappedWrite())
        {
            // Legacy callers cannot observe a commit failure. Keep the
            // operation fail-closed and emit an explicit diagnostic; new
            // callers must use CommitMappedWrite() to decide publication.
            RVX_RHI_ERROR("Vulkan buffer Unmap() failed to commit host writes");
        }
    }

    // =============================================================================
    // Vulkan Texture
    // =============================================================================
    VulkanTexture::VulkanTexture(VulkanDevice* device, const RHITextureDesc& desc)
        : m_device(device)
        , m_desc(desc)
        , m_ownsImage(true)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        VkImageCreateInfo imageInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};

        switch (desc.dimension)
        {
            case RHITextureDimension::Texture1D:
                imageInfo.imageType = VK_IMAGE_TYPE_1D;
                break;
            case RHITextureDimension::Texture2D:
                imageInfo.imageType = VK_IMAGE_TYPE_2D;
                break;
            case RHITextureDimension::Texture3D:
                imageInfo.imageType = VK_IMAGE_TYPE_3D;
                break;
            case RHITextureDimension::TextureCube:
                imageInfo.imageType = VK_IMAGE_TYPE_2D;
                imageInfo.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
                break;
        }

        imageInfo.extent.width = desc.width;
        imageInfo.extent.height = desc.height;
        imageInfo.extent.depth = desc.depth;
        imageInfo.mipLevels = desc.mipLevels;
        imageInfo.arrayLayers = GetTexturePhysicalLayerCount(desc);
        imageInfo.format = ToVkFormat(desc.format);
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.samples = static_cast<VkSampleCountFlagBits>(desc.sampleCount);

        // Usage flags
        imageInfo.usage = 0;
        if (HasFlag(desc.usage, RHITextureUsage::ShaderResource))
            imageInfo.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
        if (HasFlag(desc.usage, RHITextureUsage::UnorderedAccess))
            imageInfo.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        if (HasFlag(desc.usage, RHITextureUsage::RenderTarget))
            imageInfo.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (HasFlag(desc.usage, RHITextureUsage::DepthStencil))
            imageInfo.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

        imageInfo.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        VmaAllocationCreateInfo allocInfo = {};
        allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

        VK_CHECK(vmaCreateImage(device->GetAllocator(), &imageInfo, &allocInfo,
            &m_image, &m_allocation, nullptr));

        m_currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        // Set debug name for RenderDoc/validation layers
        if (desc.debugName)
        {
            m_device->SetObjectName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64>(m_image), desc.debugName);
        }
    }

    VulkanTexture::VulkanTexture(VulkanDevice* device, VkImage image, const RHITextureDesc& desc, bool ownsImage)
        : m_device(device)
        , m_desc(desc)
        , m_image(image)
        , m_ownsImage(ownsImage)
        , m_ownsAllocation(false)  // External image - no VMA allocation
        , m_currentLayout(VK_IMAGE_LAYOUT_UNDEFINED)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
            m_device->SetObjectName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64>(m_image), desc.debugName);
        }
    }

    VulkanTexture::~VulkanTexture()
    {
        if (m_ownsImage && m_image)
        {
            if (m_ownsAllocation && m_allocation)
            {
                // VMA-allocated resource
                vmaDestroyImage(m_device->GetAllocator(), m_image, m_allocation);
            }
            else
            {
                // Placed resource (memory owned by heap) - just destroy the image
                vkDestroyImage(m_device->GetDevice(), m_image, nullptr);
            }
        }
    }

    // =============================================================================
    // Vulkan Texture View
    // =============================================================================
    VulkanTextureView::VulkanTextureView(VulkanDevice* device, RHITexture* texture, const RHITextureViewDesc& desc)
        : RHITextureView(RHITextureRef(texture))
        , m_device(device)
        , m_texture(static_cast<VulkanTexture*>(texture))
        , m_format(desc.format == RHIFormat::Unknown ? texture->GetFormat() : desc.format)
        , m_subresourceRange(desc.subresourceRange)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        VkImageViewCreateInfo viewInfo = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = m_texture->GetImage();
        viewInfo.format = ToVkFormat(m_format);
        if (viewInfo.format == VK_FORMAT_UNDEFINED)
        {
            RVX_RHI_ERROR(
                "Vulkan: Refusing to create texture view '{}' with unresolved format: requested={}, source={}, resolved={}",
                desc.debugName ? desc.debugName : "<unnamed>",
                static_cast<uint32>(desc.format),
                static_cast<uint32>(texture->GetFormat()),
                static_cast<uint32>(m_format));
            return;
        }

        // View type based on texture dimension
        switch (texture->GetDimension())
        {
            case RHITextureDimension::Texture1D:
                viewInfo.viewType = (ResolveTextureArrayLayerCount(
                                         *texture, desc.subresourceRange) > 1) ?
                    VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
                break;
            case RHITextureDimension::Texture2D:
                viewInfo.viewType = (ResolveTextureArrayLayerCount(
                                         *texture, desc.subresourceRange) > 1) ?
                    VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
                break;
            case RHITextureDimension::Texture3D:
                viewInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
                break;
            case RHITextureDimension::TextureCube:
                viewInfo.viewType = (ResolveTextureArrayLayerCount(*texture, desc.subresourceRange) > 6) ?
                    VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
                break;
        }

        // Subresource range
        viewInfo.subresourceRange.baseMipLevel = desc.subresourceRange.baseMipLevel;
        viewInfo.subresourceRange.levelCount = (desc.subresourceRange.mipLevelCount == 0 || desc.subresourceRange.mipLevelCount == RVX_ALL_MIPS) ?
            VK_REMAINING_MIP_LEVELS : desc.subresourceRange.mipLevelCount;
        viewInfo.subresourceRange.baseArrayLayer = desc.subresourceRange.baseArrayLayer;
        viewInfo.subresourceRange.layerCount = (desc.subresourceRange.arrayLayerCount == 0 || desc.subresourceRange.arrayLayerCount == RVX_ALL_LAYERS) ?
            VK_REMAINING_ARRAY_LAYERS : desc.subresourceRange.arrayLayerCount;

        // Aspect mask follows the requested view, not just the source texture
        // usage. A depth SRV should not inherit stencil bits from a DSV path.
        if (IsDepthFormat(m_format))
        {
            switch (desc.subresourceRange.aspect)
            {
                case RHITextureAspect::Stencil:
                    viewInfo.subresourceRange.aspectMask = IsStencilFormat(m_format)
                                                               ? VK_IMAGE_ASPECT_STENCIL_BIT
                                                               : VK_IMAGE_ASPECT_DEPTH_BIT;
                    break;
                case RHITextureAspect::DepthStencil:
                    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                    if (IsStencilFormat(m_format))
                    {
                        viewInfo.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
                    }
                    break;
                case RHITextureAspect::Depth:
                case RHITextureAspect::Color:
                default:
                    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                    break;
            }
        }
        else
        {
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        }

        viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;

        VK_CHECK(vkCreateImageView(device->GetDevice(), &viewInfo, nullptr, &m_imageView));

        // Set debug name for RenderDoc/validation layers
        if (desc.debugName)
        {
            m_device->SetObjectName(VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<uint64>(m_imageView), desc.debugName);
        }
    }

    VulkanTextureView::~VulkanTextureView()
    {
        if (m_imageView)
        {
            vkDestroyImageView(m_device->GetDevice(), m_imageView, nullptr);
        }
    }

    // =============================================================================
    // Vulkan Sampler
    // =============================================================================
    static VkBorderColor ToVkBorderColor(const float borderColor[4])
    {
        auto closeTo = [](float value, float target) {
            return std::abs(value - target) < 1e-6f;
        };

        const bool isZeroRgb = closeTo(borderColor[0], 0.0f) &&
                               closeTo(borderColor[1], 0.0f) &&
                               closeTo(borderColor[2], 0.0f);
        const bool isOneRgb = closeTo(borderColor[0], 1.0f) &&
                              closeTo(borderColor[1], 1.0f) &&
                              closeTo(borderColor[2], 1.0f);
        const bool isAlphaZero = closeTo(borderColor[3], 0.0f);
        const bool isAlphaOne = closeTo(borderColor[3], 1.0f);

        if (isZeroRgb && isAlphaZero)
            return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        if (isZeroRgb && isAlphaOne)
            return VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        if (isOneRgb && isAlphaOne)
            return VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;

        RVX_RHI_WARN("Unsupported Vulkan border color (only transparent/opaque black/white supported). Using transparent black.");
        return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    }

    VulkanSampler::VulkanSampler(VulkanDevice* device, const RHISamplerDesc& desc)
        : m_device(device)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        VkSamplerCreateInfo samplerInfo = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = ToVkFilter(desc.magFilter);
        samplerInfo.minFilter = ToVkFilter(desc.minFilter);
        samplerInfo.mipmapMode = ToVkMipmapMode(desc.mipFilter);
        samplerInfo.addressModeU = ToVkSamplerAddressMode(desc.addressU);
        samplerInfo.addressModeV = ToVkSamplerAddressMode(desc.addressV);
        samplerInfo.addressModeW = ToVkSamplerAddressMode(desc.addressW);
        samplerInfo.mipLodBias = desc.mipLodBias;
        samplerInfo.anisotropyEnable = desc.anisotropyEnable;
        samplerInfo.maxAnisotropy = desc.maxAnisotropy;
        samplerInfo.compareEnable = desc.compareEnable;
        samplerInfo.compareOp = ToVkCompareOp(desc.compareOp);
        samplerInfo.minLod = desc.minLod;
        samplerInfo.maxLod = desc.maxLod;
        samplerInfo.borderColor = ToVkBorderColor(desc.borderColor);

        VK_CHECK(vkCreateSampler(device->GetDevice(), &samplerInfo, nullptr, &m_sampler));

        // Set debug name for RenderDoc/validation layers
        if (desc.debugName)
        {
            m_device->SetObjectName(VK_OBJECT_TYPE_SAMPLER, reinterpret_cast<uint64>(m_sampler), desc.debugName);
        }
    }

    VulkanSampler::~VulkanSampler()
    {
        if (m_sampler)
        {
            vkDestroySampler(m_device->GetDevice(), m_sampler, nullptr);
        }
    }

    // =============================================================================
    // Vulkan Shader
    // =============================================================================
    VulkanShader::VulkanShader(VulkanDevice* device, const RHIShaderDesc& desc)
        : RHIShader(desc)
        , m_device(device)
        , m_stage(desc.stage)
        , m_entryPoint(desc.entryPoint ? desc.entryPoint : "main")
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        // Copy bytecode
        m_bytecode.resize(desc.bytecodeSize);
        std::memcpy(m_bytecode.data(), desc.bytecode, desc.bytecodeSize);

        VkShaderModuleCreateInfo createInfo = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        createInfo.codeSize = desc.bytecodeSize;
        createInfo.pCode = reinterpret_cast<const uint32*>(desc.bytecode);

        VK_CHECK(vkCreateShaderModule(device->GetDevice(), &createInfo, nullptr, &m_shaderModule));

        // Set debug name for RenderDoc/validation layers
        if (desc.debugName)
        {
            m_device->SetObjectName(VK_OBJECT_TYPE_SHADER_MODULE, reinterpret_cast<uint64>(m_shaderModule), desc.debugName);
        }
    }

    VulkanShader::~VulkanShader()
    {
        if (m_shaderModule)
        {
            vkDestroyShaderModule(m_device->GetDevice(), m_shaderModule, nullptr);
        }
    }

    // =============================================================================
    // Vulkan Fence (Timeline Semaphore)
    // =============================================================================
    VulkanFence::VulkanFence(VulkanDevice* device, uint64 initialValue)
        : m_device(device)
        , m_nextSignalValue(initialValue + 1)
    {
        VkSemaphoreTypeCreateInfo timelineInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        timelineInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        timelineInfo.initialValue = initialValue;

        VkSemaphoreCreateInfo semaphoreInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        semaphoreInfo.pNext = &timelineInfo;

        VK_CHECK(vkCreateSemaphore(device->GetDevice(), &semaphoreInfo, nullptr, &m_semaphore));
    }

    VulkanFence::~VulkanFence()
    {
        if (m_semaphore)
        {
            vkDestroySemaphore(m_device->GetDevice(), m_semaphore, nullptr);
        }
    }

    uint64 VulkanFence::GetCompletedValue() const
    {
        uint64 value = 0;
        const VkResult result = vkGetSemaphoreCounterValue(
            m_device->GetDevice(), m_semaphore, &value);
        if (result != VK_SUCCESS)
        {
            m_device->ReportRuntimeFailure(
                result,
                RHIDeviceFaultOperation::FencePoll,
                "Vulkan timeline semaphore poll failed");
            return 0;
        }
        return value;
    }

    void VulkanFence::Signal(uint64 value)
    {
        TrackSubmittedValue(value);

        VkSemaphoreSignalInfo signalInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
        signalInfo.semaphore = m_semaphore;
        signalInfo.value = value;
        const VkResult result =
            vkSignalSemaphore(m_device->GetDevice(), &signalInfo);
        if (result != VK_SUCCESS)
        {
            m_device->ReportRuntimeFailure(
                result,
                RHIDeviceFaultOperation::CommandSubmission,
                "Vulkan host timeline signal failed");
        }
    }

    void VulkanFence::SignalOnQueue(uint64 value, RHICommandQueueType queueType)
    {
        TrackSubmittedValue(value);

        // Get the queue for the specified type
        VkQueue queue = nullptr;
        switch (queueType)
        {
            case RHICommandQueueType::Graphics: queue = m_device->GetGraphicsQueue(); break;
            case RHICommandQueueType::Compute:  queue = m_device->GetComputeQueue(); break;
            case RHICommandQueueType::Copy:     queue = m_device->GetTransferQueue(); break;
            default: queue = m_device->GetGraphicsQueue(); break;
        }

        // Submit a signal operation on the queue
        VkTimelineSemaphoreSubmitInfo timelineInfo = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        timelineInfo.signalSemaphoreValueCount = 1;
        timelineInfo.pSignalSemaphoreValues = &value;

        VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submitInfo.pNext = &timelineInfo;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &m_semaphore;

        std::lock_guard<std::mutex> lock(m_device->GetGraphicsQueueMutex());
        const VkResult result =
            vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
        if (result != VK_SUCCESS)
        {
            m_device->ReportRuntimeFailure(
                result,
                RHIDeviceFaultOperation::CommandSubmission,
                "Vulkan queue timeline signal failed");
        }
    }

    uint64 VulkanFence::AllocateSignalValue()
    {
        return m_nextSignalValue.fetch_add(1, std::memory_order_relaxed);
    }

    void VulkanFence::TrackSubmittedValue(uint64 value)
    {
        uint64 expected = m_nextSignalValue.load(std::memory_order_relaxed);
        while (expected <= value &&
               !m_nextSignalValue.compare_exchange_weak(expected, value + 1,
                                                        std::memory_order_relaxed,
                                                        std::memory_order_relaxed))
        {
        }
    }

    void VulkanFence::Wait(uint64 value, uint64 timeoutNs)
    {
        VkSemaphoreWaitInfo waitInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &m_semaphore;
        waitInfo.pValues = &value;
        const VkResult result = vkWaitSemaphores(
            m_device->GetDevice(), &waitInfo, timeoutNs);
        if (result != VK_SUCCESS && result != VK_TIMEOUT)
        {
            m_device->ReportRuntimeFailure(
                result,
                RHIDeviceFaultOperation::FenceWait,
                "Vulkan timeline semaphore wait failed");
        }
    }

    // =============================================================================
    // Factory Functions
    // =============================================================================
    RHIBufferRef CreateVulkanBuffer(VulkanDevice* device, const RHIBufferDesc& desc)
    {
        if (!device ||
            (RequiresVulkanBufferDeviceAddress(desc.usage) &&
             !device->IsBufferDeviceAddressEnabled()))
        {
            RVX_RHI_ERROR(
                "Vulkan buffer '{}' requests unavailable device-address usage",
                desc.debugName ? desc.debugName : "<unnamed>");
            return nullptr;
        }
        return Ref<VulkanBuffer>(new VulkanBuffer(device, desc));
    }

    RHITextureRef CreateVulkanTexture(VulkanDevice* device, const RHITextureDesc& desc)
    {
        return Ref<VulkanTexture>(new VulkanTexture(device, desc));
    }

    RHITextureViewRef CreateVulkanTextureView(VulkanDevice* device, RHITexture* texture, const RHITextureViewDesc& desc)
    {
        if (!texture)
        {
            RVX_RHI_ERROR("Vulkan: Cannot create texture view from null texture");
            return nullptr;
        }
        if (!IsTextureViewTypeCompatible(texture->GetUsage(), texture->GetFormat(), desc))
        {
            RVX_RHI_ERROR("Vulkan: Cannot create {} texture view for texture usage {} format {}",
                          GetTextureViewTypeName(desc.type),
                          static_cast<uint32>(texture->GetUsage()),
                          static_cast<uint32>(desc.format == RHIFormat::Unknown ? texture->GetFormat() : desc.format));
            return nullptr;
        }

        auto view = Ref<VulkanTextureView>(new VulkanTextureView(device, texture, desc));
        if (view->GetImageView() == VK_NULL_HANDLE)
        {
            RVX_RHI_ERROR("Vulkan: Failed to create native {} texture view",
                          GetTextureViewTypeName(desc.type));
            return nullptr;
        }
        return view;
    }

    RHISamplerRef CreateVulkanSampler(VulkanDevice* device, const RHISamplerDesc& desc)
    {
        return Ref<VulkanSampler>(new VulkanSampler(device, desc));
    }

    RHIShaderRef CreateVulkanShader(VulkanDevice* device, const RHIShaderDesc& desc)
    {
        return Ref<VulkanShader>(new VulkanShader(device, desc));
    }

    RHIFenceRef CreateVulkanFence(VulkanDevice* device, uint64 initialValue)
    {
        return Ref<VulkanFence>(new VulkanFence(device, initialValue));
    }

    void WaitForVulkanFence(VulkanDevice* device, RHIFence* fence, uint64 value)
    {
        if (fence)
        {
            static_cast<VulkanFence*>(fence)->Wait(value);
        }
    }

    // =============================================================================
    // Vulkan Heap Implementation (for Memory Aliasing)
    // =============================================================================
    VulkanHeap::VulkanHeap(VulkanDevice* device, const RHIHeapDesc& desc)
        : m_device(device)
        , m_size(desc.size)
        , m_type(desc.type)
        , m_flags(desc.flags)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        // Determine memory property flags based on heap type
        VkMemoryPropertyFlags requiredFlags = 0;
        VkMemoryPropertyFlags preferredFlags = 0;

        switch (desc.type)
        {
            case RHIHeapType::Default:
                requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
                break;
            case RHIHeapType::Upload:
                requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
                preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
                break;
            case RHIHeapType::Readback:
                requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
                break;
        }

        // Get physical device memory properties
        VkPhysicalDeviceMemoryProperties memProperties;
        vkGetPhysicalDeviceMemoryProperties(device->GetPhysicalDevice(), &memProperties);

        // Find a suitable memory type
        m_memoryTypeIndex = UINT32_MAX;
        for (uint32 i = 0; i < memProperties.memoryTypeCount; ++i)
        {
            VkMemoryPropertyFlags flags = memProperties.memoryTypes[i].propertyFlags;
            if ((flags & requiredFlags) == requiredFlags)
            {
                // Prefer memory types that also have preferred flags
                if (m_memoryTypeIndex == UINT32_MAX || (flags & preferredFlags) == preferredFlags)
                {
                    m_memoryTypeIndex = i;
                    if ((flags & preferredFlags) == preferredFlags)
                        break;  // Found ideal memory type
                }
            }
        }

        if (m_memoryTypeIndex == UINT32_MAX)
        {
            RVX_RHI_ERROR("Failed to find suitable memory type for Vulkan Heap");
            return;
        }
        m_memoryProperties =
            memProperties.memoryTypes[m_memoryTypeIndex].propertyFlags;

        // Allocate memory
        VkMemoryAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocInfo.allocationSize = desc.size;
        allocInfo.memoryTypeIndex = m_memoryTypeIndex;

        // A heap has no future placed-buffer usage in its public descriptor.
        // When BDA is enabled, allocate it with the Vulkan device-address flag
        // so an explicitly device-addressed placed buffer is valid instead of
        // failing after vkCreateBuffer has already succeeded.
        VkMemoryAllocateFlagsInfo addressAllocateInfo = {
            VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        if (device->IsBufferDeviceAddressEnabled())
        {
            addressAllocateInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
            allocInfo.pNext = &addressAllocateInfo;
        }

        VkResult result = vkAllocateMemory(device->GetDevice(), &allocInfo, nullptr, &m_memory);
        if (result != VK_SUCCESS)
        {
            RVX_RHI_ERROR("Failed to allocate Vulkan Heap memory: {}", static_cast<int32>(result));
            m_memory = VK_NULL_HANDLE;
            return;
        }

        // Vulkan permits at most one active vkMapMemory mapping per
        // VkDeviceMemory allocation. Host-visible placed buffers therefore
        // share this one whole-allocation mapping and only derive subranges.
        if (IsHostVisible())
        {
            result = vkMapMemory(device->GetDevice(),
                                 m_memory,
                                 0,
                                 VK_WHOLE_SIZE,
                                 0,
                                 &m_mappedData);
            if (result != VK_SUCCESS || !m_mappedData)
            {
                RVX_RHI_ERROR("Failed to map host-visible Vulkan Heap memory: {}",
                              VkResultToString(result));
                if (result == VK_SUCCESS)
                {
                    vkUnmapMemory(device->GetDevice(), m_memory);
                }
                vkFreeMemory(device->GetDevice(), m_memory, nullptr);
                m_memory = VK_NULL_HANDLE;
                m_mappedData = nullptr;
                m_memoryProperties = 0;
                return;
            }
            m_ownsHostMapping = true;
        }

        RVX_RHI_DEBUG("Created Vulkan Heap: {} bytes, memory type {}", desc.size, m_memoryTypeIndex);
    }

    VulkanHeap::~VulkanHeap()
    {
        if (!m_device || m_device->GetDevice() == VK_NULL_HANDLE)
            return;

        if (m_ownsHostMapping && m_memory != VK_NULL_HANDLE)
        {
            vkUnmapMemory(m_device->GetDevice(), m_memory);
            m_mappedData = nullptr;
            m_ownsHostMapping = false;
        }

        if (m_memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(m_device->GetDevice(), m_memory, nullptr);
            m_memory = VK_NULL_HANDLE;
        }
    }

    bool VulkanHeap::SynchronizeMappedRange(
        const VulkanMappedMemoryRange& range,
        VulkanHostMemorySynchronization synchronization)
    {
        if (synchronization == VulkanHostMemorySynchronization::None)
            return true;

        if (!m_device ||
            m_device->GetDevice() == VK_NULL_HANDLE ||
            m_device->QueryRuntimeStatus() != RHIDeviceRuntimeStatus::Ready ||
            m_memory == VK_NULL_HANDLE ||
            !m_mappedData ||
            !range.valid ||
            range.offset >= m_size ||
            (range.size != VK_WHOLE_SIZE && range.size > m_size - range.offset))
        {
            RVX_RHI_ERROR("Cannot synchronize an unavailable Vulkan Heap mapping");
            return false;
        }

        VkMappedMemoryRange mappedRange = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        mappedRange.memory = m_memory;
        mappedRange.offset = range.offset;
        mappedRange.size = range.size;
        const VkResult result = synchronization == VulkanHostMemorySynchronization::Flush
            ? vkFlushMappedMemoryRanges(m_device->GetDevice(), 1, &mappedRange)
            : vkInvalidateMappedMemoryRanges(m_device->GetDevice(), 1, &mappedRange);
        if (result != VK_SUCCESS)
        {
            m_device->ReportRuntimeFailure(
                result,
                RHIDeviceFaultOperation::Context,
                synchronization == VulkanHostMemorySynchronization::Flush
                    ? "Vulkan Heap mapped-range flush failed"
                    : "Vulkan Heap mapped-range invalidate failed");
            RVX_RHI_ERROR("Failed to {} Vulkan Heap mapping: {}",
                          synchronization == VulkanHostMemorySynchronization::Flush
                              ? "flush"
                              : "invalidate",
                          VkResultToString(result));
            return false;
        }

        return true;
    }

    RHIHeapRef CreateVulkanHeap(VulkanDevice* device, const RHIHeapDesc& desc)
    {
        auto heap = Ref<VulkanHeap>(new VulkanHeap(device, desc));
        if (!heap->GetMemory())
        {
            return nullptr;
        }
        return heap;
    }

    // =============================================================================
    // Vulkan Placed Texture Implementation
    // =============================================================================
    RHITextureRef CreateVulkanPlacedTexture(VulkanDevice* device, RHIHeap* heap, uint64 offset, const RHITextureDesc& desc)
    {
        auto* vkHeap = static_cast<VulkanHeap*>(heap);
        if (!vkHeap || !vkHeap->GetMemory())
        {
            RVX_RHI_ERROR("Invalid heap for placed texture");
            return nullptr;
        }

        // Create image without memory allocation
        VkImageCreateInfo imageInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};

        switch (desc.dimension)
        {
            case RHITextureDimension::Texture1D:
                imageInfo.imageType = VK_IMAGE_TYPE_1D;
                break;
            case RHITextureDimension::Texture2D:
                imageInfo.imageType = VK_IMAGE_TYPE_2D;
                break;
            case RHITextureDimension::Texture3D:
                imageInfo.imageType = VK_IMAGE_TYPE_3D;
                break;
            case RHITextureDimension::TextureCube:
                imageInfo.imageType = VK_IMAGE_TYPE_2D;
                imageInfo.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
                break;
        }

        imageInfo.extent.width = desc.width;
        imageInfo.extent.height = desc.height;
        imageInfo.extent.depth = desc.depth;
        imageInfo.mipLevels = desc.mipLevels;
        imageInfo.arrayLayers = GetTexturePhysicalLayerCount(desc);
        imageInfo.format = ToVkFormat(desc.format);
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.samples = static_cast<VkSampleCountFlagBits>(desc.sampleCount);

        // Usage flags
        imageInfo.usage = 0;
        if (HasFlag(desc.usage, RHITextureUsage::ShaderResource))
            imageInfo.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
        if (HasFlag(desc.usage, RHITextureUsage::UnorderedAccess))
            imageInfo.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        if (HasFlag(desc.usage, RHITextureUsage::RenderTarget))
            imageInfo.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (HasFlag(desc.usage, RHITextureUsage::DepthStencil))
            imageInfo.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

        imageInfo.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        // For placed resources, we need the ALIAS flag if memory may be shared
        imageInfo.flags |= VK_IMAGE_CREATE_ALIAS_BIT;

        VkImage image = VK_NULL_HANDLE;
        VkResult result = vkCreateImage(device->GetDevice(), &imageInfo, nullptr, &image);
        if (result != VK_SUCCESS)
        {
            RVX_RHI_ERROR("Failed to create Vulkan placed image: {}", static_cast<int32>(result));
            return nullptr;
        }

        // Bind image to the shared memory at the specified offset
        result = vkBindImageMemory(device->GetDevice(), image, vkHeap->GetMemory(), offset);
        if (result != VK_SUCCESS)
        {
            RVX_RHI_ERROR("Failed to bind Vulkan placed image to memory: {}", static_cast<int32>(result));
            vkDestroyImage(device->GetDevice(), image, nullptr);
            return nullptr;
        }

        if (desc.debugName)
        {
            if (device->vkSetDebugUtilsObjectName)
            {
                VkDebugUtilsObjectNameInfoEXT nameInfo = {VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
                nameInfo.objectType = VK_OBJECT_TYPE_IMAGE;
                nameInfo.objectHandle = reinterpret_cast<uint64>(image);
                nameInfo.pObjectName = desc.debugName;
                device->vkSetDebugUtilsObjectName(device->GetDevice(), &nameInfo);
            }
        }

        // Use the existing constructor that accepts a pre-created image
        // Pass ownsImage=true so the VkImage is destroyed when the texture is released
        return Ref<VulkanTexture>(new VulkanTexture(device, image, desc, true));
    }

    // =============================================================================
    // Vulkan Placed Buffer Implementation
    // =============================================================================
    RHIBufferRef CreateVulkanPlacedBuffer(VulkanDevice* device, RHIHeap* heap, uint64 offset, const RHIBufferDesc& desc)
    {
        auto* vkHeap = static_cast<VulkanHeap*>(heap);
        if (!vkHeap || !vkHeap->GetMemory())
        {
            RVX_RHI_ERROR("Invalid heap for placed buffer");
            return nullptr;
        }

        if (!IsVulkanPlacedBufferHeapTypeCompatible(
                vkHeap->GetType(),
                desc.memoryType))
        {
            RVX_RHI_ERROR(
                "Placed Vulkan buffer memory type {} requires a {} heap, not {}",
                GetRHIMemoryTypeName(desc.memoryType),
                GetRHIMemoryTypeName(desc.memoryType),
                GetVulkanHeapTypeName(vkHeap->GetType()));
            return nullptr;
        }

        if (desc.memoryType != RHIMemoryType::Default &&
            !vkHeap->GetMappedData())
        {
            RVX_RHI_ERROR("Placed host-visible buffer requires a host-visible Vulkan Heap mapping");
            return nullptr;
        }

        VkBufferCreateInfo bufferInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = desc.size;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        bufferInfo.usage = ToVkBufferUsage(desc.usage);
        if (RequiresVulkanBufferDeviceAddress(desc.usage) &&
            !device->IsBufferDeviceAddressEnabled())
        {
            RVX_RHI_ERROR(
                "Vulkan placed buffer '{}' requests device-address usage without an enabled logical-device feature",
                desc.debugName ? desc.debugName : "<unnamed>");
            return nullptr;
        }

        VkBuffer buffer = VK_NULL_HANDLE;
        VkResult result = vkCreateBuffer(device->GetDevice(), &bufferInfo, nullptr, &buffer);
        if (result != VK_SUCCESS)
        {
            RVX_RHI_ERROR("Failed to create Vulkan placed buffer: {}", static_cast<int32>(result));
            return nullptr;
        }

        VkMemoryRequirements memoryRequirements = {};
        vkGetBufferMemoryRequirements(device->GetDevice(), buffer, &memoryRequirements);
        const uint32 heapMemoryTypeIndex = vkHeap->GetMemoryTypeIndex();
        const bool memoryTypeCompatible =
            heapMemoryTypeIndex < 32 &&
            (memoryRequirements.memoryTypeBits & (1u << heapMemoryTypeIndex)) != 0;
        const bool offsetAligned = memoryRequirements.alignment != 0 &&
            offset % memoryRequirements.alignment == 0;
        const bool rangeWithinHeap =
            offset <= vkHeap->GetSize() &&
            memoryRequirements.size <= vkHeap->GetSize() - offset;
        if (!memoryTypeCompatible || !offsetAligned || !rangeWithinHeap)
        {
            RVX_RHI_ERROR(
                "Placed Vulkan buffer pre-bind validation failed "
                "(memoryTypeCompatible={}, offsetAligned={}, rangeWithinHeap={})",
                memoryTypeCompatible,
                offsetAligned,
                rangeWithinHeap);
            vkDestroyBuffer(device->GetDevice(), buffer, nullptr);
            return nullptr;
        }

        // Bind buffer to the shared memory at the specified offset
        result = vkBindBufferMemory(device->GetDevice(), buffer, vkHeap->GetMemory(), offset);
        if (result != VK_SUCCESS)
        {
            RVX_RHI_ERROR("Failed to bind Vulkan placed buffer to memory: {}", static_cast<int32>(result));
            vkDestroyBuffer(device->GetDevice(), buffer, nullptr);
            return nullptr;
        }

        if (desc.debugName)
        {
            if (device->vkSetDebugUtilsObjectName)
            {
                VkDebugUtilsObjectNameInfoEXT nameInfo = {VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
                nameInfo.objectType = VK_OBJECT_TYPE_BUFFER;
                nameInfo.objectHandle = reinterpret_cast<uint64>(buffer);
                nameInfo.pObjectName = desc.debugName;
                device->vkSetDebugUtilsObjectName(device->GetDevice(), &nameInfo);
            }
        }

        // Create placed buffer using the external resource constructor:
        // - ownsBuffer = true: VkBuffer will be destroyed via vkDestroyBuffer() on release
        // - m_boundMemory (VkDeviceMemory) is owned by the Heap, NOT freed here
        // IMPORTANT: Caller must ensure Heap outlives all Placed Buffers bound to it
        return Ref<VulkanBuffer>(new VulkanBuffer(device,
                                                  buffer,
                                                  vkHeap,
                                                  offset,
                                                  desc,
                                                  true));
    }

} // namespace RVX
