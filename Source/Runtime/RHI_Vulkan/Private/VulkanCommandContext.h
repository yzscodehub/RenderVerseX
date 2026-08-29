#pragma once

#include "VulkanCommon.h"
#include "VulkanResources.h"
#include "RHI/RHICommandContext.h"

namespace RVX
{
    class VulkanDevice;
    class VulkanDescriptorSetLayout;
    class VulkanPipeline;
    struct RHIQueueSubmissionPlan;

    enum class VulkanTextureCopyContractStatus : uint8
    {
        Compatible = 0,
        UnknownFormat,
        AspectMismatch,
        FormatMismatch,
        UnsupportedStencilAspect,
    };

    struct VulkanTextureCopyContract
    {
        VulkanTextureCopyContractStatus status =
            VulkanTextureCopyContractStatus::UnknownFormat;
        VkImageAspectFlags aspectMask = 0;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return status == VulkanTextureCopyContractStatus::Compatible &&
                   aspectMask != 0;
        }
    };

    [[nodiscard]] VulkanTextureCopyContract ResolveVulkanTextureCopyContract(
        RHIFormat sourceFormat,
        RHIFormat destinationFormat) noexcept;

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

        // Split Barriers
        void BeginBarrier(const RHIBufferBarrier& barrier) override;
        void BeginBarrier(const RHITextureBarrier& barrier) override;
        void EndBarrier(const RHIBufferBarrier& barrier) override;
        void EndBarrier(const RHITextureBarrier& barrier) override;

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
        void DrawIndexedIndirectCount(RHIBuffer* buffer,
                                      uint64 offset,
                                      RHIBuffer* countBuffer,
                                      uint64 countOffset,
                                      uint32 maxDrawCount,
                                      uint32 stride) override;

        void Dispatch(uint32 groupCountX, uint32 groupCountY, uint32 groupCountZ) override;
        void DispatchIndirect(RHIBuffer* buffer, uint64 offset) override;

        // =========================================================================
        // Ray Tracing Commands
        // =========================================================================
        void BuildBottomLevelAccelerationStructure(
            RHIAccelerationStructure* dst,
            const RHIBottomLevelASDesc& desc,
            RHIBuffer* scratchBuffer,
            uint64 scratchOffset,
            RHIAccelerationStructure* src) override;
        void BuildTopLevelAccelerationStructure(
            RHIAccelerationStructure* dst,
            const RHITopLevelASDesc& desc,
            RHIBuffer* scratchBuffer,
            uint64 scratchOffset,
            RHIAccelerationStructure* src) override;
        void DispatchRays(const RHIDispatchRaysDesc& desc) override;
        void WriteAccelerationStructureCompactedSizes(
            RHIAccelerationStructureCompactedSizeQuery* query,
            uint32 firstQuery,
            std::span<RHIAccelerationStructure* const> accelerationStructures) override;
        void ResolveAccelerationStructureCompactedSizes(
            RHIAccelerationStructureCompactedSizeQuery* query,
            uint32 firstQuery,
            uint32 queryCount,
            RHIBuffer* readbackBuffer,
            uint64 readbackOffset) override;
        void CopyAccelerationStructureCompact(
            RHIAccelerationStructure* dst,
            RHIAccelerationStructure* src,
            uint64 expectedCompactedSize) override;

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

        // Dynamic Render State
        void SetStencilReference(uint32 reference) override;
        void SetBlendConstants(const float constants[4]) override;
        void SetDepthBias(float constantFactor, float slopeFactor, float clamp) override;
        void SetDepthBounds(float minDepth, float maxDepth) override;
        void SetStencilReferenceSeparate(uint32 frontRef, uint32 backRef) override;
        void SetLineWidth(float width) override;

        // Synchronization
        void SignalFence(RHIFence* fence, uint64 value) override;
        void WaitFence(RHIFence* fence, uint64 value) override;

        VkCommandBuffer GetCommandBuffer() const { return m_commandBuffer; }
        RHICommandQueueType GetQueueType() const override { return m_queueType; }

        // Flush pending barriers before draw/dispatch/copy operations
        void FlushBarriers();

        /**
         * @brief Whether this recording owns a valid, not-yet-submitted build
         * for the acceleration structure.
         *
         * This is intentionally context-local. Other contexts must use the
         * submitted state instead.
         */
        bool HasCompatiblePendingAccelerationStructureBuild(
            const RHIAccelerationStructure* accelerationStructure) const
            noexcept;

        /**
         * @brief Promote only builds accepted by a Vulkan queue submission.
         *
         * These are backend submission hooks rather than RHI commands. Until a
         * submission succeeds, a pending build is intentionally invisible to
         * other command contexts.
         */
        void CommitPendingAccelerationStructureBuilds();
        void DiscardPendingAccelerationStructureBuilds();
        void CommitPendingAccelerationStructureCompactionQueries();
        void DiscardPendingAccelerationStructureCompactionQueries();

    private:
        struct PendingAccelerationStructureBuild
        {
            RHIAccelerationStructureRef accelerationStructure;
            VulkanAccelerationStructureBuildSignature signature;
        };

        bool ValidateAccelerationStructureUpdateSignature(
            const char* operation,
            VulkanAccelerationStructure* source,
            const VulkanAccelerationStructureBuildSignature& update) const;
        bool HasPendingAccelerationStructureBuild(
            const VulkanAccelerationStructure* accelerationStructure,
            bool requireCompaction) const noexcept;
        void RecordPendingAccelerationStructureBuild(
            VulkanAccelerationStructure* accelerationStructure,
            VulkanAccelerationStructureBuildSignature signature);

        void InsertAccelerationStructureBarrier(
            RHIAccelerationStructure* accelerationStructure);

        VulkanDevice* m_device;
        RHICommandQueueType m_queueType;
        VkCommandPool m_commandPool = VK_NULL_HANDLE;
        VkCommandBuffer m_commandBuffer = VK_NULL_HANDLE;

        VulkanPipeline* m_currentPipeline = nullptr;
        std::vector<VulkanDescriptorSetLayout*>
            m_boundRayTracingDescriptorSetLayouts;
        std::vector<PendingAccelerationStructureBuild>
            m_pendingAccelerationStructureBuilds;
        struct PendingAccelerationStructureCompactionQuery
        {
            RHIAccelerationStructureCompactedSizeQueryRef query;
            RHIAccelerationStructureRef source;
            RHIBufferRef readback;
        };
        std::vector<PendingAccelerationStructureCompactionQuery>
            m_pendingAccelerationStructureCompactionQueries;
        bool m_isRecording = false;
        bool m_inRenderPass = false;

        // Pending barriers for batch submission (matching DX12 design)
        std::vector<VkImageMemoryBarrier2> m_pendingImageBarriers;
        std::vector<VkBufferMemoryBarrier2> m_pendingBufferBarriers;
    };

    // Factory and submit functions
    RHICommandContextRef CreateVulkanCommandContext(VulkanDevice* device, RHICommandQueueType type);
    uint64 SubmitVulkanCommandContext(VulkanDevice* device, RHICommandContext* context, RHIFence* signalFence);
    uint64 SubmitVulkanCommandContexts(VulkanDevice* device, std::span<RHICommandContext* const> contexts, RHIFence* signalFence);
    uint64 SubmitVulkanQueuePlan(VulkanDevice* device, const RHIQueueSubmissionPlan& plan, RHIFence* terminalFence);

} // namespace RVX
