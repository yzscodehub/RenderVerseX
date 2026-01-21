/**
 * @file Constants.h
 * @brief Geometric constants and tolerances for numerical stability
 */

#pragma once

#include <cmath>
#include <limits>

namespace RVX::Geometry
{

// ============================================================================
// Tolerance Constants
// ============================================================================

/// General purpose epsilon for floating point comparisons
constexpr float EPSILON = 1e-6f;

/// Slightly larger epsilon for less precise operations
constexpr float EPSILON_LOOSE = 1e-4f;

/// Plane thickness tolerance for point-on-plane tests
constexpr float PLANE_THICKNESS = 1e-5f;

/// Tolerance for parallel line/ray tests
constexpr float PARALLEL_TOLERANCE = 1e-8f;

/// Tolerance for normalized vector checks
constexpr float NORMALIZE_TOLERANCE = 1e-8f;

/// Tolerance for area/volume degenerate checks
constexpr float DEGENERATE_TOLERANCE = 1e-10f;

// ============================================================================
// Mathematical Constants
// ============================================================================

/// Pi
constexpr float PI = 3.14159265358979323846f;

/// Two Pi
constexpr float TWO_PI = 2.0f * PI;

/// Half Pi
constexpr float HALF_PI = 0.5f * PI;

/// Inverse Pi
constexpr float INV_PI = 1.0f / PI;

/// Degrees to radians conversion factor
constexpr float DEG_TO_RAD = PI / 180.0f;

/// Radians to degrees conversion factor
constexpr float RAD_TO_DEG = 180.0f / PI;

// ============================================================================
// Utility Functions
// ============================================================================

/**
 * @brief Check if two floats are approximately equal
 */
inline bool ApproxEqual(float a, float b, float tolerance = EPSILON)
{
    return std::abs(a - b) <= tolerance;
}

/**
 * @brief Check if a float is approximately zero
 */
inline bool ApproxZero(float a, float tolerance = EPSILON)
{
    return std::abs(a) <= tolerance;
}

/**
 * @brief Clamp a value to [0, 1] range
 */
inline float Saturate(float x)
{
    return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

/**
 * @brief Square of a value
 */
inline float Square(float x)
{
    return x * x;
}

/**
 * @brief Safe reciprocal (returns large value instead of infinity for zero)
 */
inline float SafeReciprocal(float x, float fallback = 1e8f)
{
    return std::abs(x) > EPSILON ? 1.0f / x : fallback;
}

// ============================================================================
// Geometric Predicates
// ============================================================================

/**
 * @brief Orientation result for geometric predicates
 */
enum class Orientation : int8_t
{
    Negative = -1,  ///< Point is on negative side (clockwise in 2D)
    Zero = 0,       ///< Point is on the boundary (collinear/coplanar)
    Positive = 1    ///< Point is on positive side (counter-clockwise in 2D)
};

} // namespace RVX::Geometry
