#include "OpenGLResources.h"
#include "OpenGLDevice.h"
#include "Core/Log.h"
#include <algorithm>

namespace RVX
{
    // =============================================================================
    // OpenGLBuffer Implementation
    // =============================================================================

    OpenGLBuffer::OpenGLBuffer(OpenGLDevice* device, const RHIBufferDesc& desc)
        : m_device(device)
        , m_desc(desc)
    {
        // Determine target based on usage
        if (HasFlag(desc.usage, RHIBufferUsage::Vertex))
            m_target = GL_ARRAY_BUFFER;
        else if (HasFlag(desc.usage, RHIBufferUsage::Index))
            m_target = GL_ELEMENT_ARRAY_BUFFER;
        else if (HasFlag(desc.usage, RHIBufferUsage::Constant))
            m_target = GL_UNIFORM_BUFFER;
        else if (HasFlag(desc.usage, RHIBufferUsage::Structured) || 
                 HasFlag(desc.usage, RHIBufferUsage::UnorderedAccess))
            m_target = GL_SHADER_STORAGE_BUFFER;
        else if (HasFlag(desc.usage, RHIBufferUsage::IndirectArgs))
            m_target = GL_DRAW_INDIRECT_BUFFER;
        else
            m_target = GL_COPY_WRITE_BUFFER;

        // Create buffer using DSA
        GL_CHECK(glCreateBuffers(1, &m_buffer));
        
        if (m_buffer == 0)
        {
            RVX_RHI_ERROR("Failed to create OpenGL buffer");
            return;
        }

        // Determine storage flags
        GLbitfield flags = ToGLBufferStorageFlags(desc.usage, desc.memoryType);

        // Allocate storage (no initial data - must be uploaded separately)
        GL_CHECK(glNamedBufferStorage(m_buffer, desc.size, nullptr, flags));

        // Set up persistent mapping for upload/readback buffers
        if (desc.memoryType == RHIMemoryType::Upload)
        {
            GLbitfield mapFlags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_FLUSH_EXPLICIT_BIT;
            m_mappedPtr = glMapNamedBufferRange(m_buffer, 0, desc.size, mapFlags);
            m_persistentlyMapped = (m_mappedPtr != nullptr);
            
            if (!m_persistentlyMapped)
            {
                RVX_RHI_WARN("Failed to create persistent mapping for upload buffer '{}', will use transient mapping",
                            desc.debugName ? desc.debugName : "");
            }
        }
        else if (desc.memoryType == RHIMemoryType::Readback)
        {
            GLbitfield mapFlags = GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT;
            m_mappedPtr = glMapNamedBufferRange(m_buffer, 0, desc.size, mapFlags);
            m_persistentlyMapped = (m_mappedPtr != nullptr);
        }

        // Debug labeling
        if (desc.debugName && desc.debugName[0])
        {
            SetDebugName(desc.debugName);
            OpenGLDebug::Get().SetBufferLabel(m_buffer, desc.debugName);
        }

        GL_DEBUG_TRACK(m_buffer, GLResourceType::Buffer, desc.debugName);
        OpenGLDebug::Get().SetResourceSize(m_buffer, GLResourceType::Buffer, desc.size);
        ++OpenGLDebug::Get().GetStats().buffersCreated;
        OpenGLDebug::Get().GetStats().totalBufferMemory += desc.size;

        RVX_RHI_DEBUG("Created OpenGL Buffer #{} '{}': size={}, usage=0x{:X}, memory={}",
                     m_buffer, GetDebugName(), desc.size, static_cast<uint32>(desc.usage),
                     static_cast<int>(desc.memoryType));
    }

    OpenGLBuffer::~OpenGLBuffer()
    {
        if (m_buffer != 0)
        {
            // Unmap if persistently mapped
            if (m_persistentlyMapped && m_mappedPtr)
            {
                glUnmapNamedBuffer(m_buffer);
                m_mappedPtr = nullptr;
            }

            ++OpenGLDebug::Get().GetStats().buffersDestroyed;
            OpenGLDebug::Get().GetStats().totalBufferMemory -= m_desc.size;

            // Queue for deferred deletion to avoid GPU race conditions
            if (m_device)
            {
                m_device->GetDeletionQueue().QueueBuffer(m_buffer, m_device->GetTotalFrameIndex(), 
                                                         GetDebugName().c_str());
            }
            else
            {
                // Fallback: immediate deletion if device is already gone
                glDeleteBuffers(1, &m_buffer);
                GL_DEBUG_UNTRACK(m_buffer, GLResourceType::Buffer);
            }

            RVX_RHI_DEBUG("Queued OpenGL Buffer #{} '{}' for deletion", m_buffer, GetDebugName());
            m_buffer = 0;
        }
    }

    void* OpenGLBuffer::Map()
    {
        if (m_persistentlyMapped)
        {
            return m_mappedPtr;
        }

        if (m_mappedPtr != nullptr)
        {
            RVX_RHI_WARN("Buffer '{}' is already mapped", GetDebugName());
            return m_mappedPtr;
        }

        GLbitfield access = 0;
        if (m_desc.memoryType == RHIMemoryType::Upload)
            access = GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT;
        else if (m_desc.memoryType == RHIMemoryType::Readback)
            access = GL_MAP_READ_BIT;
        else
        {
            RVX_RHI_ERROR("Cannot map buffer '{}' with Default memory type", GetDebugName());
            return nullptr;
        }

        m_mappedPtr = glMapNamedBufferRange(m_buffer, 0, m_desc.size, access);
        
        if (m_mappedPtr == nullptr)
        {
            RVX_RHI_ERROR("Failed to map buffer '{}'", GetDebugName());
            GL_DEBUG_CHECK("glMapNamedBufferRange");
        }

        return m_mappedPtr;
    }

    void OpenGLBuffer::Unmap()
    {
        if (m_persistentlyMapped)
        {
            // For persistent mapping, flush the written range
            GL_CHECK(glFlushMappedNamedBufferRange(m_buffer, 0, m_desc.size));
            return;
        }

        if (m_mappedPtr == nullptr)
        {
            RVX_RHI_WARN("Buffer '{}' is not mapped", GetDebugName());
            return;
        }

        GL_CHECK(glUnmapNamedBuffer(m_buffer));
        m_mappedPtr = nullptr;
    }

    // =============================================================================
    // OpenGLTexture Implementation
    // =============================================================================

    OpenGLTexture::OpenGLTexture(OpenGLDevice* device, const RHITextureDesc& desc)
        : m_device(device)
        , m_desc(desc)
        , m_ownsTexture(true)
    {
        // Determine texture target
        bool isArray = desc.arraySize > 1;
        bool isMultisample = desc.sampleCount != RHISampleCount::Count1;
        m_target = ToGLTextureTarget(desc.dimension, isArray, isMultisample);
        m_glFormat = ToGLFormat(desc.format);

        if (m_glFormat.internalFormat == 0)
        {
            RVX_RHI_ERROR("Unsupported texture format: {}", static_cast<int>(desc.format));
            return;
        }

        // Create texture using DSA
        GL_CHECK(glCreateTextures(m_target, 1, &m_texture));

        if (m_texture == 0)
        {
            RVX_RHI_ERROR("Failed to create OpenGL texture");
            return;
        }

        // Allocate storage based on dimension
        switch (desc.dimension)
        {
            case RHITextureDimension::Texture1D:
                if (isArray)
                {
                    GL_CHECK(glTextureStorage2D(m_texture, desc.mipLevels, m_glFormat.internalFormat,
                                               desc.width, desc.arraySize));
                }
                else
                {
                    GL_CHECK(glTextureStorage1D(m_texture, desc.mipLevels, m_glFormat.internalFormat,
                                               desc.width));
                }
                break;

            case RHITextureDimension::Texture2D:
                if (isMultisample)
                {
                    if (isArray)
                    {
                        GL_CHECK(glTextureStorage3DMultisample(m_texture, static_cast<GLsizei>(desc.sampleCount),
                                                              m_glFormat.internalFormat,
                                                              desc.width, desc.height, desc.arraySize, GL_TRUE));
                    }
                    else
                    {
                        GL_CHECK(glTextureStorage2DMultisample(m_texture, static_cast<GLsizei>(desc.sampleCount),
                                                              m_glFormat.internalFormat,
                                                              desc.width, desc.height, GL_TRUE));
                    }
                }
                else if (isArray)
                {
                    GL_CHECK(glTextureStorage3D(m_texture, desc.mipLevels, m_glFormat.internalFormat,
                                               desc.width, desc.height, desc.arraySize));
                }
                else
                {
                    GL_CHECK(glTextureStorage2D(m_texture, desc.mipLevels, m_glFormat.internalFormat,
                                               desc.width, desc.height));
                }
                break;

            case RHITextureDimension::Texture3D:
                GL_CHECK(glTextureStorage3D(m_texture, desc.mipLevels, m_glFormat.internalFormat,
                                           desc.width, desc.height, desc.depth));
                break;

            case RHITextureDimension::TextureCube:
                if (isArray)
                {
                    GL_CHECK(glTextureStorage3D(m_texture, desc.mipLevels, m_glFormat.internalFormat,
                                               desc.width, desc.height, desc.arraySize * 6));
                }
                else
                {
                    GL_CHECK(glTextureStorage2D(m_texture, desc.mipLevels, m_glFormat.internalFormat,
                                               desc.width, desc.height));
                }
                break;
        }

        // Debug labeling
        if (desc.debugName && desc.debugName[0])
        {
            SetDebugName(desc.debugName);
            OpenGLDebug::Get().SetTextureLabel(m_texture, desc.debugName);
        }

        GL_DEBUG_TRACK(m_texture, GLResourceType::Texture, desc.debugName);
        ++OpenGLDebug::Get().GetStats().texturesCreated;

        // Estimate memory size
        uint64 textureSize = static_cast<uint64>(desc.width) * desc.height * 
                            GetFormatBytesPerPixel(desc.format) * 
                            std::max(1u, desc.arraySize) * std::max(1u, desc.depth);
        OpenGLDebug::Get().SetResourceSize(m_texture, GLResourceType::Texture, textureSize);
        OpenGLDebug::Get().GetStats().totalTextureMemory += textureSize;

        RVX_RHI_DEBUG("Created OpenGL Texture #{} '{}': {}x{}x{}, format={}, mips={}, arrays={}",
                     m_texture, GetDebugName(), desc.width, desc.height, desc.depth,
                     static_cast<int>(desc.format), desc.mipLevels, desc.arraySize);
    }

    OpenGLTexture::~OpenGLTexture()
    {
        if (m_texture != 0 && m_ownsTexture)
        {
            ++OpenGLDebug::Get().GetStats().texturesDestroyed;
            
            // Queue for deferred deletion to avoid GPU race conditions
            if (m_device)
            {
                // Also invalidate any FBOs that reference this texture
                m_device->GetFBOCache().InvalidateTexture(m_texture);
                m_device->GetDeletionQueue().QueueTexture(m_texture, m_device->GetTotalFrameIndex(),
                                                          GetDebugName().c_str());
            }
            else
            {
                // Fallback: immediate deletion if device is already gone
                glDeleteTextures(1, &m_texture);
                GL_DEBUG_UNTRACK(m_texture, GLResourceType::Texture);
            }

            RVX_RHI_DEBUG("Queued OpenGL Texture #{} '{}' for deletion", m_texture, GetDebugName());
            m_texture = 0;
        }
    }

    Ref<OpenGLTexture> OpenGLTexture::CreateFromExisting(OpenGLDevice* device, GLuint texture,
                                                         GLenum target, const RHITextureDesc& desc)
    {
        Ref<OpenGLTexture> tex = Ref<OpenGLTexture>(new OpenGLTexture());
        tex->m_device = device;
        tex->m_texture = texture;
        tex->m_target = target;
        tex->m_desc = desc;
        tex->m_glFormat = ToGLFormat(desc.format);
        tex->m_ownsTexture = false;  // We don't own this texture
        
        if (desc.debugName)
        {
            tex->SetDebugName(desc.debugName);
        }
        
        return tex;
    }

    // =============================================================================
    // OpenGLTextureView Implementation
    // =============================================================================

    OpenGLTextureView::OpenGLTextureView(OpenGLDevice* device, OpenGLTexture* texture, const RHITextureViewDesc& desc)
        : m_device(device)
        , m_texture(texture)
        , m_desc(desc)
    {
        // Use texture format if not specified
        if (m_desc.format == RHIFormat::Unknown)
        {
            m_desc.format = texture->GetFormat();
        }

        // Check if we can just use the original texture (common case)
        const auto& texDesc = texture->GetDesc();
        const auto& sr = desc.subresourceRange;
        
        uint32 mipCount = (sr.mipLevelCount == RVX_ALL_MIPS) ? texDesc.mipLevels : sr.mipLevelCount;
        uint32 arrayCount = (sr.arrayLayerCount == RVX_ALL_LAYERS) ? texDesc.arraySize : sr.arrayLayerCount;
        
        bool needsView = (sr.baseMipLevel != 0 || 
                         mipCount != texDesc.mipLevels ||
                         sr.baseArrayLayer != 0 ||
                         arrayCount != texDesc.arraySize ||
                         m_desc.format != texDesc.format);

        if (!needsView)
        {
            // Just use the original texture
            m_textureView = texture->GetHandle();
            m_target = texture->GetTarget();
            m_ownsView = false;
            return;
        }

        // Create a texture view
        m_target = ToGLTextureTarget(texDesc.dimension, arrayCount > 1, false);
        auto glFormat = ToGLFormat(m_desc.format);

        GL_CHECK(glGenTextures(1, &m_textureView));
        GL_CHECK(glTextureView(m_textureView, m_target, texture->GetHandle(),
                              glFormat.internalFormat, sr.baseMipLevel, mipCount,
                              sr.baseArrayLayer, arrayCount));

        m_ownsView = true;

        if (desc.debugName && desc.debugName[0])
        {
            SetDebugName(desc.debugName);
            OpenGLDebug::Get().SetTextureLabel(m_textureView, desc.debugName);
        }

        RVX_RHI_DEBUG("Created OpenGL TextureView #{} for Texture #{}", m_textureView, texture->GetHandle());
    }

    OpenGLTextureView::~OpenGLTextureView()
    {
        if (m_textureView != 0 && m_ownsView)
        {
            // Queue for deferred deletion to avoid GPU race conditions
            if (m_device)
            {
                m_device->GetDeletionQueue().QueueTexture(m_textureView, m_device->GetTotalFrameIndex(),
                                                          GetDebugName().c_str());
            }
            else
            {
                // Fallback: immediate deletion if device is already gone
                glDeleteTextures(1, &m_textureView);
            }
            RVX_RHI_DEBUG("Queued OpenGL TextureView #{} for deletion", m_textureView);
            m_textureView = 0;
        }
    }

    // =============================================================================
    // OpenGLSampler Implementation
    // =============================================================================

    OpenGLSampler::OpenGLSampler(OpenGLDevice* device, const RHISamplerDesc& desc)
        : m_device(device)
        , m_desc(desc)
    {
        GL_CHECK(glCreateSamplers(1, &m_sampler));

        if (m_sampler == 0)
        {
            RVX_RHI_ERROR("Failed to create OpenGL sampler");
            return;
        }

        // Min filter (combines minification and mipmap filtering)
        GLenum minFilter = ToGLMinFilter(desc.minFilter, desc.mipFilter);
        GLenum magFilter = ToGLMagFilter(desc.magFilter);

        GL_CHECK(glSamplerParameteri(m_sampler, GL_TEXTURE_MIN_FILTER, minFilter));
        GL_CHECK(glSamplerParameteri(m_sampler, GL_TEXTURE_MAG_FILTER, magFilter));

        // Address modes
        GL_CHECK(glSamplerParameteri(m_sampler, GL_TEXTURE_WRAP_S, ToGLAddressMode(desc.addressU)));
        GL_CHECK(glSamplerParameteri(m_sampler, GL_TEXTURE_WRAP_T, ToGLAddressMode(desc.addressV)));
        GL_CHECK(glSamplerParameteri(m_sampler, GL_TEXTURE_WRAP_R, ToGLAddressMode(desc.addressW)));

        // Border color
        if (desc.addressU == RHIAddressMode::ClampToBorder ||
            desc.addressV == RHIAddressMode::ClampToBorder ||
            desc.addressW == RHIAddressMode::ClampToBorder)
        {
            GL_CHECK(glSamplerParameterfv(m_sampler, GL_TEXTURE_BORDER_COLOR, desc.borderColor));
        }

        // LOD
        GL_CHECK(glSamplerParameterf(m_sampler, GL_TEXTURE_MIN_LOD, desc.minLod));
        GL_CHECK(glSamplerParameterf(m_sampler, GL_TEXTURE_MAX_LOD, desc.maxLod));
        GL_CHECK(glSamplerParameterf(m_sampler, GL_TEXTURE_LOD_BIAS, desc.mipLodBias));

        // Anisotropy
        if (desc.maxAnisotropy > 1.0f)
        {
            GL_CHECK(glSamplerParameterf(m_sampler, GL_TEXTURE_MAX_ANISOTROPY, desc.maxAnisotropy));
        }

        // Compare function (for shadow samplers)
        if (desc.compareEnable)
        {
            GL_CHECK(glSamplerParameteri(m_sampler, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE));
            GL_CHECK(glSamplerParameteri(m_sampler, GL_TEXTURE_COMPARE_FUNC, ToGLCompareFunc(desc.compareOp)));
        }

        if (desc.debugName && desc.debugName[0])
        {
            SetDebugName(desc.debugName);
            OpenGLDebug::Get().SetSamplerLabel(m_sampler, desc.debugName);
        }

        GL_DEBUG_TRACK(m_sampler, GLResourceType::Sampler, desc.debugName);

        RVX_RHI_DEBUG("Created OpenGL Sampler #{} '{}'", m_sampler, GetDebugName());
    }

    OpenGLSampler::~OpenGLSampler()
    {
        if (m_sampler != 0)
        {
            // Queue for deferred deletion to avoid GPU race conditions
            if (m_device)
            {
                m_device->GetDeletionQueue().QueueSampler(m_sampler, m_device->GetTotalFrameIndex(),
                                                          GetDebugName().c_str());
            }
            else
            {
                // Fallback: immediate deletion if device is already gone
                glDeleteSamplers(1, &m_sampler);
                GL_DEBUG_UNTRACK(m_sampler, GLResourceType::Sampler);
            }

            RVX_RHI_DEBUG("Queued OpenGL Sampler #{} '{}' for deletion", m_sampler, GetDebugName());
            m_sampler = 0;
        }
    }

} // namespace RVX
