#pragma once

#include "VulkanCommon.h"
#include "VulkanDevice.h"
#include "RHI/RHIUpload.h"
#include <atomic>
#include <mutex>
#include <array>

namespace RVX
{
    // =============================================================================
    // Vulkan Staging Buffer
    // Uses HOST_VISIBLE memory for efficient CPU->GPU transfers
    // =============================================================================
    class VulkanStagingBuffer : public RHIStagingBuffer
    {
    public:
        VulkanStagingBuffer(VulkanDevice* device, const RHIStagingBufferDesc& desc);
        ~VulkanStagingBuffer() override;

        // RHIStagingBuffer interface
        void* Map(uint64 offset = 0, uint64 size = RVX_WHOLE_SIZE) override;
        void Unmap() override;
        bool CommitMappedWrite() override;
        uint64 GetSize() const override { return m_size; }
        RHIBuffer* GetBuffer() const override;

        [[nodiscard]] bool IsValid() const
        {
            return m_device != nullptr && m_device->GetDevice() != VK_NULL_HANDLE &&
                   m_device->QueryRuntimeStatus() == RHIDeviceRuntimeStatus::Ready &&
                   m_buffer != VK_NULL_HANDLE &&
                   m_memory != VK_NULL_HANDLE &&
                   (m_memoryProperties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 &&
                   (m_memoryProperties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        }

        // Vulkan specific
        VkBuffer GetVKBuffer() const { return m_buffer; }

    private:
        void DiscardMappedState() noexcept;
        RHIHostWriteReceipt CommitMappedWriteRangeImpl(
            uint64 offset, uint64 size) override;
        bool CancelMappedWriteRangeImpl(uint64 offset, uint64 size) override;

        VulkanDevice* m_device = nullptr;
        VkBuffer m_buffer = VK_NULL_HANDLE;
        VkDeviceMemory m_memory = VK_NULL_HANDLE;
        uint64 m_size = 0;
        void* m_mappedData = nullptr;
        bool m_isMapped = false;
        VkMemoryPropertyFlags m_memoryProperties = 0;
        mutable RHIBufferRef m_wrapperBuffer;
    };

    // =============================================================================
    // Vulkan Ring Buffer
    // Uses HOST_VISIBLE memory with triple-buffering for per-frame allocations
    // =============================================================================
    class VulkanRingBuffer : public RHIRingBuffer
    {
    public:
        VulkanRingBuffer(VulkanDevice* device, const RHIRingBufferDesc& desc);
        ~VulkanRingBuffer() override;

        // RHIRingBuffer interface
        RHIRingAllocation Allocate(uint64 size) override;
        void Reset(uint32 frameIndex) override;
        RHIBuffer* GetBuffer() const override;
        uint64 GetSize() const override { return m_totalSize; }
        uint32 GetAlignment() const override { return m_alignment; }

        // Vulkan specific
        VkBuffer GetVKBuffer() const { return m_buffer; }

    private:
        VulkanDevice* m_device = nullptr;
        VkBuffer m_buffer = VK_NULL_HANDLE;
        VkDeviceMemory m_memory = VK_NULL_HANDLE;

        uint64 m_totalSize = 0;
        uint32 m_alignment = 256;

        std::atomic<uint64> m_head = 0;
        std::atomic<uint64> m_tail = 0;
        void* m_mappedData = nullptr;

        static constexpr uint32 MAX_FRAMES_IN_FLIGHT = 3;
        std::array<uint64, MAX_FRAMES_IN_FLIGHT> m_frameFenceValues = {};
        std::mutex m_mutex;

        mutable RHIBufferRef m_wrapperBuffer;
    };

    // =============================================================================
    // Factory Functions
    // =============================================================================
    RHIStagingBufferRef CreateVulkanStagingBuffer(VulkanDevice* device, const RHIStagingBufferDesc& desc);
    RHIRingBufferRef CreateVulkanRingBuffer(VulkanDevice* device, const RHIRingBufferDesc& desc);

} // namespace RVX
