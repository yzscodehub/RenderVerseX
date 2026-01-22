#pragma once

#include "DX12Common.h"
#include "RHI/RHIQuery.h"

namespace RVX
{
    class DX12Device;

    // =============================================================================
    // DX12 Query Pool
    // =============================================================================
    class DX12QueryPool : public RHIQueryPool
    {
    public:
        DX12QueryPool(DX12Device* device, const RHIQueryPoolDesc& desc);
        ~DX12QueryPool() override;

        // RHIQueryPool interface
        RHIQueryType GetType() const override { return m_type; }
        uint32 GetCount() const override { return m_count; }
        uint64 GetTimestampFrequency() const override { return m_timestampFrequency; }

        // DX12 specific
        ID3D12QueryHeap* GetHeap() const { return m_heap.Get(); }
        D3D12_QUERY_TYPE GetD3D12QueryType() const { return m_d3dQueryType; }

    private:
        DX12Device* m_device = nullptr;
        ComPtr<ID3D12QueryHeap> m_heap;
        RHIQueryType m_type;
        D3D12_QUERY_TYPE m_d3dQueryType;
        uint32 m_count = 0;
        uint64 m_timestampFrequency = 0;
    };

    // Factory function
    RHIQueryPoolRef CreateDX12QueryPool(DX12Device* device, const RHIQueryPoolDesc& desc);

} // namespace RVX
