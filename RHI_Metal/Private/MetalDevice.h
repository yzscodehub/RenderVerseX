#pragma once

#include "MetalCommon.h"
#include "RHI/RHIDevice.h"
#include <mutex>
#include <dispatch/dispatch.h>

namespace RVX
{
    class MetalSwapChain;

    // =============================================================================
    // Metal Device Implementation
    // =============================================================================
    class MetalDevice : public IRHIDevice
    {
    public:
        MetalDevice(const RHIDeviceDesc& desc);
        ~MetalDevice() override;

        // =========================================================================
        // IRHIDevice Interface
        // =========================================================================
        RHIBufferRef CreateBuffer(const RHIBufferDesc& desc) override;
        RHITextureRef CreateTexture(const RHITextureDesc& desc) override;
        RHITextureViewRef CreateTextureView(RHITexture* texture, const RHITextureViewDesc& desc) override;
        RHISamplerRef CreateSampler(const RHISamplerDesc& desc) override;
        RHIShaderRef CreateShader(const RHIShaderDesc& desc) override;

        RHIHeapRef CreateHeap(const RHIHeapDesc& desc) override;
        RHITextureRef CreatePlacedTexture(RHIHeap* heap, uint64 offset, const RHITextureDesc& desc) override;
        RHIBufferRef CreatePlacedBuffer(RHIHeap* heap, uint64 offset, const RHIBufferDesc& desc) override;
        MemoryRequirements GetTextureMemoryRequirements(const RHITextureDesc& desc) override;
        MemoryRequirements GetBufferMemoryRequirements(const RHIBufferDesc& desc) override;

        RHIDescriptorSetLayoutRef CreateDescriptorSetLayout(const RHIDescriptorSetLayoutDesc& desc) override;
        RHIPipelineLayoutRef CreatePipelineLayout(const RHIPipelineLayoutDesc& desc) override;
        RHIPipelineRef CreateGraphicsPipeline(const RHIGraphicsPipelineDesc& desc) override;
        RHIPipelineRef CreateComputePipeline(const RHIComputePipelineDesc& desc) override;

        RHIDescriptorSetRef CreateDescriptorSet(const RHIDescriptorSetDesc& desc) override;

        RHIQueryPoolRef CreateQueryPool(const RHIQueryPoolDesc& desc) override;

        RHICommandContextRef CreateCommandContext(RHICommandQueueType type) override;
        void SubmitCommandContext(RHICommandContext* context, RHIFence* signalFence) override;
        void SubmitCommandContexts(std::span<RHICommandContext* const> contexts, RHIFence* signalFence) override;

        RHISwapChainRef CreateSwapChain(const RHISwapChainDesc& desc) override;

        RHIFenceRef CreateFence(uint64 initialValue) override;
        void WaitForFence(RHIFence* fence, uint64 value) override;
        void WaitIdle() override;

        void BeginFrame() override;
        void EndFrame() override;
        uint32 GetCurrentFrameIndex() const override { return m_currentFrameIndex; }

        const RHICapabilities& GetCapabilities() const override { return m_capabilities; }
        RHIBackendType GetBackendType() const override { return RHIBackendType::Metal; }

        // Upload Resources
        RHIStagingBufferRef CreateStagingBuffer(const RHIStagingBufferDesc& desc) override;
        RHIRingBufferRef CreateRingBuffer(const RHIRingBufferDesc& desc) override;

        // Memory Statistics
        RHIMemoryStats GetMemoryStats() const override;

        // Debug Resource Groups
        void BeginResourceGroup(const char* name) override;
        void EndResourceGroup() override;

        // =========================================================================
        // Metal-Specific Accessors
        // =========================================================================
        id<MTLDevice> GetMTLDevice() const { return m_device; }
        id<MTLCommandQueue> GetCommandQueue() const { return m_commandQueue; }

    private:
        void QueryCapabilities();

        id<MTLDevice> m_device = nil;
        id<MTLCommandQueue> m_commandQueue = nil;

        RHICapabilities m_capabilities;
        uint32 m_currentFrameIndex = 0;

        std::mutex m_submitMutex;

        // Triple buffering synchronization
        dispatch_semaphore_t m_frameSemaphore = nullptr;
        bool m_frameInFlight = false;
    };

} // namespace RVX
