#include "OpenGLStateCache.h"

namespace RVX
{
    OpenGLStateCache::OpenGLStateCache()
    {
        Invalidate();
    }

    void OpenGLStateCache::Invalidate()
    {
        // Reset all cached state to unknown/default values
        m_boundProgram = 0;
        m_boundVAO = 0;
        m_boundFBO = 0;
        m_boundReadFBO = 0;
        m_boundDrawFBO = 0;
        m_boundIndexBuffer = 0;
        m_activeTextureSlot = 0;

        for (auto& binding : m_uboBindings)
            binding = {};
        for (auto& binding : m_ssboBindings)
            binding = {};
        for (auto& binding : m_textureBindings)
            binding = {};
        for (auto& sampler : m_samplerBindings)
            sampler = 0;
        for (auto& binding : m_vertexBufferBindings)
            binding = {};
        for (auto& blend : m_blendStates)
            blend = {};

        m_viewport = {};
        m_scissor = {};
        m_depthState = {};
        m_stencilState = {};
        m_rasterizerState = {};
        m_sampleShadingState = {};
        m_primitiveMode = GL_TRIANGLES;
        m_lineWidth = 1.0f;
        m_polygonOffsetFactor = 0.0f;
        m_polygonOffsetUnits = 0.0f;

        RVX_RHI_DEBUG("OpenGL State Cache invalidated");
    }

    void OpenGLStateCache::BindProgram(GLuint program)
    {
        if (m_boundProgram != program)
        {
            GL_CHECK(glUseProgram(program));
            m_boundProgram = program;
            GL_DEBUG_STAT_INC(programBinds);
        }
    }

    void OpenGLStateCache::BindVAO(GLuint vao)
    {
        if (m_boundVAO != vao)
        {
            GL_CHECK(glBindVertexArray(vao));
            m_boundVAO = vao;
            GL_DEBUG_STAT_INC(vaoBinds);
        }
    }

    void OpenGLStateCache::BindFramebuffer(GLuint fbo)
    {
        if (m_boundFBO != fbo)
        {
            GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, fbo));
            m_boundFBO = fbo;
            m_boundReadFBO = fbo;
            m_boundDrawFBO = fbo;
            GL_DEBUG_STAT_INC(fboBinds);
        }
    }

    void OpenGLStateCache::BindReadFramebuffer(GLuint fbo)
    {
        if (m_boundReadFBO != fbo)
        {
            GL_CHECK(glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo));
            m_boundReadFBO = fbo;
        }
    }

    void OpenGLStateCache::BindDrawFramebuffer(GLuint fbo)
    {
        if (m_boundDrawFBO != fbo)
        {
            GL_CHECK(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo));
            m_boundDrawFBO = fbo;
            GL_DEBUG_STAT_INC(fboBinds);
        }
    }

    void OpenGLStateCache::BindUniformBuffer(uint32 slot, GLuint buffer, GLintptr offset, GLsizeiptr size)
    {
        if (slot >= MAX_UBO_SLOTS)
        {
            RVX_RHI_ERROR("UBO slot {} exceeds maximum {}", slot, MAX_UBO_SLOTS);
            return;
        }

        auto& binding = m_uboBindings[slot];
        if (binding.buffer != buffer || binding.offset != offset || binding.size != size)
        {
            if (size > 0)
            {
                GL_CHECK(glBindBufferRange(GL_UNIFORM_BUFFER, slot, buffer, offset, size));
            }
            else
            {
                GL_CHECK(glBindBufferBase(GL_UNIFORM_BUFFER, slot, buffer));
            }
            binding.buffer = buffer;
            binding.offset = offset;
            binding.size = size;
            GL_DEBUG_STAT_INC(bufferBinds);
        }
    }

    void OpenGLStateCache::BindStorageBuffer(uint32 slot, GLuint buffer, GLintptr offset, GLsizeiptr size)
    {
        if (slot >= MAX_SSBO_SLOTS)
        {
            RVX_RHI_ERROR("SSBO slot {} exceeds maximum {}", slot, MAX_SSBO_SLOTS);
            return;
        }

        auto& binding = m_ssboBindings[slot];
        if (binding.buffer != buffer || binding.offset != offset || binding.size != size)
        {
            if (size > 0)
            {
                GL_CHECK(glBindBufferRange(GL_SHADER_STORAGE_BUFFER, slot, buffer, offset, size));
            }
            else
            {
                GL_CHECK(glBindBufferBase(GL_SHADER_STORAGE_BUFFER, slot, buffer));
            }
            binding.buffer = buffer;
            binding.offset = offset;
            binding.size = size;
            GL_DEBUG_STAT_INC(bufferBinds);
        }
    }

    void OpenGLStateCache::BindTexture(uint32 slot, GLenum target, GLuint texture)
    {
        if (slot >= MAX_TEXTURE_SLOTS)
        {
            RVX_RHI_ERROR("Texture slot {} exceeds maximum {}", slot, MAX_TEXTURE_SLOTS);
            return;
        }

        auto& binding = m_textureBindings[slot];
        if (binding.texture != texture || binding.target != target)
        {
            // Use DSA if available (OpenGL 4.5+)
            GL_CHECK(glBindTextureUnit(slot, texture));
            binding.target = target;
            binding.texture = texture;
            GL_DEBUG_STAT_INC(textureBinds);
        }
    }

    void OpenGLStateCache::BindSampler(uint32 slot, GLuint sampler)
    {
        if (slot >= MAX_TEXTURE_SLOTS)
        {
            RVX_RHI_ERROR("Sampler slot {} exceeds maximum {}", slot, MAX_TEXTURE_SLOTS);
            return;
        }

        if (m_samplerBindings[slot] != sampler)
        {
            GL_CHECK(glBindSampler(slot, sampler));
            m_samplerBindings[slot] = sampler;
        }
    }

    void OpenGLStateCache::BindImageTexture(uint32 slot, GLuint texture, GLint level, 
                                           GLboolean layered, GLint layer, 
                                           GLenum access, GLenum format)
    {
        // Image bindings are always set (no caching for now due to complexity)
        GL_CHECK(glBindImageTexture(slot, texture, level, layered, layer, access, format));
    }

    void OpenGLStateCache::BindVertexBuffer(uint32 slot, GLuint buffer, GLintptr offset, GLsizei stride)
    {
        if (slot >= MAX_VERTEX_BUFFERS)
        {
            RVX_RHI_ERROR("Vertex buffer slot {} exceeds maximum {}", slot, MAX_VERTEX_BUFFERS);
            return;
        }

        auto& binding = m_vertexBufferBindings[slot];
        if (binding.buffer != buffer || binding.offset != offset || binding.stride != stride)
        {
            // DSA vertex buffer binding (OpenGL 4.5)
            GL_CHECK(glVertexArrayVertexBuffer(m_boundVAO, slot, buffer, offset, stride));
            binding.buffer = buffer;
            binding.offset = offset;
            binding.stride = stride;
            GL_DEBUG_STAT_INC(bufferBinds);
        }
    }

    void OpenGLStateCache::BindIndexBuffer(GLuint buffer)
    {
        if (m_boundIndexBuffer != buffer)
        {
            // Index buffer is bound to the VAO
            GL_CHECK(glVertexArrayElementBuffer(m_boundVAO, buffer));
            m_boundIndexBuffer = buffer;
            GL_DEBUG_STAT_INC(bufferBinds);
        }
    }

    void OpenGLStateCache::SetViewport(const GLViewportState& viewport)
    {
        if (!(m_viewport == viewport))
        {
            GL_CHECK(glViewportIndexedf(0, viewport.x, viewport.y, viewport.width, viewport.height));
            GL_CHECK(glDepthRangef(viewport.minDepth, viewport.maxDepth));
            m_viewport = viewport;
            GL_DEBUG_STAT_INC(stateChanges);
        }
    }

    void OpenGLStateCache::SetScissor(const GLScissorState& scissor)
    {
        if (!(m_scissor == scissor))
        {
            GL_CHECK(glScissor(scissor.x, scissor.y, scissor.width, scissor.height));
            m_scissor = scissor;
            GL_DEBUG_STAT_INC(stateChanges);
        }
    }

    void OpenGLStateCache::SetBlendState(uint32 attachment, const GLBlendState& state)
    {
        if (attachment >= 8)
        {
            RVX_RHI_ERROR("Blend attachment {} exceeds maximum 8", attachment);
            return;
        }

        auto& cached = m_blendStates[attachment];
        if (!(cached == state))
        {
            if (state.enabled != cached.enabled)
            {
                if (state.enabled)
                    GL_CHECK(glEnablei(GL_BLEND, attachment));
                else
                    GL_CHECK(glDisablei(GL_BLEND, attachment));
            }

            if (state.enabled)
            {
                GL_CHECK(glBlendFuncSeparatei(attachment, 
                    state.srcRGB, state.dstRGB, state.srcAlpha, state.dstAlpha));
                GL_CHECK(glBlendEquationSeparatei(attachment, state.opRGB, state.opAlpha));
            }

            GL_CHECK(glColorMaski(attachment, 
                (state.writeMask & 1) != 0,
                (state.writeMask & 2) != 0,
                (state.writeMask & 4) != 0,
                (state.writeMask & 8) != 0));

            cached = state;
            GL_DEBUG_STAT_INC(stateChanges);
        }
    }

    void OpenGLStateCache::SetDepthState(const GLDepthState& state)
    {
        if (!(m_depthState == state))
        {
            if (state.testEnabled != m_depthState.testEnabled)
            {
                if (state.testEnabled)
                    GL_CHECK(glEnable(GL_DEPTH_TEST));
                else
                    GL_CHECK(glDisable(GL_DEPTH_TEST));
            }

            if (state.writeEnabled != m_depthState.writeEnabled)
            {
                GL_CHECK(glDepthMask(state.writeEnabled ? GL_TRUE : GL_FALSE));
            }

            if (state.compareFunc != m_depthState.compareFunc)
            {
                GL_CHECK(glDepthFunc(state.compareFunc));
            }

            m_depthState = state;
            GL_DEBUG_STAT_INC(stateChanges);
        }
    }

    void OpenGLStateCache::SetStencilState(const GLStencilState& state)
    {
        bool anyChange = false;

        // Enable/disable stencil test
        if (state.enabled != m_stencilState.enabled)
        {
            if (state.enabled)
                GL_CHECK(glEnable(GL_STENCIL_TEST));
            else
                GL_CHECK(glDisable(GL_STENCIL_TEST));
            anyChange = true;
        }

        if (state.enabled)
        {
            // Front face - only update what changed
            if (state.front.compareFunc != m_stencilState.front.compareFunc ||
                state.front.reference != m_stencilState.front.reference ||
                state.front.compareMask != m_stencilState.front.compareMask)
            {
                GL_CHECK(glStencilFuncSeparate(GL_FRONT, 
                    state.front.compareFunc, state.front.reference, state.front.compareMask));
                anyChange = true;
            }

            if (state.front.failOp != m_stencilState.front.failOp ||
                state.front.depthFailOp != m_stencilState.front.depthFailOp ||
                state.front.passOp != m_stencilState.front.passOp)
            {
                GL_CHECK(glStencilOpSeparate(GL_FRONT, 
                    state.front.failOp, state.front.depthFailOp, state.front.passOp));
                anyChange = true;
            }

            if (state.front.writeMask != m_stencilState.front.writeMask)
            {
                GL_CHECK(glStencilMaskSeparate(GL_FRONT, state.front.writeMask));
                anyChange = true;
            }

            // Back face - only update what changed
            if (state.back.compareFunc != m_stencilState.back.compareFunc ||
                state.back.reference != m_stencilState.back.reference ||
                state.back.compareMask != m_stencilState.back.compareMask)
            {
                GL_CHECK(glStencilFuncSeparate(GL_BACK, 
                    state.back.compareFunc, state.back.reference, state.back.compareMask));
                anyChange = true;
            }

            if (state.back.failOp != m_stencilState.back.failOp ||
                state.back.depthFailOp != m_stencilState.back.depthFailOp ||
                state.back.passOp != m_stencilState.back.passOp)
            {
                GL_CHECK(glStencilOpSeparate(GL_BACK, 
                    state.back.failOp, state.back.depthFailOp, state.back.passOp));
                anyChange = true;
            }

            if (state.back.writeMask != m_stencilState.back.writeMask)
            {
                GL_CHECK(glStencilMaskSeparate(GL_BACK, state.back.writeMask));
                anyChange = true;
            }
        }

        m_stencilState = state;
        if (anyChange)
        {
            GL_DEBUG_STAT_INC(stateChanges);
        }
    }

    void OpenGLStateCache::SetRasterizerState(const GLRasterizerState& state)
    {
        bool anyChange = false;

        // Cull mode
        if (state.cullEnabled != m_rasterizerState.cullEnabled)
        {
            if (state.cullEnabled)
                GL_CHECK(glEnable(GL_CULL_FACE));
            else
                GL_CHECK(glDisable(GL_CULL_FACE));
            anyChange = true;
        }

        if (state.cullEnabled && state.cullMode != m_rasterizerState.cullMode)
        {
            GL_CHECK(glCullFace(state.cullMode));
            anyChange = true;
        }

        // Front face
        if (state.frontFace != m_rasterizerState.frontFace)
        {
            GL_CHECK(glFrontFace(state.frontFace));
            anyChange = true;
        }

        // Polygon mode
        if (state.polygonMode != m_rasterizerState.polygonMode)
        {
            GL_CHECK(glPolygonMode(GL_FRONT_AND_BACK, state.polygonMode));
            anyChange = true;
        }

        // Scissor test
        if (state.scissorEnabled != m_rasterizerState.scissorEnabled)
        {
            if (state.scissorEnabled)
                GL_CHECK(glEnable(GL_SCISSOR_TEST));
            else
                GL_CHECK(glDisable(GL_SCISSOR_TEST));
            anyChange = true;
        }

        // Depth clamp
        if (state.depthClampEnabled != m_rasterizerState.depthClampEnabled)
        {
            if (state.depthClampEnabled)
                GL_CHECK(glEnable(GL_DEPTH_CLAMP));
            else
                GL_CHECK(glDisable(GL_DEPTH_CLAMP));
            anyChange = true;
        }

        // Line width
        if (state.lineWidth != m_rasterizerState.lineWidth)
        {
            GL_CHECK(glLineWidth(state.lineWidth));
            m_lineWidth = state.lineWidth;
            anyChange = true;
        }

        // Polygon offset
        if (state.polygonOffsetEnabled != m_rasterizerState.polygonOffsetEnabled)
        {
            if (state.polygonOffsetEnabled)
            {
                GL_CHECK(glEnable(GL_POLYGON_OFFSET_FILL));
                GL_CHECK(glEnable(GL_POLYGON_OFFSET_LINE));
            }
            else
            {
                GL_CHECK(glDisable(GL_POLYGON_OFFSET_FILL));
                GL_CHECK(glDisable(GL_POLYGON_OFFSET_LINE));
            }
            anyChange = true;
        }

        if (state.polygonOffsetEnabled && 
            (state.depthBiasConstant != m_rasterizerState.depthBiasConstant ||
             state.depthBiasSlope != m_rasterizerState.depthBiasSlope))
        {
            GL_CHECK(glPolygonOffset(state.depthBiasSlope, state.depthBiasConstant));
            m_polygonOffsetFactor = state.depthBiasSlope;
            m_polygonOffsetUnits = state.depthBiasConstant;
            anyChange = true;
        }

        m_rasterizerState = state;
        if (anyChange)
        {
            GL_DEBUG_STAT_INC(stateChanges);
        }
    }

    void OpenGLStateCache::SetSampleShadingState(const GLSampleShadingState& state)
    {
        if (!(m_sampleShadingState == state))
        {
            if (state.enabled != m_sampleShadingState.enabled)
            {
                if (state.enabled)
                    GL_CHECK(glEnable(GL_SAMPLE_SHADING));
                else
                    GL_CHECK(glDisable(GL_SAMPLE_SHADING));
            }

            if (state.enabled && state.minSampleShading != m_sampleShadingState.minSampleShading)
            {
                GL_CHECK(glMinSampleShading(state.minSampleShading));
            }

            m_sampleShadingState = state;
            GL_DEBUG_STAT_INC(stateChanges);
        }
    }

    void OpenGLStateCache::SetLineWidth(float width)
    {
        if (m_lineWidth != width)
        {
            GL_CHECK(glLineWidth(width));
            m_lineWidth = width;
            GL_DEBUG_STAT_INC(stateChanges);
        }
    }

    void OpenGLStateCache::SetPolygonOffset(float factor, float units)
    {
        if (m_polygonOffsetFactor != factor || m_polygonOffsetUnits != units)
        {
            GL_CHECK(glPolygonOffset(factor, units));
            m_polygonOffsetFactor = factor;
            m_polygonOffsetUnits = units;
            GL_DEBUG_STAT_INC(stateChanges);
        }
    }

    void OpenGLStateCache::SetPrimitiveTopology(GLenum mode)
    {
        m_primitiveMode = mode;  // No GL call needed, stored for draw calls
    }

} // namespace RVX
