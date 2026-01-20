#pragma once

#include "OpenGLCommon.h"
#include "OpenGLDebug.h"
#include "OpenGLConversions.h"
#include "RHI/RHIDevice.h"

namespace RVX
{
    class OpenGLDevice;

    // =============================================================================
    // OpenGL Buffer
    // =============================================================================
    class OpenGLBuffer : public RHIBuffer
    {
    public:
        OpenGLBuffer(OpenGLDevice* device, const RHIBufferDesc& desc);
        ~OpenGLBuffer() override;

        // RHIBuffer interface
        void* Map() override;
        void Unmap() override;
        uint64 GetSize() const override { return m_desc.size; }
        RHIBufferUsage GetUsage() const override { return m_desc.usage; }
        RHIMemoryType GetMemoryType() const override { return m_desc.memoryType; }
        uint32 GetStride() const override { return m_desc.stride; }

        // OpenGL specific
        GLuint GetHandle() const { return m_buffer; }
        GLenum GetTarget() const { return m_target; }
        bool IsMapped() const { return m_mappedPtr != nullptr; }
        bool IsPersistentlyMapped() const { return m_persistentlyMapped; }

        const RHIBufferDesc& GetDesc() const { return m_desc; }

    private:
        OpenGLDevice* m_device = nullptr;
        RHIBufferDesc m_desc;
        GLuint m_buffer = 0;
        GLenum m_target = GL_ARRAY_BUFFER;
        void* m_mappedPtr = nullptr;
        bool m_persistentlyMapped = false;
    };

    // =============================================================================
    // OpenGL Texture
    // =============================================================================
    class OpenGLTexture : public RHITexture
    {
    public:
        OpenGLTexture(OpenGLDevice* device, const RHITextureDesc& desc);
        ~OpenGLTexture() override;

        // RHITexture interface
        uint32 GetWidth() const override { return m_desc.width; }
        uint32 GetHeight() const override { return m_desc.height; }
        uint32 GetDepth() const override { return m_desc.depth; }
        uint32 GetMipLevels() const override { return m_desc.mipLevels; }
        uint32 GetArraySize() const override { return m_desc.arraySize; }
        RHIFormat GetFormat() const override { return m_desc.format; }
        RHITextureUsage GetUsage() const override { return m_desc.usage; }
        RHITextureDimension GetDimension() const override { return m_desc.dimension; }
        RHISampleCount GetSampleCount() const override { return m_desc.sampleCount; }

        // OpenGL specific
        GLuint GetHandle() const { return m_texture; }
        GLenum GetTarget() const { return m_target; }
        GLFormatInfo GetGLFormat() const { return m_glFormat; }

        const RHITextureDesc& GetDesc() const { return m_desc; }

        // For SwapChain back buffer wrapping
        static Ref<OpenGLTexture> CreateFromExisting(OpenGLDevice* device, GLuint texture, 
                                                     GLenum target, const RHITextureDesc& desc);

    private:
        OpenGLTexture() = default;  // For CreateFromExisting

        OpenGLDevice* m_device = nullptr;
        RHITextureDesc m_desc;
        GLuint m_texture = 0;
        GLenum m_target = GL_TEXTURE_2D;
        GLFormatInfo m_glFormat;
        bool m_ownsTexture = true;  // False if wrapping an existing texture
    };

    // =============================================================================
    // OpenGL Texture View
    // =============================================================================
    class OpenGLTextureView : public RHITextureView
    {
    public:
        OpenGLTextureView(OpenGLDevice* device, OpenGLTexture* texture, const RHITextureViewDesc& desc);
        ~OpenGLTextureView() override;

        RHITexture* GetTexture() const override { return m_texture.Get(); }
        RHIFormat GetFormat() const override { return m_desc.format; }
        const RHISubresourceRange& GetSubresourceRange() const override { return m_desc.subresourceRange; }

        // OpenGL specific
        GLuint GetHandle() const { return m_textureView; }
        GLenum GetTarget() const { return m_target; }

    private:
        OpenGLDevice* m_device = nullptr;
        Ref<OpenGLTexture> m_texture;
        RHITextureViewDesc m_desc;
        GLuint m_textureView = 0;
        GLenum m_target = GL_TEXTURE_2D;
        bool m_ownsView = true;  // False if using the original texture directly
    };

    // =============================================================================
    // OpenGL Sampler
    // =============================================================================
    class OpenGLSampler : public RHISampler
    {
    public:
        OpenGLSampler(OpenGLDevice* device, const RHISamplerDesc& desc);
        ~OpenGLSampler() override;

        // OpenGL specific
        GLuint GetHandle() const { return m_sampler; }

        const RHISamplerDesc& GetDesc() const { return m_desc; }

    private:
        OpenGLDevice* m_device = nullptr;
        RHISamplerDesc m_desc;
        GLuint m_sampler = 0;
    };

} // namespace RVX
