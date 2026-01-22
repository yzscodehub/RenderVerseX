#pragma once

#include "OpenGLCommon.h"
#include "OpenGLDebug.h"
#include "OpenGLStateCache.h"
#include "OpenGLDeletionQueue.h"
#include "OpenGLCaches.h"
#include "RHI/RHIDevice.h"
#include <mutex>
#include <thread>
#include <functional>
#include <vector>
#include <memory>

namespace RVX
{
    // =============================================================================
    // OpenGL Device Implementation
    // =============================================================================
    class OpenGLDevice : public IRHIDevice
    {
    public:
        OpenGLDevice(const RHIDeviceDesc& desc);
        ~OpenGLDevice() override;

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
        RHIBackendType GetBackendType() const override { return RHIBackendType::OpenGL; }

        // =========================================================================
        // OpenGL Specific
        // =========================================================================
        const OpenGLExtensions& GetExtensions() const { return m_extensions; }
        
        // Thread safety: check if on GL thread
        bool IsOnGLThread() const { return std::this_thread::get_id() == m_glThreadId; }

        // Internal accessors for resource classes
        OpenGLStateCache& GetStateCache() { return m_stateCache; }
        OpenGLDeletionQueue& GetDeletionQueue() { return m_deletionQueue; }
        OpenGLFramebufferCache& GetFBOCache() { return m_fboCache; }
        OpenGLVAOCache& GetVAOCache() { return m_vaoCache; }
        uint64 GetTotalFrameIndex() const { return m_frameIndex; }

    private:
        bool InitializeContext();
        void QueryCapabilities();
        void LoadExtensions();

        RHICapabilities m_capabilities;
        OpenGLExtensions m_extensions;
        uint32 m_currentFrameIndex = 0;
        uint64 m_frameIndex = 0;  // Total frame count for deletion queue
        
        std::thread::id m_glThreadId;
        bool m_initialized = false;

        // Subsystems
        OpenGLStateCache m_stateCache;
        OpenGLDeletionQueue m_deletionQueue;
        OpenGLFramebufferCache m_fboCache;
        OpenGLVAOCache m_vaoCache;
    };

} // namespace RVX
