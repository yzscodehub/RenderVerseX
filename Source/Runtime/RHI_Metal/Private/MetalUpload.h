#pragma once

#include "MetalCommon.h"
#include "RHI/RHIUpload.h"
#include <mutex>
#include <vector>

namespace RVX
{
    class MetalDevice;

    // =============================================================================
    // Metal Staging Buffer
    // Uses shared storage mode buffer for efficient CPU->GPU transfers
    // =============================================================================
    class MetalStagingBuffer : public RHIStagingBuffer
    {
    public:
        MetalStagingBuffer(id<MTLDevice> device, const RHIStagingBufferDesc& desc);
        ~MetalStagingBuffer() override;

        // RHIStagingBuffer interface
        void* Map(uint64 offset = 0, uint64 size = RVX_WHOLE_SIZE) override;
        void Unmap() override;
        uint64 GetSize() const override { return m_size; }
        RHIBuffer* GetBuffer() const override;

        // Metal specific
        id<MTLBuffer> GetMTLBuffer() const { return m_buffer; }

    private:
        id<MTLDevice> m_device = nil;
        id<MTLBuffer> m_buffer = nil;
        uint64 m_size = 0;
        void* m_mappedPtr = nullptr;
        bool m_isMapped = false;

        // We need a wrapper RHIBuffer for GetBuffer()
        mutable RHIBufferRef m_wrapperBuffer;
    };

    // =============================================================================
    // Metal Ring Buffer
    // Uses triple-buffered shared storage for per-frame allocations
    // =============================================================================
    class MetalRingBuffer : public RHIRingBuffer
    {
    public:
        MetalRingBuffer(id<MTLDevice> device, const RHIRingBufferDesc& desc);
        ~MetalRingBuffer() override;

        // RHIRingBuffer interface
        RHIRingAllocation Allocate(uint64 size) override;
        void Reset(uint32 frameIndex) override;
        RHIBuffer* GetBuffer() const override;
        uint64 GetSize() const override { return m_totalSize; }
        uint32 GetAlignment() const override { return m_alignment; }

        // Metal specific
        id<MTLBuffer> GetMTLBuffer() const { return m_buffer; }

    private:
        id<MTLDevice> m_device = nil;
        id<MTLBuffer> m_buffer = nil;
        
        uint64 m_totalSize = 0;
        uint32 m_alignment = 256;
        
        // Current allocation state
        uint64 m_currentOffset = 0;
        void* m_mappedPtr = nullptr;

        // Triple buffering - track allocations per frame
        static constexpr uint32 MAX_FRAMES_IN_FLIGHT = 3;
        struct FrameAllocation
        {
            uint64 startOffset = 0;
            uint64 endOffset = 0;
        };
        std::array<FrameAllocation, MAX_FRAMES_IN_FLIGHT> m_frameAllocations;
        uint32 m_currentFrameIndex = 0;

        std::mutex m_mutex;

        // Wrapper RHIBuffer for GetBuffer()
        mutable RHIBufferRef m_wrapperBuffer;
    };

} // namespace RVX
