/**
 * @file EditorViewportProjection.cpp
 * @brief Native viewport projection utilities implementation
 */

#include "Editor/UI/EditorViewportProjection.h"

#include <cmath>

namespace RVX::Editor
{
namespace
{
    constexpr float RVX_VIEWPORT_PROJECTION_EPSILON = 0.00001f;
}

EditorViewportProjectedPoint EditorViewportProjection::ProjectWorldPoint(
    const EditorViewportCameraFrame& frame,
    const Vec3& worldPosition)
{
    EditorViewportProjectedPoint result;
    if (!frame.valid ||
        frame.viewportBounds.width <= 0.0f ||
        frame.viewportBounds.height <= 0.0f)
    {
        return result;
    }

    const Vec4 clip =
        frame.projectionMatrix * frame.viewMatrix * Vec4(worldPosition, 1.0f);
    result.clipW = clip.w;
    if (std::abs(clip.w) <= RVX_VIEWPORT_PROJECTION_EPSILON)
    {
        return result;
    }

    result.ndc = Vec3(clip) / clip.w;
    result.valid = true;
    result.insideViewport =
        result.ndc.x >= -1.0f &&
        result.ndc.x <= 1.0f &&
        result.ndc.y >= -1.0f &&
        result.ndc.y <= 1.0f &&
        result.ndc.z >= 0.0f &&
        result.ndc.z <= 1.0f;

    const float normalizedX = result.ndc.x * 0.5f + 0.5f;
    const float normalizedY = 1.0f - (result.ndc.y * 0.5f + 0.5f);
    result.screenPosition =
        Vec2(frame.viewportBounds.x + normalizedX * frame.viewportBounds.width,
             frame.viewportBounds.y + normalizedY * frame.viewportBounds.height);
    return result;
}

EditorViewportScreenRay EditorViewportProjection::BuildScreenRay(
    const EditorViewportCameraFrame& frame,
    const Vec2& screenPosition)
{
    EditorViewportScreenRay result;
    if (!frame.valid ||
        frame.viewportBounds.width <= 0.0f ||
        frame.viewportBounds.height <= 0.0f)
    {
        return result;
    }

    const float localX = screenPosition.x - frame.viewportBounds.x;
    const float localY = screenPosition.y - frame.viewportBounds.y;
    result.insideViewport =
        localX >= 0.0f &&
        localY >= 0.0f &&
        localX <= frame.viewportBounds.width &&
        localY <= frame.viewportBounds.height;
    result.ndc.x = (2.0f * localX / frame.viewportBounds.width) - 1.0f;
    result.ndc.y = 1.0f - (2.0f * localY / frame.viewportBounds.height);

    const Mat4 invViewProjection = inverse(frame.projectionMatrix * frame.viewMatrix);
    Vec4 nearPoint = invViewProjection * Vec4(result.ndc.x, result.ndc.y, 0.0f, 1.0f);
    Vec4 farPoint = invViewProjection * Vec4(result.ndc.x, result.ndc.y, 1.0f, 1.0f);
    if (std::abs(nearPoint.w) <= RVX_VIEWPORT_PROJECTION_EPSILON ||
        std::abs(farPoint.w) <= RVX_VIEWPORT_PROJECTION_EPSILON)
    {
        return result;
    }

    nearPoint /= nearPoint.w;
    farPoint /= farPoint.w;
    const Vec3 direction = Vec3(farPoint) - Vec3(nearPoint);
    if (length(direction) <= RVX_VIEWPORT_PROJECTION_EPSILON)
    {
        return result;
    }

    result.nearPoint = Vec3(nearPoint);
    result.farPoint = Vec3(farPoint);
    result.ray = Ray::FromPoints(result.nearPoint, result.farPoint);
    result.valid = true;
    return result;
}

EditorViewportRayPlaneHit EditorViewportProjection::IntersectRayPlane(
    const Ray& ray,
    const Vec3& planePoint,
    const Vec3& planeNormal)
{
    EditorViewportRayPlaneHit hit;
    if (length(planeNormal) <= RVX_VIEWPORT_PROJECTION_EPSILON)
    {
        return hit;
    }

    const Vec3 normal = normalize(planeNormal);
    const float denominator = dot(ray.direction, normal);
    if (std::abs(denominator) <= RVX_VIEWPORT_PROJECTION_EPSILON)
    {
        return hit;
    }

    const float distance = dot(planePoint - ray.origin, normal) / denominator;
    if (distance < ray.tMin || distance > ray.tMax)
    {
        return hit;
    }

    hit.valid = true;
    hit.distance = distance;
    hit.position = ray.At(distance);
    return hit;
}

} // namespace RVX::Editor
