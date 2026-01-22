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
        m_vaoKeyDirty = true;

        for (auto& vb : m_vertexBuffers)
        {
            vb.buffer = nullptr;
            vb.offset = 0;
        }
        for (auto& dsb : m_descriptorSetBindings)
        {
            dsb.set = nullptr;
            dsb.dynamicOffsets.clear();
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
    // Resource Barriers
    // =========================================================================

    // Helper to map RHIResourceState to GL memory barrier bits for buffers
    static GLbitfield GetBufferBarrierBits(RHIResourceState stateBefore, RHIResourceState stateAfter)
    {
        GLbitfield bits = 0;

        // Barrier needed when transitioning FROM UAV (after compute writes)
        if (stateBefore == RHIResourceState::UnorderedAccess)
        {
            bits |= GL_SHADER_STORAGE_BARRIER_BIT;
        }

        // Barrier needed based on destination state
        switch (stateAfter)
        {
            case RHIResourceState::VertexBuffer:
                bits |= GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT;
                break;
            case RHIResourceState::IndexBuffer:
                bits |= GL_ELEMENT_ARRAY_BARRIER_BIT;
                break;
            case RHIResourceState::ConstantBuffer:
                bits |= GL_UNIFORM_BARRIER_BIT;
                break;
            case RHIResourceState::ShaderResource:
                bits |= GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT;
                break;
            case RHIResourceState::UnorderedAccess:
                bits |= GL_SHADER_STORAGE_BARRIER_BIT;
                break;
            case RHIResourceState::IndirectArgument:
                bits |= GL_COMMAND_BARRIER_BIT;
                break;
            case RHIResourceState::CopyDest:
            case RHIResourceState::CopySource:
                bits |= GL_BUFFER_UPDATE_BARRIER_BIT;
                break;
            default:
                break;
        }

        return bits;
    }

    // Helper to map RHIResourceState to GL memory barrier bits for textures
    static GLbitfield GetTextureBarrierBits(RHIResourceState stateBefore, RHIResourceState stateAfter)
    {
        GLbitfield bits = 0;

        // Barrier needed when transitioning FROM UAV (after compute image writes)
        if (stateBefore == RHIResourceState::UnorderedAccess)
        {
            bits |= GL_SHADER_IMAGE_ACCESS_BARRIER_BIT;
        }

        // Barrier needed based on destination state
        switch (stateAfter)
        {
            case RHIResourceState::ShaderResource:
                bits |= GL_TEXTURE_FETCH_BARRIER_BIT;
                break;
            case RHIResourceState::UnorderedAccess:
                bits |= GL_SHADER_IMAGE_ACCESS_BARRIER_BIT;
                break;
            case RHIResourceState::RenderTarget:
            case RHIResourceState::DepthWrite:
            case RHIResourceState::DepthRead:
                bits |= GL_FRAMEBUFFER_BARRIER_BIT;
                break;
            case RHIResourceState::CopyDest:
            case RHIResourceState::CopySource:
                bits |= GL_TEXTURE_UPDATE_BARRIER_BIT;
                break;
            case RHIResourceState::Present:
                bits |= GL_FRAMEBUFFER_BARRIER_BIT;
                break;
            default:
                break;
        }

        return bits;
    }

    void OpenGLCommandContext::BufferBarrier(const RHIBufferBarrier& barrier)
    {
        GLbitfield bits = GetBufferBarrierBits(barrier.stateBefore, barrier.stateAfter);
        if (bits != 0)
        {
            GL_CHECK(glMemoryBarrier(bits));
        }
    }

    void OpenGLCommandContext::TextureBarrier(const RHITextureBarrier& barrier)
    {
        GLbitfield bits = GetTextureBarrierBits(barrier.stateBefore, barrier.stateAfter);
        if (bits != 0)
        {
            GL_CHECK(glMemoryBarrier(bits));
        }
    }

    void OpenGLCommandContext::Barriers(
        std::span<const RHIBufferBarrier> bufferBarriers,
        std::span<const RHITextureBarrier> textureBarriers)
    {
        GLbitfield combinedBits = 0;

        // Accumulate barrier bits for all buffer transitions
        for (const auto& barrier : bufferBarriers)
        {
            combinedBits |= GetBufferBarrierBits(barrier.stateBefore, barrier.stateAfter);
        }

        // Accumulate barrier bits for all texture transitions
        for (const auto& barrier : textureBarriers)
        {
            combinedBits |= GetTextureBarrierBits(barrier.stateBefore, barrier.stateAfter);
        }

        // Issue a single combined memory barrier
        if (combinedBits != 0)
        {
            GL_CHECK(glMemoryBarrier(combinedBits));
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
            auto* newPipeline = static_cast<OpenGLGraphicsPipeline*>(pipeline);
            if (m_currentGraphicsPipeline != newPipeline)
            {
                m_currentGraphicsPipeline = newPipeline;
                m_vaoKeyDirty = true;  // Input layout may have changed
            }
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

        if (m_vertexBuffers[slot].buffer != buffer || m_vertexBuffers[slot].offset != offset)
        {
            m_vertexBuffers[slot].buffer = buffer;
            m_vertexBuffers[slot].offset = offset;
            m_vertexBuffersDirty = true;
            m_vaoKeyDirty = true;  // VAO needs to be rebuilt
        }
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
        if (m_indexBuffer != buffer || m_indexFormat != format || m_indexBufferOffset != offset)
        {
            m_indexBuffer = buffer;
            m_indexFormat = format;
            m_indexBufferOffset = offset;
            m_vertexBuffersDirty = true;
            m_vaoKeyDirty = true;  // VAO includes index buffer
        }
    }

    // =========================================================================
    // Descriptor Sets
    // =========================================================================

    void OpenGLCommandContext::SetDescriptorSet(uint32 slot, RHIDescriptorSet* set,
                                                std::span<const uint32> dynamicOffsets)
    {
        if (slot >= m_descriptorSetBindings.size())
        {
            RVX_RHI_ERROR("Descriptor set slot {} exceeds maximum", slot);
            return;
        }

        m_descriptorSetBindings[slot].set = static_cast<OpenGLDescriptorSet*>(set);
        m_descriptorSetBindings[slot].dynamicOffsets.assign(dynamicOffsets.begin(), dynamicOffsets.end());
        m_descriptorSetsDirty = true;
    }

    void OpenGLCommandContext::SetPushConstants(const void* data, uint32 size, uint32 offset)
    {
        m_pushConstantBuffer.Update(data, size, offset);
        m_pushConstantsDirty = true;
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
        if (viewports.empty())
        {
            return;
        }

        if (viewports.size() == 1)
        {
            SetViewport(viewports[0]);
            return;
        }

        // Multi-viewport support using glViewportArrayv (OpenGL 4.1+)
        // Pack viewport data: x, y, width, height for each viewport
        std::vector<float> viewportData(viewports.size() * 4);
        std::vector<double> depthRanges(viewports.size() * 2);

        for (size_t i = 0; i < viewports.size(); ++i)
        {
            viewportData[i * 4 + 0] = viewports[i].x;
            viewportData[i * 4 + 1] = viewports[i].y;
            viewportData[i * 4 + 2] = viewports[i].width;
            viewportData[i * 4 + 3] = viewports[i].height;

            depthRanges[i * 2 + 0] = static_cast<double>(viewports[i].minDepth);
            depthRanges[i * 2 + 1] = static_cast<double>(viewports[i].maxDepth);
        }

        GL_CHECK(glViewportArrayv(0, static_cast<GLsizei>(viewports.size()), viewportData.data()));
        GL_CHECK(glDepthRangeArrayv(0, static_cast<GLsizei>(viewports.size()), depthRanges.data()));
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
        if (scissors.empty())
        {
            return;
        }

        if (scissors.size() == 1)
        {
            SetScissor(scissors[0]);
            return;
        }

        // Multi-scissor support using glScissorArrayv (OpenGL 4.1+)
        // Pack scissor data: x, y, width, height for each scissor
        std::vector<GLint> scissorData(scissors.size() * 4);

        for (size_t i = 0; i < scissors.size(); ++i)
        {
            scissorData[i * 4 + 0] = scissors[i].x;
            scissorData[i * 4 + 1] = scissors[i].y;
            scissorData[i * 4 + 2] = static_cast<GLint>(scissors[i].width);
            scissorData[i * 4 + 3] = static_cast<GLint>(scissors[i].height);
        }

        GL_CHECK(glScissorArrayv(0, static_cast<GLsizei>(scissors.size()), scissorData.data()));
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

        // Bind push constants if dirty
        if (m_pushConstantsDirty)
        {
            m_pushConstantBuffer.Bind();
            m_pushConstantsDirty = false;
        }

        // Bind descriptor sets if dirty
        if (m_descriptorSetsDirty)
        {
            for (uint32 i = 0; i < m_descriptorSetBindings.size(); ++i)
            {
                auto& binding = m_descriptorSetBindings[i];
                if (binding.set)
                {
                    binding.set->Bind(m_device->GetStateCache(), i, binding.dynamicOffsets);
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

        // Apply blend state to all render targets
        // If independentBlendEnable is false, RT[0] settings apply to all
        for (uint32 i = 0; i < RVX_MAX_RENDER_TARGETS; ++i)
        {
            // Use RT[0] settings for all if independent blend is disabled
            uint32 rtIndex = blend.independentBlendEnable ? i : 0;
            const auto& rtBlend = blend.renderTargets[rtIndex];

            GLBlendState blendState;
            blendState.enabled = rtBlend.blendEnable;
            blendState.srcRGB = ToGLBlendFactor(rtBlend.srcColorBlend);
            blendState.dstRGB = ToGLBlendFactor(rtBlend.dstColorBlend);
            blendState.srcAlpha = ToGLBlendFactor(rtBlend.srcAlphaBlend);
            blendState.dstAlpha = ToGLBlendFactor(rtBlend.dstAlphaBlend);
            blendState.opRGB = ToGLBlendOp(rtBlend.colorBlendOp);
            blendState.opAlpha = ToGLBlendOp(rtBlend.alphaBlendOp);
            blendState.writeMask = rtBlend.colorWriteMask;
            m_device->GetStateCache().SetBlendState(i, blendState);
        }

        // Primitive topology
        m_device->GetStateCache().SetPrimitiveTopology(m_currentGraphicsPipeline->GetPrimitiveMode());
    }

    GLuint OpenGLCommandContext::GetOrCreateVAO()
    {
        // Only rebuild the VAO key if something changed
        if (m_vaoKeyDirty)
        {
            const auto& inputLayout = m_currentGraphicsPipeline->GetInputLayout();

            // Reset and build VAO cache key
            m_cachedVaoKey = VAOCacheKey{};
            m_cachedVaoKey.pipelineLayoutHash = m_currentGraphicsPipeline->GetInputLayoutHash();
            
            // Set up vertex buffer bindings in key
            uint32 maxSlot = 0;
            for (const auto& elem : inputLayout.elements)
            {
                maxSlot = std::max(maxSlot, elem.inputSlot + 1);
            }

            m_cachedVaoKey.vertexBufferCount = 0;
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

                    m_cachedVaoKey.vertexBuffers[slot].buffer = glBuffer->GetHandle();
                    m_cachedVaoKey.vertexBuffers[slot].stride = stride;
                    m_cachedVaoKey.vertexBuffers[slot].offset = static_cast<GLintptr>(m_vertexBuffers[slot].offset);
                    m_cachedVaoKey.vertexBuffers[slot].divisor = 0;  // Updated below if per-instance
                    m_cachedVaoKey.vertexBufferCount = slot + 1;
                }
            }

            // Set up vertex attributes in key
            uint32 currentOffset = 0;
            m_cachedVaoKey.attributeCount = static_cast<uint32>(inputLayout.elements.size());
            for (size_t i = 0; i < inputLayout.elements.size(); ++i)
            {
                const auto& elem = inputLayout.elements[i];
                auto vertexFormat = ToGLVertexFormat(elem.format);

                uint32 offset = (elem.alignedByteOffset == 0xFFFFFFFF) ? currentOffset : elem.alignedByteOffset;

                m_cachedVaoKey.attributes[i].location = static_cast<uint32>(i);
                m_cachedVaoKey.attributes[i].binding = elem.inputSlot;
                m_cachedVaoKey.attributes[i].type = vertexFormat.type;
                m_cachedVaoKey.attributes[i].components = vertexFormat.components;
                m_cachedVaoKey.attributes[i].normalized = vertexFormat.normalized;
                m_cachedVaoKey.attributes[i].offset = offset;

                if (elem.perInstance)
                {
                    m_cachedVaoKey.vertexBuffers[elem.inputSlot].divisor = elem.instanceDataStepRate;
                }

                currentOffset = offset + vertexFormat.size;
            }

            // Index buffer
            if (m_indexBuffer)
            {
                auto* glBuffer = static_cast<OpenGLBuffer*>(m_indexBuffer);
                m_cachedVaoKey.indexBuffer = glBuffer->GetHandle();
            }
            else
            {
                m_cachedVaoKey.indexBuffer = 0;
            }

            m_vaoKeyDirty = false;
        }

        // Get or create VAO from cache using the cached key
        m_currentVAO = m_device->GetVAOCache().GetOrCreateVAO(
            m_cachedVaoKey, m_device->GetTotalFrameIndex(), "DrawVAO");
        
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

        // Bind push constants if dirty
        if (m_pushConstantsDirty)
        {
            m_pushConstantBuffer.Bind();
            m_pushConstantsDirty = false;
        }

        // Bind descriptor sets
        if (m_descriptorSetsDirty)
        {
            for (uint32 i = 0; i < m_descriptorSetBindings.size(); ++i)
            {
                auto& binding = m_descriptorSetBindings[i];
                if (binding.set)
                {
                    binding.set->Bind(m_device->GetStateCache(), i, binding.dynamicOffsets);
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

        // Extract mip level from subresource index
        // Subresource = mipLevel + (arrayLayer * mipLevels)
        uint32 srcMipLevel = desc.srcSubresource % srcGL->GetMipLevels();
        uint32 dstMipLevel = desc.dstSubresource % dstGL->GetMipLevels();

        // Calculate dimensions at the specified mip level if not provided
        uint32 srcMipWidth = std::max(1u, srcGL->GetWidth() >> srcMipLevel);
        uint32 srcMipHeight = std::max(1u, srcGL->GetHeight() >> srcMipLevel);
        uint32 srcMipDepth = std::max(1u, srcGL->GetDepth() >> srcMipLevel);

        uint32 width = desc.width > 0 ? desc.width : srcMipWidth;
        uint32 height = desc.height > 0 ? desc.height : srcMipHeight;
        uint32 depth = desc.depth > 0 ? desc.depth : srcMipDepth;

        GL_CHECK(glCopyImageSubData(
            srcGL->GetHandle(), srcGL->GetTarget(), static_cast<GLint>(srcMipLevel), 
            desc.srcX, desc.srcY, desc.srcZ,
            dstGL->GetHandle(), dstGL->GetTarget(), static_cast<GLint>(dstMipLevel), 
            desc.dstX, desc.dstY, desc.dstZ,
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

    // =========================================================================
    // Query Commands (OpenGL stubs - queries not yet implemented)
    // =========================================================================

    void OpenGLCommandContext::BeginQuery(RHIQueryPool* /*pool*/, uint32 /*index*/)
    {
        // TODO: Implement OpenGL query support using glBeginQuery
        RVX_RHI_WARN("OpenGLCommandContext::BeginQuery not yet implemented");
    }

    void OpenGLCommandContext::EndQuery(RHIQueryPool* /*pool*/, uint32 /*index*/)
    {
        // TODO: Implement OpenGL query support using glEndQuery
        RVX_RHI_WARN("OpenGLCommandContext::EndQuery not yet implemented");
    }

    void OpenGLCommandContext::WriteTimestamp(RHIQueryPool* /*pool*/, uint32 /*index*/)
    {
        // TODO: Implement OpenGL timestamp queries using glQueryCounter
        RVX_RHI_WARN("OpenGLCommandContext::WriteTimestamp not yet implemented");
    }

    void OpenGLCommandContext::ResolveQueries(RHIQueryPool* /*pool*/, uint32 /*firstQuery*/, 
                                              uint32 /*queryCount*/, RHIBuffer* /*destBuffer*/, 
                                              uint64 /*destOffset*/)
    {
        // TODO: Implement query result retrieval using glGetQueryObjectui64v
        RVX_RHI_WARN("OpenGLCommandContext::ResolveQueries not yet implemented");
    }

    void OpenGLCommandContext::ResetQueries(RHIQueryPool* /*pool*/, uint32 /*firstQuery*/, 
                                            uint32 /*queryCount*/)
    {
        // OpenGL queries don't require explicit reset
    }

    // =========================================================================
    // Dynamic Render State
    // =========================================================================

    void OpenGLCommandContext::SetStencilReference(uint32 reference)
    {
        // Note: OpenGL requires setting stencil function again with new reference
        // This sets both front and back faces to the same reference
        // The actual compare function and mask come from the current pipeline state
        // For simplicity, we use GL_ALWAYS and 0xFF - the pipeline should override this
        GL_CHECK(glStencilFunc(GL_ALWAYS, static_cast<GLint>(reference), 0xFF));
    }

    void OpenGLCommandContext::SetBlendConstants(const float constants[4])
    {
        GL_CHECK(glBlendColor(constants[0], constants[1], constants[2], constants[3]));
    }

    void OpenGLCommandContext::SetDepthBias(float constantFactor, float slopeFactor, float clamp)
    {
        // OpenGL uses glPolygonOffset for depth bias
        // Note: clamp is not supported in core OpenGL
        (void)clamp;
        GL_CHECK(glPolygonOffset(slopeFactor, constantFactor));
    }

    void OpenGLCommandContext::SetDepthBounds(float minDepth, float maxDepth)
    {
        // Depth bounds testing is not supported in core OpenGL
        // Available as EXT_depth_bounds_test extension but rarely used
        (void)minDepth;
        (void)maxDepth;
    }

    void OpenGLCommandContext::SetStencilReferenceSeparate(uint32 frontRef, uint32 backRef)
    {
        // OpenGL supports separate stencil functions
        GL_CHECK(glStencilFuncSeparate(GL_FRONT, GL_ALWAYS, static_cast<GLint>(frontRef), 0xFF));
        GL_CHECK(glStencilFuncSeparate(GL_BACK, GL_ALWAYS, static_cast<GLint>(backRef), 0xFF));
    }

    void OpenGLCommandContext::SetLineWidth(float width)
    {
        GL_CHECK(glLineWidth(width));
    }

    // =============================================================================
    // Split Barriers (no-op for OpenGL)
    // =============================================================================
    void OpenGLCommandContext::BeginBarrier(const RHIBufferBarrier& barrier)
    {
        // OpenGL handles barriers automatically
        (void)barrier;
    }

    void OpenGLCommandContext::BeginBarrier(const RHITextureBarrier& barrier)
    {
        // OpenGL handles barriers automatically
        (void)barrier;
    }

    void OpenGLCommandContext::EndBarrier(const RHIBufferBarrier& barrier)
    {
        // OpenGL handles barriers automatically
        (void)barrier;
    }

    void OpenGLCommandContext::EndBarrier(const RHITextureBarrier& barrier)
    {
        // OpenGL handles barriers automatically
        (void)barrier;
    }

} // namespace RVX
