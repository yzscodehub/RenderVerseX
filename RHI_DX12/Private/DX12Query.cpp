#include "DX12Query.h"
#include "DX12Device.h"

namespace RVX
{
    DX12QueryPool::DX12QueryPool(DX12Device* device, const RHIQueryPoolDesc& desc)
        : m_device(device)
        , m_type(desc.type)
        , m_count(desc.count)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        auto d3dDevice = device->GetD3DDevice();

        // Convert RHI query type to D3D12 query type
        D3D12_QUERY_HEAP_TYPE heapType;
        switch (desc.type)
        {
            case RHIQueryType::Timestamp:
                m_d3dQueryType = D3D12_QUERY_TYPE_TIMESTAMP;
                heapType = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
                break;
            case RHIQueryType::Occlusion:
                m_d3dQueryType = D3D12_QUERY_TYPE_OCCLUSION;
                heapType = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
                break;
            case RHIQueryType::BinaryOcclusion:
                m_d3dQueryType = D3D12_QUERY_TYPE_BINARY_OCCLUSION;
                heapType = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
                break;
            case RHIQueryType::PipelineStatistics:
                m_d3dQueryType = D3D12_QUERY_TYPE_PIPELINE_STATISTICS;
                heapType = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
                break;
            default:
                RVX_RHI_ERROR("Unknown query type: {}", static_cast<int>(desc.type));
                return;
        }

        // Create query heap
        D3D12_QUERY_HEAP_DESC heapDesc = {};
        heapDesc.Type = heapType;
        heapDesc.Count = desc.count;
        heapDesc.NodeMask = 0;

        HRESULT hr = d3dDevice->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&m_heap));
        if (FAILED(hr))
        {
            RVX_RHI_ERROR("Failed to create query heap: 0x{:08X}", static_cast<uint32>(hr));
            return;
        }

        if (desc.debugName)
        {
            wchar_t wname[256];
            MultiByteToWideChar(CP_UTF8, 0, desc.debugName, -1, wname, 256);
            m_heap->SetName(wname);
        }

        // Get timestamp frequency for timestamp queries
        if (desc.type == RHIQueryType::Timestamp)
        {
            device->GetGraphicsQueue()->GetTimestampFrequency(&m_timestampFrequency);
        }

        RVX_RHI_DEBUG("Created query pool: type={}, count={}", static_cast<int>(desc.type), desc.count);
    }

    DX12QueryPool::~DX12QueryPool()
    {
        // ComPtr will release the heap
    }

    RHIQueryPoolRef CreateDX12QueryPool(DX12Device* device, const RHIQueryPoolDesc& desc)
    {
        auto pool = Ref<DX12QueryPool>(new DX12QueryPool(device, desc));
        if (!pool->GetHeap())
        {
            return nullptr;
        }
        return pool;
    }

} // namespace RVX
