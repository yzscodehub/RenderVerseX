#include "Render/Renderer/RenderDrawItem.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace RVX
{
    namespace
    {
        uint64 MixSortBits(uint64 value)
        {
            value ^= value >> 33;
            value *= 0xff51afd7ed558ccdULL;
            value ^= value >> 33;
            value *= 0xc4ceb9fe1a85ec53ULL;
            value ^= value >> 33;
            return value;
        }
    } // namespace

    uint64 BuildOpaqueDrawSortKey(const RenderDrawItem& item)
    {
        const uint64 materialBits = MixSortBits(item.materialId) & 0xFFFF'FFFFULL;
        const uint64 meshBits = MixSortBits(item.meshId) & 0xFFFF'0000ULL;
        return (materialBits << 32) | meshBits | static_cast<uint64>(item.submeshIndex);
    }

    uint64 BuildTransparentDrawSortKey(const RenderDrawItem& item)
    {
        const float safeDepth = std::isfinite(item.depthFromCamera) ? item.depthFromCamera : 0.0f;
        const float clampedDepth = std::clamp(safeDepth, 0.0f, 1000000.0f);
        const uint64 depthBits = static_cast<uint64>(clampedDepth * 1000.0f);
        return (depthBits << 24) | (MixSortBits(item.materialId) & 0x00FF'FFFFULL);
    }

} // namespace RVX
