#pragma once

#include "OpenGLCommon.h"
#include "OpenGLDebug.h"
#include "RHI/RHIDefinitions.h"
#include <unordered_map>
#include <vector>
#include <list>
#include <mutex>
#include <functional>

namespace RVX
{
    // =============================================================================
    // FBO Cache Key
    // Used to uniquely identify a framebuffer configuration
    // =============================================================================
    struct FBOCacheKey
    {
        static constexpr uint32 MAX_COLOR_ATTACHMENTS = 8;

        // Color attachments
        struct ColorAttachment
        {
            GLuint texture = 0;
            uint32 mipLevel = 0;
            uint32 arrayLayer = 0;
            GLenum format = 0;

            bool operator==(const ColorAttachment& other) const
            {
                return texture == other.texture && mipLevel == other.mipLevel &&
                       arrayLayer == other.arrayLayer && format == other.format;
            }
        };
        std::array<ColorAttachment, MAX_COLOR_ATTACHMENTS> colorAttachments;
        uint32 colorAttachmentCount = 0;

        // Depth-stencil attachment
        GLuint depthStencilTexture = 0;
        uint32 depthStencilMipLevel = 0;
        uint32 depthStencilArrayLayer = 0;
        GLenum depthStencilFormat = 0;

        // Dimensions
        uint32 width = 0;
        uint32 height = 0;
        uint32 layers = 1;  // For layered rendering

        bool operator==(const FBOCacheKey& other) const
        {
            if (colorAttachmentCount != other.colorAttachmentCount) return false;
            if (depthStencilTexture != other.depthStencilTexture) return false;
            if (depthStencilMipLevel != other.depthStencilMipLevel) return false;
            if (depthStencilArrayLayer != other.depthStencilArrayLayer) return false;
            if (width != other.width || height != other.height) return false;
            if (layers != other.layers) return false;

            for (uint32 i = 0; i < colorAttachmentCount; ++i)
            {
                if (!(colorAttachments[i] == other.colorAttachments[i]))
                    return false;
            }
            return true;
        }

        struct Hash
        {
            size_t operator()(const FBOCacheKey& key) const
            {
                size_t hash = 0;
                auto combine = [&hash](size_t value) {
                    hash ^= value + 0x9e3779b9 + (hash << 6) + (hash >> 2);
                };

                combine(key.colorAttachmentCount);
                for (uint32 i = 0; i < key.colorAttachmentCount; ++i)
                {
                    combine(key.colorAttachments[i].texture);
                    combine(key.colorAttachments[i].mipLevel);
                    combine(key.colorAttachments[i].arrayLayer);
                }
                combine(key.depthStencilTexture);
                combine(key.depthStencilMipLevel);
                combine(key.width);
                combine(key.height);
                return hash;
            }
        };
    };

    // =============================================================================
    // Cached FBO Entry
    // =============================================================================
    struct CachedFBO
    {
        GLuint fbo = 0;
        FBOCacheKey key;
        uint64 lastUsedFrame = 0;
        std::string debugName;
    };

    // =============================================================================
    // OpenGL Framebuffer Cache
    // Caches FBOs based on their attachment configuration
    // =============================================================================
    class OpenGLFramebufferCache
    {
    public:
        static constexpr size_t MAX_CACHED_FBOS = 64;
        static constexpr uint64 MAX_UNUSED_FRAMES = 120;  // ~2 seconds at 60fps

        OpenGLFramebufferCache() = default;
        ~OpenGLFramebufferCache();

        // Get or create an FBO for the given configuration
        GLuint GetOrCreateFBO(const FBOCacheKey& key, uint64 currentFrame, const char* debugName = nullptr);

        // Invalidate all FBOs that reference the given texture (call when texture is destroyed)
        void InvalidateTexture(GLuint texture);

        // Clear unused FBOs (call periodically, e.g., at frame end)
        void Cleanup(uint64 currentFrame);

        // Clear all cached FBOs
        void Clear();

        // Statistics
        uint32 GetCachedCount() const { return static_cast<uint32>(m_cache.size()); }
        uint32 GetHits() const { return m_hits; }
        uint32 GetMisses() const { return m_misses; }
        void ResetStats() { m_hits = m_misses = 0; }

    private:
        GLuint CreateFBO(const FBOCacheKey& key, const char* debugName);
        void DeleteFBO(GLuint fbo);

        std::unordered_map<FBOCacheKey, CachedFBO, FBOCacheKey::Hash> m_cache;
        std::list<FBOCacheKey> m_lruList;  // For LRU eviction
        
        mutable std::mutex m_mutex;
        uint32 m_hits = 0;
        uint32 m_misses = 0;
    };

    // =============================================================================
    // VAO Cache Key
    // Used to uniquely identify a vertex array configuration
    // =============================================================================
    struct VAOCacheKey
    {
        static constexpr uint32 MAX_VERTEX_BUFFERS = 16;
        static constexpr uint32 MAX_VERTEX_ATTRIBUTES = 16;

        // Vertex buffer bindings
        struct VertexBufferBinding
        {
            GLuint buffer = 0;
            GLsizei stride = 0;
            GLintptr offset = 0;
            uint32 divisor = 0;  // 0 = per-vertex, >0 = per-instance

            bool operator==(const VertexBufferBinding& other) const
            {
                return buffer == other.buffer && stride == other.stride &&
                       offset == other.offset && divisor == other.divisor;
            }
        };
        std::array<VertexBufferBinding, MAX_VERTEX_BUFFERS> vertexBuffers;
        uint32 vertexBufferCount = 0;

        // Index buffer
        GLuint indexBuffer = 0;

        // Vertex attributes (from pipeline layout)
        struct VertexAttribute
        {
            uint32 location = 0;
            uint32 binding = 0;    // Which vertex buffer to use
            GLenum type = GL_FLOAT;
            GLint components = 4;
            GLboolean normalized = GL_FALSE;
            uint32 offset = 0;

            bool operator==(const VertexAttribute& other) const
            {
                return location == other.location && binding == other.binding &&
                       type == other.type && components == other.components &&
                       normalized == other.normalized && offset == other.offset;
            }
        };
        std::array<VertexAttribute, MAX_VERTEX_ATTRIBUTES> attributes;
        uint32 attributeCount = 0;

        // Pipeline vertex layout hash (for quick comparison)
        uint64 pipelineLayoutHash = 0;

        bool operator==(const VAOCacheKey& other) const
        {
            if (vertexBufferCount != other.vertexBufferCount) return false;
            if (indexBuffer != other.indexBuffer) return false;
            if (attributeCount != other.attributeCount) return false;
            if (pipelineLayoutHash != other.pipelineLayoutHash) return false;

            for (uint32 i = 0; i < vertexBufferCount; ++i)
            {
                if (!(vertexBuffers[i] == other.vertexBuffers[i]))
                    return false;
            }
            for (uint32 i = 0; i < attributeCount; ++i)
            {
                if (!(attributes[i] == other.attributes[i]))
                    return false;
            }
            return true;
        }

        struct Hash
        {
            size_t operator()(const VAOCacheKey& key) const
            {
                size_t hash = 0;
                auto combine = [&hash](size_t value) {
                    hash ^= value + 0x9e3779b9 + (hash << 6) + (hash >> 2);
                };

                combine(key.pipelineLayoutHash);
                combine(key.indexBuffer);
                for (uint32 i = 0; i < key.vertexBufferCount; ++i)
                {
                    combine(key.vertexBuffers[i].buffer);
                    combine(key.vertexBuffers[i].stride);
                }
                return hash;
            }
        };
    };

    // =============================================================================
    // Cached VAO Entry
    // =============================================================================
    struct CachedVAO
    {
        GLuint vao = 0;
        VAOCacheKey key;
        uint64 lastUsedFrame = 0;
        std::string debugName;
    };

    // =============================================================================
    // OpenGL VAO Cache
    // Caches VAOs based on pipeline + vertex buffer binding configuration
    // =============================================================================
    class OpenGLVAOCache
    {
    public:
        static constexpr size_t MAX_CACHED_VAOS = 128;
        static constexpr uint64 MAX_UNUSED_FRAMES = 120;

        OpenGLVAOCache() = default;
        ~OpenGLVAOCache();

        // Get or create a VAO for the given configuration
        GLuint GetOrCreateVAO(const VAOCacheKey& key, uint64 currentFrame, const char* debugName = nullptr);

        // Invalidate all VAOs that reference the given buffer (call when buffer is destroyed)
        void InvalidateBuffer(GLuint buffer);

        // Clear unused VAOs
        void Cleanup(uint64 currentFrame);

        // Clear all cached VAOs
        void Clear();

        // Statistics
        uint32 GetCachedCount() const { return static_cast<uint32>(m_cache.size()); }
        uint32 GetHits() const { return m_hits; }
        uint32 GetMisses() const { return m_misses; }
        void ResetStats() { m_hits = m_misses = 0; }

    private:
        GLuint CreateVAO(const VAOCacheKey& key, const char* debugName);
        void DeleteVAO(GLuint vao);

        std::unordered_map<VAOCacheKey, CachedVAO, VAOCacheKey::Hash> m_cache;
        std::list<VAOCacheKey> m_lruList;
        
        mutable std::mutex m_mutex;
        uint32 m_hits = 0;
        uint32 m_misses = 0;
    };

} // namespace RVX
