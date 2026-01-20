#include "OpenGLCommandContext.h"
#include "OpenGLDevice.h"
#include "OpenGLResources.h"
#include "OpenGLDescriptor.h"
#include "OpenGLConversions.h"
#include "Core/Log.h"
#include <algorithm>

namespace RVX
{
    OpenGLCommandContext::OpenGLCommandContext(OpenGLDevice* device, RHICommandQueueType type)
        : m_device(device)
        , m_queueType(type)
    {
        RVX_RHI_DEBUG("Created OpenGL CommandContext (type: {})", static_cast<int>(type));
    }

    OpenGLCommandContext::~OpenGLCommandContext()
    {
        RVX_RHI_DEBUG("Destroyed OpenGL CommandContext");
    }

    // =========================================================================
    // Lifecycle
    // =========================================================================

    void OpenGLCommandContext::Begin()
    {
        // Reset state
        m_currentGraphicsPipeline = nullptr;
        m_currentComputePipeline = nullptr;
        m_pipelineStateDirty = true;
        m_vertexBuffersDirty = true;
        m_descriptorSetsDirty = true;
        m_inRenderPass = false;
        m_currentFBO = 0;
        m_currentVAO = 0;

        for (auto& vb : m_vertexBuffers)
        {
            vb.buffer = nullptr;
            vb.offset = 0;
        }
        for (auto& ds : m_descriptorSets)
        {
            ds = nullptr;
        }
    }

    void OpenGLCommandContext::End()
    {
        if (m_inRenderPass)
        {
            RVX_RHI_WARN("CommandContext::End called while still in render pass");
            EndRenderPass();
        }
    }

    void OpenGLCommandContext::Reset()
    {
        Begin();
    }

    // =========================================================================
    // Debug Markers
    // =========================================================================

    void OpenGLCommandContext::BeginEvent(const char* name, uint32 /*color*/)
    {
        OpenGLDebug::Get().PushDebugGroup(name);
    }

    void OpenGLCommandContext::EndEvent()
    {
        OpenGLDebug::Get().PopDebugGroup();
    }

    void OpenGLCommandContext::SetMarker(const char* name, uint32 /*color*/)
    {
        // OpenGL doesn't have markers, use a zero-length debug group
        OpenGLDebug::Get().PushDebugGroup(name);
        OpenGLDebug::Get().PopDebugGroup();
    }

    // =========================================================================
    // Resource Barriers (OpenGL handles synchronization automatically)
    // =========================================================================

    void OpenGLCommandContext::BufferBarrier(const RHIBufferBarrier& /*barrier*/)
    {
        // OpenGL handles synchronization implicitly
        // For compute shaders, we might need glMemoryBarrier
    }

    void OpenGLCommandContext::TextureBarrier(const RHITextureBarrier& /*barrier*/)
    {
        // OpenGL handles synchronization implicitly
    }

    void OpenGLCommandContext::Barriers(
        std::span<const RHIBufferBarrier> /*bufferBarriers*/,
        std::span<const RHITextureBarrier> /*textureBarriers*/)
    {
        // For compute shaders, issue a full memory barrier
        if (m_currentComputePipeline)
        {
            GL_CHECK(glMemoryBarrier(GL_ALL_BARRIER_BITS));
        }
    }

    // =========================================================================
    // Render Pass
    // =========================================================================

    void OpenGLCommandContext::BeginRenderPass(const RHIRenderPassDesc& desc)
    {
        GL_DEBUG_SCOPE("BeginRenderPass");

        if (m_inRenderPass)
        {
            RVX_RHI_WARN("BeginRenderPass called while already in render pass");
            EndRenderPass();
        }

        // Build FBO cache key
        FBOCacheKey fboKey;
        fboKey.colorAttachmentCount = desc.colorAttachmentCount;

        // Track if we're rendering to the default framebuffer (swap chain)
        // In OpenGL, texture handle 0 means the default framebuffer
        bool useDefaultFramebuffer = true;

        for (uint32 i = 0; i < desc.colorAttachmentCount; ++i)
        {
            const auto& attachment = desc.colorAttachments[i];
            if (attachment.view)
            {
                auto* glView = static_cast<OpenGLTextureView*>(attachment.view);
                auto* glTexture = static_cast<OpenGLTexture*>(glView->GetTexture());
                
                GLuint textureHandle = glView->GetHandle();
                fboKey.colorAttachments[i].texture = textureHandle;
                fboKey.colorAttachments[i].mipLevel = 0;  // TODO: From view
                fboKey.colorAttachments[i].arrayLayer = 0;
                fboKey.colorAttachments[i].format = glTexture->GetGLFormat().internalFormat;

                // If any color attachment has a non-zero texture, we need a custom FBO
                if (textureHandle != 0)
                {
                    useDefaultFramebuffer = false;
                }

                if (i == 0)
                {
                    fboKey.width = glTexture->GetWidth();
                    fboKey.height = glTexture->GetHeight();
                }
            }
        }

        if (desc.hasDepthStencil && desc.depthStencilAttachment.view)
        {
            auto* glView = static_cast<OpenGLTextureView*>(desc.depthStencilAttachment.view);
            auto* glTexture = static_cast<OpenGLTexture*>(glView->GetTexture());
            
            GLuint depthHandle = glView->GetHandle();
            fboKey.depthStencilTexture = depthHandle;
            fboKey.depthStencilMipLevel = 0;
            fboKey.depthStencilFormat = glTexture->GetGLFormat().internalFormat;

            // If depth attachment has a non-zero texture, we need a custom FBO
            if (depthHandle != 0)
            {
                useDefaultFramebuffer = false;
            }

            if (fboKey.colorAttachmentCount == 0)
            {
                fboKey.width = glTexture->GetWidth();
                fboKey.height = glTexture->GetHeight();
            }
        }

        // Get or create FBO using cache
        // Use default framebuffer (FBO 0) when rendering to swap chain back buffer
        if (useDefaultFramebuffer || (fboKey.colorAttachmentCount == 0 && fboKey.depthStencilTexture == 0))
        {
            // Default framebuffer (swap chain back buffer)
            m_currentFBO = 0;
            m_ownsFBO = false;
        }
        else
        {
            // Use FBO cache for off-screen render targets
            m_currentFBO = m_device->GetFBOCache().GetOrCreateFBO(
                fboKey, m_device->GetTotalFrameIndex(), "RenderPass_FBO");
            m_ownsFBO = false;  // Cache owns the FBO
        }

        // Bind FBO
        m_device->GetStateCache().BindFramebuffer(m_currentFBO);

        // Set render area
        m_renderArea = desc.renderArea;
        if (m_renderArea.width == 0 || m_renderArea.height == 0)
        {
            m_renderArea.x = 0;
            m_renderArea.y = 0;
            m_renderArea.width = fboKey.width;
            m_renderArea.height = fboKey.height;
        }

        // Set viewport to render area
        GLViewportState viewport;
        viewport.x = static_cast<float>(m_renderArea.x);
        viewport.y = static_cast<float>(m_renderArea.y);
        viewport.width = static_cast<float>(m_renderArea.width);
        viewport.height = static_cast<float>(m_renderArea.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        m_device->GetStateCache().SetViewport(viewport);

        // Clear attachments based on load ops
        for (uint32 i = 0; i < desc.colorAttachmentCount; ++i)
        {
            const auto& attachment = desc.colorAttachments[i];
            if (attachment.loadOp == RHILoadOp::Clear)
            {
                float clearColor[4] = {
                    attachment.clearColor.r,
                    attachment.clearColor.g,
                    attachment.clearColor.b,
                    attachment.clearColor.a
                };
                GL_CHECK(glClearNamedFramebufferfv(m_currentFBO, GL_COLOR, i, clearColor));
            }
        }

        if (desc.hasDepthStencil)
        {
            const auto& ds = desc.depthStencilAttachment;
            if (ds.depthLoadOp == RHILoadOp::Clear)
            {
                GL_CHECK(glClearNamedFramebufferfv(m_currentFBO, GL_DEPTH, 0, &ds.clearValue.depth));
            }
            if (ds.stencilLoadOp == RHILoadOp::Clear)
            {
                GLint stencil = ds.clearValue.stencil;
                GL_CHECK(glClearNamedFramebufferiv(m_currentFBO, GL_STENCIL, 0, &stencil));
            }
        }

        m_inRenderPass = true;
    }

    void OpenGLCommandContext::EndRenderPass()
    {
        if (!m_inRenderPass)
        {
            RVX_RHI_WARN("EndRenderPass called but not in render pass");
            return;
        }

        // FBO is managed by cache, no need to delete
        m_currentFBO = 0;
        m_ownsFBO = false;
        m_inRenderPass = false;
    }

    // =========================================================================
    // Pipeline Binding
    // =========================================================================

    void OpenGLCommandContext::SetPipeline(RHIPipeline* pipeline)
    {
        if (!pipeline) return;

        if (pipeline->IsCompute())
        {
            m_currentComputePipeline = static_cast<OpenGLComputePipeline*>(pipeline);
            m_currentGraphicsPipeline = nullptr;
        }
        else
        {
            m_currentGraphicsPipeline = static_cast<OpenGLGraphicsPipeline*>(pipeline);
            m_currentComputePipeline = nullptr;
        }

        m_pipelineStateDirty = true;
    }

    // =========================================================================
    // Vertex/Index Buffers
    // =========================================================================

    void OpenGLCommandContext::SetVertexBuffer(uint32 slot, RHIBuffer* buffer, uint64 offset)
    {
        if (slot >= m_vertexBuffers.size())
        {
            RVX_RHI_ERROR("Vertex buffer slot {} exceeds maximum", slot);
            return;
        }

        m_vertexBuffers[slot].buffer = buffer;
        m_vertexBuffers[slot].offset = offset;
        m_vertexBuffersDirty = true;
    }

    void OpenGLCommandContext::SetVertexBuffers(uint32 startSlot, std::span<RHIBuffer* const> buffers,
                                                std::span<const uint64> offsets)
    {
        for (size_t i = 0; i < buffers.size(); ++i)
        {
            uint64 offset = (i < offsets.size()) ? offsets[i] : 0;
            SetVertexBuffer(startSlot + static_cast<uint32>(i), buffers[i], offset);
        }
    }

    void OpenGLCommandContext::SetIndexBuffer(RHIBuffer* buffer, RHIFormat format, uint64 offset)
    {
        m_indexBuffer = buffer;
        m_indexFormat = format;
        m_indexBufferOffset = offset;
        m_vertexBuffersDirty = true;
    }

    // =========================================================================
    // Descriptor Sets
    // =========================================================================

    void OpenGLCommandContext::SetDescriptorSet(uint32 slot, RHIDescriptorSet* set,
                                                std::span<const uint32> /*dynamicOffsets*/)
    {
        if (slot >= m_descriptorSets.size())
        {
            RVX_RHI_ERROR("Descriptor set slot {} exceeds maximum", slot);
            return;
        }

        m_descriptorSets[slot] = static_cast<OpenGLDescriptorSet*>(set);
        m_descriptorSetsDirty = true;
    }

    void OpenGLCommandContext::SetPushConstants(const void* data, uint32 size, uint32 offset)
    {
        m_pushConstantBuffer.Update(data, size, offset);
    }

    // =========================================================================
    // Viewport/Scissor
    // =========================================================================

    void OpenGLCommandContext::SetViewport(const RHIViewport& viewport)
    {
        GLViewportState state;
        state.x = viewport.x;
        state.y = viewport.y;
        state.width = viewport.width;
        state.height = viewport.height;
        state.minDepth = viewport.minDepth;
        state.maxDepth = viewport.maxDepth;
        m_device->GetStateCache().SetViewport(state);
    }

    void OpenGLCommandContext::SetViewports(std::span<const RHIViewport> viewports)
    {
        if (!viewports.empty())
        {
            SetViewport(viewports[0]);
        }
    }

    void OpenGLCommandContext::SetScissor(const RHIRect& scissor)
    {
        GLScissorState state;
        state.x = scissor.x;
        state.y = scissor.y;
        state.width = scissor.width;
        state.height = scissor.height;
        m_device->GetStateCache().SetScissor(state);
    }

    void OpenGLCommandContext::SetScissors(std::span<const RHIRect> scissors)
    {
        if (!scissors.empty())
        {
            SetScissor(scissors[0]);
        }
    }

    // =========================================================================
    // Draw Commands
    // =========================================================================

    void OpenGLCommandContext::PrepareForDraw()
    {
        if (!m_currentGraphicsPipeline)
        {
            RVX_RHI_ERROR("No graphics pipeline bound for draw call");
            return;
        }

        // Bind program
        m_device->GetStateCache().BindProgram(m_currentGraphicsPipeline->GetProgramHandle());

        // Apply pipeline state if dirty
        if (m_pipelineStateDirty)
        {
            ApplyGraphicsPipelineState();
            m_pipelineStateDirty = false;
        }

        // Bind descriptor sets if dirty
        if (m_descriptorSetsDirty)
        {
            m_pushConstantBuffer.Bind();

            for (uint32 i = 0; i < m_descriptorSets.size(); ++i)
            {
                if (m_descriptorSets[i])
                {
                    m_descriptorSets[i]->Bind(m_device->GetStateCache(), i);
                }
            }
            m_descriptorSetsDirty = false;
        }

        // Get or create VAO
        GLuint vao = GetOrCreateVAO();
        m_device->GetStateCache().BindVAO(vao);
    }

    void OpenGLCommandContext::ApplyGraphicsPipelineState()
    {
        const auto& raster = m_currentGraphicsPipeline->GetRasterizerState();
        const auto& depth = m_currentGraphicsPipeline->GetDepthStencilState();
        const auto& blend = m_currentGraphicsPipeline->GetBlendState();

        // Rasterizer state
        GLRasterizerState rasterizerState;
        rasterizerState.cullEnabled = raster.cullMode != RHICullMode::None;
        rasterizerState.cullMode = ToGLCullMode(raster.cullMode);
        rasterizerState.frontFace = ToGLFrontFace(raster.frontFace);
        rasterizerState.polygonMode = ToGLPolygonMode(raster.fillMode);
        rasterizerState.depthClampEnabled = !raster.depthClipEnable;
        m_device->GetStateCache().SetRasterizerState(rasterizerState);

        // Depth state
        GLDepthState depthState;
        depthState.testEnabled = depth.depthTestEnable;
        depthState.writeEnabled = depth.depthWriteEnable;
        depthState.compareFunc = ToGLCompareFunc(depth.depthCompareOp);
        m_device->GetStateCache().SetDepthState(depthState);

        // Blend state (for first render target)
        GLBlendState blendState;
        blendState.enabled = blend.renderTargets[0].blendEnable;
        blendState.srcRGB = ToGLBlendFactor(blend.renderTargets[0].srcColorBlend);
        blendState.dstRGB = ToGLBlendFactor(blend.renderTargets[0].dstColorBlend);
        blendState.srcAlpha = ToGLBlendFactor(blend.renderTargets[0].srcAlphaBlend);
        blendState.dstAlpha = ToGLBlendFactor(blend.renderTargets[0].dstAlphaBlend);
        blendState.opRGB = ToGLBlendOp(blend.renderTargets[0].colorBlendOp);
        blendState.opAlpha = ToGLBlendOp(blend.renderTargets[0].alphaBlendOp);
        blendState.writeMask = blend.renderTargets[0].colorWriteMask;
        m_device->GetStateCache().SetBlendState(0, blendState);

        // Primitive topology
        m_device->GetStateCache().SetPrimitiveTopology(m_currentGraphicsPipeline->GetPrimitiveMode());
    }

    GLuint OpenGLCommandContext::GetOrCreateVAO()
    {
        const auto& inputLayout = m_currentGraphicsPipeline->GetInputLayout();

        // Build VAO cache key
        VAOCacheKey vaoKey;
        vaoKey.pipelineLayoutHash = m_currentGraphicsPipeline->GetInputLayoutHash();
        
        // Set up vertex buffer bindings in key
        uint32 maxSlot = 0;
        for (const auto& elem : inputLayout.elements)
        {
            maxSlot = std::max(maxSlot, elem.inputSlot + 1);
        }

        vaoKey.vertexBufferCount = 0;
        for (uint32 slot = 0; slot < maxSlot && slot < m_vertexBuffers.size(); ++slot)
        {
            if (m_vertexBuffers[slot].buffer)
            {
                auto* glBuffer = static_cast<OpenGLBuffer*>(m_vertexBuffers[slot].buffer);
                
                // Calculate stride for this slot
                GLsizei stride = 0;
                for (const auto& e : inputLayout.elements)
                {
                    if (e.inputSlot == slot)
                    {
                        stride += ToGLVertexFormat(e.format).size;
                    }
                }

                vaoKey.vertexBuffers[slot].buffer = glBuffer->GetHandle();
                vaoKey.vertexBuffers[slot].stride = stride;
                vaoKey.vertexBuffers[slot].offset = static_cast<GLintptr>(m_vertexBuffers[slot].offset);
                vaoKey.vertexBuffers[slot].divisor = 0;  // Updated below if per-instance
                vaoKey.vertexBufferCount = slot + 1;
            }
        }

        // Set up vertex attributes in key
        uint32 currentOffset = 0;
        vaoKey.attributeCount = static_cast<uint32>(inputLayout.elements.size());
        for (size_t i = 0; i < inputLayout.elements.size(); ++i)
        {
            const auto& elem = inputLayout.elements[i];
            auto vertexFormat = ToGLVertexFormat(elem.format);

            uint32 offset = (elem.alignedByteOffset == 0xFFFFFFFF) ? currentOffset : elem.alignedByteOffset;

            vaoKey.attributes[i].location = static_cast<uint32>(i);
            vaoKey.attributes[i].binding = elem.inputSlot;
            vaoKey.attributes[i].type = vertexFormat.type;
            vaoKey.attributes[i].components = vertexFormat.components;
            vaoKey.attributes[i].normalized = vertexFormat.normalized;
            vaoKey.attributes[i].offset = offset;

            if (elem.perInstance)
            {
                vaoKey.vertexBuffers[elem.inputSlot].divisor = elem.instanceDataStepRate;
            }

            currentOffset = offset + vertexFormat.size;
        }

        // Index buffer
        if (m_indexBuffer)
        {
            auto* glBuffer = static_cast<OpenGLBuffer*>(m_indexBuffer);
            vaoKey.indexBuffer = glBuffer->GetHandle();
        }

        // Get or create VAO from cache
        m_currentVAO = m_device->GetVAOCache().GetOrCreateVAO(
            vaoKey, m_device->GetTotalFrameIndex(), "DrawVAO");
        
        return m_currentVAO;
    }

    void OpenGLCommandContext::Draw(uint32 vertexCount, uint32 instanceCount,
                                   uint32 firstVertex, uint32 firstInstance)
    {
        PrepareForDraw();

        GLenum mode = m_device->GetStateCache().GetPrimitiveTopology();

        if (instanceCount > 1 || firstInstance > 0)
        {
            GL_CHECK(glDrawArraysInstancedBaseInstance(mode, firstVertex, vertexCount,
                                                       instanceCount, firstInstance));
        }
        else
        {
            GL_CHECK(glDrawArrays(mode, firstVertex, vertexCount));
        }

        GL_DEBUG_STAT_INC(drawCalls);
    }

    void OpenGLCommandContext::DrawIndexed(uint32 indexCount, uint32 instanceCount,
                                          uint32 firstIndex, int32 vertexOffset,
                                          uint32 firstInstance)
    {
        PrepareForDraw();

        GLenum mode = m_device->GetStateCache().GetPrimitiveTopology();
        GLenum indexType = ToGLIndexType(m_indexFormat);
        uint32 indexSize = GetIndexSize(m_indexFormat);
        const void* offset = reinterpret_cast<const void*>(
            m_indexBufferOffset + static_cast<size_t>(firstIndex) * indexSize);

        if (instanceCount > 1 || firstInstance > 0 || vertexOffset != 0)
        {
            GL_CHECK(glDrawElementsInstancedBaseVertexBaseInstance(
                mode, indexCount, indexType, offset,
                instanceCount, vertexOffset, firstInstance));
        }
        else
        {
            GL_CHECK(glDrawElements(mode, indexCount, indexType, offset));
        }

        GL_DEBUG_STAT_INC(drawCalls);
    }

    void OpenGLCommandContext::DrawIndirect(RHIBuffer* buffer, uint64 offset,
                                           uint32 drawCount, uint32 stride)
    {
        PrepareForDraw();

        auto* glBuffer = static_cast<OpenGLBuffer*>(buffer);
        GL_CHECK(glBindBuffer(GL_DRAW_INDIRECT_BUFFER, glBuffer->GetHandle()));

        GLenum mode = m_device->GetStateCache().GetPrimitiveTopology();

        GL_CHECK(glMultiDrawArraysIndirect(mode, reinterpret_cast<const void*>(offset),
                                          drawCount, stride));

        GL_DEBUG_STAT_INC(drawCalls);
    }

    void OpenGLCommandContext::DrawIndexedIndirect(RHIBuffer* buffer, uint64 offset,
                                                  uint32 drawCount, uint32 stride)
    {
        PrepareForDraw();

        auto* glBuffer = static_cast<OpenGLBuffer*>(buffer);
        GL_CHECK(glBindBuffer(GL_DRAW_INDIRECT_BUFFER, glBuffer->GetHandle()));

        GLenum mode = m_device->GetStateCache().GetPrimitiveTopology();
        GLenum indexType = ToGLIndexType(m_indexFormat);

        GL_CHECK(glMultiDrawElementsIndirect(mode, indexType,
                                            reinterpret_cast<const void*>(offset),
                                            drawCount, stride));

        GL_DEBUG_STAT_INC(drawCalls);
    }

    // =========================================================================
    // Compute Commands
    // =========================================================================

    void OpenGLCommandContext::PrepareForDispatch()
    {
        if (!m_currentComputePipeline)
        {
            RVX_RHI_ERROR("No compute pipeline bound for dispatch");
            return;
        }

        m_device->GetStateCache().BindProgram(m_currentComputePipeline->GetProgramHandle());

        // Bind descriptor sets
        if (m_descriptorSetsDirty)
        {
            m_pushConstantBuffer.Bind();

            for (uint32 i = 0; i < m_descriptorSets.size(); ++i)
            {
                if (m_descriptorSets[i])
                {
                    m_descriptorSets[i]->Bind(m_device->GetStateCache(), i);
                }
            }
            m_descriptorSetsDirty = false;
        }
    }

    void OpenGLCommandContext::Dispatch(uint32 groupCountX, uint32 groupCountY, uint32 groupCountZ)
    {
        PrepareForDispatch();

        GL_CHECK(glDispatchCompute(groupCountX, groupCountY, groupCountZ));
        GL_DEBUG_STAT_INC(dispatchCalls);
    }

    void OpenGLCommandContext::DispatchIndirect(RHIBuffer* buffer, uint64 offset)
    {
        PrepareForDispatch();

        auto* glBuffer = static_cast<OpenGLBuffer*>(buffer);
        GL_CHECK(glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, glBuffer->GetHandle()));
        GL_CHECK(glDispatchComputeIndirect(static_cast<GLintptr>(offset)));
        GL_DEBUG_STAT_INC(dispatchCalls);
    }

    // =========================================================================
    // Copy Commands
    // =========================================================================

    void OpenGLCommandContext::CopyBuffer(RHIBuffer* src, RHIBuffer* dst,
                                         uint64 srcOffset, uint64 dstOffset, uint64 size)
    {
        auto* srcGL = static_cast<OpenGLBuffer*>(src);
        auto* dstGL = static_cast<OpenGLBuffer*>(dst);

        GL_CHECK(glCopyNamedBufferSubData(srcGL->GetHandle(), dstGL->GetHandle(),
                                         srcOffset, dstOffset, size));
    }

    void OpenGLCommandContext::CopyTexture(RHITexture* src, RHITexture* dst,
                                          const RHITextureCopyDesc& desc)
    {
        auto* srcGL = static_cast<OpenGLTexture*>(src);
        auto* dstGL = static_cast<OpenGLTexture*>(dst);

        uint32 width = desc.width > 0 ? desc.width : srcGL->GetWidth();
        uint32 height = desc.height > 0 ? desc.height : srcGL->GetHeight();
        uint32 depth = desc.depth > 0 ? desc.depth : srcGL->GetDepth();

        GL_CHECK(glCopyImageSubData(
            srcGL->GetHandle(), srcGL->GetTarget(), 0, desc.srcX, desc.srcY, desc.srcZ,
            dstGL->GetHandle(), dstGL->GetTarget(), 0, desc.dstX, desc.dstY, desc.dstZ,
            width, height, depth));
    }

    void OpenGLCommandContext::CopyBufferToTexture(RHIBuffer* src, RHITexture* dst,
                                                  const RHIBufferTextureCopyDesc& desc)
    {
        auto* srcGL = static_cast<OpenGLBuffer*>(src);
        auto* dstGL = static_cast<OpenGLTexture*>(dst);
        auto glFormat = dstGL->GetGLFormat();

        // Bind PBO for transfer
        GL_CHECK(glBindBuffer(GL_PIXEL_UNPACK_BUFFER, srcGL->GetHandle()));

        uint32 width = desc.textureRegion.width > 0 ? desc.textureRegion.width : dstGL->GetWidth();
        uint32 height = desc.textureRegion.height > 0 ? desc.textureRegion.height : dstGL->GetHeight();

        GL_CHECK(glTextureSubImage2D(
            dstGL->GetHandle(), 0,
            desc.textureRegion.x, desc.textureRegion.y,
            width, height,
            glFormat.format, glFormat.type,
            reinterpret_cast<const void*>(desc.bufferOffset)));

        GL_CHECK(glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0));
    }

    void OpenGLCommandContext::CopyTextureToBuffer(RHITexture* src, RHIBuffer* dst,
                                                  const RHIBufferTextureCopyDesc& desc)
    {
        auto* srcGL = static_cast<OpenGLTexture*>(src);
        auto* dstGL = static_cast<OpenGLBuffer*>(dst);
        auto glFormat = srcGL->GetGLFormat();

        // Bind PBO for transfer
        GL_CHECK(glBindBuffer(GL_PIXEL_PACK_BUFFER, dstGL->GetHandle()));

        uint32 width = desc.textureRegion.width > 0 ? desc.textureRegion.width : srcGL->GetWidth();
        uint32 height = desc.textureRegion.height > 0 ? desc.textureRegion.height : srcGL->GetHeight();

        GL_CHECK(glGetTextureSubImage(
            srcGL->GetHandle(), 0,
            desc.textureRegion.x, desc.textureRegion.y, 0,
            width, height, 1,
            glFormat.format, glFormat.type,
            static_cast<GLsizei>(dstGL->GetSize() - desc.bufferOffset),
            reinterpret_cast<void*>(desc.bufferOffset)));

        GL_CHECK(glBindBuffer(GL_PIXEL_PACK_BUFFER, 0));
    }

} // namespace RVX
