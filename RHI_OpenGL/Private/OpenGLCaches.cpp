#include "OpenGLCaches.h"
#include "Core/Log.h"

namespace RVX
{
    // =============================================================================
    // OpenGLFramebufferCache Implementation
    // =============================================================================

    OpenGLFramebufferCache::~OpenGLFramebufferCache()
    {
        Clear();
    }

    GLuint OpenGLFramebufferCache::GetOrCreateFBO(const FBOCacheKey& key, uint64 currentFrame, const char* debugName)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Look for existing FBO
        auto it = m_cache.find(key);
        if (it != m_cache.end())
        {
            it->second.lastUsedFrame = currentFrame;
            ++m_hits;
            return it->second.fbo;
        }

        // Cache miss - create new FBO
        ++m_misses;

        // Evict if at capacity
        if (m_cache.size() >= MAX_CACHED_FBOS)
        {
            // Find least recently used
            auto oldestIt = m_cache.begin();
            uint64 oldestFrame = UINT64_MAX;
            for (auto cacheIt = m_cache.begin(); cacheIt != m_cache.end(); ++cacheIt)
            {
                if (cacheIt->second.lastUsedFrame < oldestFrame)
                {
                    oldestFrame = cacheIt->second.lastUsedFrame;
                    oldestIt = cacheIt;
                }
            }

            RVX_RHI_DEBUG("FBO Cache: evicting FBO #{} (last used frame {})",
                         oldestIt->second.fbo, oldestFrame);
            DeleteFBO(oldestIt->second.fbo);
            m_cache.erase(oldestIt);
        }

        // Create new FBO
        GLuint fbo = CreateFBO(key, debugName);
        if (fbo == 0)
        {
            return 0;
        }

        CachedFBO entry;
        entry.fbo = fbo;
        entry.key = key;
        entry.lastUsedFrame = currentFrame;
        entry.debugName = debugName ? debugName : "";

        m_cache[key] = entry;

        RVX_RHI_DEBUG("FBO Cache: created FBO #{} '{}' (cache size: {})",
                     fbo, entry.debugName, m_cache.size());

        return fbo;
    }

    GLuint OpenGLFramebufferCache::CreateFBO(const FBOCacheKey& key, const char* debugName)
    {
        GLuint fbo = 0;
        GL_CHECK(glCreateFramebuffers(1, &fbo));

        if (fbo == 0)
        {
            RVX_RHI_ERROR("Failed to create framebuffer");
            return 0;
        }

        // Attach color attachments
        GLenum drawBuffers[FBOCacheKey::MAX_COLOR_ATTACHMENTS];
        for (uint32 i = 0; i < key.colorAttachmentCount; ++i)
        {
            const auto& attachment = key.colorAttachments[i];
            if (attachment.texture != 0)
            {
                if (key.layers > 1)
                {
                    GL_CHECK(glNamedFramebufferTextureLayer(fbo, GL_COLOR_ATTACHMENT0 + i,
                                                           attachment.texture, attachment.mipLevel,
                                                           attachment.arrayLayer));
                }
                else
                {
                    GL_CHECK(glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0 + i,
                                                       attachment.texture, attachment.mipLevel));
                }
                drawBuffers[i] = GL_COLOR_ATTACHMENT0 + i;
            }
            else
            {
                drawBuffers[i] = GL_NONE;
            }
        }

        // Set draw buffers
        if (key.colorAttachmentCount > 0)
        {
            GL_CHECK(glNamedFramebufferDrawBuffers(fbo, key.colorAttachmentCount, drawBuffers));
        }

        // Attach depth-stencil
        if (key.depthStencilTexture != 0)
        {
            GLenum attachment = GL_DEPTH_ATTACHMENT;
            // Determine if it's depth-only or depth-stencil based on format
            switch (key.depthStencilFormat)
            {
                case GL_DEPTH24_STENCIL8:
                case GL_DEPTH32F_STENCIL8:
                    attachment = GL_DEPTH_STENCIL_ATTACHMENT;
                    break;
                default:
                    attachment = GL_DEPTH_ATTACHMENT;
                    break;
            }

            if (key.layers > 1)
            {
                GL_CHECK(glNamedFramebufferTextureLayer(fbo, attachment,
                                                        key.depthStencilTexture,
                                                        key.depthStencilMipLevel,
                                                        key.depthStencilArrayLayer));
            }
            else
            {
                GL_CHECK(glNamedFramebufferTexture(fbo, attachment,
                                                   key.depthStencilTexture,
                                                   key.depthStencilMipLevel));
            }
        }

        // Validate completeness
        GLenum status = glCheckNamedFramebufferStatus(fbo, GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE)
        {
            RVX_RHI_ERROR("Framebuffer {} '{}' is incomplete: {}",
                         fbo, debugName ? debugName : "", FBOStatusToString(status));
            LogFBOStatus(fbo, "CreateFBO");
            glDeleteFramebuffers(1, &fbo);
            return 0;
        }

        // Debug label
        if (debugName && debugName[0])
        {
            OpenGLDebug::Get().SetFramebufferLabel(fbo, debugName);
        }

        GL_DEBUG_TRACK(fbo, GLResourceType::FBO, debugName);

        return fbo;
    }

    void OpenGLFramebufferCache::DeleteFBO(GLuint fbo)
    {
        if (fbo != 0)
        {
            GL_CHECK(glDeleteFramebuffers(1, &fbo));
            GL_DEBUG_UNTRACK(fbo, GLResourceType::FBO);
        }
    }

    void OpenGLFramebufferCache::InvalidateTexture(GLuint texture)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        std::vector<FBOCacheKey> keysToRemove;

        for (const auto& [key, entry] : m_cache)
        {
            bool usesTexture = false;

            // Check color attachments
            for (uint32 i = 0; i < key.colorAttachmentCount; ++i)
            {
                if (key.colorAttachments[i].texture == texture)
                {
                    usesTexture = true;
                    break;
                }
            }

            // Check depth-stencil
            if (key.depthStencilTexture == texture)
            {
                usesTexture = true;
            }

            if (usesTexture)
            {
                keysToRemove.push_back(key);
            }
        }

        for (const auto& key : keysToRemove)
        {
            auto it = m_cache.find(key);
            if (it != m_cache.end())
            {
                RVX_RHI_DEBUG("FBO Cache: invalidating FBO #{} due to texture {} destruction",
                             it->second.fbo, texture);
                DeleteFBO(it->second.fbo);
                m_cache.erase(it);
            }
        }
    }

    void OpenGLFramebufferCache::Cleanup(uint64 currentFrame)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        std::vector<FBOCacheKey> keysToRemove;

        for (const auto& [key, entry] : m_cache)
        {
            if (currentFrame > entry.lastUsedFrame + MAX_UNUSED_FRAMES)
            {
                keysToRemove.push_back(key);
            }
        }

        for (const auto& key : keysToRemove)
        {
            auto it = m_cache.find(key);
            if (it != m_cache.end())
            {
                RVX_RHI_DEBUG("FBO Cache: cleanup FBO #{} (unused for {} frames)",
                             it->second.fbo, currentFrame - it->second.lastUsedFrame);
                DeleteFBO(it->second.fbo);
                m_cache.erase(it);
            }
        }
    }

    void OpenGLFramebufferCache::Clear()
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        for (const auto& [key, entry] : m_cache)
        {
            DeleteFBO(entry.fbo);
        }
        m_cache.clear();

        RVX_RHI_DEBUG("FBO Cache cleared");
    }

    // =============================================================================
    // OpenGLVAOCache Implementation
    // =============================================================================

    OpenGLVAOCache::~OpenGLVAOCache()
    {
        Clear();
    }

    GLuint OpenGLVAOCache::GetOrCreateVAO(const VAOCacheKey& key, uint64 currentFrame, const char* debugName)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Look for existing VAO
        auto it = m_cache.find(key);
        if (it != m_cache.end())
        {
            it->second.lastUsedFrame = currentFrame;
            ++m_hits;
            return it->second.vao;
        }

        // Cache miss - create new VAO
        ++m_misses;

        // Evict if at capacity
        if (m_cache.size() >= MAX_CACHED_VAOS)
        {
            auto oldestIt = m_cache.begin();
            uint64 oldestFrame = UINT64_MAX;
            for (auto cacheIt = m_cache.begin(); cacheIt != m_cache.end(); ++cacheIt)
            {
                if (cacheIt->second.lastUsedFrame < oldestFrame)
                {
                    oldestFrame = cacheIt->second.lastUsedFrame;
                    oldestIt = cacheIt;
                }
            }

            RVX_RHI_DEBUG("VAO Cache: evicting VAO #{} (last used frame {})",
                         oldestIt->second.vao, oldestFrame);
            DeleteVAO(oldestIt->second.vao);
            m_cache.erase(oldestIt);
        }

        // Create new VAO
        GLuint vao = CreateVAO(key, debugName);
        if (vao == 0)
        {
            return 0;
        }

        CachedVAO entry;
        entry.vao = vao;
        entry.key = key;
        entry.lastUsedFrame = currentFrame;
        entry.debugName = debugName ? debugName : "";

        m_cache[key] = entry;

        RVX_RHI_DEBUG("VAO Cache: created VAO #{} '{}' (cache size: {})",
                     vao, entry.debugName, m_cache.size());

        return vao;
    }

    GLuint OpenGLVAOCache::CreateVAO(const VAOCacheKey& key, const char* debugName)
    {
        GLuint vao = 0;
        GL_CHECK(glCreateVertexArrays(1, &vao));

        if (vao == 0)
        {
            RVX_RHI_ERROR("Failed to create vertex array object");
            return 0;
        }

        // Set up vertex buffer bindings using DSA
        for (uint32 i = 0; i < key.vertexBufferCount; ++i)
        {
            const auto& binding = key.vertexBuffers[i];
            if (binding.buffer != 0)
            {
                GL_CHECK(glVertexArrayVertexBuffer(vao, i, binding.buffer, 
                                                   binding.offset, binding.stride));
                
                // Set divisor for instancing
                if (binding.divisor > 0)
                {
                    GL_CHECK(glVertexArrayBindingDivisor(vao, i, binding.divisor));
                }
            }
        }

        // Set up vertex attributes using DSA
        for (uint32 i = 0; i < key.attributeCount; ++i)
        {
            const auto& attr = key.attributes[i];
            
            GL_CHECK(glEnableVertexArrayAttrib(vao, attr.location));
            GL_CHECK(glVertexArrayAttribBinding(vao, attr.location, attr.binding));

            // Use the appropriate format function based on type
            if (attr.type == GL_FLOAT || attr.type == GL_HALF_FLOAT || 
                attr.type == GL_DOUBLE || attr.normalized)
            {
                GL_CHECK(glVertexArrayAttribFormat(vao, attr.location, attr.components,
                                                   attr.type, attr.normalized, attr.offset));
            }
            else
            {
                // Integer types
                GL_CHECK(glVertexArrayAttribIFormat(vao, attr.location, attr.components,
                                                    attr.type, attr.offset));
            }
        }

        // Bind index buffer if present
        if (key.indexBuffer != 0)
        {
            GL_CHECK(glVertexArrayElementBuffer(vao, key.indexBuffer));
        }

        // Debug label
        if (debugName && debugName[0])
        {
            OpenGLDebug::Get().SetVAOLabel(vao, debugName);
        }

        GL_DEBUG_TRACK(vao, GLResourceType::VAO, debugName);

        return vao;
    }

    void OpenGLVAOCache::DeleteVAO(GLuint vao)
    {
        if (vao != 0)
        {
            GL_CHECK(glDeleteVertexArrays(1, &vao));
            GL_DEBUG_UNTRACK(vao, GLResourceType::VAO);
        }
    }

    void OpenGLVAOCache::InvalidateBuffer(GLuint buffer)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        std::vector<VAOCacheKey> keysToRemove;

        for (const auto& [key, entry] : m_cache)
        {
            bool usesBuffer = false;

            // Check vertex buffers
            for (uint32 i = 0; i < key.vertexBufferCount; ++i)
            {
                if (key.vertexBuffers[i].buffer == buffer)
                {
                    usesBuffer = true;
                    break;
                }
            }

            // Check index buffer
            if (key.indexBuffer == buffer)
            {
                usesBuffer = true;
            }

            if (usesBuffer)
            {
                keysToRemove.push_back(key);
            }
        }

        for (const auto& key : keysToRemove)
        {
            auto it = m_cache.find(key);
            if (it != m_cache.end())
            {
                RVX_RHI_DEBUG("VAO Cache: invalidating VAO #{} due to buffer {} destruction",
                             it->second.vao, buffer);
                DeleteVAO(it->second.vao);
                m_cache.erase(it);
            }
        }
    }

    void OpenGLVAOCache::Cleanup(uint64 currentFrame)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        std::vector<VAOCacheKey> keysToRemove;

        for (const auto& [key, entry] : m_cache)
        {
            if (currentFrame > entry.lastUsedFrame + MAX_UNUSED_FRAMES)
            {
                keysToRemove.push_back(key);
            }
        }

        for (const auto& key : keysToRemove)
        {
            auto it = m_cache.find(key);
            if (it != m_cache.end())
            {
                RVX_RHI_DEBUG("VAO Cache: cleanup VAO #{} (unused for {} frames)",
                             it->second.vao, currentFrame - it->second.lastUsedFrame);
                DeleteVAO(it->second.vao);
                m_cache.erase(it);
            }
        }
    }

    void OpenGLVAOCache::Clear()
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        for (const auto& [key, entry] : m_cache)
        {
            DeleteVAO(entry.vao);
        }
        m_cache.clear();

        RVX_RHI_DEBUG("VAO Cache cleared");
    }

} // namespace RVX
