#include "MetalSynchronization.h"

namespace RVX
{
    MetalFence::MetalFence(id<MTLDevice> device, uint64 initialValue)
    {
        m_currentValue.store(initialValue, std::memory_order_release);
        m_event = [device newSharedEvent];
        m_event.signaledValue = initialValue;
    }

    MetalFence::~MetalFence()
    {
        m_event = nil;
    }

    uint64 MetalFence::GetCompletedValue() const
    {
        return m_event.signaledValue;
    }

    void MetalFence::Signal(uint64 value)
    {
        m_event.signaledValue = value;
        m_currentValue.store(value, std::memory_order_release);
    }

    void MetalFence::Wait(uint64 value, uint64 timeoutNs)
    {
        // Fast path: check if already signaled
        if (m_event.signaledValue >= value)
        {
            return;
        }

        // Create a listener to wait for the event
        MTLSharedEventListener* listener = [[MTLSharedEventListener alloc] init];

        dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);

        [m_event notifyListener:listener
                        atValue:value
                          block:^(id<MTLSharedEvent> event, uint64_t val) {
            dispatch_semaphore_signal(semaphore);
        }];

        // Wait for the semaphore with timeout
        dispatch_time_t timeout = (timeoutNs == UINT64_MAX) 
            ? DISPATCH_TIME_FOREVER 
            : dispatch_time(DISPATCH_TIME_NOW, (int64_t)timeoutNs);
        dispatch_semaphore_wait(semaphore, timeout);
    }

    void MetalFence::SignalFromCommandBuffer(id<MTLCommandBuffer> commandBuffer)
    {
        // Thread-safe increment using atomic fetch_add
        uint64 newValue = m_currentValue.fetch_add(1, std::memory_order_acq_rel) + 1;
        [commandBuffer encodeSignalEvent:m_event value:newValue];
    }

} // namespace RVX
