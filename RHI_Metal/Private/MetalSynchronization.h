#pragma once

#include "MetalCommon.h"
#include "RHI/RHISynchronization.h"
#include <atomic>

namespace RVX
{
    // =============================================================================
    // Metal Fence
    // Uses MTLSharedEvent for timeline semaphore functionality
    // Thread-safe implementation using atomic operations
    // =============================================================================
    class MetalFence : public RHIFence
    {
    public:
        MetalFence(id<MTLDevice> device, uint64 initialValue);
        ~MetalFence() override;

        uint64 GetCompletedValue() const override;
        void Signal(uint64 value) override;
        void Wait(uint64 value, uint64 timeoutNs = UINT64_MAX) override;

        // Signal from command buffer completion (thread-safe)
        void SignalFromCommandBuffer(id<MTLCommandBuffer> commandBuffer);

        // Get the next signal value without incrementing (for queries)
        uint64 GetPendingValue() const { return m_currentValue.load(std::memory_order_acquire); }

        id<MTLSharedEvent> GetMTLSharedEvent() const { return m_event; }

    private:
        id<MTLSharedEvent> m_event = nil;
        std::atomic<uint64> m_currentValue{0};
    };

} // namespace RVX
