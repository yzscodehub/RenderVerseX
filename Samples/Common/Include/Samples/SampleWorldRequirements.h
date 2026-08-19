#pragma once

/**
 * @file SampleWorldRequirements.h
 * @brief World configuration requested before a Product Sample is set up.
 */

#include "World/World.h"

namespace RVX
{
    /** @brief Immutable pre-initialization requirements declared by a sample. */
    struct SampleWorldRequirements
    {
        WorldConfig world{};
    };
} // namespace RVX
