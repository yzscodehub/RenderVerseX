#pragma once

/**
 * @file RenderTransportTypes.h
 * @brief Public bounded-transport capacities and render-iteration budgets.
 */

#include "Core/Types.h"

#include <chrono>

namespace RVX
{
    struct RenderTransportConfig
    {
        uint32 frameCapacity = 3;
        uint32 uploadRequestCapacity = 1024;
        uint64 uploadByteCapacity = 256ull * 1024ull * 1024ull;
        uint32 statusSlotCapacity = 262144;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return frameCapacity >= 2U && frameCapacity <= 4U &&
                   uploadRequestCapacity != 0U &&
                   uploadByteCapacity != 0U &&
                   statusSlotCapacity >= 1024U;
        }
    };

    struct RenderIterationBudgets
    {
        uint32 uploadRequestCount = 64;
        uint64 uploadBytes = 32ull * 1024ull * 1024ull;
        std::chrono::milliseconds uploadTime{2};
        uint32 releaseCount = 1024;
        std::chrono::milliseconds releaseTime{1};

        [[nodiscard]] bool IsValid() const noexcept
        {
            return uploadRequestCount != 0U && uploadBytes != 0U &&
                   uploadTime > std::chrono::milliseconds::zero() &&
                   releaseCount != 0U &&
                   releaseTime > std::chrono::milliseconds::zero();
        }
    };
} // namespace RVX
