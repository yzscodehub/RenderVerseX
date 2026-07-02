/**
 * @file EditorViewportProjection.h
 * @brief Native viewport projection utilities
 */

#pragma once

#include "Core/Math/Ray.h"
#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "UI/UITypes.h"

namespace RVX::Editor
{

struct EditorViewportCameraFrame
{
    UI::Rect viewportBounds;
    Mat4 viewMatrix{1.0f};
    Mat4 projectionMatrix{1.0f};
    bool valid = false;
};

struct EditorViewportProjectedPoint
{
    bool valid = false;
    bool insideViewport = false;
    Vec2 screenPosition{0.0f};
    Vec3 ndc{0.0f};
    float clipW = 0.0f;
};

struct EditorViewportScreenRay
{
    bool valid = false;
    bool insideViewport = false;
    Ray ray;
    Vec2 ndc{0.0f};
    Vec3 nearPoint{0.0f};
    Vec3 farPoint{0.0f};
};

struct EditorViewportRayPlaneHit
{
    bool valid = false;
    float distance = 0.0f;
    Vec3 position{0.0f};
};

class EditorViewportProjection
{
public:
    static EditorViewportProjectedPoint ProjectWorldPoint(
        const EditorViewportCameraFrame& frame,
        const Vec3& worldPosition);

    static EditorViewportScreenRay BuildScreenRay(
        const EditorViewportCameraFrame& frame,
        const Vec2& screenPosition);

    static EditorViewportRayPlaneHit IntersectRayPlane(
        const Ray& ray,
        const Vec3& planePoint,
        const Vec3& planeNormal);
};

} // namespace RVX::Editor
