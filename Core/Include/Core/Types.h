#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <memory>
#include <vector>
#include <array>
#include <span>
#include <optional>
#include <functional>
#include <type_traits>

namespace RVX
{
    // =============================================================================
    // Fixed-width integer types
    // =============================================================================
    using int8   = std::int8_t;
    using int16  = std::int16_t;
    using int32  = std::int32_t;
    using int64  = std::int64_t;

    using uint8  = std::uint8_t;
    using uint16 = std::uint16_t;
    using uint32 = std::uint32_t;
    using uint64 = std::uint64_t;

    using float32 = float;
    using float64 = double;

    // =============================================================================
    // Common constants
    // =============================================================================
    constexpr uint32 RVX_ALL_MIPS      = ~0u;
    constexpr uint32 RVX_ALL_LAYERS    = ~0u;
    constexpr uint64 RVX_WHOLE_SIZE    = ~0ull;
    constexpr uint32 RVX_INVALID_INDEX = ~0u;

    // =============================================================================
    // Frame constants
    // =============================================================================
    constexpr uint32 RVX_MAX_FRAME_COUNT        = 3;
    constexpr uint32 RVX_MAX_RENDER_TARGETS     = 8;
    constexpr uint32 RVX_MAX_VERTEX_BUFFERS     = 16;
    constexpr uint32 RVX_MAX_DESCRIPTOR_SETS    = 4;
    constexpr uint32 RVX_MAX_PUSH_CONSTANT_SIZE = 128;

    // =============================================================================
    // Non-copyable base class
    // =============================================================================
    class NonCopyable
    {
    protected:
        NonCopyable() = default;
        ~NonCopyable() = default;

    private:
        NonCopyable(const NonCopyable&) = delete;
        NonCopyable& operator=(const NonCopyable&) = delete;
    };

    // =============================================================================
    // Non-movable base class
    // =============================================================================
    class NonMovable : public NonCopyable
    {
    protected:
        NonMovable() = default;
        ~NonMovable() = default;

    private:
        NonMovable(NonMovable&&) = delete;
        NonMovable& operator=(NonMovable&&) = delete;
    };

} // namespace RVX
