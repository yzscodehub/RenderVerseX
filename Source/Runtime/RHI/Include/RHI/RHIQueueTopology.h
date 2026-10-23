#pragma once

/** @file RHIQueueTopology.h @brief Public physical queue-domain completion contract */

#include "RHI/RHIDefinitions.h"

#include <array>

namespace RVX
{
    /** @brief Stable physical submission domains exposed to Render. */
    enum class GPUQueueDomain : uint8
    {
        Graphics = 0,
        Compute,
        Copy,
    };

    /** @brief Backend completion mechanism used by the published queue topology. */
    enum class RHIQueueCompletionMode : uint8
    {
        None = 0,
        NativeTimeline,
        CompatibilityWaitIdle,
    };

    /** @brief One comparable point on exactly one physical queue-domain timeline. */
    struct GPUCompletionPoint
    {
        GPUQueueDomain domain = GPUQueueDomain::Graphics;
        uint64 value = 0;

        bool operator==(const GPUCompletionPoint&) const = default;
    };

    /** @brief Logical RHI queue mapping and completion behavior published by a backend. */
    struct RHIQueueTopology
    {
        RHIQueueCompletionMode completionMode = RHIQueueCompletionMode::None;
        std::array<GPUQueueDomain, 3> logicalQueueDomains = {
            GPUQueueDomain::Graphics,
            GPUQueueDomain::Graphics,
            GPUQueueDomain::Graphics,
        };
        uint8 activeDomainCount = 0;
    };

    constexpr bool IsDeclaredGPUQueueDomain(GPUQueueDomain domain)
    {
        return static_cast<uint8>(domain) <= static_cast<uint8>(GPUQueueDomain::Copy);
    }

    constexpr bool IsDeclaredQueueCompletionMode(RHIQueueCompletionMode mode)
    {
        return mode == RHIQueueCompletionMode::NativeTimeline ||
               mode == RHIQueueCompletionMode::CompatibilityWaitIdle;
    }

    constexpr bool TryGetGPUQueueDomain(const RHIQueueTopology& topology,
                                        RHICommandQueueType queueType,
                                        GPUQueueDomain& domain)
    {
        const uint8 index = static_cast<uint8>(queueType);
        if (index > static_cast<uint8>(RHICommandQueueType::Copy))
        {
            return false;
        }

        domain = topology.logicalQueueDomains[index];
        return IsDeclaredGPUQueueDomain(domain);
    }

    const char* GetGPUQueueDomainName(GPUQueueDomain domain);
    const char* GetRHIQueueCompletionModeName(RHIQueueCompletionMode mode);

} // namespace RVX
