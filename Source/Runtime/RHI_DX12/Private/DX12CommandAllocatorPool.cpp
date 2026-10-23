#include "DX12CommandAllocatorPool.h"
#include "DX12Device.h"

namespace RVX
{
    void DX12CommandAllocatorPool::Initialize(DX12Device* device)
    {
        m_device = device;
        m_nextFenceValues = {};

        auto d3dDevice = m_device ? m_device->GetD3DDevice() : nullptr;
        if (d3dDevice)
        {
            for (auto& fence : m_fences)
            {
                DX12_CHECK(d3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
            }
        }

        RVX_RHI_DEBUG("Command Allocator Pool initialized");
    }

    void DX12CommandAllocatorPool::Shutdown()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        for (uint32 i = 0; i < TYPE_COUNT; ++i)
        {
            m_availableAllocators[i].clear();
            while (!m_pendingAllocators[i].empty())
            {
                m_pendingAllocators[i].pop();
            }
            m_fences[i].Reset();
            m_nextFenceValues[i] = 0;
        }

        m_device = nullptr;
        
        RVX_RHI_DEBUG("Command Allocator Pool shutdown");
    }

    ComPtr<ID3D12CommandAllocator> DX12CommandAllocatorPool::Acquire(D3D12_COMMAND_LIST_TYPE type)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        uint32 index = TypeToIndex(type);
        
        RecycleCompletedLocked(index);

        if (!m_availableAllocators[index].empty())
        {
            ComPtr<ID3D12CommandAllocator> allocator = m_availableAllocators[index].back();
            m_availableAllocators[index].pop_back();
            
            // Reset the allocator for reuse
            HRESULT hr = allocator->Reset();
            if (SUCCEEDED(hr))
            {
                return allocator;
            }
            else
            {
                RVX_RHI_WARN("Failed to reset command allocator, creating new");
                // Fall through to create new
            }
        }
        
        // Create new allocator
        auto d3dDevice = m_device->GetD3DDevice();
        ComPtr<ID3D12CommandAllocator> newAllocator;
        
        HRESULT hr = d3dDevice->CreateCommandAllocator(type, IID_PPV_ARGS(&newAllocator));
        if (FAILED(hr))
        {
            RVX_RHI_ERROR("Failed to create command allocator: 0x{:08X}", static_cast<uint32>(hr));
            return nullptr;
        }
        
        RVX_RHI_DEBUG("Created new command allocator (type: {})", static_cast<int>(type));
        return newAllocator;
    }

    void DX12CommandAllocatorPool::Release(
        ComPtr<ID3D12CommandAllocator> allocator,
        D3D12_COMMAND_LIST_TYPE type,
        ID3D12CommandQueue* queue)
    {
        if (!allocator)
            return;

        std::lock_guard<std::mutex> lock(m_mutex);

        uint32 index = TypeToIndex(type);
        if (!queue || !m_fences[index])
        {
            RVX_RHI_WARN("Command allocator released without a valid queue/fence, discarding allocator");
            return;
        }

        const uint64 fenceValue = ++m_nextFenceValues[index];
        HRESULT hr = queue->Signal(m_fences[index].Get(), fenceValue);
        if (FAILED(hr))
        {
            RVX_RHI_ERROR("Failed to signal command allocator fence: 0x{:08X}", static_cast<uint32>(hr));
            return;
        }

        m_pendingAllocators[index].push({std::move(allocator), fenceValue});
    }

    void DX12CommandAllocatorPool::Tick()
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        for (uint32 i = 0; i < TYPE_COUNT; ++i)
        {
            RecycleCompletedLocked(i);
        }
    }

    void DX12CommandAllocatorPool::RecycleCompletedLocked(uint32 index)
    {
        if (!m_fences[index])
            return;

        const uint64 completedFenceValue = m_fences[index]->GetCompletedValue();
        auto& pending = m_pendingAllocators[index];
        auto& available = m_availableAllocators[index];

        while (!pending.empty())
        {
            auto& front = pending.front();
            if (front.fenceValue <= completedFenceValue)
            {
                available.push_back(std::move(front.allocator));
                pending.pop();
            }
            else
            {
                break;
            }
        }
    }

    DX12CommandAllocatorPool::PoolStats DX12CommandAllocatorPool::GetStats() const
    {
        // Note: Not locking for stats to avoid contention
        PoolStats stats;
        
        for (uint32 i = 0; i < TYPE_COUNT; ++i)
        {
            stats.availableAllocators += static_cast<uint32>(m_availableAllocators[i].size());
            stats.pendingAllocators += static_cast<uint32>(m_pendingAllocators[i].size());
        }
        
        stats.totalAllocators = stats.availableAllocators + stats.pendingAllocators;
        return stats;
    }

} // namespace RVX
