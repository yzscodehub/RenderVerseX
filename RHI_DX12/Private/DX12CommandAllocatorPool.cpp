#include "DX12CommandAllocatorPool.h"
#include "DX12Device.h"

namespace RVX
{
    void DX12CommandAllocatorPool::Initialize(DX12Device* device)
    {
        m_device = device;
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
        }
        
        RVX_RHI_DEBUG("Command Allocator Pool shutdown");
    }

    ComPtr<ID3D12CommandAllocator> DX12CommandAllocatorPool::Acquire(D3D12_COMMAND_LIST_TYPE type)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        uint32 index = TypeToIndex(type);
        
        // Check if there's an available allocator
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
        uint64 fenceValue)
    {
        if (!allocator)
            return;
        
        std::lock_guard<std::mutex> lock(m_mutex);
        
        uint32 index = TypeToIndex(type);
        m_pendingAllocators[index].push({std::move(allocator), fenceValue});
    }

    void DX12CommandAllocatorPool::Tick(uint64 completedFenceValue)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        for (uint32 i = 0; i < TYPE_COUNT; ++i)
        {
            auto& pending = m_pendingAllocators[i];
            auto& available = m_availableAllocators[i];
            
            // Move completed allocators to available pool
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
                    // Pending queue is ordered, so we can stop here
                    break;
                }
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
