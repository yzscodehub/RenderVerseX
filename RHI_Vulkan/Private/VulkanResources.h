#pragma once

#include "VulkanCommon.h"
#include "RHI/RHIBuffer.h"
#include "RHI/RHITexture.h"
#include "RHI/RHISampler.h"
#include "RHI/RHIShader.h"
#include "RHI/RHISynchronization.h"
#include "RHI/RHIHeap.h"
#include <atomic>
#include <limits>
#include <numeric>

namespace RVX
{
    /** @brief Whether an RHI buffer description needs Vulkan device-address memory. */
    inline bool RequiresVulkanBufferDeviceAddress(RHIBufferUsage usage)
    {
        return HasFlag(usage, RHIBufferUsage::DeviceAddress) ||
               HasFlag(usage, RHIBufferUsage::AccelerationStructureStorage) ||
               HasFlag(usage, RHIBufferUsage::AccelerationStructureInput) ||
               HasFlag(usage, RHIBufferUsage::ShaderBindingTable);
    }

    /** @brief Single Vulkan buffer-usage mapping shared by normal and placed buffers. */
    inline VkBufferUsageFlags ToVkBufferUsage(RHIBufferUsage usage)
    {
        VkBufferUsageFlags flags = 0;
        if (HasFlag(usage, RHIBufferUsage::Vertex))
            flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        if (HasFlag(usage, RHIBufferUsage::Index))
            flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        if (HasFlag(usage, RHIBufferUsage::Constant))
            flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        if (HasFlag(usage, RHIBufferUsage::ShaderResource) ||
            HasFlag(usage, RHIBufferUsage::UnorderedAccess) ||
            HasFlag(usage, RHIBufferUsage::Structured))
        {
            flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        }
        if (HasFlag(usage, RHIBufferUsage::IndirectArgs))
            flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;

        // The RHI permits generic copies for every buffer; keep the actual
        // creation and placed-resource requirement queries byte-for-byte
        // consistent with that contract.
        flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

        if (RequiresVulkanBufferDeviceAddress(usage))
            flags |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        return flags;
    }

    /** @brief Cache operation required before exposing a mapped buffer to its consumer. */
    enum class VulkanHostMemorySynchronization : uint8
    {
        None = 0,
        Flush,
        Invalidate,
    };

    /** @brief Select the required cache operation for the RHI host-memory direction. */
    inline constexpr VulkanHostMemorySynchronization
    GetVulkanHostMemorySynchronization(RHIMemoryType memoryType)
    {
        switch (memoryType)
        {
            case RHIMemoryType::Upload:
                return VulkanHostMemorySynchronization::Flush;
            case RHIMemoryType::Readback:
                return VulkanHostMemorySynchronization::Invalidate;
            case RHIMemoryType::Default:
            default:
                return VulkanHostMemorySynchronization::None;
        }
    }

    /** @brief A validated host-operation range for a manually allocated VkDeviceMemory block. */
    struct VulkanMappedMemoryRange
    {
        VkDeviceSize offset = 0;
        VkDeviceSize size = 0;
        VkDeviceSize resourceOffset = 0;
        bool valid = false;
    };

    /**
     * @brief Expand a placed-buffer range to Vulkan's mapping and cache boundaries.
     *
     * If rounding up would run beyond the allocation, VK_WHOLE_SIZE is the only
     * valid way to cover the tail of an allocation whose size is not atom aligned.
     */
    inline VulkanMappedMemoryRange MakeVulkanMappedMemoryRange(
        VkDeviceSize memoryOffset,
        VkDeviceSize resourceSize,
        VkDeviceSize allocationSize,
        VkDeviceSize nonCoherentAtomSize,
        VkDeviceSize minMemoryMapAlignment)
    {
        if (resourceSize == 0 ||
            nonCoherentAtomSize == 0 ||
            minMemoryMapAlignment == 0 ||
            memoryOffset >= allocationSize ||
            resourceSize > allocationSize - memoryOffset)
        {
            return {};
        }

        const VkDeviceSize alignmentGcd =
            std::gcd(nonCoherentAtomSize, minMemoryMapAlignment);
        if (nonCoherentAtomSize / alignmentGcd >
            std::numeric_limits<VkDeviceSize>::max() / minMemoryMapAlignment)
        {
            return {};
        }

        const VkDeviceSize requiredAlignment =
            (nonCoherentAtomSize / alignmentGcd) * minMemoryMapAlignment;
        const VkDeviceSize resourceEnd = memoryOffset + resourceSize;
        const VkDeviceSize alignedOffset =
            memoryOffset - (memoryOffset % requiredAlignment);
        if (resourceEnd > std::numeric_limits<VkDeviceSize>::max() -
                              (requiredAlignment - 1))
        {
            return {alignedOffset,
                    VK_WHOLE_SIZE,
                    memoryOffset - alignedOffset,
                    true};
        }

        const VkDeviceSize alignedEnd =
            ((resourceEnd + requiredAlignment - 1) / requiredAlignment) *
            requiredAlignment;
        if (alignedEnd >= allocationSize)
        {
            return {alignedOffset,
                    VK_WHOLE_SIZE,
                    memoryOffset - alignedOffset,
                    true};
        }

        return {alignedOffset,
                alignedEnd - alignedOffset,
                memoryOffset - alignedOffset,
                true};
    }

    /**
     * @brief Expand only a requested CPU write to Vulkan's non-coherent atom.
     *
     * The returned range is relative to the VkDeviceMemory allocation. It is
     * intentionally independent of minMemoryMapAlignment: the allocation is
     * already mapped, so cache maintenance must use nonCoherentAtomSize rather
     * than widening the flush to a mapping-alignment least common multiple.
     */
    inline VulkanMappedMemoryRange MakeVulkanHostWriteSynchronizationRange(
        VkDeviceSize memoryOffset,
        VkDeviceSize resourceOffset,
        VkDeviceSize resourceSize,
        VkDeviceSize allocationSize,
        VkDeviceSize nonCoherentAtomSize)
    {
        if (resourceSize == 0 ||
            nonCoherentAtomSize == 0 ||
            memoryOffset > allocationSize ||
            resourceOffset > allocationSize - memoryOffset ||
            resourceSize > allocationSize - memoryOffset - resourceOffset)
        {
            return {};
        }

        const VkDeviceSize writeOffset = memoryOffset + resourceOffset;
        const VkDeviceSize writeEnd = writeOffset + resourceSize;
        const VkDeviceSize alignedOffset =
            writeOffset - (writeOffset % nonCoherentAtomSize);
        if (writeEnd > std::numeric_limits<VkDeviceSize>::max() -
                           (nonCoherentAtomSize - 1))
        {
            return {alignedOffset,
                    VK_WHOLE_SIZE,
                    writeOffset - alignedOffset,
                    true};
        }

        const VkDeviceSize alignedEnd =
            ((writeEnd + nonCoherentAtomSize - 1) / nonCoherentAtomSize) *
            nonCoherentAtomSize;
        if (alignedEnd >= allocationSize)
        {
            return {alignedOffset,
                    VK_WHOLE_SIZE,
                    writeOffset - alignedOffset,
                    true};
        }

        return {alignedOffset,
                alignedEnd - alignedOffset,
                writeOffset - alignedOffset,
                true};
    }

    /**
     * @brief Return the finite allocation bytes covered by a Vulkan API range.
     *
     * VK_WHOLE_SIZE is an API sentinel, not a byte count suitable for public
     * diagnostics. A range beginning at a nonzero offset covers only the tail
     * of the allocation even when Vulkan must encode it with VK_WHOLE_SIZE.
     */
    inline VkDeviceSize GetVulkanMappedMemoryRangeCoveredSize(
        const VulkanMappedMemoryRange& range,
        VkDeviceSize allocationSize)
    {
        if (!range.valid || range.offset >= allocationSize)
        {
            return 0;
        }

        return range.size == VK_WHOLE_SIZE
            ? allocationSize - range.offset
            : range.size;
    }

    /** @brief Whether a mapped-memory range actually covers its full allocation. */
    inline bool DoesVulkanMappedMemoryRangeCoverWholeAllocation(
        const VulkanMappedMemoryRange& range,
        VkDeviceSize allocationSize)
    {
        return range.offset == 0 &&
               GetVulkanMappedMemoryRangeCoveredSize(range, allocationSize) ==
                   allocationSize;
    }

    class VulkanDevice;
    class VulkanHeap;

    // =============================================================================
    // Vulkan Buffer
    // =============================================================================
    class VulkanBuffer : public RHIBuffer
    {
    public:
        VulkanBuffer(VulkanDevice* device, const RHIBufferDesc& desc);
        // Compatibility wrapper for an externally owned non-heap buffer. It
        // deliberately owns neither the buffer nor its host mapping.
        VulkanBuffer(VulkanDevice* device, VkBuffer buffer, VkDeviceMemory memory,
                     uint64 memoryOffset, const RHIBufferDesc& desc, bool ownsBuffer);
        // Constructor for placed resources (buffer memory and its host mapping are
        // owned by the heap, which must outlive all placed resources).
        VulkanBuffer(VulkanDevice* device, VkBuffer buffer, VulkanHeap* heap,
                     uint64 memoryOffset, const RHIBufferDesc& desc, bool ownsBuffer);
        ~VulkanBuffer() override;

        uint64 GetSize() const override { return m_desc.size; }
        RHIBufferUsage GetUsage() const override { return m_desc.usage; }
        RHIMemoryType GetMemoryType() const override { return m_desc.memoryType; }
        uint32 GetStride() const override { return m_desc.stride; }

        void* Map() override;
        void Unmap() override;
        bool CommitMappedWrite() override;

        VkBuffer GetBuffer() const { return m_buffer; }
        VkDeviceAddress GetDeviceAddress() const { return m_deviceAddress; }

    private:
        bool IsHostAccessReady() const;
        bool IsHostCoherentMemory() const;
        bool GetPlacedMappedMemoryRange(uint64 resourceOffset,
                                        uint64 resourceSize,
                                        VulkanMappedMemoryRange& range) const;
        bool SynchronizeMappedMemory(VulkanHostMemorySynchronization synchronization,
                                     uint64 resourceOffset,
                                     uint64 resourceSize,
                                     VulkanMappedMemoryRange* synchronizedRange = nullptr,
                                     VkDeviceSize* synchronizedAllocationSize = nullptr);
        void* MapWriteRangeImpl(uint64 offset, uint64 size) override;
        RHIHostWriteReceipt CommitMappedWriteRangeImpl(
            uint64 offset, uint64 size) override;
        bool CancelMappedWriteRangeImpl(uint64 offset, uint64 size) override;
        void ReleaseTransientMapping();

        VulkanDevice* m_device = nullptr;
        RHIBufferDesc m_desc;
        VkBuffer m_buffer = VK_NULL_HANDLE;
        VmaAllocation m_allocation = VK_NULL_HANDLE;
        VkDeviceAddress m_deviceAddress = 0;
        void* m_mappedMemoryBase = nullptr;
        void* m_mappedData = nullptr;
        bool m_isPersistentMapping = false;
        bool m_isMappedForAccess = false;
        bool m_ownsBuffer = true;           // False for placed resources
        VulkanHeap* m_placedHeap = nullptr;     // Non-owning; RHI heap lifetime outlives placed buffers
        VkDeviceMemory m_boundMemory = VK_NULL_HANDLE;  // For placed resources (memory owned by heap)
        uint64 m_memoryOffset = 0;          // Offset within the heap memory
    };

    // =============================================================================
    // Vulkan Texture
    // =============================================================================
    class VulkanTexture : public RHITexture
    {
    public:
        VulkanTexture(VulkanDevice* device, const RHITextureDesc& desc);
        VulkanTexture(VulkanDevice* device, VkImage image, const RHITextureDesc& desc, bool ownsImage = false);  // For swapchain/placed
        ~VulkanTexture() override;

        uint32 GetWidth() const override { return m_desc.width; }
        uint32 GetHeight() const override { return m_desc.height; }
        uint32 GetDepth() const override { return m_desc.depth; }
        uint32 GetMipLevels() const override { return m_desc.mipLevels; }
        uint32 GetArraySize() const override { return m_desc.arraySize; }
        RHIFormat GetFormat() const override { return m_desc.format; }
        RHITextureDimension GetDimension() const override { return m_desc.dimension; }
        RHITextureUsage GetUsage() const override { return m_desc.usage; }
        RHISampleCount GetSampleCount() const override { return m_desc.sampleCount; }

        VkImage GetImage() const { return m_image; }
        VkImageLayout GetCurrentLayout() const { return m_currentLayout; }
        void SetCurrentLayout(VkImageLayout layout) { m_currentLayout = layout; }

    private:
        VulkanDevice* m_device;
        RHITextureDesc m_desc;
        VkImage m_image = VK_NULL_HANDLE;
        VmaAllocation m_allocation = VK_NULL_HANDLE;
        VkImageLayout m_currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bool m_ownsImage = true;  // True for VMA-allocated and placed resources
        bool m_ownsAllocation = true;  // False for placed resources (memory owned by heap)
    };

    // =============================================================================
    // Vulkan Texture View
    // =============================================================================
    class VulkanTextureView : public RHITextureView
    {
    public:
        VulkanTextureView(VulkanDevice* device, RHITexture* texture, const RHITextureViewDesc& desc);
        ~VulkanTextureView() override;

        RHIFormat GetFormat() const override { return m_format; }
        const RHISubresourceRange& GetSubresourceRange() const override { return m_subresourceRange; }

        VkImageView GetImageView() const { return m_imageView; }
        VulkanTexture* GetVulkanTexture() const { return m_texture; }

    private:
        VulkanDevice* m_device;
        VulkanTexture* m_texture;
        VkImageView m_imageView = VK_NULL_HANDLE;
        RHIFormat m_format;
        RHISubresourceRange m_subresourceRange;
    };

    // =============================================================================
    // Vulkan Sampler
    // =============================================================================
    class VulkanSampler : public RHISampler
    {
    public:
        VulkanSampler(VulkanDevice* device, const RHISamplerDesc& desc);
        ~VulkanSampler() override;

        VkSampler GetSampler() const { return m_sampler; }

    private:
        VulkanDevice* m_device;
        VkSampler m_sampler = VK_NULL_HANDLE;
    };

    // =============================================================================
    // Vulkan Shader
    // =============================================================================
    class VulkanShader : public RHIShader
    {
    public:
        VulkanShader(VulkanDevice* device, const RHIShaderDesc& desc);
        ~VulkanShader() override;

        RHIShaderStage GetStage() const override { return m_stage; }
        const std::vector<uint8>& GetBytecode() const override { return m_bytecode; }

        VkShaderModule GetShaderModule() const { return m_shaderModule; }
        const char* GetEntryPoint() const { return m_entryPoint.c_str(); }

    private:
        VulkanDevice* m_device;
        RHIShaderStage m_stage;
        std::vector<uint8> m_bytecode;
        std::string m_entryPoint;
        VkShaderModule m_shaderModule = VK_NULL_HANDLE;
    };

    // =============================================================================
    // Vulkan Fence (Timeline Semaphore)
    // =============================================================================
    class VulkanFence : public RHIFence
    {
    public:
        VulkanFence(VulkanDevice* device, uint64 initialValue);
        ~VulkanFence() override;

        uint64 GetCompletedValue() const override;
        void Signal(uint64 value) override;
        void SignalOnQueue(uint64 value, RHICommandQueueType queueType) override;
        void Wait(uint64 value, uint64 timeoutNs = UINT64_MAX) override;

        VkSemaphore GetSemaphore() const { return m_semaphore; }
        uint64 AllocateSignalValue();

    private:
        void TrackSubmittedValue(uint64 value);

        VulkanDevice* m_device;
        VkSemaphore m_semaphore = VK_NULL_HANDLE;
        std::atomic<uint64> m_nextSignalValue{1};
    };

    // =============================================================================
    // Vulkan Heap (for Memory Aliasing / Placed Resources)
    // =============================================================================
    class VulkanHeap : public RHIHeap
    {
    public:
        VulkanHeap(VulkanDevice* device, const RHIHeapDesc& desc);
        ~VulkanHeap() override;

        // RHIHeap interface
        uint64 GetSize() const override { return m_size; }
        RHIHeapType GetType() const override { return m_type; }
        RHIHeapFlags GetFlags() const override { return m_flags; }

        // Vulkan Specific
        VkDeviceMemory GetMemory() const { return m_memory; }
        uint32 GetMemoryTypeIndex() const { return m_memoryTypeIndex; }
        void* GetMappedData() const { return m_mappedData; }
        bool IsHostVisible() const { return m_type != RHIHeapType::Default; }
        bool IsHostCoherent() const
        {
            return (m_memoryProperties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        }
        bool SynchronizeMappedRange(const VulkanMappedMemoryRange& range,
                                    VulkanHostMemorySynchronization synchronization);

    private:
        VulkanDevice* m_device = nullptr;
        VkDeviceMemory m_memory = VK_NULL_HANDLE;
        uint64 m_size = 0;
        RHIHeapType m_type = RHIHeapType::Default;
        RHIHeapFlags m_flags = RHIHeapFlags::AllowAll;
        uint32 m_memoryTypeIndex = 0;
        VkMemoryPropertyFlags m_memoryProperties = 0;
        void* m_mappedData = nullptr;
        bool m_ownsHostMapping = false;
    };

    // =============================================================================
    // Factory Functions
    // =============================================================================
    RHIBufferRef CreateVulkanBuffer(VulkanDevice* device, const RHIBufferDesc& desc);
    RHITextureRef CreateVulkanTexture(VulkanDevice* device, const RHITextureDesc& desc);
    RHITextureViewRef CreateVulkanTextureView(VulkanDevice* device, RHITexture* texture, const RHITextureViewDesc& desc);
    RHISamplerRef CreateVulkanSampler(VulkanDevice* device, const RHISamplerDesc& desc);
    RHIShaderRef CreateVulkanShader(VulkanDevice* device, const RHIShaderDesc& desc);
    RHIFenceRef CreateVulkanFence(VulkanDevice* device, uint64 initialValue);
    void WaitForVulkanFence(VulkanDevice* device, RHIFence* fence, uint64 value);

    // Heap functions (for Memory Aliasing)
    RHIHeapRef CreateVulkanHeap(VulkanDevice* device, const RHIHeapDesc& desc);
    RHITextureRef CreateVulkanPlacedTexture(VulkanDevice* device, RHIHeap* heap, uint64 offset, const RHITextureDesc& desc);
    RHIBufferRef CreateVulkanPlacedBuffer(VulkanDevice* device, RHIHeap* heap, uint64 offset, const RHIBufferDesc& desc);

} // namespace RVX
