#pragma once

/**
 * @file SkinningPaletteHash.h
 * @brief Canonical, finite skinning-palette hashing for value receipts.
 */

#include "Core/MathTypes.h"
#include "Core/Types.h"

#include <bit>
#include <cmath>
#include <limits>
#include <span>
#include <string_view>

namespace RVX
{
    inline constexpr uint64 RVX_SKINNING_PALETTE_FNV1A64_OFFSET_BASIS =
        14695981039346656037ULL;
    inline constexpr uint64 RVX_SKINNING_PALETTE_FNV1A64_PRIME =
        1099511628211ULL;

    /** @brief Canonical hash and cardinality for one complete skinning palette. */
    struct SkinningPaletteHash
    {
        uint64 value = 0;
        uint32 matrixCount = 0;
        bool valid = false;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return valid && value != 0 && matrixCount != 0;
        }
    };

    /**
     * @brief Hash finite matrices in column-major element order using FNV-1a64.
     *
     * Negative zero is normalized to positive zero before hashing so numerically
     * equivalent palette values produce an identical receipt. Non-finite input
     * deliberately has no hash and is rejected by callers.
     */
    [[nodiscard]] inline SkinningPaletteHash ComputeSkinningPaletteHash(
        std::span<const Mat4> palette) noexcept
    {
        SkinningPaletteHash result;
        if (palette.empty() ||
            palette.size() > std::numeric_limits<uint32>::max())
        {
            return result;
        }

        uint64 hash = RVX_SKINNING_PALETTE_FNV1A64_OFFSET_BASIS;
        const auto hashByte = [&hash](uint8 byte) noexcept
        {
            hash ^= byte;
            hash *= RVX_SKINNING_PALETTE_FNV1A64_PRIME;
        };
        constexpr std::string_view domain = "RVX.SkinningPalette.v1";
        for (const char character : domain)
        {
            hashByte(static_cast<uint8>(character));
        }
        // Delimit the fixed domain before the cardinality and value bytes.
        hashByte(0U);
        const uint32 matrixCount = static_cast<uint32>(palette.size());
        for (uint32 byteIndex = 0; byteIndex < sizeof(matrixCount);
             ++byteIndex)
        {
            hashByte(static_cast<uint8>(
                (matrixCount >> (byteIndex * 8U)) & 0xFFU));
        }
        for (const Mat4& matrix : palette)
        {
            for (uint32 column = 0; column < 4; ++column)
            {
                for (uint32 row = 0; row < 4; ++row)
                {
                    float32 value = matrix[column][row];
                    if (!std::isfinite(value))
                    {
                        return {};
                    }
                    if (value == 0.0f)
                    {
                        value = 0.0f;
                    }
                    const uint32 bits = std::bit_cast<uint32>(value);
                    for (uint32 byteIndex = 0; byteIndex < sizeof(bits);
                         ++byteIndex)
                    {
                        hashByte(static_cast<uint8>(
                            (bits >> (byteIndex * 8U)) & 0xFFU));
                    }
                }
            }
        }

        result.value = hash;
        result.matrixCount = matrixCount;
        result.valid = true;
        return result;
    }
} // namespace RVX
