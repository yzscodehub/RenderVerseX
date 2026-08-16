#include "DX12Query.h"
#include "DX12Device.h"

namespace RVX
{
    DX12QueryPool::DX12QueryPool(DX12Device* device, const RHIQueryPoolDesc& desc)
        : RHIQueryPool(
            desc.queueType,
            desc.type == RHIQueryType::Timestamp ? 64 : 0)
        , m_device(device)
        , m_type(desc.type)
        , m_count(desc.count)
    {
        const RHIQueryValidationResult validation = ValidateRHIQueryPoolDesc(desc);
        if (!validation)
        {
            RVX_RHI_ERROR("DX12: Invalid query pool description: {}", validation.message);
            return;
        }

        if (device == nullptr || device->GetD3DDevice() == nullptr)
        {
            RVX_RHI_ERROR("DX12: Query pool creation requires a live DX12 device");
            return;
        }

        if (desc.type == RHIQueryType::Timestamp)
        {
            const RHICapabilities& capabilities = device->GetCapabilities();
            if (!capabilities.supportsTimestampQueries ||
                capabilities.timestampFrequency == 0 ||
                desc.queueType != RHICommandQueueType::Graphics)
            {
                RVX_RHI_ERROR(
                    "DX12: Timestamp query pools require verified Graphics timestamp support");
                return;
            }

            // The device verified this exact Graphics-queue frequency before
            // publishing the capability.  Do not make a second unchecked API
            // call here, which could silently diverge from the declaration.
            m_timestampFrequency = capabilities.timestampFrequency;
        }

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

        RVX_RHI_DEBUG("Created query pool: type={}, count={}", static_cast<int>(desc.type), desc.count);
    }

    DX12QueryPool::~DX12QueryPool()
    {
        // ComPtr will release the heap
    }

    RHIQueryPoolRef CreateDX12QueryPool(DX12Device* device, const RHIQueryPoolDesc& desc)
    {
        const RHIQueryValidationResult validation = ValidateRHIQueryPoolDesc(desc);
        if (!validation)
        {
            RVX_RHI_ERROR("DX12: Query pool creation rejected: {}", validation.message);
            return nullptr;
        }

        if (device == nullptr)
        {
            RVX_RHI_ERROR("DX12: Query pool creation requires a device");
            return nullptr;
        }

        if (desc.type == RHIQueryType::Timestamp)
        {
            const RHICapabilities& capabilities = device->GetCapabilities();
            if (!capabilities.supportsTimestampQueries ||
                capabilities.timestampFrequency == 0)
            {
                RVX_RHI_ERROR("DX12: Timestamp queries are unavailable on the Graphics queue");
                return nullptr;
            }
        }

        auto pool = Ref<DX12QueryPool>(new DX12QueryPool(device, desc));
        if (!pool->GetHeap())
        {
            return nullptr;
        }
        return pool;
    }

} // namespace RVX
