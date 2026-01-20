#include "OpenGLSync.h"
#include "OpenGLDevice.h"
#include "Core/Log.h"

namespace RVX
{
    OpenGLFence::OpenGLFence(OpenGLDevice* device, uint64 initialValue)
        : m_device(device)
        , m_completedValue(initialValue)
        , m_signaledValue(initialValue)
    {
        RVX_RHI_DEBUG("Created OpenGL Fence (initial value: {})", initialValue);
    }

    OpenGLFence::~OpenGLFence()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        // Delete all pending sync objects
        for (auto& sp : m_pendingSyncs)
        {
            if (sp.sync)
            {
                glDeleteSync(sp.sync);
            }
        }
        m_pendingSyncs.clear();

        RVX_RHI_DEBUG("Destroyed OpenGL Fence");
    }

    uint64 OpenGLFence::GetCompletedValue() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        // Check all pending syncs for completion
        for (auto it = m_pendingSyncs.begin(); it != m_pendingSyncs.end();)
        {
            if (it->sync)
            {
                GLint status;
                glGetSynciv(it->sync, GL_SYNC_STATUS, sizeof(status), nullptr, &status);
                
                if (status == GL_SIGNALED)
                {
                    // Update completed value
                    if (it->value > m_completedValue.load())
                    {
                        m_completedValue.store(it->value);
                    }
                    
                    glDeleteSync(it->sync);
                    it = m_pendingSyncs.erase(it);
                    continue;
                }
            }
            ++it;
        }
        
        return m_completedValue.load();
    }

    void OpenGLFence::Signal(uint64 value)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        if (value <= m_signaledValue)
        {
            RVX_RHI_WARN("Fence::Signal value {} is not greater than current signaled value {}",
                        value, m_signaledValue);
            return;
        }

        // Insert a sync object
        GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (sync)
        {
            m_pendingSyncs.push_back({sync, value});
            m_signaledValue = value;
            RVX_RHI_DEBUG("Fence signaled with value {}", value);
        }
        else
        {
            RVX_RHI_ERROR("Failed to create fence sync object");
        }
    }

    void OpenGLFence::Wait(uint64 value, uint64 timeoutNs)
    {
        // First check if already completed
        if (GetCompletedValue() >= value)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        
        // Find the sync object for this value or the next higher one
        GLsync syncToWait = nullptr;
        for (const auto& sp : m_pendingSyncs)
        {
            if (sp.value >= value && sp.sync)
            {
                syncToWait = sp.sync;
                break;
            }
        }

        if (!syncToWait)
        {
            // Value already completed or not yet signaled
            return;
        }

        // Wait for the sync
        GLenum result = glClientWaitSync(syncToWait, GL_SYNC_FLUSH_COMMANDS_BIT,
                                        static_cast<GLuint64>(timeoutNs));
        
        switch (result)
        {
            case GL_ALREADY_SIGNALED:
            case GL_CONDITION_SATISFIED:
                RVX_RHI_DEBUG("Fence wait completed for value {}", value);
                break;
            case GL_TIMEOUT_EXPIRED:
                RVX_RHI_WARN("Fence wait timed out for value {}", value);
                break;
            case GL_WAIT_FAILED:
                RVX_RHI_ERROR("Fence wait failed for value {}", value);
                break;
        }

        // Update completed value
        m_completedValue.store(value);
    }

    void OpenGLFence::InsertSyncPoint(uint64 value)
    {
        Signal(value);
    }

    void OpenGLFence::CleanupCompletedSyncs()
    {
        // Called periodically to clean up completed syncs
        // This is done in GetCompletedValue()
    }

} // namespace RVX
