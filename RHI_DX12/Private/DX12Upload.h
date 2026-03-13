#pragma once

/**
 * @file DX12Upload.h
 * @brief DX12 upload buffer implementations (StagingBuffer, RingBuffer)
 */

#include "DX12Common.h"
#include "DX12Resources.h"
#include "RHI/RHIUpload.h"

#include <atomic>
#include <mutex>
#include <array>

namespace RVX
{
    class DX12Device;

    // =============================================================================
    // DX12 Staging Buffer - Used for CPU->GPU data transfer
    // Uses D3D12_HEAP_TYPE_UPLOAD for CPU-visible, GPU-readable memory
    // =============================================================================
    class DX12StagingBuffer : public RHIStagingBuffer
    {
    public:
        DX12StagingBuffer(DX12Device* device, const RHIStagingBufferDesc& desc);
        ~DX12StagingBuffer() override;

        // =========================================================================
        // RHIStagingBuffer Interface
        // =========================================================================
        void* Map(uint64 offset = 0, uint64 size = RVX_WHOLE_SIZE) override;
        void Unmap() override;
        uint64 GetSize() const override { return m_size; }
        RHIBuffer* GetBuffer() const override;

        // =========================================================================
        // DX12 Specific
        // =========================================================================
        ID3D12Resource* GetResource() const { return m_resource.Get(); }

    private:
        DX12Device* m_device = nullptr;
        ComPtr<ID3D12Resource> m_resource;
        uint64 m_size = 0;
        void* m_mappedData = nullptr;
        bool m_isMapped = false;

        // Wrapper buffer for GetBuffer() interface
        mutable RHIBufferRef m_wrapperBuffer;
    };

    // =============================================================================
    // DX12 Ring Buffer - Used for per-frame temporary data (Constant Buffer, etc.)
    // Uses triple-buffered upload heap for efficient per-frame allocations
    // =============================================================================
    class DX12RingBuffer : public RHIRingBuffer
    {
    public:
        DX12RingBuffer(DX12Device* device, const RHIRingBufferDesc& desc);
        ~DX12RingBuffer() override;

        // =========================================================================
        // RHIRingBuffer Interface
        // =========================================================================
        RHIRingAllocation Allocate(uint64 size) override;
        void Reset(uint32 frameIndex) override;
        RHIBuffer* GetBuffer() const override;
        uint64 GetSize() const override { return m_totalSize; }
        uint32 GetAlignment() const override { return m_alignment; }

        // =========================================================================
        // DX12 Specific
        // =========================================================================
        ID3D12Resource* GetResource() const { return m_resource.Get(); }

    private:
        DX12Device* m_device = nullptr;
        ComPtr<ID3D12Resource> m_resource;

        uint64 m_totalSize = 0;
        uint32 m_alignment = 256;

        // Ring buffer state
        std::atomic<uint64> m_head = 0;
        std::atomic<uint64> m_tail = 0;
        void* m_mappedData = nullptr;

        // Triple buffering - track allocations per frame
        static constexpr uint32 MAX_FRAMES_IN_FLIGHT = 3;
        std::array<uint64, MAX_FRAMES_IN_FLIGHT> m_frameFenceValues = {};

        std::mutex m_mutex;

        // Wrapper buffer for GetBuffer() interface
        mutable RHIBufferRef m_wrapperBuffer;
    };

    // =============================================================================
    // Factory Functions
    // =============================================================================
    RHIStagingBufferRef CreateDX12StagingBuffer(DX12Device* device, const RHIStagingBufferDesc& desc);
    RHIRingBufferRef CreateDX12RingBuffer(DX12Device* device, const RHIRingBufferDesc& desc);

} // namespace RVX
