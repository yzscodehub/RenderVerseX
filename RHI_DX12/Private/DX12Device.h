#pragma once

#include "DX12Common.h"
#include "DX12DescriptorHeap.h"
#include "DX12PipelineCache.h"
#include "DX12CommandAllocatorPool.h"
#include "RHI/RHIDevice.h"
#include "RHI/RHICapabilities.h"
#include <array>
#include <functional>
#include <atomic>
#include <mutex>
#include <unordered_map>

namespace RVX
{
    // Forward declarations
    class DX12Buffer;
    class DX12Texture;
    class DX12SwapChain;
    class DX12CommandContext;
    class DX12Fence;
    
    // Root Signature cache key
    struct RootSignatureCacheKey
    {
        std::vector<std::pair<uint32, uint8>> bindings;  // (binding, type) pairs per set
        uint32 pushConstantSize = 0;
        uint32 setCount = 0;
        
        bool operator==(const RootSignatureCacheKey& other) const
        {
            return pushConstantSize == other.pushConstantSize 
                && setCount == other.setCount 
                && bindings == other.bindings;
        }
    };
    
    struct RootSignatureCacheKeyHash
    {
        size_t operator()(const RootSignatureCacheKey& key) const
        {
            size_t hash = std::hash<uint32>{}(key.pushConstantSize);
            hash ^= std::hash<uint32>{}(key.setCount) << 1;
            for (const auto& binding : key.bindings)
            {
                hash ^= (std::hash<uint32>{}(binding.first) ^ (std::hash<uint8>{}(binding.second) << 16)) << 1;
            }
            return hash;
        }
    };

    // =============================================================================
    // DX12 Device Implementation
    // =============================================================================
    class DX12Device : public IRHIDevice
    {
    public:
        DX12Device();
        ~DX12Device() override;

        bool Initialize(const RHIDeviceDesc& desc);
        void Shutdown();

        // =========================================================================
        // IRHIDevice Implementation
        // =========================================================================

        // Resource Creation
        RHIBufferRef CreateBuffer(const RHIBufferDesc& desc) override;
        RHITextureRef CreateTexture(const RHITextureDesc& desc) override;
        RHITextureViewRef CreateTextureView(RHITexture* texture, const RHITextureViewDesc& desc) override;
        RHISamplerRef CreateSampler(const RHISamplerDesc& desc) override;
        RHIShaderRef CreateShader(const RHIShaderDesc& desc) override;

        // Memory Heap Management (for Placed Resources / Memory Aliasing)
        RHIHeapRef CreateHeap(const RHIHeapDesc& desc) override;
        RHITextureRef CreatePlacedTexture(RHIHeap* heap, uint64 offset, const RHITextureDesc& desc) override;
        RHIBufferRef CreatePlacedBuffer(RHIHeap* heap, uint64 offset, const RHIBufferDesc& desc) override;
        MemoryRequirements GetTextureMemoryRequirements(const RHITextureDesc& desc) override;
        MemoryRequirements GetBufferMemoryRequirements(const RHIBufferDesc& desc) override;

        // Pipeline Creation
        RHIDescriptorSetLayoutRef CreateDescriptorSetLayout(const RHIDescriptorSetLayoutDesc& desc) override;
        RHIPipelineLayoutRef CreatePipelineLayout(const RHIPipelineLayoutDesc& desc) override;
        RHIPipelineRef CreateGraphicsPipeline(const RHIGraphicsPipelineDesc& desc) override;
        RHIPipelineRef CreateComputePipeline(const RHIComputePipelineDesc& desc) override;

        // Descriptor Set
        RHIDescriptorSetRef CreateDescriptorSet(const RHIDescriptorSetDesc& desc) override;

        // Query Pool
        RHIQueryPoolRef CreateQueryPool(const RHIQueryPoolDesc& desc) override;

        // Command Context
        RHICommandContextRef CreateCommandContext(RHICommandQueueType type) override;
        void SubmitCommandContext(RHICommandContext* context, RHIFence* signalFence) override;
        void SubmitCommandContexts(std::span<RHICommandContext* const> contexts, RHIFence* signalFence) override;

        // SwapChain
        RHISwapChainRef CreateSwapChain(const RHISwapChainDesc& desc) override;

        // Synchronization
        RHIFenceRef CreateFence(uint64 initialValue) override;
        void WaitForFence(RHIFence* fence, uint64 value) override;
        void WaitIdle() override;

        // Frame Management
        void BeginFrame() override;
        void EndFrame() override;
        uint32 GetCurrentFrameIndex() const override { return m_frameIndex; }

        // Capabilities
        const RHICapabilities& GetCapabilities() const override { return m_capabilities; }
        RHIBackendType GetBackendType() const override { return RHIBackendType::DX12; }

        // =========================================================================
        // DX12 Specific Accessors
        // =========================================================================
        ID3D12Device* GetD3DDevice() const { return m_device.Get(); }
        IDXGIFactory6* GetDXGIFactory() const { return m_factory.Get(); }
        ID3D12CommandQueue* GetGraphicsQueue() const { return m_graphicsQueue.Get(); }
        ID3D12CommandQueue* GetComputeQueue() const { return m_computeQueue.Get(); }
        ID3D12CommandQueue* GetCopyQueue() const { return m_copyQueue.Get(); }
        
        DX12DescriptorHeapManager& GetDescriptorHeapManager() { return m_descriptorHeapManager; }
        DX12PipelineCache& GetPipelineCache() { return m_pipelineCache; }
        DX12CommandAllocatorPool& GetAllocatorPool() { return m_allocatorPool; }
        
        ID3D12CommandQueue* GetQueue(RHICommandQueueType type) const;

        ID3D12CommandSignature* GetDrawCommandSignature();
        ID3D12CommandSignature* GetDrawIndexedCommandSignature();
        ID3D12CommandSignature* GetDispatchCommandSignature();

        #ifdef RVX_USE_D3D12MA
        D3D12MA::Allocator* GetMemoryAllocator() const { return m_memoryAllocator.Get(); }
        #endif

        // =========================================================================
        // Device Lost Handling
        // =========================================================================
        using DeviceLostCallback = std::function<void(HRESULT reason)>;
        
        void SetDeviceLostCallback(DeviceLostCallback callback) { m_deviceLostCallback = std::move(callback); }
        bool IsDeviceLost() const { return m_deviceLost.load(std::memory_order_acquire); }
        HRESULT GetDeviceRemovedReason() const;
        void HandleDeviceLost(HRESULT reason);

        // =========================================================================
        // Root Signature Cache
        // =========================================================================
        ComPtr<ID3D12RootSignature> GetOrCreateRootSignature(
            const RootSignatureCacheKey& key,
            const std::function<ComPtr<ID3D12RootSignature>()>& createFunc);
        
        static RootSignatureCacheKey BuildRootSignatureKey(const RHIPipelineLayoutDesc& desc);

    private:
        bool CreateFactory(bool enableDebugLayer);
        bool SelectAdapter(uint32 preferredIndex);
        bool CreateDevice(bool enableGPUValidation);
        bool CreateCommandQueues();
        bool InitializeCapabilities();
        void EnableDebugLayer(bool enableGPUValidation);

        // DXGI/D3D12 Core Objects
        ComPtr<IDXGIFactory6> m_factory;
        ComPtr<IDXGIAdapter4> m_adapter;
        ComPtr<ID3D12Device> m_device;

        // Command Queues
        ComPtr<ID3D12CommandQueue> m_graphicsQueue;
        ComPtr<ID3D12CommandQueue> m_computeQueue;
        ComPtr<ID3D12CommandQueue> m_copyQueue;

        // Command Signatures (for indirect)
        ComPtr<ID3D12CommandSignature> m_drawCommandSignature;
        ComPtr<ID3D12CommandSignature> m_drawIndexedCommandSignature;
        ComPtr<ID3D12CommandSignature> m_dispatchCommandSignature;

        // Frame synchronization
        ComPtr<ID3D12Fence> m_frameFence;
        HANDLE m_fenceEvent = nullptr;
        std::array<uint64, RVX_MAX_FRAME_COUNT> m_frameFenceValues = {};
        uint32 m_frameIndex = 0;

        // Memory Allocator
        #ifdef RVX_USE_D3D12MA
        ComPtr<D3D12MA::Allocator> m_memoryAllocator;
        #endif

        // Descriptor Heaps
        DX12DescriptorHeapManager m_descriptorHeapManager;

        // Pipeline Cache
        DX12PipelineCache m_pipelineCache;

        // Command Allocator Pool
        DX12CommandAllocatorPool m_allocatorPool;

        // Capabilities
        RHICapabilities m_capabilities;

        // Debug
        bool m_debugLayerEnabled = false;

        // Device Lost Handling
        std::atomic<bool> m_deviceLost{false};
        DeviceLostCallback m_deviceLostCallback;
        void EnableDRED();  // Device Removed Extended Data
        void LogDREDInfo(); // Log DRED breadcrumbs and page fault info

        // Root Signature Cache
        std::unordered_map<RootSignatureCacheKey, ComPtr<ID3D12RootSignature>, RootSignatureCacheKeyHash> m_rootSignatureCache;
        std::mutex m_rootSignatureCacheMutex;
    };

} // namespace RVX
