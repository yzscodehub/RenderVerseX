#pragma once

#include "RHI/RHIResources.h"
#include "RHI/RHIRenderPass.h"
#include <span>

namespace RVX
{
    // =============================================================================
    // Buffer Barrier
    // =============================================================================
    struct RHIBufferBarrier
    {
        RHIBuffer* buffer = nullptr;
        RHIResourceState stateBefore = RHIResourceState::Common;
        RHIResourceState stateAfter = RHIResourceState::Common;
        uint64 offset = 0;
        uint64 size = RVX_WHOLE_SIZE;
    };

    // =============================================================================
    // Texture Barrier
    // =============================================================================
    struct RHITextureBarrier
    {
        RHITexture* texture = nullptr;
        RHIResourceState stateBefore = RHIResourceState::Common;
        RHIResourceState stateAfter = RHIResourceState::Common;
        RHISubresourceRange subresourceRange;
    };

    // =============================================================================
    // Buffer-Texture Copy Description
    // =============================================================================
    struct RHIBufferTextureCopyDesc
    {
        uint64 bufferOffset = 0;
        uint32 bufferRowPitch = 0;    // 0 = tightly packed
        uint32 bufferImageHeight = 0; // 0 = tightly packed
        uint32 textureSubresource = 0;
        RHIRect textureRegion = {0, 0, 0, 0};  // 0,0,0,0 = full texture
        uint32 textureDepthSlice = 0;
    };

    // =============================================================================
    // Texture Copy Description
    // =============================================================================
    struct RHITextureCopyDesc
    {
        uint32 srcSubresource = 0;
        uint32 srcX = 0, srcY = 0, srcZ = 0;
        uint32 dstSubresource = 0;
        uint32 dstX = 0, dstY = 0, dstZ = 0;
        uint32 width = 0, height = 0, depth = 0;  // 0 = full size
    };

    // =============================================================================
    // Command Context Interface
    // =============================================================================
    class RHICommandContext : public RHIResource
    {
    public:
        virtual ~RHICommandContext() = default;

        // =========================================================================
        // Lifecycle
        // =========================================================================
        virtual void Begin() = 0;
        virtual void End() = 0;
        virtual void Reset() = 0;

        // =========================================================================
        // Debug Markers (PIX/RenderDoc support)
        // =========================================================================
        virtual void BeginEvent(const char* name, uint32 color = 0) = 0;
        virtual void EndEvent() = 0;
        virtual void SetMarker(const char* name, uint32 color = 0) = 0;

        // =========================================================================
        // Resource Barriers
        // =========================================================================
        virtual void BufferBarrier(const RHIBufferBarrier& barrier) = 0;
        virtual void TextureBarrier(const RHITextureBarrier& barrier) = 0;
        virtual void Barriers(
            std::span<const RHIBufferBarrier> bufferBarriers,
            std::span<const RHITextureBarrier> textureBarriers) = 0;

        // Convenience overloads
        void BufferBarrier(RHIBuffer* buffer, RHIResourceState before, RHIResourceState after)
        {
            BufferBarrier({buffer, before, after, 0, RVX_WHOLE_SIZE});
        }

        void TextureBarrier(RHITexture* texture, RHIResourceState before, RHIResourceState after)
        {
            TextureBarrier({texture, before, after, RHISubresourceRange::All()});
        }

        // =========================================================================
        // Render Pass
        // =========================================================================
        virtual void BeginRenderPass(const RHIRenderPassDesc& desc) = 0;
        virtual void EndRenderPass() = 0;

        // =========================================================================
        // Pipeline Binding
        // =========================================================================
        virtual void SetPipeline(RHIPipeline* pipeline) = 0;

        // =========================================================================
        // Vertex/Index Buffers
        // =========================================================================
        virtual void SetVertexBuffer(uint32 slot, RHIBuffer* buffer, uint64 offset = 0) = 0;
        virtual void SetVertexBuffers(uint32 startSlot, std::span<RHIBuffer* const> buffers, std::span<const uint64> offsets = {}) = 0;
        virtual void SetIndexBuffer(RHIBuffer* buffer, RHIFormat format, uint64 offset = 0) = 0;

        // =========================================================================
        // Descriptor Sets
        // =========================================================================
        virtual void SetDescriptorSet(uint32 slot, RHIDescriptorSet* set, std::span<const uint32> dynamicOffsets = {}) = 0;
        virtual void SetPushConstants(const void* data, uint32 size, uint32 offset = 0) = 0;

        // =========================================================================
        // Viewport/Scissor
        // =========================================================================
        virtual void SetViewport(const RHIViewport& viewport) = 0;
        virtual void SetViewports(std::span<const RHIViewport> viewports) = 0;
        virtual void SetScissor(const RHIRect& scissor) = 0;
        virtual void SetScissors(std::span<const RHIRect> scissors) = 0;

        // =========================================================================
        // Draw Commands
        // =========================================================================
        virtual void Draw(uint32 vertexCount, uint32 instanceCount = 1, uint32 firstVertex = 0, uint32 firstInstance = 0) = 0;
        virtual void DrawIndexed(uint32 indexCount, uint32 instanceCount = 1, uint32 firstIndex = 0, int32 vertexOffset = 0, uint32 firstInstance = 0) = 0;
        virtual void DrawIndirect(RHIBuffer* buffer, uint64 offset, uint32 drawCount, uint32 stride) = 0;
        virtual void DrawIndexedIndirect(RHIBuffer* buffer, uint64 offset, uint32 drawCount, uint32 stride) = 0;

        // =========================================================================
        // Compute Commands
        // =========================================================================
        virtual void Dispatch(uint32 groupCountX, uint32 groupCountY, uint32 groupCountZ) = 0;
        virtual void DispatchIndirect(RHIBuffer* buffer, uint64 offset) = 0;

        // =========================================================================
        // Copy Commands
        // =========================================================================
        virtual void CopyBuffer(RHIBuffer* src, RHIBuffer* dst, uint64 srcOffset, uint64 dstOffset, uint64 size) = 0;
        virtual void CopyTexture(RHITexture* src, RHITexture* dst, const RHITextureCopyDesc& desc = {}) = 0;
        virtual void CopyBufferToTexture(RHIBuffer* src, RHITexture* dst, const RHIBufferTextureCopyDesc& desc) = 0;
        virtual void CopyTextureToBuffer(RHITexture* src, RHIBuffer* dst, const RHIBufferTextureCopyDesc& desc) = 0;
    };

} // namespace RVX
