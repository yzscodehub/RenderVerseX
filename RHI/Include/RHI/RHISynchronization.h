#pragma once

#include "RHI/RHIResources.h"
#include "RHI/RHIDefinitions.h"

namespace RVX
{
    // =============================================================================
    // Fence Interface
    // =============================================================================
    class RHIFence : public RHIResource
    {
    public:
        virtual ~RHIFence() = default;

        /**
         * @brief Get the last completed fence value
         */
        virtual uint64 GetCompletedValue() const = 0;
        
        /**
         * @brief Signal the fence from CPU (graphics queue by default)
         * @param value The value to signal
         */
        virtual void Signal(uint64 value) = 0;
        
        /**
         * @brief Signal the fence on a specific GPU queue
         * @param value The value to signal
         * @param queueType The queue to signal on
         */
        virtual void SignalOnQueue(uint64 value, RHICommandQueueType queueType) = 0;
        
        /**
         * @brief Wait for the fence to reach a value (CPU wait)
         * @param value The value to wait for
         * @param timeoutNs Timeout in nanoseconds (UINT64_MAX = infinite)
         */
        virtual void Wait(uint64 value, uint64 timeoutNs = UINT64_MAX) = 0;
    };

} // namespace RVX
