#pragma once

#include "Core/MathTypes.h"
#include "Core/Types.h"

#include <algorithm>
#include <cmath>

namespace RVX
{
    // Bounded coordinates keep all float-to-integer conversions representable.
    inline constexpr uint32 RVX_TRANSMISSION_FOOTPRINT_MAX_EXTENT = 65536;

    struct TransmissionTexelBounds
    {
        UVec2 first{0};
        UVec2 last{0};
        bool valid = false;
    };

    struct TransmissionMipFootprint
    {
        uint32 lowerMip = 0;
        uint32 upperMip = 0;
        TransmissionTexelBounds lower{};
        TransmissionTexelBounds upper{};
        bool valid = false;
    };

    [[nodiscard]] inline uint32 GetTransmissionMipCount(UVec2 extent) noexcept
    {
        if (extent.x == 0 || extent.y == 0 ||
            extent.x > RVX_TRANSMISSION_FOOTPRINT_MAX_EXTENT ||
            extent.y > RVX_TRANSMISSION_FOOTPRINT_MAX_EXTENT) return 0;
        uint32 count = 1;
        while (extent.x > 1 || extent.y > 1)
        {
            extent = {std::max(1u, extent.x / 2), std::max(1u, extent.y / 2)};
            ++count;
        }
        return count;
    }

    [[nodiscard]] inline UVec2 GetTransmissionMipExtent(UVec2 base, uint32 mip) noexcept
    {
        if (mip >= GetTransmissionMipCount(base)) return UVec2(0);
        return {std::max(1u, base.x >> mip), std::max(1u, base.y >> mip)};
    }

    /**
     * Inclusive support of the existing four-bilinear color reduction (or
     * full-resolution linear copy). The one-source-texel guard band covers
     * coordinate/filter rounding; it may over-reject, never omit a contributor.
     * Consumers must recursively reduce these bounds, not assume a plain 2x2
     * box or equal mip numbers imply equal source coverage.
     */
    [[nodiscard]] inline TransmissionTexelBounds ResolveTransmissionReductionFootprint(
        Vec2 uv, UVec2 sourceExtent, bool fullResolution) noexcept
    {
        TransmissionTexelBounds result;
        if (GetTransmissionMipCount(sourceExtent) == 0 ||
            !std::isfinite(uv.x) || !std::isfinite(uv.y) ||
            uv.x < 0 || uv.y < 0 || uv.x > 1 || uv.y > 1) return result;
        const float32 radius = fullResolution ? 0.0f : 0.5f;
        for (uint32 axis = 0; axis < 2; ++axis)
        {
            const float32 coordinate = uv[axis] * static_cast<float32>(sourceExtent[axis]) - 0.5f;
            const int32 maximum = static_cast<int32>(sourceExtent[axis] - 1);
            result.first[axis] = static_cast<uint32>(std::clamp(
                static_cast<int32>(std::floor(coordinate - radius)) - 1, 0, maximum));
            result.last[axis] = static_cast<uint32>(std::clamp(
                static_cast<int32>(std::floor(coordinate + radius)) + 2, 0, maximum));
        }
        result.valid = true;
        return result;
    }

    [[nodiscard]] inline TransmissionTexelBounds ResolveTransmissionBilinearFootprint(
        Vec2 uv, UVec2 extent) noexcept
    {
        return ResolveTransmissionReductionFootprint(uv, extent, true);
    }

    [[nodiscard]] inline TransmissionMipFootprint ResolveTransmissionMipFootprint(
        Vec2 uv, UVec2 baseExtent, uint32 mipCount, float32 lod) noexcept
    {
        TransmissionMipFootprint result;
        const uint32 available = GetTransmissionMipCount(baseExtent);
        if (mipCount == 0 || mipCount > available || !std::isfinite(lod)) return result;
        const float32 bounded = std::clamp(lod, 0.0f, static_cast<float32>(mipCount - 1));
        result.lowerMip = static_cast<uint32>(std::floor(bounded));
        result.upperMip = static_cast<uint32>(std::ceil(bounded));
        result.lower = ResolveTransmissionBilinearFootprint(
            uv, GetTransmissionMipExtent(baseExtent, result.lowerMip));
        result.upper = ResolveTransmissionBilinearFootprint(
            uv, GetTransmissionMipExtent(baseExtent, result.upperMip));
        result.valid = result.lower.valid && result.upper.valid;
        return result;
    }

    /** Increasing [0,1] depth on both backends. Invalid data is an occluder. */
    [[nodiscard]] inline float32 CanonicalTransmissionDepth(float32 depth, bool reverseZ) noexcept
    {
        if (!std::isfinite(depth) || depth < 0.0f || depth > 1.0f) return 0.0f;
        return reverseZ ? 1.0f - depth : depth;
    }
}
