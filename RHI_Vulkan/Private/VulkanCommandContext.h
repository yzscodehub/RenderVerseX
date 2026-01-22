#pragma once

#include "VulkanCommon.h"
#include "RHI/RHICommandContext.h"

namespace RVX
{
    class VulkanDevice;
    class VulkanPipeline;

    // =============================================================================
    // Vulkan Command Context
    // =============================================================================
    class VulkanCommandContext : public RHICommandContext
    {
    public:
        VulkanCommandContext(VulkanDevice* device, RHICommandQueueType type);
        ~VulkanCommandContext() override;

        void Begin() override;
        void End() override;
        void Reset() override;

        void BeginEvent(const char* name, uint32 color) override;
        void EndEvent() override;
        void SetMarker(const char* name, uint32 color) override;

        void BufferBarrier(const RHIBufferBarrier& barrier) override;
        void TextureBarrier(const RHITextureBarrier& barrier) override;
        void Barriers(std::span<const RHIBufferBarrier> bufferBarriers,
                      std::span<const RHITextureBarrier> textureBarriers) override;

        void BeginRenderPass(const RHIRenderPassDesc& desc) override;
        void EndRenderPass() override;

        void SetPipeline(RHIPipeline* pipeline) override;

        void SetVertexBuffer(uint32 slot, RHIBuffer* buffer, uint64 offset) override;
        void SetVertexBuffers(uint32 startSlot, std::span<RHIBuffer* const> buffers, std::span<const uint64> offsets) override;
        void SetIndexBuffer(RHIBuffer* buffer, RHIFormat format, uint64 offset) override;

        void SetDescriptorSet(uint32 slot, RHIDescriptorSet* set, std::span<const uint32> dynamicOffsets) override;
        void SetPushConstants(const void* data, uint32 size, uint32 offset) override;

        void SetViewport(const RHIViewport& viewport) override;
        void SetViewports(std::span<const RHIViewport> viewports) override;
        void SetScissor(const RHIRect& scissor) override;
        void SetScissors(std::span<const RHIRect> scissors) override;

        void Draw(uint32 vertexCount, uint32 instanceCount, uint32 firstVertex, uint32 firstInstance) override;
        void DrawIndexed(uint32 indexCount, uint32 instanceCount, uint32 firstIndex, int32 vertexOffset, uint32 firstInstance) override;
        void DrawIndirect(RHIBuffer* buffer, uint64 offset, uint32 drawCount, uint32 stride) override;
        void DrawIndexedIndirect(RHIBuffer* buffer, uint64 offset, uint32 drawCount, uint32 stride) override;

        void Dispatch(uint32 groupCountX, uint32 groupCountY, uint32 groupCountZ) override;
        void DispatchIndirect(RHIBuffer* buffer, uint64 offset) override;

        void CopyBuffer(RHIBuffer* src, RHIBuffer* dst, uint64 srcOffset, uint64 dstOffset, uint64 size) override;
        void CopyTexture(RHITexture* src, RHITexture* dst, const RHITextureCopyDesc& desc) override;
        void CopyBufferToTexture(RHIBuffer* src, RHITexture* dst, const RHIBufferTextureCopyDesc& desc) override;
        void CopyTextureToBuffer(RHITexture* src, RHIBuffer* dst, const RHIBufferTextureCopyDesc& desc) override;

        void BeginQuery(RHIQueryPool* pool, uint32 index) override;
        void EndQuery(RHIQueryPool* pool, uint32 index) override;
        void WriteTimestamp(RHIQueryPool* pool, uint32 index) override;
        void ResolveQueries(RHIQueryPool* pool, uint32 firstQuery, uint32 queryCount,
                           RHIBuffer* destBuffer, uint64 destOffset) override;
        void ResetQueries(RHIQueryPool* pool, uint32 firstQuery, uint32 queryCount) override;

        VkCommandBuffer GetCommandBuffer() const { return m_commandBuffer; }
        RHICommandQueueType GetQueueType() const { return m_queueType; }

        // Flush pending barriers before draw/dispatch/copy operations
        void FlushBarriers();

    private:
        VulkanDevice* m_device;
        RHICommandQueueType m_queueType;
        VkCommandPool m_commandPool = VK_NULL_HANDLE;
        VkCommandBuffer m_commandBuffer = VK_NULL_HANDLE;

        VulkanPipeline* m_currentPipeline = nullptr;
        bool m_isRecording = false;
        bool m_inRenderPass = false;

        // Pending barriers for batch submission (matching DX12 design)
        std::vector<VkImageMemoryBarrier2> m_pendingImageBarriers;
        std::vector<VkBufferMemoryBarrier2> m_pendingBufferBarriers;
    };

    // Factory and submit functions
    RHICommandContextRef CreateVulkanCommandContext(VulkanDevice* device, RHICommandQueueType type);
    void SubmitVulkanCommandContext(VulkanDevice* device, RHICommandContext* context, RHIFence* signalFence);
    void SubmitVulkanCommandContexts(VulkanDevice* device, std::span<RHICommandContext* const> contexts, RHIFence* signalFence);

} // namespace RVX
