#pragma once

#include "DX11Common.h"
#include "DX11Debug.h"
#include "DX11StateCache.h"
#include "RHI/RHIDevice.h"
#include "RHI/RHICapabilities.h"

#include <memory>

namespace RVX
{
    // Forward declarations
    class DX11Buffer;
    class DX11Texture;
    class DX11SwapChain;
    class DX11CommandContext;

    // =============================================================================
    // DX11 Device Implementation
    // =============================================================================
    class DX11Device : public IRHIDevice
    {
    public:
        DX11Device();
        ~DX11Device() override;

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

        // Heap Management (DX11: not supported, returns nullptr or creates standalone)
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
        // DX11 uses a single immediate context, so always return 0 to use the same command context
        uint32 GetCurrentFrameIndex() const override { return 0; }

        // Capabilities
        const RHICapabilities& GetCapabilities() const override { return m_capabilities; }
        RHIBackendType GetBackendType() const override { return RHIBackendType::DX11; }

        // Upload Resources
        RHIStagingBufferRef CreateStagingBuffer(const RHIStagingBufferDesc& desc) override;
        RHIRingBufferRef CreateRingBuffer(const RHIRingBufferDesc& desc) override;

        // Memory Statistics
        RHIMemoryStats GetMemoryStats() const override;

        // Debug Resource Groups
        void BeginResourceGroup(const char* name) override;
        void EndResourceGroup() override;

        // =========================================================================
        // DX11 Specific Accessors
        // =========================================================================
        ID3D11Device* GetD3DDevice() const { return m_device.Get(); }
        ID3D11Device1* GetD3DDevice1() const { return m_device1.Get(); }
        ID3D11Device5* GetD3DDevice5() const { return m_device5.Get(); }
        ID3D11DeviceContext* GetImmediateContext() const { return m_immediateContext.Get(); }
        ID3D11DeviceContext1* GetImmediateContext1() const { return m_immediateContext1.Get(); }
        IDXGIFactory2* GetDXGIFactory() const { return m_factory.Get(); }

        D3D_FEATURE_LEVEL GetFeatureLevel() const { return m_featureLevel; }

        // Threading mode
        bool SupportsDeferredContext() const { return m_capabilities.dx11.supportsDeferredContext; }
        DX11ThreadingMode GetThreadingMode() const { return m_threadingMode; }

        // Create deferred context for multi-threaded command recording
        ComPtr<ID3D11DeviceContext> CreateDeferredContext();

        // State cache for pipeline state objects
        DX11StateCache* GetStateCache() { return m_stateCache.get(); }

    private:
        bool CreateFactory();
        bool SelectAdapter(uint32 preferredIndex);
        bool CreateDevice(bool enableDebugLayer);
        void QueryCapabilities();
        void EnableDebugLayer();

        // DXGI/D3D11 Core Objects
        ComPtr<IDXGIFactory2> m_factory;
        ComPtr<IDXGIAdapter1> m_adapter;
        ComPtr<ID3D11Device> m_device;
        ComPtr<ID3D11Device1> m_device1;
        ComPtr<ID3D11Device5> m_device5;  // Optional: for ID3D11Fence support
        ComPtr<ID3D11DeviceContext> m_immediateContext;
        ComPtr<ID3D11DeviceContext1> m_immediateContext1;

        D3D_FEATURE_LEVEL m_featureLevel = D3D_FEATURE_LEVEL_11_0;
        DX11ThreadingMode m_threadingMode = DX11ThreadingMode::SingleThreaded;

        // Frame management
        uint32 m_frameIndex = 0;
        uint64 m_totalFrameCount = 0;

        // Capabilities
        RHICapabilities m_capabilities;
        bool m_debugLayerEnabled = false;
        bool m_initialized = false;

        // State cache
        std::unique_ptr<DX11StateCache> m_stateCache;
    };

} // namespace RVX
