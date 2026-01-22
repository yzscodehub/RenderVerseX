#pragma once

#include "DX12Common.h"
#include "DX12DescriptorHeap.h"
#include "RHI/RHICommandContext.h"

namespace RVX
{
    class DX12Device;
    class DX12Pipeline;

    // =============================================================================
    // DX12 Command Context Implementation
    // =============================================================================
    class DX12CommandContext : public RHICommandContext
    {
    public:
        DX12CommandContext(DX12Device* device, RHICommandQueueType type);
        ~DX12CommandContext() override;

        // =========================================================================
        // RHICommandContext Lifecycle
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
        void Barriers(
            std::span<const RHIBufferBarrier> bufferBarriers,
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
        // Query Commands
        // =========================================================================
        void BeginQuery(RHIQueryPool* pool, uint32 index) override;
        void EndQuery(RHIQueryPool* pool, uint32 index) override;
        void WriteTimestamp(RHIQueryPool* pool, uint32 index) override;
        void ResolveQueries(RHIQueryPool* pool, uint32 firstQuery, uint32 queryCount,
                            RHIBuffer* destBuffer, uint64 destOffset) override;
        void ResetQueries(RHIQueryPool* pool, uint32 firstQuery, uint32 queryCount) override;

        // =========================================================================
        // DX12 Specific
        // =========================================================================
        ID3D12GraphicsCommandList* GetCommandList() const { return m_commandList.Get(); }
        RHICommandQueueType GetQueueType() const { return m_queueType; }

    private:
        void FlushBarriers();

        DX12Device* m_device = nullptr;
        RHICommandQueueType m_queueType = RHICommandQueueType::Graphics;

        ComPtr<ID3D12CommandAllocator> m_commandAllocator;
        ComPtr<ID3D12GraphicsCommandList> m_commandList;

        // Current state
        DX12Pipeline* m_currentPipeline = nullptr;
        bool m_isRecording = false;
        bool m_inRenderPass = false;

        // Pending barriers (batched for efficiency)
        std::vector<D3D12_RESOURCE_BARRIER> m_pendingBarriers;
    };

    // Factory functions
    RHICommandContextRef CreateDX12CommandContext(DX12Device* device, RHICommandQueueType type);
    void SubmitDX12CommandContext(DX12Device* device, RHICommandContext* context, RHIFence* signalFence);
    void SubmitDX12CommandContexts(DX12Device* device, std::span<RHICommandContext* const> contexts, RHIFence* signalFence);

} // namespace RVX
