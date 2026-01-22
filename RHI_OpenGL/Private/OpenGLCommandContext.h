#pragma once

#include "OpenGLCommon.h"
#include "OpenGLDebug.h"
#include "OpenGLStateCache.h"
#include "OpenGLCaches.h"
#include "OpenGLPipeline.h"
#include "RHI/RHICommandContext.h"
#include <vector>

namespace RVX
{
    class OpenGLDevice;
    class OpenGLGraphicsPipeline;
    class OpenGLComputePipeline;
    class OpenGLDescriptorSet;

    // =============================================================================
    // OpenGL Command Context
    // Implements immediate-mode execution of RHI commands
    // =============================================================================
    class OpenGLCommandContext : public RHICommandContext
    {
    public:
        OpenGLCommandContext(OpenGLDevice* device, RHICommandQueueType type);
        ~OpenGLCommandContext() override;

        // =========================================================================
        // Lifecycle
        // =========================================================================
        void Begin() override;
        void End() override;
        void Reset() override;

        // =========================================================================
        // Debug Markers
        // =========================================================================
        void BeginEvent(const char* name, uint32 color = 0) override;
        void EndEvent() override;
        void SetMarker(const char* name, uint32 color = 0) override;

        // =========================================================================
        // Resource Barriers (OpenGL doesn't need explicit barriers)
        // =========================================================================
        void BufferBarrier(const RHIBufferBarrier& barrier) override;
        void TextureBarrier(const RHITextureBarrier& barrier) override;
        void Barriers(std::span<const RHIBufferBarrier> bufferBarriers,
                     std::span<const RHITextureBarrier> textureBarriers) override;

        // Split Barriers (no-op for OpenGL)
        void BeginBarrier(const RHIBufferBarrier& barrier) override;
        void BeginBarrier(const RHITextureBarrier& barrier) override;
        void EndBarrier(const RHIBufferBarrier& barrier) override;
        void EndBarrier(const RHITextureBarrier& barrier) override;

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
        void SetVertexBuffer(uint32 slot, RHIBuffer* buffer, uint64 offset = 0) override;
        void SetVertexBuffers(uint32 startSlot, std::span<RHIBuffer* const> buffers, 
                             std::span<const uint64> offsets = {}) override;
        void SetIndexBuffer(RHIBuffer* buffer, RHIFormat format, uint64 offset = 0) override;

        // =========================================================================
        // Descriptor Sets
        // =========================================================================
        void SetDescriptorSet(uint32 slot, RHIDescriptorSet* set, 
                             std::span<const uint32> dynamicOffsets = {}) override;
        void SetPushConstants(const void* data, uint32 size, uint32 offset = 0) override;

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
        void Draw(uint32 vertexCount, uint32 instanceCount = 1, 
                 uint32 firstVertex = 0, uint32 firstInstance = 0) override;
        void DrawIndexed(uint32 indexCount, uint32 instanceCount = 1, 
                        uint32 firstIndex = 0, int32 vertexOffset = 0, 
                        uint32 firstInstance = 0) override;
        void DrawIndirect(RHIBuffer* buffer, uint64 offset, 
                         uint32 drawCount, uint32 stride) override;
        void DrawIndexedIndirect(RHIBuffer* buffer, uint64 offset, 
                                uint32 drawCount, uint32 stride) override;

        // =========================================================================
        // Compute Commands
        // =========================================================================
        void Dispatch(uint32 groupCountX, uint32 groupCountY, uint32 groupCountZ) override;
        void DispatchIndirect(RHIBuffer* buffer, uint64 offset) override;

        // =========================================================================
        // Copy Commands
        // =========================================================================
        void CopyBuffer(RHIBuffer* src, RHIBuffer* dst, 
                       uint64 srcOffset, uint64 dstOffset, uint64 size) override;
        void CopyTexture(RHITexture* src, RHITexture* dst, 
                        const RHITextureCopyDesc& desc = {}) override;
        void CopyBufferToTexture(RHIBuffer* src, RHITexture* dst, 
                                const RHIBufferTextureCopyDesc& desc) override;
        void CopyTextureToBuffer(RHITexture* src, RHIBuffer* dst, 
                                const RHIBufferTextureCopyDesc& desc) override;

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
        // Dynamic Render State
        // =========================================================================
        void SetStencilReference(uint32 reference) override;
        void SetBlendConstants(const float constants[4]) override;
        void SetDepthBias(float constantFactor, float slopeFactor, float clamp) override;
        void SetDepthBounds(float minDepth, float maxDepth) override;
        void SetStencilReferenceSeparate(uint32 frontRef, uint32 backRef) override;
        void SetLineWidth(float width) override;

        // =========================================================================
        // OpenGL Specific
        // =========================================================================
        RHICommandQueueType GetQueueType() const { return m_queueType; }

    private:
        void ApplyGraphicsPipelineState();
        void PrepareForDraw();
        void PrepareForDispatch();
        GLuint GetOrCreateVAO();

        OpenGLDevice* m_device = nullptr;
        RHICommandQueueType m_queueType;

        // Current state
        OpenGLGraphicsPipeline* m_currentGraphicsPipeline = nullptr;
        OpenGLComputePipeline* m_currentComputePipeline = nullptr;
        bool m_pipelineStateDirty = true;

        // Vertex/Index buffers
        struct VertexBufferBinding
        {
            RHIBuffer* buffer = nullptr;
            uint64 offset = 0;
        };
        std::array<VertexBufferBinding, 16> m_vertexBuffers;
        RHIBuffer* m_indexBuffer = nullptr;
        RHIFormat m_indexFormat = RHIFormat::R32_UINT;
        uint64 m_indexBufferOffset = 0;
        bool m_vertexBuffersDirty = true;

        // Descriptor sets with dynamic offsets
        struct DescriptorSetBinding
        {
            OpenGLDescriptorSet* set = nullptr;
            std::vector<uint32> dynamicOffsets;
        };
        std::array<DescriptorSetBinding, 4> m_descriptorSetBindings = {};
        bool m_descriptorSetsDirty = true;

        // Push constants
        OpenGLPushConstantBuffer m_pushConstantBuffer;
        bool m_pushConstantsDirty = false;

        // Render pass state
        bool m_inRenderPass = false;
        GLuint m_currentFBO = 0;
        bool m_ownsFBO = false;  // True if we own the FBO and must delete it
        RHIRect m_renderArea;

        // Cached VAO for current pipeline + vertex buffers
        GLuint m_currentVAO = 0;
        VAOCacheKey m_cachedVaoKey;
        bool m_vaoKeyDirty = true;
    };

} // namespace RVX
