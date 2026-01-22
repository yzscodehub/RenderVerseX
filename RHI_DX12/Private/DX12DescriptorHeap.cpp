#include "DX12DescriptorHeap.h"
#include <algorithm>

namespace RVX
{
    // =============================================================================
    // Static Descriptor Heap Implementation
    // =============================================================================
    void DX12StaticDescriptorHeap::Initialize(
        ID3D12Device* device,
        D3D12_DESCRIPTOR_HEAP_TYPE type,
        uint32 maxDescriptors,
        bool shaderVisible)
    {
        m_type = type;
        m_maxDescriptors = maxDescriptors;
        m_shaderVisible = shaderVisible;
        m_descriptorSize = device->GetDescriptorHandleIncrementSize(type);

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = type;
        heapDesc.NumDescriptors = maxDescriptors;
        heapDesc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        heapDesc.NodeMask = 0;

        DX12_CHECK(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_heap)));

        m_cpuStart = m_heap->GetCPUDescriptorHandleForHeapStart();
        if (shaderVisible)
        {
            m_gpuStart = m_heap->GetGPUDescriptorHandleForHeapStart();
        }

        m_allocated.resize(maxDescriptors, false);

        RVX_RHI_DEBUG("Created DX12 Descriptor Heap: type={}, count={}, shaderVisible={}",
            static_cast<int>(type), maxDescriptors, shaderVisible);
    }

    DX12DescriptorHandle DX12StaticDescriptorHeap::Allocate()
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        uint32 index = RVX_INVALID_INDEX;

        // Try to reuse from free list first
        while (!m_freeList.empty())
        {
            uint32 candidate = m_freeList.front();
            m_freeList.pop();

            if (candidate < m_maxDescriptors && !m_allocated[candidate])
            {
                index = candidate;
                break;
            }
        }

        if (index == RVX_INVALID_INDEX && m_nextFreeIndex < m_maxDescriptors)
        {
            index = m_nextFreeIndex++;
        }
        else if (index == RVX_INVALID_INDEX)
        {
            RVX_RHI_ERROR("Descriptor heap exhausted! Type: {}", static_cast<int>(m_type));
            return {};
        }

        m_allocated[index] = true;

        DX12DescriptorHandle handle;
        handle.heapIndex = index;
        handle.cpuHandle.ptr = m_cpuStart.ptr + index * m_descriptorSize;
        if (m_shaderVisible)
        {
            handle.gpuHandle.ptr = m_gpuStart.ptr + index * m_descriptorSize;
        }

        return handle;
    }

    DX12DescriptorHandle DX12StaticDescriptorHeap::AllocateRange(uint32 count)
    {
        if (count == 0)
            return {};

        if (count == 1)
            return Allocate();

        std::lock_guard<std::mutex> lock(m_mutex);

        uint32 startIndex = RVX_INVALID_INDEX;
        uint32 searchStart = 0;

        // Try a simple linear scan for a contiguous free range
        for (uint32 i = searchStart; i + count <= m_maxDescriptors; ++i)
        {
            bool available = true;
            for (uint32 j = 0; j < count; ++j)
            {
                if (m_allocated[i + j])
                {
                    available = false;
                    i += j; // Skip ahead to after the first allocated slot
                    break;
                }
            }

            if (available)
            {
                startIndex = i;
                break;
            }
        }

        if (startIndex == RVX_INVALID_INDEX)
        {
            RVX_RHI_ERROR("Descriptor heap range allocation failed! Type: {}, Count: {}",
                static_cast<int>(m_type), count);
            return {};
        }

        for (uint32 j = 0; j < count; ++j)
        {
            m_allocated[startIndex + j] = true;
        }
        m_nextFreeIndex = std::max(m_nextFreeIndex, startIndex + count);

        DX12DescriptorHandle handle;
        handle.heapIndex = startIndex;
        handle.cpuHandle.ptr = m_cpuStart.ptr + startIndex * m_descriptorSize;
        if (m_shaderVisible)
        {
            handle.gpuHandle.ptr = m_gpuStart.ptr + startIndex * m_descriptorSize;
        }

        return handle;
    }

    void DX12StaticDescriptorHeap::Free(DX12DescriptorHandle handle)
    {
        if (!handle.IsValid())
            return;

        std::lock_guard<std::mutex> lock(m_mutex);

        if (handle.heapIndex < m_maxDescriptors && m_allocated[handle.heapIndex])
        {
            m_allocated[handle.heapIndex] = false;
            m_freeList.push(handle.heapIndex);
        }
    }

    void DX12StaticDescriptorHeap::FreeRange(DX12DescriptorHandle handle, uint32 count)
    {
        if (!handle.IsValid() || count == 0)
            return;

        std::lock_guard<std::mutex> lock(m_mutex);

        for (uint32 i = 0; i < count; ++i)
        {
            uint32 index = handle.heapIndex + i;
            if (index < m_maxDescriptors && m_allocated[index])
            {
                m_allocated[index] = false;
                m_freeList.push(index);
            }
        }
    }

    // =============================================================================
    // Ring Buffer Descriptor Heap Implementation
    // =============================================================================
    void DX12RingDescriptorHeap::Initialize(
        ID3D12Device* device,
        D3D12_DESCRIPTOR_HEAP_TYPE type,
        uint32 maxDescriptors)
    {
        m_type = type;
        m_maxDescriptors = maxDescriptors;
        m_descriptorSize = device->GetDescriptorHandleIncrementSize(type);

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = type;
        heapDesc.NumDescriptors = maxDescriptors;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        heapDesc.NodeMask = 0;

        DX12_CHECK(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_heap)));

        m_cpuStart = m_heap->GetCPUDescriptorHandleForHeapStart();
        m_gpuStart = m_heap->GetGPUDescriptorHandleForHeapStart();

        RVX_RHI_DEBUG("Created DX12 Ring Descriptor Heap: type={}, count={}",
            static_cast<int>(type), maxDescriptors);
    }

    DX12DescriptorHandle DX12RingDescriptorHeap::Allocate(uint32 count)
    {
        uint32 offset = m_currentOffset.fetch_add(count, std::memory_order_relaxed);

        if (offset + count > m_maxDescriptors)
        {
            RVX_RHI_ERROR("Ring descriptor heap overflow! Requested: {}, Available: {}",
                count, m_maxDescriptors - offset);
            return {};
        }

        DX12DescriptorHandle handle;
        handle.heapIndex = offset;
        handle.cpuHandle.ptr = m_cpuStart.ptr + offset * m_descriptorSize;
        handle.gpuHandle.ptr = m_gpuStart.ptr + offset * m_descriptorSize;

        return handle;
    }

    void DX12RingDescriptorHeap::Reset()
    {
        m_currentOffset.store(0, std::memory_order_relaxed);
    }

    // =============================================================================
    // Descriptor Heap Manager Implementation
    // =============================================================================
    void DX12DescriptorHeapManager::Initialize(ID3D12Device* device)
    {
        m_device = device;

        // Initialize static heaps
        m_cbvSrvUavHeap.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            MAX_CBV_SRV_UAV_DESCRIPTORS, true);
        m_samplerHeap.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
            MAX_SAMPLER_DESCRIPTORS, true);
        m_rtvHeap.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
            MAX_RTV_DESCRIPTORS, false);
        m_dsvHeap.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
            MAX_DSV_DESCRIPTORS, false);

        // Initialize per-frame transient heaps
        for (uint32 i = 0; i < RVX_MAX_FRAME_COUNT; ++i)
        {
            m_transientHeaps[i].Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                RING_BUFFER_SIZE);
        }

        RVX_RHI_INFO("DX12 Descriptor Heap Manager initialized");
    }

    void DX12DescriptorHeapManager::Shutdown()
    {
        // ComPtr will handle cleanup
        RVX_RHI_INFO("DX12 Descriptor Heap Manager shutdown");
    }

    DX12DescriptorHandle DX12DescriptorHeapManager::AllocateTransientCbvSrvUav(uint32 count)
    {
        uint32 frameIndex = m_currentFrameIndex.load(std::memory_order_acquire);
        return m_transientHeaps[frameIndex].Allocate(count);
    }

    void DX12DescriptorHeapManager::ResetTransientHeaps()
    {
        // Atomically advance to next frame index
        uint32 newIndex = (m_currentFrameIndex.load(std::memory_order_relaxed) + 1) % RVX_MAX_FRAME_COUNT;
        m_currentFrameIndex.store(newIndex, std::memory_order_release);
        m_transientHeaps[newIndex].Reset();
    }

} // namespace RVX
