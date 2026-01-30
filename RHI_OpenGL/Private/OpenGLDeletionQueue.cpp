#include "OpenGLDeletionQueue.h"
#include "Core/Log.h"

namespace RVX
{
    OpenGLDeletionQueue::~OpenGLDeletionQueue()
    {
        if (!m_pendingDeletions.empty())
        {
            RVX_RHI_WARN("OpenGLDeletionQueue destroyed with {} pending deletions", 
                        m_pendingDeletions.size());
            FlushAll();
        }
    }

    void OpenGLDeletionQueue::QueueBuffer(GLuint buffer, uint64 currentFrame, const char* debugName)
    {
        QueueDeletion(GLResourceType::Buffer, buffer, currentFrame, debugName);
    }

    void OpenGLDeletionQueue::QueueTexture(GLuint texture, uint64 currentFrame, const char* debugName)
    {
        QueueDeletion(GLResourceType::Texture, texture, currentFrame, debugName);
    }

    void OpenGLDeletionQueue::QueueSampler(GLuint sampler, uint64 currentFrame, const char* debugName)
    {
        QueueDeletion(GLResourceType::Sampler, sampler, currentFrame, debugName);
    }

    void OpenGLDeletionQueue::QueueShader(GLuint shader, uint64 currentFrame, const char* debugName)
    {
        QueueDeletion(GLResourceType::Shader, shader, currentFrame, debugName);
    }

    void OpenGLDeletionQueue::QueueProgram(GLuint program, uint64 currentFrame, const char* debugName)
    {
        QueueDeletion(GLResourceType::Program, program, currentFrame, debugName);
    }

    void OpenGLDeletionQueue::QueueVAO(GLuint vao, uint64 currentFrame, const char* debugName)
    {
        QueueDeletion(GLResourceType::VAO, vao, currentFrame, debugName);
    }

    void OpenGLDeletionQueue::QueueFBO(GLuint fbo, uint64 currentFrame, const char* debugName)
    {
        QueueDeletion(GLResourceType::FBO, fbo, currentFrame, debugName);
    }

    void OpenGLDeletionQueue::QueueSync(GLsync sync, uint64 currentFrame)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        GLDeletionEntry entry;
        entry.type = GLResourceType::Sync;
        entry.handle = 0;  // GLsync is a pointer, not a handle
        entry.frameQueued = currentFrame;
        entry.debugName = "GLsync";
        entry.customDeleter = [sync]() {
            if (sync)
            {
                glDeleteSync(sync);
            }
        };
        
        m_pendingDeletions.push_back(std::move(entry));
        
        RVX_RHI_DEBUG("Queued GLsync for deletion (frame {})", currentFrame);
    }

    void OpenGLDeletionQueue::QueueQuery(GLuint query, uint64 currentFrame, const char* debugName)
    {
        QueueDeletion(GLResourceType::Query, query, currentFrame, debugName);
    }

    void OpenGLDeletionQueue::DeleteQueries(const std::vector<GLuint>& queries)
    {
        if (queries.empty()) return;

        // For immediate deletion without frame delay (used during cleanup)
        GL_CHECK(glDeleteQueries(static_cast<GLsizei>(queries.size()), queries.data()));
        
        RVX_RHI_DEBUG("Deleted {} queries immediately", queries.size());
    }

    void OpenGLDeletionQueue::QueueCustomDeletion(std::function<void()> deleter, uint64 currentFrame, const char* debugName)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        GLDeletionEntry entry;
        entry.type = GLResourceType::Unknown;
        entry.handle = 0;
        entry.frameQueued = currentFrame;
        entry.debugName = debugName ? debugName : "Custom";
        entry.customDeleter = std::move(deleter);
        
        m_pendingDeletions.push_back(std::move(entry));
    }

    void OpenGLDeletionQueue::QueueDeletion(GLResourceType type, GLuint handle, uint64 currentFrame, 
                                            const char* debugName, std::function<void()> customDeleter)
    {
        if (handle == 0) return;

        std::lock_guard<std::mutex> lock(m_mutex);
        
        GLDeletionEntry entry;
        entry.type = type;
        entry.handle = handle;
        entry.frameQueued = currentFrame;
        entry.debugName = debugName ? debugName : "";
        entry.customDeleter = std::move(customDeleter);
        
        m_pendingDeletions.push_back(std::move(entry));
        
        RVX_RHI_DEBUG("Queued {} #{} '{}' for deletion (frame {})", 
                     ToString(type), handle, entry.debugName, currentFrame);
    }

    void OpenGLDeletionQueue::ProcessDeletions(uint64 currentFrame)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        if (m_pendingDeletions.empty()) return;

        size_t deletedCount = 0;
        
        // Iterate and delete resources that have waited long enough
        auto it = m_pendingDeletions.begin();
        while (it != m_pendingDeletions.end())
        {
            // Check if enough frames have passed
            if (currentFrame >= it->frameQueued + FRAME_DELAY)
            {
                DeleteResource(*it);
                it = m_pendingDeletions.erase(it);
                deletedCount++;
            }
            else
            {
                ++it;
            }
        }

        if (deletedCount > 0)
        {
            RVX_RHI_DEBUG("Processed {} deferred deletions (frame {})", deletedCount, currentFrame);
        }
    }

    void OpenGLDeletionQueue::FlushAll()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        RVX_RHI_INFO("Flushing all {} pending deletions", m_pendingDeletions.size());
        
        for (const auto& entry : m_pendingDeletions)
        {
            DeleteResource(entry);
        }
        
        m_pendingDeletions.clear();
    }

    void OpenGLDeletionQueue::DeleteResource(const GLDeletionEntry& entry)
    {
        // If custom deleter is provided, use it
        if (entry.customDeleter)
        {
            entry.customDeleter();
            GL_DEBUG_UNTRACK(entry.handle, entry.type);
            return;
        }

        // Otherwise, use type-specific deletion
        switch (entry.type)
        {
            case GLResourceType::Buffer:
                if (glIsBuffer(entry.handle))
                {
                    GL_CHECK(glDeleteBuffers(1, &entry.handle));
                    RVX_RHI_DEBUG("Deleted Buffer #{} '{}'", entry.handle, entry.debugName);
                }
                else
                {
                    RVX_RHI_WARN("Attempted to delete invalid Buffer #{}", entry.handle);
                }
                break;

            case GLResourceType::Texture:
                if (glIsTexture(entry.handle))
                {
                    GL_CHECK(glDeleteTextures(1, &entry.handle));
                    RVX_RHI_DEBUG("Deleted Texture #{} '{}'", entry.handle, entry.debugName);
                }
                else
                {
                    RVX_RHI_WARN("Attempted to delete invalid Texture #{}", entry.handle);
                }
                break;

            case GLResourceType::Sampler:
                if (glIsSampler(entry.handle))
                {
                    GL_CHECK(glDeleteSamplers(1, &entry.handle));
                    RVX_RHI_DEBUG("Deleted Sampler #{} '{}'", entry.handle, entry.debugName);
                }
                else
                {
                    RVX_RHI_WARN("Attempted to delete invalid Sampler #{}", entry.handle);
                }
                break;

            case GLResourceType::Shader:
                if (glIsShader(entry.handle))
                {
                    GL_CHECK(glDeleteShader(entry.handle));
                    RVX_RHI_DEBUG("Deleted Shader #{} '{}'", entry.handle, entry.debugName);
                }
                else
                {
                    RVX_RHI_WARN("Attempted to delete invalid Shader #{}", entry.handle);
                }
                break;

            case GLResourceType::Program:
                if (glIsProgram(entry.handle))
                {
                    GL_CHECK(glDeleteProgram(entry.handle));
                    RVX_RHI_DEBUG("Deleted Program #{} '{}'", entry.handle, entry.debugName);
                }
                else
                {
                    RVX_RHI_WARN("Attempted to delete invalid Program #{}", entry.handle);
                }
                break;

            case GLResourceType::VAO:
                if (glIsVertexArray(entry.handle))
                {
                    GL_CHECK(glDeleteVertexArrays(1, &entry.handle));
                    RVX_RHI_DEBUG("Deleted VAO #{} '{}'", entry.handle, entry.debugName);
                }
                else
                {
                    RVX_RHI_WARN("Attempted to delete invalid VAO #{}", entry.handle);
                }
                break;

            case GLResourceType::FBO:
                if (glIsFramebuffer(entry.handle))
                {
                    GL_CHECK(glDeleteFramebuffers(1, &entry.handle));
                    RVX_RHI_DEBUG("Deleted FBO #{} '{}'", entry.handle, entry.debugName);
                }
                else
                {
                    RVX_RHI_WARN("Attempted to delete invalid FBO #{}", entry.handle);
                }
                break;

            case GLResourceType::Query:
                if (glIsQuery(entry.handle))
                {
                    GL_CHECK(glDeleteQueries(1, &entry.handle));
                    RVX_RHI_DEBUG("Deleted Query #{} '{}'", entry.handle, entry.debugName);
                }
                else
                {
                    RVX_RHI_WARN("Attempted to delete invalid Query #{}", entry.handle);
                }
                break;

            default:
                RVX_RHI_WARN("Unknown resource type {} for deletion", static_cast<int>(entry.type));
                break;
        }

        GL_DEBUG_UNTRACK(entry.handle, entry.type);
    }

    uint32 OpenGLDeletionQueue::GetPendingCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return static_cast<uint32>(m_pendingDeletions.size());
    }

} // namespace RVX
