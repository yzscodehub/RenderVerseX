#pragma once

#include "DX12Common.h"
#include <mutex>
#include <vector>
#include <queue>

namespace RVX
{
    // =============================================================================
    // Descriptor Handle
    // =============================================================================
    struct DX12DescriptorHandle
    {
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = {};
        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = {};
        uint32 heapIndex = RVX_INVALID_INDEX;

        bool IsValid() const { return heapIndex != RVX_INVALID_INDEX; }
        bool IsShaderVisible() const { return gpuHandle.ptr != 0; }
    };

    // =============================================================================
    // Static Descriptor Heap
    // Used for persistent descriptors (textures, samplers that live across frames)
    // =============================================================================
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
        void Free(DX12DescriptorHandle handle);
        void FreeRange(DX12DescriptorHandle handle, uint32 count);

        ID3D12DescriptorHeap* GetHeap() const { return m_heap.Get(); }
        uint32 GetDescriptorSize() const { return m_descriptorSize; }
        D3D12_DESCRIPTOR_HEAP_TYPE GetType() const { return m_type; }

    private:
        ComPtr<ID3D12DescriptorHeap> m_heap;
        D3D12_DESCRIPTOR_HEAP_TYPE m_type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        uint32 m_descriptorSize = 0;
        uint32 m_maxDescriptors = 0;
        bool m_shaderVisible = false;

        D3D12_CPU_DESCRIPTOR_HANDLE m_cpuStart = {};
        D3D12_GPU_DESCRIPTOR_HANDLE m_gpuStart = {};

        std::mutex m_mutex;
        std::vector<bool> m_allocated;
        std::queue<uint32> m_freeList;
        uint32 m_nextFreeIndex = 0;
    };

    // =============================================================================
    // Ring Buffer Descriptor Heap
    // Used for transient descriptors (per-frame allocations, reset each frame)
    // =============================================================================
    class DX12RingDescriptorHeap
    {
    public:
        DX12RingDescriptorHeap() = default;
        ~DX12RingDescriptorHeap() = default;

        void Initialize(
            ID3D12Device* device,
            D3D12_DESCRIPTOR_HEAP_TYPE type,
            uint32 maxDescriptors);

        // Allocate a contiguous range of descriptors (returns first handle)
        DX12DescriptorHandle Allocate(uint32 count = 1);

        // Reset for new frame (called at frame start)
        void Reset();

        ID3D12DescriptorHeap* GetHeap() const { return m_heap.Get(); }
        uint32 GetDescriptorSize() const { return m_descriptorSize; }
        uint32 GetAllocatedCount() const { return m_currentOffset; }

    private:
        ComPtr<ID3D12DescriptorHeap> m_heap;
        D3D12_DESCRIPTOR_HEAP_TYPE m_type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        uint32 m_descriptorSize = 0;
        uint32 m_maxDescriptors = 0;

        D3D12_CPU_DESCRIPTOR_HANDLE m_cpuStart = {};
        D3D12_GPU_DESCRIPTOR_HANDLE m_gpuStart = {};

        std::atomic<uint32> m_currentOffset{0};
    };

    // =============================================================================
    // Descriptor Heap Manager
    // Manages all descriptor heaps for a device
    // =============================================================================
    class DX12DescriptorHeapManager
    {
    public:
        static constexpr uint32 MAX_CBV_SRV_UAV_DESCRIPTORS = 1000000;  // 1M for bindless
        static constexpr uint32 MAX_SAMPLER_DESCRIPTORS = 2048;
        static constexpr uint32 MAX_RTV_DESCRIPTORS = 1024;
        static constexpr uint32 MAX_DSV_DESCRIPTORS = 256;
        static constexpr uint32 RING_BUFFER_SIZE = 65536;  // Per-frame transient descriptors

        void Initialize(ID3D12Device* device);
        void Shutdown();

        // Get heaps for binding
        ID3D12DescriptorHeap* GetCbvSrvUavHeap() const { return m_cbvSrvUavHeap.GetHeap(); }
        ID3D12DescriptorHeap* GetSamplerHeap() const { return m_samplerHeap.GetHeap(); }

        // Static allocations (persistent resources)
        DX12DescriptorHandle AllocateCbvSrvUav() { return m_cbvSrvUavHeap.Allocate(); }
        DX12DescriptorHandle AllocateSampler() { return m_samplerHeap.Allocate(); }
        DX12DescriptorHandle AllocateRTV() { return m_rtvHeap.Allocate(); }
        DX12DescriptorHandle AllocateDSV() { return m_dsvHeap.Allocate(); }

        void FreeCbvSrvUav(DX12DescriptorHandle handle) { m_cbvSrvUavHeap.Free(handle); }
        void FreeSampler(DX12DescriptorHandle handle) { m_samplerHeap.Free(handle); }
        void FreeRTV(DX12DescriptorHandle handle) { m_rtvHeap.Free(handle); }
        void FreeDSV(DX12DescriptorHandle handle) { m_dsvHeap.Free(handle); }

        // Static range allocations (contiguous descriptors for descriptor tables)
        DX12DescriptorHandle AllocateCbvSrvUavRange(uint32 count) { return m_cbvSrvUavHeap.AllocateRange(count); }
        DX12DescriptorHandle AllocateSamplerRange(uint32 count) { return m_samplerHeap.AllocateRange(count); }
        void FreeCbvSrvUavRange(DX12DescriptorHandle handle, uint32 count) { m_cbvSrvUavHeap.FreeRange(handle, count); }
        void FreeSamplerRange(DX12DescriptorHandle handle, uint32 count) { m_samplerHeap.FreeRange(handle, count); }

        // Transient allocations (per-frame, auto-reset)
        DX12DescriptorHandle AllocateTransientCbvSrvUav(uint32 count = 1);
        void ResetTransientHeaps();

        // Descriptor sizes
        uint32 GetCbvSrvUavDescriptorSize() const { return m_cbvSrvUavHeap.GetDescriptorSize(); }
        uint32 GetSamplerDescriptorSize() const { return m_samplerHeap.GetDescriptorSize(); }
        uint32 GetRTVDescriptorSize() const { return m_rtvHeap.GetDescriptorSize(); }
        uint32 GetDSVDescriptorSize() const { return m_dsvHeap.GetDescriptorSize(); }

    private:
        ID3D12Device* m_device = nullptr;

        // Static heaps (persistent descriptors)
        DX12StaticDescriptorHeap m_cbvSrvUavHeap;  // Shader visible
        DX12StaticDescriptorHeap m_samplerHeap;     // Shader visible
        DX12StaticDescriptorHeap m_rtvHeap;         // CPU only
        DX12StaticDescriptorHeap m_dsvHeap;         // CPU only

        // Ring buffer for transient descriptors
        std::array<DX12RingDescriptorHeap, RVX_MAX_FRAME_COUNT> m_transientHeaps;
        uint32 m_currentFrameIndex = 0;
    };

} // namespace RVX
