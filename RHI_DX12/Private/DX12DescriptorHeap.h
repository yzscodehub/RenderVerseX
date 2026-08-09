#pragma once

#include "DX12Common.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <queue>
#include <utility>
#include <vector>

namespace RVX
{
    // =========================================================================
    // Descriptor Handle
    // =========================================================================
    /** @brief Generation-safe descriptor allocation identity. */
    struct DX12DescriptorHandle
    {
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = {};
        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = {};
        uint64 allocatorIdentity = 0;
        uint32 pageIndex = RVX_INVALID_INDEX;
        uint32 slotIndex = RVX_INVALID_INDEX;
        uint32 generation = 0;

        bool IsValid() const
        {
            return allocatorIdentity != 0 &&
                   pageIndex != RVX_INVALID_INDEX &&
                   slotIndex != RVX_INVALID_INDEX &&
                   generation != 0;
        }

        bool IsShaderVisible() const { return gpuHandle.ptr != 0; }
        bool HasValidCpuHandle() const { return IsValid() && cpuHandle.ptr != 0; }
    };

    /** @brief Central fail-closed gate for every native CPU descriptor write. */
    template<typename TCreateFunction>
    bool TryCreateDX12CpuDescriptor(
        const DX12DescriptorHandle& handle,
        TCreateFunction&& createFunction)
    {
        if (!handle.HasValidCpuHandle())
        {
            return false;
        }

        std::forward<TCreateFunction>(createFunction)(handle.cpuHandle);
        return true;
    }

    /** @brief Observable state for one CPU-only paged descriptor allocator. */
    struct DX12DescriptorAllocatorStats
    {
        uint32 pageSize = 0;
        uint32 maxPages = 0;
        uint32 currentPages = 0;
        uint32 peakPages = 0;
        uint32 activeDescriptors = 0;
        uint32 peakActiveDescriptors = 0;
        uint64 allocationFailures = 0;
        uint64 validationFailures = 0;

        uint64 GetCurrentCapacity() const
        {
            return static_cast<uint64>(currentPages) * pageSize;
        }

        uint64 GetMaximumCapacity() const
        {
            return static_cast<uint64>(maxPages) * pageSize;
        }
    };

    // =========================================================================
    // Paged CPU Descriptor Allocator
    // =========================================================================
    /**
     * @brief Lazily allocates CPU-only descriptor heap pages.
     *
     * Every returned handle is tied to this allocator, page, slot and allocation
     * generation. Stale, duplicate and foreign frees are rejected atomically.
     */
    class DX12PagedDescriptorAllocator
    {
    public:
        DX12PagedDescriptorAllocator();
        ~DX12PagedDescriptorAllocator();

        DX12PagedDescriptorAllocator(const DX12PagedDescriptorAllocator&) = delete;
        DX12PagedDescriptorAllocator& operator=(const DX12PagedDescriptorAllocator&) = delete;

        bool Initialize(
            ID3D12Device* device,
            D3D12_DESCRIPTOR_HEAP_TYPE type,
            uint32 pageSize,
            uint32 maxPages);
        void Shutdown();

        DX12DescriptorHandle Allocate();
        bool Free(DX12DescriptorHandle handle);

        uint32 GetDescriptorSize() const { return m_descriptorSize; }
        D3D12_DESCRIPTOR_HEAP_TYPE GetType() const { return m_type; }
        DX12DescriptorAllocatorStats GetStats() const;

    private:
        struct Page;

        Page* CreatePageLocked();

        ID3D12Device* m_device = nullptr;
        D3D12_DESCRIPTOR_HEAP_TYPE m_type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        uint32 m_descriptorSize = 0;
        uint32 m_pageSize = 0;
        uint32 m_maxPages = 0;
        uint64 m_allocatorIdentity = 0;

        mutable std::mutex m_mutex;
        std::vector<std::unique_ptr<Page>> m_pages;
        uint32 m_peakPages = 0;
        uint32 m_activeDescriptors = 0;
        uint32 m_peakActiveDescriptors = 0;
        uint64 m_allocationFailures = 0;
        uint64 m_validationFailures = 0;
    };

    // =========================================================================
    // Static Descriptor Heap
    // =========================================================================
    // Used only for persistent shader-visible descriptor tables. CPU-only
    // resource descriptors use DX12PagedDescriptorAllocator.
    class DX12StaticDescriptorHeap
    {
    public:
        DX12StaticDescriptorHeap() = default;
        ~DX12StaticDescriptorHeap() = default;

        void Initialize(
            ID3D12Device* device,
            D3D12_DESCRIPTOR_HEAP_TYPE type,
            uint32 maxDescriptors,
            bool shaderVisible);

        DX12DescriptorHandle Allocate();
        DX12DescriptorHandle AllocateRange(uint32 count);
        bool Free(DX12DescriptorHandle handle);
        bool FreeRange(DX12DescriptorHandle handle, uint32 count);

        ID3D12DescriptorHeap* GetHeap() const { return m_heap.Get(); }
        uint32 GetDescriptorSize() const { return m_descriptorSize; }
        D3D12_DESCRIPTOR_HEAP_TYPE GetType() const { return m_type; }

    private:
        uint32 NextGenerationLocked();

        ComPtr<ID3D12DescriptorHeap> m_heap;
        D3D12_DESCRIPTOR_HEAP_TYPE m_type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        uint32 m_descriptorSize = 0;
        uint32 m_maxDescriptors = 0;
        bool m_shaderVisible = false;
        uint64 m_allocatorIdentity = 0;
        uint32 m_nextGeneration = 0;

        D3D12_CPU_DESCRIPTOR_HANDLE m_cpuStart = {};
        D3D12_GPU_DESCRIPTOR_HANDLE m_gpuStart = {};

        std::mutex m_mutex;
        std::vector<bool> m_allocated;
        std::vector<uint32> m_generations;
        std::queue<uint32> m_freeList;
        uint32 m_nextFreeIndex = 0;
    };

    // =========================================================================
    // Ring Buffer Descriptor Heap
    // =========================================================================
    class DX12RingDescriptorHeap
    {
    public:
        DX12RingDescriptorHeap() = default;
        ~DX12RingDescriptorHeap() = default;

        void Initialize(
            ID3D12Device* device,
            D3D12_DESCRIPTOR_HEAP_TYPE type,
            uint32 maxDescriptors);

        DX12DescriptorHandle Allocate(uint32 count = 1);
        void Reset();

        ID3D12DescriptorHeap* GetHeap() const { return m_heap.Get(); }
        uint32 GetDescriptorSize() const { return m_descriptorSize; }
        uint32 GetAllocatedCount() const { return m_currentOffset.load(std::memory_order_acquire); }

    private:
        ComPtr<ID3D12DescriptorHeap> m_heap;
        D3D12_DESCRIPTOR_HEAP_TYPE m_type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        uint32 m_descriptorSize = 0;
        uint32 m_maxDescriptors = 0;
        uint64 m_allocatorIdentity = 0;

        D3D12_CPU_DESCRIPTOR_HANDLE m_cpuStart = {};
        D3D12_GPU_DESCRIPTOR_HANDLE m_gpuStart = {};

        std::atomic<uint32> m_currentOffset{0};
        std::atomic<uint32> m_generation{1};
    };

    // =========================================================================
    // Descriptor Heap Manager
    // =========================================================================
    class DX12DescriptorHeapManager
    {
    public:
        static constexpr uint32 CPU_CBV_SRV_UAV_PAGE_SIZE = 16384;
        static constexpr uint32 CPU_CBV_SRV_UAV_MAX_PAGES = 64;
        static constexpr uint32 CPU_SAMPLER_PAGE_SIZE = 256;
        static constexpr uint32 CPU_SAMPLER_MAX_PAGES = 8;
        static constexpr uint32 RTV_PAGE_SIZE = 512;
        static constexpr uint32 RTV_MAX_PAGES = 32;
        static constexpr uint32 DSV_PAGE_SIZE = 256;
        static constexpr uint32 DSV_MAX_PAGES = 16;

        static constexpr uint32 MAX_CPU_CBV_SRV_UAV_DESCRIPTORS =
            CPU_CBV_SRV_UAV_PAGE_SIZE * CPU_CBV_SRV_UAV_MAX_PAGES;
        static constexpr uint32 MAX_CPU_SAMPLER_DESCRIPTORS =
            CPU_SAMPLER_PAGE_SIZE * CPU_SAMPLER_MAX_PAGES;
        static constexpr uint32 MAX_RTV_DESCRIPTORS = RTV_PAGE_SIZE * RTV_MAX_PAGES;
        static constexpr uint32 MAX_DSV_DESCRIPTORS = DSV_PAGE_SIZE * DSV_MAX_PAGES;

        // Public shader-visible table capacities are intentionally unchanged.
        static constexpr uint32 SHADER_VISIBLE_CBV_SRV_UAV_DESCRIPTORS = 1000000;
        static constexpr uint32 SHADER_VISIBLE_SAMPLER_DESCRIPTORS = 2048;
        static constexpr uint32 RING_BUFFER_SIZE = 65536;

        void Initialize(ID3D12Device* device);
        void Shutdown();

        ID3D12DescriptorHeap* GetCbvSrvUavHeap() const { return m_cbvSrvUavHeap.GetHeap(); }
        ID3D12DescriptorHeap* GetSamplerHeap() const { return m_samplerHeap.GetHeap(); }

        DX12DescriptorHandle AllocateCpuCbvSrvUav() { return m_cpuCbvSrvUavHeap.Allocate(); }
        DX12DescriptorHandle AllocateCpuSampler() { return m_cpuSamplerHeap.Allocate(); }
        DX12DescriptorHandle AllocateRTV() { return m_rtvHeap.Allocate(); }
        DX12DescriptorHandle AllocateDSV() { return m_dsvHeap.Allocate(); }

        bool FreeCpuCbvSrvUav(DX12DescriptorHandle handle) { return m_cpuCbvSrvUavHeap.Free(handle); }
        bool FreeCpuSampler(DX12DescriptorHandle handle) { return m_cpuSamplerHeap.Free(handle); }
        bool FreeRTV(DX12DescriptorHandle handle) { return m_rtvHeap.Free(handle); }
        bool FreeDSV(DX12DescriptorHandle handle) { return m_dsvHeap.Free(handle); }

        DX12DescriptorHandle AllocateGpuCbvSrvUavRange(uint32 count) { return m_cbvSrvUavHeap.AllocateRange(count); }
        DX12DescriptorHandle AllocateGpuSamplerRange(uint32 count) { return m_samplerHeap.AllocateRange(count); }
        bool FreeGpuCbvSrvUavRange(DX12DescriptorHandle handle, uint32 count) { return m_cbvSrvUavHeap.FreeRange(handle, count); }
        bool FreeGpuSamplerRange(DX12DescriptorHandle handle, uint32 count) { return m_samplerHeap.FreeRange(handle, count); }

        DX12DescriptorHandle AllocateTransientCbvSrvUav(uint32 count = 1);
        void ResetTransientHeaps();

        uint32 GetCbvSrvUavDescriptorSize() const { return m_cbvSrvUavHeap.GetDescriptorSize(); }
        uint32 GetSamplerDescriptorSize() const { return m_samplerHeap.GetDescriptorSize(); }
        uint32 GetRTVDescriptorSize() const { return m_rtvHeap.GetDescriptorSize(); }
        uint32 GetDSVDescriptorSize() const { return m_dsvHeap.GetDescriptorSize(); }

        DX12DescriptorAllocatorStats GetCpuCbvSrvUavStats() const { return m_cpuCbvSrvUavHeap.GetStats(); }
        DX12DescriptorAllocatorStats GetCpuSamplerStats() const { return m_cpuSamplerHeap.GetStats(); }
        DX12DescriptorAllocatorStats GetRTVStats() const { return m_rtvHeap.GetStats(); }
        DX12DescriptorAllocatorStats GetDSVStats() const { return m_dsvHeap.GetStats(); }

    private:
        ID3D12Device* m_device = nullptr;

        DX12PagedDescriptorAllocator m_cpuCbvSrvUavHeap;
        DX12PagedDescriptorAllocator m_cpuSamplerHeap;
        DX12StaticDescriptorHeap m_cbvSrvUavHeap;
        DX12StaticDescriptorHeap m_samplerHeap;
        DX12PagedDescriptorAllocator m_rtvHeap;
        DX12PagedDescriptorAllocator m_dsvHeap;

        std::array<DX12RingDescriptorHeap, RVX_MAX_FRAME_COUNT> m_transientHeaps;
        std::atomic<uint32> m_currentFrameIndex{0};
    };

} // namespace RVX
