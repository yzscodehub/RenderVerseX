#include "DX12DescriptorHeap.h"

#include <algorithm>
#include <limits>

namespace RVX
{
    namespace
    {
        std::atomic<uint64> g_nextDescriptorAllocatorIdentity{1};

        uint64 AllocateDescriptorAllocatorIdentity()
        {
            uint64 identity = g_nextDescriptorAllocatorIdentity.fetch_add(1, std::memory_order_relaxed);
            if (identity == 0)
            {
                identity = g_nextDescriptorAllocatorIdentity.fetch_add(1, std::memory_order_relaxed);
            }
            return identity;
        }

        uint32 AdvanceGeneration(uint32 generation)
        {
            ++generation;
            return generation == 0 ? 1 : generation;
        }
    } // namespace

    // =========================================================================
    // Paged CPU Descriptor Allocator
    // =========================================================================
    struct DX12PagedDescriptorAllocator::Page
    {
        ComPtr<ID3D12DescriptorHeap> heap;
        D3D12_CPU_DESCRIPTOR_HANDLE cpuStart = {};
        std::vector<uint8> allocated;
        std::vector<uint32> generations;
        std::vector<uint32> freeSlots;
        uint32 nextFreeSlot = 0;

        bool HasCapacity(uint32 pageSize) const
        {
            return !freeSlots.empty() || nextFreeSlot < pageSize;
        }
    };

    DX12PagedDescriptorAllocator::DX12PagedDescriptorAllocator() = default;

    DX12PagedDescriptorAllocator::~DX12PagedDescriptorAllocator()
    {
        Shutdown();
    }

    bool DX12PagedDescriptorAllocator::Initialize(
        ID3D12Device* device,
        D3D12_DESCRIPTOR_HEAP_TYPE type,
        uint32 pageSize,
        uint32 maxPages)
    {
        Shutdown();

        if (!device || pageSize == 0 || maxPages == 0)
        {
            RVX_RHI_ERROR(
                "Cannot initialize DX12 paged descriptor allocator: device={}, pageSize={}, maxPages={}",
                static_cast<const void*>(device),
                pageSize,
                maxPages);
            return false;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        m_device = device;
        m_type = type;
        m_descriptorSize = device->GetDescriptorHandleIncrementSize(type);
        m_pageSize = pageSize;
        m_maxPages = maxPages;
        m_allocatorIdentity = AllocateDescriptorAllocatorIdentity();
        m_peakPages = 0;
        m_activeDescriptors = 0;
        m_peakActiveDescriptors = 0;
        m_allocationFailures = 0;
        m_validationFailures = 0;
        return true;
    }

    void DX12PagedDescriptorAllocator::Shutdown()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_activeDescriptors != 0)
        {
            RVX_RHI_WARN(
                "Destroying DX12 paged descriptor allocator type={} with {} active descriptors",
                static_cast<int>(m_type),
                m_activeDescriptors);
        }

        m_pages.clear();
        m_device = nullptr;
        m_descriptorSize = 0;
        m_pageSize = 0;
        m_maxPages = 0;
        m_allocatorIdentity = 0;
        m_activeDescriptors = 0;
    }

    DX12PagedDescriptorAllocator::Page* DX12PagedDescriptorAllocator::CreatePageLocked()
    {
        if (!m_device || m_pages.size() >= m_maxPages)
        {
            ++m_allocationFailures;
            RVX_RHI_ERROR(
                "DX12 CPU descriptor budget exhausted: type={}, pages={}/{}, pageSize={}, active={}",
                static_cast<int>(m_type),
                m_pages.size(),
                m_maxPages,
                m_pageSize,
                m_activeDescriptors);
            return nullptr;
        }

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = m_type;
        heapDesc.NumDescriptors = m_pageSize;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        heapDesc.NodeMask = 0;

        auto page = std::make_unique<Page>();
        const HRESULT result = m_device->CreateDescriptorHeap(
            &heapDesc,
            IID_PPV_ARGS(&page->heap));
        if (FAILED(result))
        {
            ++m_allocationFailures;
            RVX_RHI_ERROR(
                "Failed to create DX12 CPU descriptor page: type={}, page={}, descriptors={}, HRESULT=0x{:08X}",
                static_cast<int>(m_type),
                m_pages.size(),
                m_pageSize,
                static_cast<uint32>(result));
            return nullptr;
        }

        page->cpuStart = page->heap->GetCPUDescriptorHandleForHeapStart();
        page->allocated.resize(m_pageSize, 0);
        page->generations.resize(m_pageSize, 0);

        Page* resultPage = page.get();
        m_pages.push_back(std::move(page));
        m_peakPages = std::max(m_peakPages, static_cast<uint32>(m_pages.size()));

        RVX_RHI_DEBUG(
            "Created DX12 CPU descriptor page: type={}, page={}, descriptors={}, pages={}/{}",
            static_cast<int>(m_type),
            m_pages.size() - 1,
            m_pageSize,
            m_pages.size(),
            m_maxPages);
        return resultPage;
    }

    DX12DescriptorHandle DX12PagedDescriptorAllocator::Allocate()
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!m_device || m_allocatorIdentity == 0)
        {
            ++m_allocationFailures;
            RVX_RHI_ERROR("DX12 CPU descriptor allocation attempted on an uninitialized allocator");
            return {};
        }

        uint32 pageIndex = RVX_INVALID_INDEX;
        Page* page = nullptr;
        for (uint32 index = 0; index < static_cast<uint32>(m_pages.size()); ++index)
        {
            if (m_pages[index]->HasCapacity(m_pageSize))
            {
                pageIndex = index;
                page = m_pages[index].get();
                break;
            }
        }

        if (!page)
        {
            page = CreatePageLocked();
            if (!page)
            {
                return {};
            }
            pageIndex = static_cast<uint32>(m_pages.size() - 1);
        }

        uint32 slotIndex = RVX_INVALID_INDEX;
        if (!page->freeSlots.empty())
        {
            slotIndex = page->freeSlots.back();
            page->freeSlots.pop_back();
        }
        else if (page->nextFreeSlot < m_pageSize)
        {
            slotIndex = page->nextFreeSlot++;
        }

        if (slotIndex == RVX_INVALID_INDEX || slotIndex >= m_pageSize || page->allocated[slotIndex] != 0)
        {
            ++m_allocationFailures;
            RVX_RHI_ERROR("DX12 CPU descriptor allocator internal state is inconsistent");
            return {};
        }

        const uint32 generation = AdvanceGeneration(page->generations[slotIndex]);
        page->generations[slotIndex] = generation;
        page->allocated[slotIndex] = 1;
        ++m_activeDescriptors;
        m_peakActiveDescriptors = std::max(m_peakActiveDescriptors, m_activeDescriptors);

        DX12DescriptorHandle handle;
        handle.allocatorIdentity = m_allocatorIdentity;
        handle.pageIndex = pageIndex;
        handle.slotIndex = slotIndex;
        handle.generation = generation;
        handle.cpuHandle.ptr = page->cpuStart.ptr +
                               static_cast<SIZE_T>(slotIndex) * m_descriptorSize;
        return handle;
    }

    bool DX12PagedDescriptorAllocator::Free(DX12DescriptorHandle handle)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        const bool identityValid = handle.IsValid() &&
                                   handle.allocatorIdentity == m_allocatorIdentity;
        const bool locationValid = identityValid &&
                                   handle.pageIndex < m_pages.size() &&
                                   handle.slotIndex < m_pageSize;
        if (!locationValid)
        {
            ++m_validationFailures;
            RVX_RHI_ERROR(
                "Rejected foreign or invalid DX12 descriptor free: allocator={}, page={}, slot={}, generation={}",
                handle.allocatorIdentity,
                handle.pageIndex,
                handle.slotIndex,
                handle.generation);
            return false;
        }

        Page& page = *m_pages[handle.pageIndex];
        if (page.allocated[handle.slotIndex] == 0 ||
            page.generations[handle.slotIndex] != handle.generation)
        {
            ++m_validationFailures;
            RVX_RHI_ERROR(
                "Rejected stale or duplicate DX12 descriptor free: page={}, slot={}, generation={}, currentGeneration={}, allocated={}",
                handle.pageIndex,
                handle.slotIndex,
                handle.generation,
                page.generations[handle.slotIndex],
                page.allocated[handle.slotIndex]);
            return false;
        }

        page.allocated[handle.slotIndex] = 0;
        page.freeSlots.push_back(handle.slotIndex);
        --m_activeDescriptors;
        return true;
    }

    DX12DescriptorAllocatorStats DX12PagedDescriptorAllocator::GetStats() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        DX12DescriptorAllocatorStats stats;
        stats.pageSize = m_pageSize;
        stats.maxPages = m_maxPages;
        stats.currentPages = static_cast<uint32>(m_pages.size());
        stats.peakPages = m_peakPages;
        stats.activeDescriptors = m_activeDescriptors;
        stats.peakActiveDescriptors = m_peakActiveDescriptors;
        stats.allocationFailures = m_allocationFailures;
        stats.validationFailures = m_validationFailures;
        return stats;
    }

    // =========================================================================
    // Static Descriptor Heap
    // =========================================================================
    void DX12StaticDescriptorHeap::Initialize(
        ID3D12Device* device,
        D3D12_DESCRIPTOR_HEAP_TYPE type,
        uint32 maxDescriptors,
        bool shaderVisible)
    {
        m_type = type;
        m_maxDescriptors = maxDescriptors;
        m_shaderVisible = shaderVisible;
        m_allocatorIdentity = AllocateDescriptorAllocatorIdentity();
        m_descriptorSize = device->GetDescriptorHandleIncrementSize(type);

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = type;
        heapDesc.NumDescriptors = maxDescriptors;
        heapDesc.Flags = shaderVisible
            ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
            : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        heapDesc.NodeMask = 0;

        DX12_CHECK(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_heap)));

        m_cpuStart = m_heap->GetCPUDescriptorHandleForHeapStart();
        if (shaderVisible)
        {
            m_gpuStart = m_heap->GetGPUDescriptorHandleForHeapStart();
        }

        m_allocated.assign(maxDescriptors, false);
        m_generations.assign(maxDescriptors, 0);
        m_nextFreeIndex = 0;
        m_nextGeneration = 0;
        std::queue<uint32> empty;
        m_freeList.swap(empty);

        RVX_RHI_DEBUG("Created DX12 Descriptor Heap: type={}, count={}, shaderVisible={}",
            static_cast<int>(type), maxDescriptors, shaderVisible);
    }

    uint32 DX12StaticDescriptorHeap::NextGenerationLocked()
    {
        m_nextGeneration = AdvanceGeneration(m_nextGeneration);
        return m_nextGeneration;
    }

    DX12DescriptorHandle DX12StaticDescriptorHeap::Allocate()
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        uint32 index = RVX_INVALID_INDEX;
        while (!m_freeList.empty())
        {
            const uint32 candidate = m_freeList.front();
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

        const uint32 generation = NextGenerationLocked();
        m_allocated[index] = true;
        m_generations[index] = generation;

        DX12DescriptorHandle handle;
        handle.allocatorIdentity = m_allocatorIdentity;
        handle.pageIndex = 0;
        handle.slotIndex = index;
        handle.generation = generation;
        handle.cpuHandle.ptr = m_cpuStart.ptr + static_cast<SIZE_T>(index) * m_descriptorSize;
        if (m_shaderVisible)
        {
            handle.gpuHandle.ptr = m_gpuStart.ptr + static_cast<UINT64>(index) * m_descriptorSize;
        }
        return handle;
    }

    DX12DescriptorHandle DX12StaticDescriptorHeap::AllocateRange(uint32 count)
    {
        if (count == 0 || count > m_maxDescriptors)
        {
            return {};
        }
        if (count == 1)
        {
            return Allocate();
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        uint32 startIndex = RVX_INVALID_INDEX;
        for (uint32 index = 0; index + count <= m_maxDescriptors; ++index)
        {
            bool available = true;
            for (uint32 offset = 0; offset < count; ++offset)
            {
                if (m_allocated[index + offset])
                {
                    available = false;
                    index += offset;
                    break;
                }
            }
            if (available)
            {
                startIndex = index;
                break;
            }
        }

        if (startIndex == RVX_INVALID_INDEX)
        {
            RVX_RHI_ERROR("Descriptor heap range allocation failed! Type: {}, Count: {}",
                static_cast<int>(m_type), count);
            return {};
        }

        const uint32 generation = NextGenerationLocked();
        for (uint32 offset = 0; offset < count; ++offset)
        {
            m_allocated[startIndex + offset] = true;
            m_generations[startIndex + offset] = generation;
        }
        m_nextFreeIndex = std::max(m_nextFreeIndex, startIndex + count);

        DX12DescriptorHandle handle;
        handle.allocatorIdentity = m_allocatorIdentity;
        handle.pageIndex = 0;
        handle.slotIndex = startIndex;
        handle.generation = generation;
        handle.cpuHandle.ptr = m_cpuStart.ptr + static_cast<SIZE_T>(startIndex) * m_descriptorSize;
        if (m_shaderVisible)
        {
            handle.gpuHandle.ptr = m_gpuStart.ptr + static_cast<UINT64>(startIndex) * m_descriptorSize;
        }
        return handle;
    }

    bool DX12StaticDescriptorHeap::Free(DX12DescriptorHandle handle)
    {
        return FreeRange(handle, 1);
    }

    bool DX12StaticDescriptorHeap::FreeRange(DX12DescriptorHandle handle, uint32 count)
    {
        if (count == 0)
        {
            return false;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        const bool rangeValid = handle.IsValid() &&
                                handle.allocatorIdentity == m_allocatorIdentity &&
                                handle.pageIndex == 0 &&
                                handle.slotIndex <= m_maxDescriptors &&
                                count <= m_maxDescriptors - handle.slotIndex;
        if (!rangeValid)
        {
            RVX_RHI_ERROR("Rejected invalid or foreign static DX12 descriptor free");
            return false;
        }

        for (uint32 offset = 0; offset < count; ++offset)
        {
            const uint32 index = handle.slotIndex + offset;
            if (!m_allocated[index] || m_generations[index] != handle.generation)
            {
                RVX_RHI_ERROR(
                    "Rejected stale or duplicate static DX12 descriptor free: slot={}, generation={}, currentGeneration={}, allocated={}",
                    index,
                    handle.generation,
                    m_generations[index],
                    static_cast<bool>(m_allocated[index]));
                return false;
            }
        }

        for (uint32 offset = 0; offset < count; ++offset)
        {
            const uint32 index = handle.slotIndex + offset;
            m_allocated[index] = false;
            m_freeList.push(index);
        }
        return true;
    }

    // =========================================================================
    // Ring Buffer Descriptor Heap
    // =========================================================================
    void DX12RingDescriptorHeap::Initialize(
        ID3D12Device* device,
        D3D12_DESCRIPTOR_HEAP_TYPE type,
        uint32 maxDescriptors)
    {
        m_type = type;
        m_maxDescriptors = maxDescriptors;
        m_allocatorIdentity = AllocateDescriptorAllocatorIdentity();
        m_descriptorSize = device->GetDescriptorHandleIncrementSize(type);
        m_currentOffset.store(0, std::memory_order_release);
        m_generation.store(1, std::memory_order_release);

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
        if (count == 0 || count > m_maxDescriptors)
        {
            return {};
        }

        uint32 offset = m_currentOffset.load(std::memory_order_relaxed);
        while (true)
        {
            if (offset > m_maxDescriptors - count)
            {
                RVX_RHI_ERROR("Ring descriptor heap overflow! Requested: {}, Available: {}",
                    count, m_maxDescriptors - std::min(offset, m_maxDescriptors));
                return {};
            }
            if (m_currentOffset.compare_exchange_weak(
                    offset,
                    offset + count,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed))
            {
                break;
            }
        }

        DX12DescriptorHandle handle;
        handle.allocatorIdentity = m_allocatorIdentity;
        handle.pageIndex = 0;
        handle.slotIndex = offset;
        handle.generation = m_generation.load(std::memory_order_acquire);
        handle.cpuHandle.ptr = m_cpuStart.ptr + static_cast<SIZE_T>(offset) * m_descriptorSize;
        handle.gpuHandle.ptr = m_gpuStart.ptr + static_cast<UINT64>(offset) * m_descriptorSize;
        return handle;
    }

    void DX12RingDescriptorHeap::Reset()
    {
        m_currentOffset.store(0, std::memory_order_release);
        uint32 generation = m_generation.load(std::memory_order_relaxed);
        m_generation.store(AdvanceGeneration(generation), std::memory_order_release);
    }

    // =========================================================================
    // Descriptor Heap Manager
    // =========================================================================
    void DX12DescriptorHeapManager::Initialize(ID3D12Device* device)
    {
        m_device = device;

        const bool cpuDescriptorsReady =
            m_cpuCbvSrvUavHeap.Initialize(
                device,
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                CPU_CBV_SRV_UAV_PAGE_SIZE,
                CPU_CBV_SRV_UAV_MAX_PAGES) &&
            m_cpuSamplerHeap.Initialize(
                device,
                D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                CPU_SAMPLER_PAGE_SIZE,
                CPU_SAMPLER_MAX_PAGES) &&
            m_rtvHeap.Initialize(
                device,
                D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
                RTV_PAGE_SIZE,
                RTV_MAX_PAGES) &&
            m_dsvHeap.Initialize(
                device,
                D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
                DSV_PAGE_SIZE,
                DSV_MAX_PAGES);
        RVX_ASSERT_MSG(cpuDescriptorsReady, "Failed to initialize DX12 CPU descriptor allocators");

        // Shader-visible heap capacities and binding ABI remain unchanged.
        m_cbvSrvUavHeap.Initialize(
            device,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            SHADER_VISIBLE_CBV_SRV_UAV_DESCRIPTORS,
            true);
        m_samplerHeap.Initialize(
            device,
            D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
            SHADER_VISIBLE_SAMPLER_DESCRIPTORS,
            true);

        for (uint32 index = 0; index < RVX_MAX_FRAME_COUNT; ++index)
        {
            m_transientHeaps[index].Initialize(
                device,
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                RING_BUFFER_SIZE);
        }

        RVX_RHI_INFO("DX12 Descriptor Heap Manager initialized with lazy CPU descriptor pages");
    }

    void DX12DescriptorHeapManager::Shutdown()
    {
        m_cpuCbvSrvUavHeap.Shutdown();
        m_cpuSamplerHeap.Shutdown();
        m_rtvHeap.Shutdown();
        m_dsvHeap.Shutdown();
        m_device = nullptr;
        RVX_RHI_INFO("DX12 Descriptor Heap Manager shutdown");
    }

    DX12DescriptorHandle DX12DescriptorHeapManager::AllocateTransientCbvSrvUav(uint32 count)
    {
        const uint32 frameIndex = m_currentFrameIndex.load(std::memory_order_acquire);
        return m_transientHeaps[frameIndex].Allocate(count);
    }

    void DX12DescriptorHeapManager::ResetTransientHeaps()
    {
        const uint32 newIndex =
            (m_currentFrameIndex.load(std::memory_order_relaxed) + 1) % RVX_MAX_FRAME_COUNT;
        m_currentFrameIndex.store(newIndex, std::memory_order_release);
        m_transientHeaps[newIndex].Reset();
    }

} // namespace RVX
