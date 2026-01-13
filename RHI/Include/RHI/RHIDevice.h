#pragma once

#include "RHI/RHIResources.h"
#include "RHI/RHIBuffer.h"
#include "RHI/RHITexture.h"
#include "RHI/RHISampler.h"
#include "RHI/RHIShader.h"
#include "RHI/RHIPipeline.h"
#include "RHI/RHIDescriptor.h"
#include "RHI/RHISwapChain.h"
#include "RHI/RHISynchronization.h"
#include "RHI/RHICapabilities.h"

namespace RVX
{
    // =============================================================================
    // Device Description
    // =============================================================================
    struct RHIDeviceDesc
    {
        bool enableDebugLayer = true;
        bool enableGPUValidation = false;
        uint32 preferredAdapterIndex = 0;  // 0 = auto-select
        const char* applicationName = "RenderVerseX";
    };

    // =============================================================================
    // Device Interface
    // =============================================================================
    class IRHIDevice
    {
    public:
        virtual ~IRHIDevice() = default;

        // =========================================================================
        // Resource Creation
        // =========================================================================
        virtual RHIBufferRef CreateBuffer(const RHIBufferDesc& desc) = 0;
        virtual RHITextureRef CreateTexture(const RHITextureDesc& desc) = 0;
        virtual RHITextureViewRef CreateTextureView(RHITexture* texture, const RHITextureViewDesc& desc = {}) = 0;
        virtual RHISamplerRef CreateSampler(const RHISamplerDesc& desc) = 0;
        virtual RHIShaderRef CreateShader(const RHIShaderDesc& desc) = 0;

        // =========================================================================
        // Pipeline Creation
        // =========================================================================
        virtual RHIDescriptorSetLayoutRef CreateDescriptorSetLayout(const RHIDescriptorSetLayoutDesc& desc) = 0;
        virtual RHIPipelineLayoutRef CreatePipelineLayout(const RHIPipelineLayoutDesc& desc) = 0;
        virtual RHIPipelineRef CreateGraphicsPipeline(const RHIGraphicsPipelineDesc& desc) = 0;
        virtual RHIPipelineRef CreateComputePipeline(const RHIComputePipelineDesc& desc) = 0;

        // =========================================================================
        // Descriptor Set
        // =========================================================================
        virtual RHIDescriptorSetRef CreateDescriptorSet(const RHIDescriptorSetDesc& desc) = 0;

        // =========================================================================
        // Command Context
        // =========================================================================
        virtual RHICommandContextRef CreateCommandContext(RHICommandQueueType type) = 0;
        virtual void SubmitCommandContext(RHICommandContext* context, RHIFence* signalFence = nullptr) = 0;
        virtual void SubmitCommandContexts(std::span<RHICommandContext* const> contexts, RHIFence* signalFence = nullptr) = 0;

        // =========================================================================
        // SwapChain
        // =========================================================================
        virtual RHISwapChainRef CreateSwapChain(const RHISwapChainDesc& desc) = 0;

        // =========================================================================
        // Synchronization
        // =========================================================================
        virtual RHIFenceRef CreateFence(uint64 initialValue = 0) = 0;
        virtual void WaitForFence(RHIFence* fence, uint64 value) = 0;
        virtual void WaitIdle() = 0;

        // =========================================================================
        // Frame Management
        // =========================================================================
        virtual void BeginFrame() = 0;
        virtual void EndFrame() = 0;
        virtual uint32 GetCurrentFrameIndex() const = 0;

        // =========================================================================
        // Capabilities
        // =========================================================================
        virtual const RHICapabilities& GetCapabilities() const = 0;
        virtual RHIBackendType GetBackendType() const = 0;
    };

    // =============================================================================
    // Device Factory
    // =============================================================================
    std::unique_ptr<IRHIDevice> CreateRHIDevice(RHIBackendType backend, const RHIDeviceDesc& desc);

} // namespace RVX
