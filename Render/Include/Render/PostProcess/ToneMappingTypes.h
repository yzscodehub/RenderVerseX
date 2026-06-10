#pragma once

/**
 * @file ToneMappingTypes.h
 * @brief Shared tone mapping configuration types
 */

#include "Core/Types.h"

namespace RVX
{
    /**
     * @brief Tone mapping operator types
     */
    enum class ToneMappingOperator : uint8
    {
        Reinhard,           // Simple Reinhard
        ReinhardExtended,   // Extended Reinhard with white point
        ACES,               // ACES filmic
        Uncharted2,         // Filmic curve from Uncharted 2
        Neutral,            // Neutral tonemapper
        None                // No tone mapping (pass-through)
    };

} // namespace RVX
