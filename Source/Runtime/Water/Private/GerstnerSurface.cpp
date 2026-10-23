#include "Water/GerstnerSurface.h"

#include "RenderContracts/WaterGerstnerMath.h"

#include <cmath>

namespace RVX::Water
{
GerstnerSurfaceEvaluation EvaluateGerstnerSurface(
    const WaterRenderSnapshotPayload& packet,
    const Vec2& localPositionXZ,
    float64 simulationSeconds) noexcept
{
    GerstnerSurfaceEvaluation result;
    if (!packet.IsStructurallyValid() ||
        packet.payloadStatus != WaterRenderSnapshotPayloadStatus::Ready ||
        !std::isfinite(localPositionXZ.x) || !std::isfinite(localPositionXZ.y) ||
        !std::isfinite(simulationSeconds))
    {
        return result;
    }

    WaterGerstnerSurfaceMathSample sample;
    if (!EvaluateWaterGerstnerSurfaceMath(
            packet.gerstnerWaves, localPositionXZ, simulationSeconds, sample))
    {
        return result;
    }

    const Vec3 normal = cross(sample.tangentZ, sample.tangentX);
    const float32 normalLengthSquared = dot(normal, normal);
    const float32 tangentXLengthSquared = dot(sample.tangentX, sample.tangentX);
    const float32 tangentZLengthSquared = dot(sample.tangentZ, sample.tangentZ);
    if (!std::isfinite(normalLengthSquared) ||
        normalLengthSquared <= 1.0e-12f ||
        tangentXLengthSquared <= 1.0e-12f ||
        tangentZLengthSquared <= 1.0e-12f)
    {
        return GerstnerSurfaceEvaluation{};
    }

    result.valid = true;
    result.displacement = sample.displacement;
    result.surfacePosition =
        Vec3{localPositionXZ.x, 0.0f, localPositionXZ.y} + sample.displacement;
    result.normal = normalize(normal);
    result.tangentX = normalize(sample.tangentX);
    result.tangentZ = normalize(sample.tangentZ);
    result.horizontalVelocity = sample.horizontalVelocity;
    result.signedHeight = sample.displacement.y;
    return result;
}

GerstnerSignedVerticalDistance EvaluateGerstnerSignedVerticalDistance(
    const WaterRenderSnapshotPayload& packet,
    const Vec3& localPoint,
    float64 simulationSeconds) noexcept
{
    GerstnerSignedVerticalDistance result;
    if (!std::isfinite(localPoint.x) || !std::isfinite(localPoint.y) ||
        !std::isfinite(localPoint.z))
    {
        return result;
    }
    const GerstnerSurfaceEvaluation surface = EvaluateGerstnerSurface(
        packet, Vec2{localPoint.x, localPoint.z}, simulationSeconds);
    if (!surface.valid)
    {
        return result;
    }
    result.valid = true;
    result.surfacePosition = surface.surfacePosition;
    result.signedVerticalDistance = localPoint.y - surface.surfacePosition.y;
    return result;
}
} // namespace RVX::Water
