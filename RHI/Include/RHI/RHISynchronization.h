#pragma once

#include "RHI/RHIResources.h"

namespace RVX
{
    // =============================================================================
    // Fence Interface
    // =============================================================================
    class RHIFence : public RHIResource
    {
    public:
        virtual ~RHIFence() = default;

        virtual uint64 GetCompletedValue() const = 0;
        virtual void Signal(uint64 value) = 0;
        virtual void Wait(uint64 value, uint64 timeoutNs = UINT64_MAX) = 0;
    };

} // namespace RVX
