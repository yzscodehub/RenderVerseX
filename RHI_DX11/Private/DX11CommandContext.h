#pragma once

#include "DX11Common.h"
#include "RHI/RHICommandContext.h"

namespace RVX
{
    class DX11Device;
    class DX11GraphicsPipeline;
    class DX11ComputePipeline;
    class DX11DescriptorSet;

    // =============================================================================
    // DX11 Command Context (Phase 4)
    // =============================================================================
    class DX11CommandContext : public RHICommandContext
    {
    public:
        DX11CommandContext(DX11Device* device, RHICommandQueueType queueType);
        ~DX11CommandContext() override;

        // =========================================================================
        // RHICommandContext Implementation
        // =========================================================================

        // Lifecycle
        void Begin() override;
        void End() override;
        void Reset() override;

        // Debug Markers
        void BeginEvent(const char* name, uint32 color = 0) override;
        void EndEvent() override;
        void SetMarker(const char* name, uint32 color = 0) override;

        // Resource Barriers (no-op for DX11)
        void BufferBarrier(const RHIBufferBarrier& barrier) override;
        void TextureBarrier(const RHITextureBarrier& barrier) override;
        void Barriers(std::span<const RHIBufferBarrier> bufferBarriers,
                     std::span<const RHITextureBarrier> textureBarriers) override;

        // Render Pass
        void BeginRenderPass(const RHIRenderPassDesc& desc) override;
        void EndRenderPass() override;

        // Pipeline
        void SetPipeline(RHIPipeline* pipeline) override;
        void SetDescriptorSet(uint32 slot, RHIDescriptorSet* set, std::span<const uint32> dynamicOffsets = {}) override;
        void SetPushConstants(const void* data, uint32 size, uint32 offset = 0) override;

        // Vertex/Index Buffers
        void SetVertexBuffer(uint32 slot, RHIBuffer* buffer, uint64 offset = 0) override;
        void SetVertexBuffers(uint32 startSlot, std::span<RHIBuffer* const> buffers, std::span<const uint64> offsets = {}) override;
        void SetIndexBuffer(RHIBuffer* buffer, RHIFormat format, uint64 offset = 0) override;

        // Viewport/Scissor
        void SetViewport(const RHIViewport& viewport) override;
        void SetViewports(std::span<const RHIViewport> viewports) override;
        void SetScissor(const RHIRect& scissor) override;
        void SetScissors(std::span<const RHIRect> scissors) override;

        // Draw
        void Draw(uint32 vertexCount, uint32 instanceCount = 1, uint32 firstVertex = 0, uint32 firstInstance = 0) override;
        void DrawIndexed(uint32 indexCount, uint32 instanceCount = 1, uint32 firstIndex = 0, int32 vertexOffset = 0, uint32 firstInstance = 0) override;
        void DrawIndirect(RHIBuffer* buffer, uint64 offset, uint32 drawCount, uint32 stride) override;
        void DrawIndexedIndirect(RHIBuffer* buffer, uint64 offset, uint32 drawCount, uint32 stride) override;

        // Dispatch
        void Dispatch(uint32 groupCountX, uint32 groupCountY, uint32 groupCountZ) override;
        void DispatchIndirect(RHIBuffer* buffer, uint64 offset) override;

        // Copy Operations
        void CopyBuffer(RHIBuffer* src, RHIBuffer* dst, uint64 srcOffset, uint64 dstOffset, uint64 size) override;
        void CopyTexture(RHITexture* src, RHITexture* dst, const RHITextureCopyDesc& desc = {}) override;
        void CopyBufferToTexture(RHIBuffer* src, RHITexture* dst, const RHIBufferTextureCopyDesc& desc) override;
        void CopyTextureToBuffer(RHITexture* src, RHIBuffer* dst, const RHIBufferTextureCopyDesc& desc) override;

        // =========================================================================
        // DX11 Specific
        // =========================================================================
        ID3D11DeviceContext* GetContext() const { return m_context.Get(); }
        ID3D11CommandList* GetCommandList() const { return m_commandList.Get(); }
        bool IsDeferred() const { return m_isDeferred; }

        void FlushBindings();
        void Submit();

    private:
        void ApplyDescriptorSets();

        DX11Device* m_device = nullptr;
        RHICommandQueueType m_queueType = RHICommandQueueType::Graphics;

        ComPtr<ID3D11DeviceContext> m_context;
        ComPtr<ID3D11CommandList> m_commandList;
        bool m_isDeferred = false;
        bool m_isOpen = false;

        // Current state
        DX11GraphicsPipeline* m_currentGraphicsPipeline = nullptr;
        DX11ComputePipeline* m_currentComputePipeline = nullptr;

        // Descriptor sets
        std::array<DX11DescriptorSet*, 4> m_descriptorSets = {};
        bool m_descriptorSetsDirty = false;

        // Current render targets
        std::array<ID3D11RenderTargetView*, DX11_MAX_RENDER_TARGETS> m_rtvs = {};
        ID3D11DepthStencilView* m_dsv = nullptr;
        uint32 m_rtvCount = 0;
    };

} // namespace RVX
