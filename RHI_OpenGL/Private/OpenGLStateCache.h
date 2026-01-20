#pragma once

#include "OpenGLCommon.h"
#include "OpenGLDebug.h"
#include "RHI/RHIDefinitions.h"
#include <array>

namespace RVX
{
    // =============================================================================
    // Viewport State
    // =============================================================================
    struct GLViewportState
    {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        float minDepth = 0.0f;
        float maxDepth = 1.0f;

        bool operator==(const GLViewportState& other) const
        {
            return x == other.x && y == other.y && 
                   width == other.width && height == other.height &&
                   minDepth == other.minDepth && maxDepth == other.maxDepth;
        }
    };

    // =============================================================================
    // Scissor State
    // =============================================================================
    struct GLScissorState
    {
        int32 x = 0;
        int32 y = 0;
        uint32 width = 0;
        uint32 height = 0;

        bool operator==(const GLScissorState& other) const
        {
            return x == other.x && y == other.y && 
                   width == other.width && height == other.height;
        }
    };

    // =============================================================================
    // Blend State
    // =============================================================================
    struct GLBlendState
    {
        bool enabled = false;
        GLenum srcRGB = GL_ONE;
        GLenum dstRGB = GL_ZERO;
        GLenum srcAlpha = GL_ONE;
        GLenum dstAlpha = GL_ZERO;
        GLenum opRGB = GL_FUNC_ADD;
        GLenum opAlpha = GL_FUNC_ADD;
        std::array<float, 4> blendColor = {0.0f, 0.0f, 0.0f, 0.0f};
        uint8 writeMask = 0xF;  // RGBA write mask

        bool operator==(const GLBlendState& other) const
        {
            return enabled == other.enabled && 
                   srcRGB == other.srcRGB && dstRGB == other.dstRGB &&
                   srcAlpha == other.srcAlpha && dstAlpha == other.dstAlpha &&
                   opRGB == other.opRGB && opAlpha == other.opAlpha &&
                   writeMask == other.writeMask;
        }
    };

    // =============================================================================
    // Depth State
    // =============================================================================
    struct GLDepthState
    {
        bool testEnabled = false;
        bool writeEnabled = true;
        GLenum compareFunc = GL_LESS;

        bool operator==(const GLDepthState& other) const
        {
            return testEnabled == other.testEnabled && 
                   writeEnabled == other.writeEnabled &&
                   compareFunc == other.compareFunc;
        }
    };

    // =============================================================================
    // Stencil State
    // =============================================================================
    struct GLStencilFaceState
    {
        GLenum failOp = GL_KEEP;
        GLenum depthFailOp = GL_KEEP;
        GLenum passOp = GL_KEEP;
        GLenum compareFunc = GL_ALWAYS;
        uint32 compareMask = 0xFFFFFFFF;
        uint32 writeMask = 0xFFFFFFFF;
        uint32 reference = 0;
    };

    struct GLStencilState
    {
        bool enabled = false;
        GLStencilFaceState front;
        GLStencilFaceState back;

        bool operator==(const GLStencilState& other) const
        {
            return enabled == other.enabled;
            // Note: simplified comparison - full comparison would check all fields
        }
    };

    // =============================================================================
    // Rasterizer State
    // =============================================================================
    struct GLRasterizerState
    {
        GLenum cullMode = GL_BACK;
        GLenum frontFace = GL_CCW;
        GLenum polygonMode = GL_FILL;
        bool cullEnabled = true;
        bool scissorEnabled = false;
        bool depthClampEnabled = false;
        float depthBiasConstant = 0.0f;
        float depthBiasSlope = 0.0f;
        float lineWidth = 1.0f;

        bool operator==(const GLRasterizerState& other) const
        {
            return cullMode == other.cullMode && frontFace == other.frontFace &&
                   polygonMode == other.polygonMode && cullEnabled == other.cullEnabled &&
                   scissorEnabled == other.scissorEnabled;
        }
    };

    // =============================================================================
    // OpenGL State Cache
    // Caches OpenGL state to avoid redundant state changes
    // =============================================================================
    class OpenGLStateCache
    {
    public:
        static constexpr uint32 MAX_TEXTURE_SLOTS = 32;
        static constexpr uint32 MAX_UBO_SLOTS = 16;
        static constexpr uint32 MAX_SSBO_SLOTS = 16;
        static constexpr uint32 MAX_VERTEX_BUFFERS = 16;

        OpenGLStateCache();
        ~OpenGLStateCache() = default;

        // Reset all cached state (call at frame start or context loss)
        void Invalidate();

        // =================================================================
        // Binding State
        // =================================================================
        
        // Program binding
        void BindProgram(GLuint program);
        GLuint GetBoundProgram() const { return m_boundProgram; }

        // VAO binding
        void BindVAO(GLuint vao);
        GLuint GetBoundVAO() const { return m_boundVAO; }

        // FBO binding
        void BindFramebuffer(GLuint fbo);
        void BindReadFramebuffer(GLuint fbo);
        void BindDrawFramebuffer(GLuint fbo);
        GLuint GetBoundFramebuffer() const { return m_boundFBO; }

        // Buffer bindings (for indexed targets)
        void BindUniformBuffer(uint32 slot, GLuint buffer, GLintptr offset = 0, GLsizeiptr size = 0);
        void BindStorageBuffer(uint32 slot, GLuint buffer, GLintptr offset = 0, GLsizeiptr size = 0);

        // Texture binding
        void BindTexture(uint32 slot, GLenum target, GLuint texture);
        void BindSampler(uint32 slot, GLuint sampler);
        void BindImageTexture(uint32 slot, GLuint texture, GLint level, GLboolean layered, 
                             GLint layer, GLenum access, GLenum format);

        // Vertex buffer binding (for DSA)
        void BindVertexBuffer(uint32 slot, GLuint buffer, GLintptr offset, GLsizei stride);

        // Index buffer binding
        void BindIndexBuffer(GLuint buffer);
        GLuint GetBoundIndexBuffer() const { return m_boundIndexBuffer; }

        // =================================================================
        // Pipeline State
        // =================================================================
        
        void SetViewport(const GLViewportState& viewport);
        void SetScissor(const GLScissorState& scissor);
        void SetBlendState(uint32 attachment, const GLBlendState& state);
        void SetDepthState(const GLDepthState& state);
        void SetStencilState(const GLStencilState& state);
        void SetRasterizerState(const GLRasterizerState& state);
        void SetPrimitiveTopology(GLenum mode);

        const GLViewportState& GetViewport() const { return m_viewport; }
        const GLScissorState& GetScissor() const { return m_scissor; }
        const GLDepthState& GetDepthState() const { return m_depthState; }
        GLenum GetPrimitiveTopology() const { return m_primitiveMode; }

    private:
        // Binding state
        GLuint m_boundProgram = 0;
        GLuint m_boundVAO = 0;
        GLuint m_boundFBO = 0;
        GLuint m_boundReadFBO = 0;
        GLuint m_boundDrawFBO = 0;
        GLuint m_boundIndexBuffer = 0;

        // Indexed buffer bindings
        struct BufferBinding
        {
            GLuint buffer = 0;
            GLintptr offset = 0;
            GLsizeiptr size = 0;
        };
        std::array<BufferBinding, MAX_UBO_SLOTS> m_uboBindings;
        std::array<BufferBinding, MAX_SSBO_SLOTS> m_ssboBindings;

        // Texture/Sampler bindings
        struct TextureBinding
        {
            GLenum target = 0;
            GLuint texture = 0;
        };
        std::array<TextureBinding, MAX_TEXTURE_SLOTS> m_textureBindings;
        std::array<GLuint, MAX_TEXTURE_SLOTS> m_samplerBindings = {};

        // Vertex buffer bindings
        struct VertexBufferBinding
        {
            GLuint buffer = 0;
            GLintptr offset = 0;
            GLsizei stride = 0;
        };
        std::array<VertexBufferBinding, MAX_VERTEX_BUFFERS> m_vertexBufferBindings;

        // Pipeline state
        GLViewportState m_viewport;
        GLScissorState m_scissor;
        std::array<GLBlendState, 8> m_blendStates;
        GLDepthState m_depthState;
        GLStencilState m_stencilState;
        GLRasterizerState m_rasterizerState;
        GLenum m_primitiveMode = GL_TRIANGLES;

        // Active texture slot for legacy binding
        uint32 m_activeTextureSlot = 0;
    };

} // namespace RVX
