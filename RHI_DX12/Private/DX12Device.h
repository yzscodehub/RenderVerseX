#pragma once

#include "DX12Common.h"
#include "DX12DescriptorHeap.h"
#include "RHI/RHIDevice.h"
#include "RHI/RHICapabilities.h"
#include <array>

namespace RVX
{
    // Forward declarations
    class DX12Buffer;
    class DX12Texture;
    class DX12SwapChain;
    class DX12CommandContext;
    class DX12Fence;

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
        
        ID3D12CommandQueue* GetQueue(RHICommandQueueType type) const;

        ID3D12CommandSignature* GetDrawCommandSignature();
        ID3D12CommandSignature* GetDrawIndexedCommandSignature();
        ID3D12CommandSignature* GetDispatchCommandSignature();

        #ifdef RVX_USE_D3D12MA
        D3D12MA::Allocator* GetMemoryAllocator() const { return m_memoryAllocator.Get(); }
        #endif

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

        // Capabilities
        RHICapabilities m_capabilities;

        // Debug
        bool m_debugLayerEnabled = false;
    };

} // namespace RVX
