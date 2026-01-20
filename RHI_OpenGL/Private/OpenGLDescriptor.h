#pragma once

#include "OpenGLCommon.h"
#include "OpenGLDebug.h"
#include "RHI/RHIDescriptor.h"
#include <vector>

namespace RVX
{
    class OpenGLDevice;
    class OpenGLDescriptorSetLayout;
    class OpenGLBuffer;
    class OpenGLTexture;
    class OpenGLTextureView;
    class OpenGLSampler;

    // =============================================================================
    // Binding Entry (resolved for OpenGL)
    // =============================================================================
    struct OpenGLBindingEntry
    {
        uint32 glBinding = 0;          // OpenGL binding point
        RHIBindingType type = RHIBindingType::UniformBuffer;

        // Buffer info
        GLuint buffer = 0;
        GLintptr offset = 0;
        GLsizeiptr size = 0;

        // Texture info
        GLuint texture = 0;
        GLenum textureTarget = GL_TEXTURE_2D;

        // Sampler info
        GLuint sampler = 0;

        // Image info (for storage textures)
        GLint imageLevel = 0;
        GLboolean imageLayered = GL_FALSE;
        GLint imageLayer = 0;
        GLenum imageAccess = GL_READ_WRITE;
        GLenum imageFormat = GL_RGBA8;
    };

    // =============================================================================
    // OpenGL Descriptor Set
    // Represents a collection of resource bindings
    // =============================================================================
    class OpenGLDescriptorSet : public RHIDescriptorSet
    {
    public:
        OpenGLDescriptorSet(OpenGLDevice* device, const RHIDescriptorSetDesc& desc);
        ~OpenGLDescriptorSet() override = default;

        // RHIDescriptorSet interface
        void Update(const std::vector<RHIDescriptorBinding>& bindings) override;

        // OpenGL specific
        const std::vector<OpenGLBindingEntry>& GetBindings() const { return m_bindings; }
        OpenGLDescriptorSetLayout* GetLayout() const { return m_layout; }

        // Apply all bindings to the current OpenGL state
        void Bind(class OpenGLStateCache& stateCache, uint32 setIndex);

    private:
        void ResolveBinding(const RHIDescriptorBinding& binding);

        OpenGLDevice* m_device = nullptr;
        OpenGLDescriptorSetLayout* m_layout = nullptr;
        std::vector<OpenGLBindingEntry> m_bindings;
    };

} // namespace RVX
