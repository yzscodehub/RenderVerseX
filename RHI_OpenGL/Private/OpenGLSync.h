#pragma once

#include "OpenGLCommon.h"
#include "OpenGLDebug.h"
#include "RHI/RHISynchronization.h"
#include <atomic>
#include <vector>
#include <mutex>

namespace RVX
{
    class OpenGLDevice;

    // =============================================================================
    // OpenGL Fence (GLsync wrapper)
    // =============================================================================
    class OpenGLFence : public RHIFence
    {
    public:
        OpenGLFence(OpenGLDevice* device, uint64 initialValue);
        ~OpenGLFence() override;

        // RHIFence interface
        uint64 GetCompletedValue() const override;
        void Signal(uint64 value) override;
        void SignalOnQueue(uint64 value, RHICommandQueueType queueType) override;
        void Wait(uint64 value, uint64 timeoutNs = UINT64_MAX) override;

        // OpenGL specific
        void InsertSyncPoint(uint64 value);

    private:
        struct SyncPoint
        {
            GLsync sync = nullptr;
            uint64 value = 0;
        };

        void CleanupCompletedSyncs();

        OpenGLDevice* m_device = nullptr;
        mutable std::mutex m_mutex;
        mutable std::vector<SyncPoint> m_pendingSyncs;  // mutable for cleanup in const GetCompletedValue
        mutable std::atomic<uint64> m_completedValue;
        uint64 m_signaledValue = 0;
    };

} // namespace RVX
