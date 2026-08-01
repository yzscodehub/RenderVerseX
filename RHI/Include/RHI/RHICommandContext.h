#pragma once

#include "RHI/RHIAccess.h"
#include "RHI/RHIResources.h"
#include "RHI/RHIRenderPass.h"
#include "RHI/RHIQuery.h"
#include "RHI/RHIRayTracing.h"
#include <span>

namespace RVX
{
    /** @brief Order two placed resources that reuse overlapping heap memory. */
    struct RHIResourceAliasingBarrier
    {
        RHIResource* resourceBefore = nullptr;
        RHIResource* resourceAfter = nullptr;
    };

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
        RHIAccessSnapshot accessBefore;
        RHIAccessSnapshot accessAfter;
        RHIDependencyKind dependencyKind = RHIDependencyKind::None;
        RHIDiscardIntent discardIntent = RHIDiscardIntent::Preserve;
        bool hasScopedAccess = false;
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
        RHIAccessSnapshot accessBefore;
        RHIAccessSnapshot accessAfter;
        RHIDependencyKind dependencyKind = RHIDependencyKind::None;
        RHIDiscardIntent discardIntent = RHIDiscardIntent::Preserve;
        bool hasScopedAccess = false;
    };

    inline RHIBufferBarrier MakeRHIBufferBarrier(
        RHIBuffer* buffer,
        const RHIAccessSnapshot& before,
        const RHIAccessSnapshot& after,
        uint64 offset = 0,
        uint64 size = RVX_WHOLE_SIZE,
        RHIDiscardIntent discardIntent = RHIDiscardIntent::Preserve)
    {
        RHIBufferBarrier barrier;
        barrier.buffer = buffer;
        barrier.stateBefore = ProjectRHIResourceState(before);
        barrier.stateAfter = ProjectRHIResourceState(after);
        barrier.offset = offset;
        barrier.size = size;
        barrier.accessBefore = before;
        barrier.accessAfter = after;
        barrier.dependencyKind = ClassifyRHIDependency(before, after, discardIntent);
        barrier.discardIntent = discardIntent;
        barrier.hasScopedAccess = true;
        return barrier;
    }

    inline RHITextureBarrier MakeRHITextureBarrier(
        RHITexture* texture,
        const RHIAccessSnapshot& before,
        const RHIAccessSnapshot& after,
        const RHISubresourceRange& range = RHISubresourceRange::All(),
        RHIDiscardIntent discardIntent = RHIDiscardIntent::Preserve)
    {
        RHITextureBarrier barrier;
        barrier.texture = texture;
        barrier.stateBefore = ProjectRHIResourceState(before);
        barrier.stateAfter = ProjectRHIResourceState(after);
        barrier.subresourceRange = range;
        barrier.accessBefore = before;
        barrier.accessAfter = after;
        barrier.dependencyKind = ClassifyRHIDependency(before, after, discardIntent);
        barrier.discardIntent = discardIntent;
        barrier.hasScopedAccess = true;
        return barrier;
    }

    // =============================================================================
    // Buffer-Texture Copy Description
    // =============================================================================
    struct RHIBufferTextureCopyDesc
    {
        uint64 bufferOffset = 0;
        uint32 bufferRowPitch = 0;    // Bytes per row; 0 = tightly packed
        uint32 bufferImageHeight = 0; // Rows per image, or compressed block rows; 0 = tightly packed
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

    /**
     * @brief Standard indexed indirect draw command argument layout.
     *
     * Matches D3D12_DRAW_INDEXED_ARGUMENTS and VkDrawIndexedIndirectCommand.
     */
    struct IndirectDrawIndexedCommand
    {
        uint32 indexCount = 0;
        uint32 instanceCount = 0;
        uint32 firstIndex = 0;
        int32 vertexOffset = 0;
        uint32 firstInstance = 0;
    };

    // =============================================================================
    // Command Context Interface
    // =============================================================================
    class RHICommandContext : public RHIResource
    {
    public:
        virtual ~RHICommandContext() = default;

        /** @brief Logical queue that owns this command context. */
        virtual RHICommandQueueType GetQueueType() const
        {
            return static_cast<RHICommandQueueType>(0xFF);
        }

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
        /**
         * @brief Transition a buffer between resource states.
         * @note Equal legacy states may still carry a scoped memory dependency.
         */
        virtual void BufferBarrier(const RHIBufferBarrier& barrier) = 0;

        /**
         * @brief Transition a texture between resource states.
         * @note Equal legacy states may still carry a scoped memory dependency.
         */
        virtual void TextureBarrier(const RHITextureBarrier& barrier) = 0;

        /**
         * @brief Batch resource barriers.
         * @note Empty spans and null resources are no-work; equal states may carry memory dependencies.
         */
        virtual void Barriers(
            std::span<const RHIBufferBarrier> bufferBarriers,
            std::span<const RHITextureBarrier> textureBarriers) = 0;

        /**
         * @brief Batch explicit placed-resource aliasing barriers.
         *
         * Backends advertise real support through
         * RHICapabilities::supportsExplicitAliasingBarriers. The default is a
         * compatibility no-op and must never be used when the capability is false.
         */
        virtual void AliasingBarriers(
            std::span<const RHIResourceAliasingBarrier>) {}

        // Convenience overloads
        void BufferBarrier(RHIBuffer* buffer, RHIResourceState before, RHIResourceState after)
        {
            BufferBarrier({buffer, before, after, 0, RVX_WHOLE_SIZE});
        }

        void TextureBarrier(RHITexture* texture, RHIResourceState before, RHIResourceState after)
        {
            TextureBarrier({texture, before, after, RHISubresourceRange::All()});
        }

        void BufferBarrier(RHIBuffer* buffer,
                           const RHIAccessSnapshot& before,
                           const RHIAccessSnapshot& after,
                           uint64 offset = 0,
                           uint64 size = RVX_WHOLE_SIZE,
                           RHIDiscardIntent discardIntent = RHIDiscardIntent::Preserve)
        {
            BufferBarrier(MakeRHIBufferBarrier(buffer, before, after, offset, size, discardIntent));
        }

        void TextureBarrier(RHITexture* texture,
                            const RHIAccessSnapshot& before,
                            const RHIAccessSnapshot& after,
                            const RHISubresourceRange& range = RHISubresourceRange::All(),
                            RHIDiscardIntent discardIntent = RHIDiscardIntent::Preserve)
        {
            TextureBarrier(MakeRHITextureBarrier(texture, before, after, range, discardIntent));
        }

        // =========================================================================
        // Split Barriers (for hiding synchronization latency)
        // =========================================================================
        
        /**
         * @brief Begin a resource state transition (asynchronous)
         * 
         * Requires RHICapabilities::supportsSplitBarrier for real split-barrier
         * behavior. Backends without split barriers must leave BeginBarrier as
         * visible no-work and perform any full barrier fallback in EndBarrier.
         * 
         * @param barrier Buffer barrier description
         */
        virtual void BeginBarrier(const RHIBufferBarrier& barrier) = 0;

        /**
         * @brief Begin a resource state transition (asynchronous)
         * @param barrier Texture barrier description
         */
        virtual void BeginBarrier(const RHITextureBarrier& barrier) = 0;

        /**
         * @brief Complete a resource state transition
         * 
         * If split barriers are supported and BeginBarrier was called earlier,
         * this completes the transition. If split barriers are unsupported,
         * this may perform a full barrier when the backend supports explicit
         * resource barriers, otherwise it is an emulated/no-work path.
         * 
         * @param barrier Buffer barrier description
         */
        virtual void EndBarrier(const RHIBufferBarrier& barrier) = 0;

        /**
         * @brief Complete a resource state transition
         * @param barrier Texture barrier description
         */
        virtual void EndBarrier(const RHITextureBarrier& barrier) = 0;

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
        virtual void DrawIndexedIndirectCount(RHIBuffer* buffer,
                                              uint64 offset,
                                              RHIBuffer* countBuffer,
                                              uint64 countOffset,
                                              uint32 maxDrawCount,
                                              uint32 stride)
        {
            (void)countBuffer;
            (void)countOffset;
            DrawIndexedIndirect(buffer, offset, maxDrawCount, stride);
        }

        // =========================================================================
        // Compute Commands
        // =========================================================================
        virtual void Dispatch(uint32 groupCountX, uint32 groupCountY, uint32 groupCountZ) = 0;
        virtual void DispatchIndirect(RHIBuffer* buffer, uint64 offset) = 0;

        // =========================================================================
        // Ray Tracing Commands
        // =========================================================================
        virtual void BuildBottomLevelAccelerationStructure(
            RHIAccelerationStructure* dst,
            const RHIBottomLevelASDesc& desc,
            RHIBuffer* scratchBuffer,
            uint64 scratchOffset = 0,
            RHIAccelerationStructure* src = nullptr)
        {
            (void)dst;
            (void)desc;
            (void)scratchBuffer;
            (void)scratchOffset;
            (void)src;
        }

        virtual void BuildTopLevelAccelerationStructure(
            RHIAccelerationStructure* dst,
            const RHITopLevelASDesc& desc,
            RHIBuffer* scratchBuffer,
            uint64 scratchOffset = 0,
            RHIAccelerationStructure* src = nullptr)
        {
            (void)dst;
            (void)desc;
            (void)scratchBuffer;
            (void)scratchOffset;
            (void)src;
        }

        virtual void DispatchRays(const RHIDispatchRaysDesc& desc)
        {
            (void)desc;
        }

        // =========================================================================
        // Copy Commands
        // =========================================================================
        virtual void CopyBuffer(RHIBuffer* src, RHIBuffer* dst, uint64 srcOffset, uint64 dstOffset, uint64 size) = 0;
        virtual void CopyTexture(RHITexture* src, RHITexture* dst, const RHITextureCopyDesc& desc = {}) = 0;
        virtual void CopyBufferToTexture(RHIBuffer* src, RHITexture* dst, const RHIBufferTextureCopyDesc& desc) = 0;
        virtual void CopyTextureToBuffer(RHITexture* src, RHIBuffer* dst, const RHIBufferTextureCopyDesc& desc) = 0;

        // =========================================================================
        // Query Commands
        // =========================================================================
        
        /**
         * @brief Begin a query (for occlusion/pipeline stats)
         * @param pool The query pool
         * @param index Query index within the pool
         */
        virtual void BeginQuery(RHIQueryPool* pool, uint32 index) = 0;
        
        /**
         * @brief End a query
         * @param pool The query pool
         * @param index Query index within the pool
         */
        virtual void EndQuery(RHIQueryPool* pool, uint32 index) = 0;
        
        /**
         * @brief Write a GPU timestamp
         * @param pool The timestamp query pool
         * @param index Query index within the pool
         */
        virtual void WriteTimestamp(RHIQueryPool* pool, uint32 index) = 0;
        
        /**
         * @brief Resolve query results to a buffer
         * @param pool The query pool
         * @param firstQuery First query index
         * @param queryCount Number of queries to resolve
         * @param destBuffer Destination buffer
         * @param destOffset Offset in destination buffer
         */
        virtual void ResolveQueries(RHIQueryPool* pool, uint32 firstQuery, uint32 queryCount,
                                    RHIBuffer* destBuffer, uint64 destOffset) = 0;
        
        /**
         * @brief Reset queries before use
         * @param pool The query pool
         * @param firstQuery First query index
         * @param queryCount Number of queries to reset
         */
        virtual void ResetQueries(RHIQueryPool* pool, uint32 firstQuery, uint32 queryCount) = 0;

        // =========================================================================
        // Dynamic Render State
        // =========================================================================
        
        /**
         * @brief Set stencil reference value
         * @param reference Reference value (0-255)
         */
        virtual void SetStencilReference(uint32 reference) = 0;

        /**
         * @brief Set blend constant color
         * @param constants RGBA constant values [0.0, 1.0]
         */
        virtual void SetBlendConstants(const float constants[4]) = 0;

        /**
         * @brief Set depth bias for shadow mapping / decals to prevent Z-fighting
         * @param constantFactor Constant depth offset
         * @param slopeFactor Slope-based depth offset
         * @param clamp Maximum depth bias (0 = no clamp)
         */
        virtual void SetDepthBias(float constantFactor, float slopeFactor, float clamp = 0.0f) = 0;

        /**
         * @brief Set depth bounds test range (requires caps.supportsDepthBounds)
         * @param minDepth Minimum depth [0.0, 1.0]
         * @param maxDepth Maximum depth [0.0, 1.0]
         * @note No-op on backends that don't support depth bounds (DX11, OpenGL, Metal)
         */
        virtual void SetDepthBounds(float minDepth, float maxDepth) = 0;

        /**
         * @brief Set separate stencil reference values for front and back faces
         * @param frontRef Front face reference value
         * @param backRef Back face reference value
         */
        virtual void SetStencilReferenceSeparate(uint32 frontRef, uint32 backRef) = 0;

        /**
         * @brief Set line width for line primitives (OpenGL/Vulkan only)
         * @param width Line width in pixels
         * @note No-op on DX11/DX12/Metal (always 1.0)
         */
        virtual void SetLineWidth(float width) = 0;

        // =========================================================================
        // Synchronization (for cross-queue synchronization)
        // =========================================================================

        /**
         * @brief Signal a fence after all commands in this context complete
         * @param fence The fence to signal
         * @param value The fence value to signal
         * @note Requires RHICapabilities::supportsExplicitQueueFenceSignal for real queue synchronization.
         */
        virtual void SignalFence(RHIFence* fence, uint64 value) = 0;

        /**
         * @brief Wait for a fence to reach a value before proceeding
         * @param fence The fence to wait on
         * @param value The fence value to wait for
         * @note Requires RHICapabilities::supportsQueueFenceWait for real queue synchronization.
         */
        virtual void WaitFence(RHIFence* fence, uint64 value) = 0;
    };

} // namespace RVX
