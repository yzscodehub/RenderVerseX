#pragma once

/**
 * @file GerstnerSurface.h
 * @brief Pure, deterministic gameplay-side evaluator for the Water packet.
 */

#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "RenderContracts/WaterRenderSnapshot.h"

namespace RVX::Water
{
    /** @brief Analytical surface information at one local XZ coordinate. */
    struct GerstnerSurfaceEvaluation
    {
        bool valid = false;
        Vec3 displacement{0.0f};
        Vec3 surfacePosition{0.0f};
        Vec3 normal{0.0f, 1.0f, 0.0f};
        Vec3 tangentX{1.0f, 0.0f, 0.0f};
        Vec3 tangentZ{0.0f, 0.0f, 1.0f};
        Vec3 horizontalVelocity{0.0f};
        float32 signedHeight = 0.0f;
    };

    /** @brief Explicit signed vertical distance from a local point to the evaluated surface. */
    struct GerstnerSignedVerticalDistance
    {
        bool valid = false;
        float32 signedVerticalDistance = 0.0f;
        Vec3 surfacePosition{0.0f};
    };

    /**
     * @brief Evaluates a sealed Gerstner packet without state, allocation, or I/O.
     *
     * `localPositionXZ` is expressed relative to the horizontal translation
     * stored in the water packet. `simulationSeconds` must be the caller's
     * frozen deterministic frame time; this API never reads a wall clock.
     * Each wave reduces that finite time by its own period in float64 before
     * phase evaluation, preventing long-running temporal overflow. Horizontal
     * amplitude is `steepness / waveNumber`; packet validation bounds the sum
     * of active steepness at one.
     */
    [[nodiscard]] GerstnerSurfaceEvaluation EvaluateGerstnerSurface(
        const WaterRenderSnapshotPayload& packet,
        const Vec2& localPositionXZ,
        float64 simulationSeconds) noexcept;

    /**
     * @brief Returns `localPoint.y - surface.y` at the point's local XZ coordinate.
     *
     * This is a vertical signed-distance query, not an Euclidean closest-point
     * distance. It remains deterministic because it shares the same explicit
     * time argument as `EvaluateGerstnerSurface`.
     */
    [[nodiscard]] GerstnerSignedVerticalDistance
    EvaluateGerstnerSignedVerticalDistance(
        const WaterRenderSnapshotPayload& packet,
        const Vec3& localPoint,
        float64 simulationSeconds) noexcept;
} // namespace RVX::Water
