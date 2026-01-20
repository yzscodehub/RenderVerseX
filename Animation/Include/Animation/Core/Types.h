/**
 * @file Types.h
 * @brief Animation system core types and constants
 * 
 * Migrated from found::animation::types
 */

#pragma once

#include <cstdint>
#include <string>

namespace RVX::Animation
{

// ============================================================================
// Time Types
// ============================================================================

/**
 * @brief Time unit in microseconds
 * 
 * All internal animation timestamps use microseconds for precision.
 */
using TimeUs = int64_t;

/// Microseconds per second constant
constexpr TimeUs kMicrosecondsPerSecond = 1'000'000;

/// Microseconds per millisecond constant
constexpr TimeUs kMicrosecondsPerMillisecond = 1'000;

/// Default frame rate
constexpr int kDefaultFrameRate = 30;

// ============================================================================
// Time Conversion Utilities
// ============================================================================

/**
 * @brief Convert frame number to time in microseconds
 */
inline constexpr TimeUs FrameToTimeUs(int frame, int fps = kDefaultFrameRate)
{
    return (static_cast<TimeUs>(frame) * kMicrosecondsPerSecond) / fps;
}

/**
 * @brief Convert time in microseconds to frame number
 */
inline constexpr int TimeUsToFrame(TimeUs timeUs, int fps = kDefaultFrameRate)
{
    return static_cast<int>((timeUs * fps) / kMicrosecondsPerSecond);
}

/**
 * @brief Convert seconds to microseconds
 */
inline constexpr TimeUs SecondsToTimeUs(double seconds)
{
    return static_cast<TimeUs>(seconds * kMicrosecondsPerSecond);
}

/**
 * @brief Convert microseconds to seconds
 */
inline constexpr double TimeUsToSeconds(TimeUs timeUs)
{
    return static_cast<double>(timeUs) / kMicrosecondsPerSecond;
}

/**
 * @brief Convert milliseconds to microseconds
 */
inline constexpr TimeUs MillisecondsToTimeUs(int64_t ms)
{
    return ms * kMicrosecondsPerMillisecond;
}

/**
 * @brief Convert microseconds to milliseconds
 */
inline constexpr int64_t TimeUsToMilliseconds(TimeUs timeUs)
{
    return timeUs / kMicrosecondsPerMillisecond;
}

// ============================================================================
// Animation Enumerations
// ============================================================================

/**
 * @brief Animation wrap/loop mode
 */
enum class WrapMode : uint8_t
{
    Once,           ///< Play once and stop at end
    Loop,           ///< Loop continuously from start
    PingPong,       ///< Alternate forward and backward
    ClampForever    ///< Play once and hold last frame
};

/**
 * @brief Interpolation mode for keyframes
 */
enum class InterpolationMode : uint8_t
{
    Step,           ///< No interpolation, hold previous value
    Linear,         ///< Linear interpolation
    CubicSpline,    ///< Cubic spline (glTF CUBICSPLINE)
    Bezier,         ///< Bezier curve (FBX)
    TCB             ///< Tension-Continuity-Bias (FBX)
};

/**
 * @brief Topology mode for vertex cache animation
 */
enum class TopologyMode : uint8_t
{
    Static,         ///< Fixed topology (vertex count unchanged)
    Dynamic,        ///< Dynamic topology (vertex/index count may change)
    Hybrid          ///< Mostly static with occasional topology changes
};

/**
 * @brief Track target type
 */
enum class TrackTargetType : uint8_t
{
    Bone,           ///< Skeleton bone
    Node,           ///< Scene node
    Mesh,           ///< Mesh (for BlendShape/VertexCache)
    Material,       ///< Material property
    Camera,         ///< Camera property
    Light           ///< Light property
};

/**
 * @brief Track data type
 */
enum class TrackType : uint8_t
{
    Transform,      ///< Transform track (TRS or Matrix)
    BlendShape,     ///< BlendShape/Morph target weights
    VertexCache,    ///< Vertex cache animation
    Property,       ///< Generic property animation
    Visibility      ///< Visibility toggle
};

/**
 * @brief Property value type for PropertyTrack
 */
enum class PropertyValueType : uint8_t
{
    Float,
    Vec2,
    Vec3,
    Vec4,
    Color,          ///< RGBA color (stored as vec4)
    Int,
    Bool
};

/**
 * @brief Animation layer blend mode
 */
enum class LayerBlendMode : uint8_t
{
    Override,       ///< Replace lower layers
    Additive        ///< Add to lower layers
};

/**
 * @brief Animation playback state
 */
enum class PlaybackState : uint8_t
{
    Stopped,
    Playing,
    Paused
};

// ============================================================================
// Enum String Conversion
// ============================================================================

inline const char* ToString(WrapMode mode)
{
    switch (mode)
    {
        case WrapMode::Once: return "Once";
        case WrapMode::Loop: return "Loop";
        case WrapMode::PingPong: return "PingPong";
        case WrapMode::ClampForever: return "ClampForever";
        default: return "Unknown";
    }
}

inline const char* ToString(InterpolationMode mode)
{
    switch (mode)
    {
        case InterpolationMode::Step: return "Step";
        case InterpolationMode::Linear: return "Linear";
        case InterpolationMode::CubicSpline: return "CubicSpline";
        case InterpolationMode::Bezier: return "Bezier";
        case InterpolationMode::TCB: return "TCB";
        default: return "Unknown";
    }
}

inline const char* ToString(TrackType type)
{
    switch (type)
    {
        case TrackType::Transform: return "Transform";
        case TrackType::BlendShape: return "BlendShape";
        case TrackType::VertexCache: return "VertexCache";
        case TrackType::Property: return "Property";
        case TrackType::Visibility: return "Visibility";
        default: return "Unknown";
    }
}

inline const char* ToString(PlaybackState state)
{
    switch (state)
    {
        case PlaybackState::Stopped: return "Stopped";
        case PlaybackState::Playing: return "Playing";
        case PlaybackState::Paused: return "Paused";
        default: return "Unknown";
    }
}

} // namespace RVX::Animation
