#pragma once

#include "MetalCommon.h"
#include "RHI/RHICommandContext.h"

namespace RVX
{
    class MetalDevice;
    class MetalFence;

    // =============================================================================
    // Metal Command Context
    // Wraps MTLCommandBuffer and manages encoders
    // =============================================================================
    class MetalCommandContext : public RHICommandContext
    {
    public:
        MetalCommandContext(MetalDevice* device, RHICommandQueueType type);
        ~MetalCommandContext() override;

        // =========================================================================
        // Lifecycle
        // =========================================================================
        void Begin() override;
        void End() override;
        void Reset() override;

        // =========================================================================
        // Debug Markers
        // =========================================================================
        void BeginEvent(const char* name, uint32 color) override;
        void EndEvent() override;
        void SetMarker(const char* name, uint32 color) override;

        // =========================================================================
        // Resource Barriers
        // =========================================================================
        void BufferBarrier(const RHIBufferBarrier& barrier) override;
        void TextureBarrier(const RHITextureBarrier& barrier) override;
        void Barriers(std::span<const RHIBufferBarrier> bufferBarriers,
                      std::span<const RHITextureBarrier> textureBarriers) override;

        // =========================================================================
        // Render Pass
        // =========================================================================
        void BeginRenderPass(const RHIRenderPassDesc& desc) override;
        void EndRenderPass() override;

        // =========================================================================
        // Pipeline Binding
        // =========================================================================
        void SetPipeline(RHIPipeline* pipeline) override;

        // =========================================================================
        // Vertex/Index Buffers
        // =========================================================================
        void SetVertexBuffer(uint32 slot, RHIBuffer* buffer, uint64 offset) override;
        void SetVertexBuffers(uint32 startSlot, std::span<RHIBuffer* const> buffers, std::span<const uint64> offsets) override;
        void SetIndexBuffer(RHIBuffer* buffer, RHIFormat format, uint64 offset) override;

        // =========================================================================
        // Descriptor Sets
        // =========================================================================
        void SetDescriptorSet(uint32 slot, RHIDescriptorSet* set, std::span<const uint32> dynamicOffsets) override;
        void SetPushConstants(const void* data, uint32 size, uint32 offset) override;

        // =========================================================================
        // Viewport/Scissor
        // =========================================================================
        void SetViewport(const RHIViewport& viewport) override;
        void SetViewports(std::span<const RHIViewport> viewports) override;
        void SetScissor(const RHIRect& scissor) override;
        void SetScissors(std::span<const RHIRect> scissors) override;

        // =========================================================================
        // Draw Commands
        // =========================================================================
        void Draw(uint32 vertexCount, uint32 instanceCount, uint32 firstVertex, uint32 firstInstance) override;
        void DrawIndexed(uint32 indexCount, uint32 instanceCount, uint32 firstIndex, int32 vertexOffset, uint32 firstInstance) override;
        void DrawIndirect(RHIBuffer* buffer, uint64 offset, uint32 drawCount, uint32 stride) override;
        void DrawIndexedIndirect(RHIBuffer* buffer, uint64 offset, uint32 drawCount, uint32 stride) override;

        // =========================================================================
        // Compute Commands
        // =========================================================================
        void Dispatch(uint32 groupCountX, uint32 groupCountY, uint32 groupCountZ) override;
        void DispatchIndirect(RHIBuffer* buffer, uint64 offset) override;

        // =========================================================================
        // Copy Commands
        // =========================================================================
        void CopyBuffer(RHIBuffer* src, RHIBuffer* dst, uint64 srcOffset, uint64 dstOffset, uint64 size) override;
        void CopyTexture(RHITexture* src, RHITexture* dst, const RHITextureCopyDesc& desc) override;
        void CopyBufferToTexture(RHIBuffer* src, RHITexture* dst, const RHIBufferTextureCopyDesc& desc) override;
        void CopyTextureToBuffer(RHITexture* src, RHIBuffer* dst, const RHIBufferTextureCopyDesc& desc) override;

        // =========================================================================
        // Metal-Specific
        // =========================================================================
        void Submit(RHIFence* signalFence);
        id<MTLCommandBuffer> GetCommandBuffer() const { return m_commandBuffer; }

        // SwapChain presentation support
        void SetPendingDrawable(id<CAMetalDrawable> drawable) { m_pendingDrawable = drawable; }
        id<CAMetalDrawable> GetPendingDrawable() const { return m_pendingDrawable; }
        bool HasPendingDrawable() const { return m_pendingDrawable != nil; }

    private:
        void EndCurrentEncoder();
        void EnsureRenderEncoder();
        void EnsureComputeEncoder();
        void EnsureBlitEncoder();

        MetalDevice* m_device = nullptr;
        RHICommandQueueType m_queueType;

        id<MTLCommandBuffer> m_commandBuffer = nil;
        id<MTLRenderCommandEncoder> m_renderEncoder = nil;
        id<MTLComputeCommandEncoder> m_computeEncoder = nil;
        id<MTLBlitCommandEncoder> m_blitEncoder = nil;

        // Current state
        bool m_inRenderPass = false;
        id<MTLBuffer> m_currentIndexBuffer = nil;
        MTLIndexType m_currentIndexType = MTLIndexTypeUInt32;
        uint64 m_currentIndexBufferOffset = 0;

        // Current pipeline state
        class MetalGraphicsPipeline* m_currentGraphicsPipeline = nullptr;
        class MetalComputePipeline* m_currentComputePipeline = nullptr;

        // Pending drawable for presentation
        id<CAMetalDrawable> m_pendingDrawable = nil;
    };

} // namespace RVX
