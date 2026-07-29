#pragma once

/**
 * @file ISkinningPaletteProvider.h
 * @brief Scene-owned contract for exposing immutable skinning palettes.
 */

#include "Core/MathTypes.h"

#include <span>

namespace RVX
{
    /**
     * @brief Supplies the current skinning palette during render extraction.
     *
     * The returned view remains valid for the duration of the extraction call.
     * Implementations are owned by higher-level feature modules such as
     * Animation; Scene depends only on this contract.
     */
    class ISkinningPaletteProvider
    {
    public:
        virtual ~ISkinningPaletteProvider() = default;

        [[nodiscard]] virtual std::span<const Mat4>
            GetSkinningPalette() const noexcept = 0;
    };
} // namespace RVX
