#pragma once

/**
 * @file RenderIdentity.h
 * @brief Stable value identities used by immutable render contracts.
 */

#include "Core/Types.h"

#include <compare>
#include <cstddef>
#include <functional>

namespace RVX
{
    /** @brief Stable logical asset identity. */
    struct AssetId
    {
        uint64 value = 0;

        [[nodiscard]] bool IsValid() const noexcept { return value != 0; }
        auto operator<=>(const AssetId&) const = default;
    };

    /** @brief Generational identity for a Render-owned resource. */
    struct RenderResourceHandle
    {
        uint32 slot = 0;
        uint32 generation = 0;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return slot != 0 && generation != 0;
        }

        auto operator<=>(const RenderResourceHandle&) const = default;
    };

    /** @brief Hash stable asset identities by their stored value. */
    struct AssetIdHash final
    {
        [[nodiscard]] size_t operator()(AssetId id) const noexcept
        {
            return std::hash<uint64>{}(id.value);
        }
    };

    /** @brief Hash both parts of a generational render handle. */
    struct RenderResourceHandleHash final
    {
        [[nodiscard]] size_t operator()(RenderResourceHandle handle) const noexcept
        {
            const uint64 packed =
                (static_cast<uint64>(handle.slot) << 32U) |
                static_cast<uint64>(handle.generation);
            return std::hash<uint64>{}(packed);
        }
    };

    enum class RenderResourceKind : uint8
    {
        Invalid = 0,
        Mesh = 1,
        Texture = 2,
        Material = 3
    };

    enum class RenderUploadPriority : uint8
    {
        Low = 0,
        Normal = 1,
        High = 2
    };

    enum class RenderDependencyReadiness : uint8
    {
        RequireAll = 0,
        AllowFallback = 1
    };

    static_assert(static_cast<uint8>(RenderResourceKind::Invalid) == 0);
    static_assert(static_cast<uint8>(RenderResourceKind::Mesh) == 1);
    static_assert(static_cast<uint8>(RenderResourceKind::Texture) == 2);
    static_assert(static_cast<uint8>(RenderResourceKind::Material) == 3);
    static_assert(static_cast<uint8>(RenderUploadPriority::Low) == 0);
    static_assert(static_cast<uint8>(RenderUploadPriority::Normal) == 1);
    static_assert(static_cast<uint8>(RenderUploadPriority::High) == 2);
    static_assert(static_cast<uint8>(RenderDependencyReadiness::RequireAll) == 0);
    static_assert(static_cast<uint8>(RenderDependencyReadiness::AllowFallback) == 1);
} // namespace RVX
