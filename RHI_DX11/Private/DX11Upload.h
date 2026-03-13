#pragma once

#include "DX11Device.h"
#include "RHI/RHIUpload.h"
#include <atomic>
#include <mutex>
#include <array>

namespace RVX
{
    // =============================================================================
    // DX11 Staging Buffer
    // Uses D3D11_USAGE_STAGING for CPU->GPU transfers
    // =============================================================================
    class DX11StagingBuffer : public RHIStagingBuffer
    {
    public:
        DX11StagingBuffer(DX11Device* device, const RHIStagingBufferDesc& desc);
        ~DX11StagingBuffer() override;

        // RHIStagingBuffer interface
        void* Map(uint64 offset = 0, uint64 size = RVX_WHOLE_SIZE) override;
        void Unmap() override;
        uint64 GetSize() const override { return m_size; }
        RHIBuffer* GetBuffer() const override;

        // DX11 specific
        ID3D11Buffer* GetDXBuffer() const { return m_buffer.Get(); }

    private:
        DX11Device* m_device = nullptr;
        ComPtr<ID3D11Buffer> m_buffer;
        uint64 m_size = 0;
        void* m_mappedData = nullptr;
        bool m_isMapped = false;
        mutable RHIBufferRef m_wrapperBuffer;
    };

    // =============================================================================
    // DX11 Ring Buffer
    // Uses double-buffering strategy
    // =============================================================================
    class DX11RingBuffer : public RHIRingBuffer
    {
    public:
        DX11RingBuffer(DX11Device* device, const RHIRingBufferDesc& desc);
        ~DX11RingBuffer() override;

        // RHIRingBuffer interface
        RHIRingAllocation Allocate(uint64 size) override;
        void Reset(uint32 frameIndex) override;
        RHIBuffer* GetBuffer() const override;
        uint64 GetSize() const override { return m_totalSize; }
        uint32 GetAlignment() const override { return m_alignment; }

        // DX11 specific
        ID3D11Buffer* GetDXBuffer() const { return m_buffers[m_currentBuffer].Get(); }

    private:
        DX11Device* m_device = nullptr;
        std::array<ComPtr<ID3D11Buffer>, 2> m_buffers;

        uint64 m_totalSize = 0;
        uint32 m_alignment = 256;
        uint32 m_currentBuffer = 0;

        uint64 m_currentOffset = 0;
        void* m_mappedData = nullptr;

        std::mutex m_mutex;
        mutable RHIBufferRef m_wrapperBuffer;
    };

    // =============================================================================
    // Factory Functions
    // =============================================================================
    RHIStagingBufferRef CreateDX11StagingBuffer(DX11Device* device, const RHIStagingBufferDesc& desc);
    RHIRingBufferRef CreateDX11RingBuffer(DX11Device* device, const RHIRingBufferDesc& desc);

} // namespace RVX
