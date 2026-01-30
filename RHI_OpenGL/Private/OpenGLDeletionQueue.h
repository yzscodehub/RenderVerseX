#pragma once

#include "OpenGLCommon.h"
#include "OpenGLDebug.h"
#include <vector>
#include <mutex>
#include <functional>

namespace RVX
{
    // =============================================================================
    // Deletion Queue Entry
    // =============================================================================
    struct GLDeletionEntry
    {
        GLResourceType type;
        GLuint handle;
        uint64 frameQueued;
        std::string debugName;  // For debugging

        // Optional custom deleter for complex resources
        std::function<void()> customDeleter;
    };

    // =============================================================================
    // OpenGL Deletion Queue
    // Defers OpenGL resource deletion to ensure GPU has finished using them.
    // Resources are deleted after FRAME_DELAY frames have passed.
    // =============================================================================
    class OpenGLDeletionQueue
    {
    public:
        // Number of frames to wait before actually deleting resources
        // Should match the number of frames in flight (typically 2-3)
        static constexpr uint32 FRAME_DELAY = 3;

        OpenGLDeletionQueue() = default;
        ~OpenGLDeletionQueue();

        // Queue a resource for deletion
        void QueueBuffer(GLuint buffer, uint64 currentFrame, const char* debugName = nullptr);
        void QueueTexture(GLuint texture, uint64 currentFrame, const char* debugName = nullptr);
        void QueueSampler(GLuint sampler, uint64 currentFrame, const char* debugName = nullptr);
        void QueueShader(GLuint shader, uint64 currentFrame, const char* debugName = nullptr);
        void QueueProgram(GLuint program, uint64 currentFrame, const char* debugName = nullptr);
        void QueueVAO(GLuint vao, uint64 currentFrame, const char* debugName = nullptr);
        void QueueFBO(GLuint fbo, uint64 currentFrame, const char* debugName = nullptr);
        void QueueSync(GLsync sync, uint64 currentFrame);
        void QueueQuery(GLuint query, uint64 currentFrame, const char* debugName = nullptr);

        // Queue multiple queries for deletion (batch)
        void DeleteQueries(const std::vector<GLuint>& queries);

        // Queue a custom deletion operation
        void QueueCustomDeletion(std::function<void()> deleter, uint64 currentFrame, const char* debugName = nullptr);

        // Process the queue - delete resources that are safe to delete
        // Call this at the beginning of each frame
        void ProcessDeletions(uint64 currentFrame);

        // Force delete all pending resources (call during shutdown)
        void FlushAll();

        // Statistics
        uint32 GetPendingCount() const;

    private:
        void QueueDeletion(GLResourceType type, GLuint handle, uint64 currentFrame, 
                          const char* debugName, std::function<void()> customDeleter = nullptr);
        void DeleteResource(const GLDeletionEntry& entry);

        mutable std::mutex m_mutex;
        std::vector<GLDeletionEntry> m_pendingDeletions;
    };

} // namespace RVX
