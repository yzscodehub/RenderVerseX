/**
 * @file EditorViewportManipulatorModel.cpp
 * @brief Native viewport transform manipulator model implementation
 */

#include "Editor/UI/EditorViewportManipulatorModel.h"
#include "Scene/SceneEntity.h"
#include "UI/UIContext.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace RVX::Editor
{
namespace
{
    constexpr float RVX_MANIPULATOR_EPSILON = 0.00001f;
    constexpr float RVX_MANIPULATOR_PI = 3.14159265358979323846f;

    float SnapComponent(float value, float snapValue)
    {
        return std::round(value / snapValue) * snapValue;
    }

    float DistanceToSegment(const Vec2& point, const Vec2& start, const Vec2& end)
    {
        const Vec2 segment = end - start;
        const float segmentLengthSq = dot(segment, segment);
        if (segmentLengthSq <= RVX_MANIPULATOR_EPSILON)
        {
            return length(point - start);
        }

        const float t = std::clamp(dot(point - start, segment) / segmentLengthSq,
                                   0.0f,
                                   1.0f);
        return length(point - (start + segment * t));
    }

    UI::Rect SegmentBounds(const Vec2& start, const Vec2& end, float padding)
    {
        const float left = std::min(start.x, end.x) - padding;
        const float right = std::max(start.x, end.x) + padding;
        const float top = std::min(start.y, end.y) - padding;
        const float bottom = std::max(start.y, end.y) + padding;
        return UI::Rect(left, top, right - left, bottom - top);
    }

    UI::Rect PointBounds(const std::array<Vec2, 4>& points)
    {
        float left = points[0].x;
        float right = points[0].x;
        float top = points[0].y;
        float bottom = points[0].y;
        for (const Vec2& point : points)
        {
            left = std::min(left, point.x);
            right = std::max(right, point.x);
            top = std::min(top, point.y);
            bottom = std::max(bottom, point.y);
        }
        return UI::Rect(left, top, right - left, bottom - top);
    }

    UI::Rect RingBounds(
        const std::array<Vec2, RVX_EDITOR_MANIPULATOR_RING_SAMPLE_COUNT>& points,
        uint32 count)
    {
        if (count == 0)
        {
            return UI::Rect();
        }

        float left = points[0].x;
        float right = points[0].x;
        float top = points[0].y;
        float bottom = points[0].y;
        for (uint32 index = 0; index < count; ++index)
        {
            left = std::min(left, points[index].x);
            right = std::max(right, points[index].x);
            top = std::min(top, points[index].y);
            bottom = std::max(bottom, points[index].y);
        }
        return UI::Rect(left, top, right - left, bottom - top);
    }

    float Cross2D(const Vec2& a, const Vec2& b)
    {
        return a.x * b.y - a.y * b.x;
    }

    float PolygonArea(const std::array<Vec2, 4>& points)
    {
        float area = 0.0f;
        for (uint32 index = 0; index < 4; ++index)
        {
            const Vec2& a = points[index];
            const Vec2& b = points[(index + 1) % 4];
            area += Cross2D(a, b);
        }
        return std::abs(area) * 0.5f;
    }

    bool PointInConvexQuad(const Vec2& point, const std::array<Vec2, 4>& points)
    {
        float sign = 0.0f;
        for (uint32 index = 0; index < 4; ++index)
        {
            const Vec2& a = points[index];
            const Vec2& b = points[(index + 1) % 4];
            const float edgeSign = Cross2D(b - a, point - a);
            if (std::abs(edgeSign) <= RVX_MANIPULATOR_EPSILON)
            {
                continue;
            }

            if (std::abs(sign) <= RVX_MANIPULATOR_EPSILON)
            {
                sign = edgeSign;
                continue;
            }

            if (edgeSign * sign < 0.0f)
            {
                return false;
            }
        }

        return true;
    }

    float DistanceToRingSamples(
        const Vec2& point,
        const std::array<Vec2, RVX_EDITOR_MANIPULATOR_RING_SAMPLE_COUNT>& samples,
        uint32 count)
    {
        if (count == 0)
        {
            return std::numeric_limits<float>::max();
        }

        float bestDistance = std::numeric_limits<float>::max();
        for (uint32 index = 0; index < count; ++index)
        {
            const Vec2& start = samples[index];
            const Vec2& end = samples[(index + 1) % count];
            bestDistance = std::min(bestDistance, DistanceToSegment(point, start, end));
        }
        return bestDistance;
    }
}

bool EditorViewportManipulatorModel::IsSupportedMode() const
{
    return m_toolState.mode == EditorContext::GizmoMode::Translate ||
           m_toolState.mode == EditorContext::GizmoMode::Rotate ||
           m_toolState.mode == EditorContext::GizmoMode::Scale;
}

void EditorViewportManipulatorModel::Build(EditorContext& context,
                                           const EditorViewportManipulatorBuildDesc& desc)
{
    if (m_activeHandle != EditorViewportManipulatorHandle::None &&
        context.GetSelectedEntity() != m_dragEntity)
    {
        CancelDrag();
    }

    m_handleCount = 0;
    m_toolState = desc.toolState;
    m_viewportBounds = desc.viewportBounds;
    m_cameraFrame = desc.cameraFrame;
    m_worldUnitsPerPixel = desc.worldUnitsPerPixel > RVX_MANIPULATOR_EPSILON
                               ? desc.worldUnitsPerPixel
                               : 0.01f;

    m_lastStats = {};
    m_lastStats.built = true;
    m_lastStats.toolState = m_toolState;
    m_lastStats.viewportBounds = m_viewportBounds;
    m_lastStats.usesLegacyGizmoAdapter = false;
    m_lastStats.dragging = m_activeHandle != EditorViewportManipulatorHandle::None;
    m_lastStats.activeHandle = m_activeHandle;
    m_lastStats.usesRayPlaneConstraint = m_dragUsesRayPlaneConstraint;
    m_lastStats.dragPlaneValid = m_dragPlaneValid;

    SceneEntity* selectedEntity = context.GetSelectedEntity();
    m_lastStats.hasSelection = selectedEntity != nullptr;
    m_lastStats.unsupportedMode = !IsSupportedMode();

    if (!selectedEntity ||
        m_lastStats.unsupportedMode ||
        m_viewportBounds.width <= 0.0f ||
        m_viewportBounds.height <= 0.0f)
    {
        return;
    }

    Vec2 anchor = m_viewportBounds.Center();
    if (desc.cameraFrame.valid)
    {
        const EditorViewportProjectedPoint projected =
            EditorViewportProjection::ProjectWorldPoint(desc.cameraFrame,
                                                        selectedEntity->GetWorldPosition());
        m_lastStats.anchorProjected = projected.valid;
        m_lastStats.anchorVisible = projected.valid && projected.insideViewport;
        if (!m_lastStats.anchorVisible)
        {
            return;
        }

        anchor = projected.screenPosition;
    }
    else
    {
        m_lastStats.anchorVisible = true;
    }

    const float centerSize = 14.0f;
    const float axisThickness = 8.0f;
    const float axisGap = 12.0f;
    const float axisLength =
        std::min(56.0f,
                 std::max(24.0f,
                          std::min(m_viewportBounds.width, m_viewportBounds.height) * 0.18f));

    AddRectHandle(EditorViewportManipulatorHandle::Center,
                  UI::Rect(anchor.x - centerSize * 0.5f,
                           anchor.y - centerSize * 0.5f,
                           centerSize,
                           centerSize),
                  Vec3(1.0f, 1.0f, 0.0f));

    if (m_toolState.mode == EditorContext::GizmoMode::Rotate)
    {
        const float ringHitRadius = std::max(axisThickness * 1.15f, 8.0f);
        const float ringWorldRadius = 0.32f;
        const float fallbackRingRadius =
            std::min(72.0f,
                     std::max(34.0f,
                              std::min(m_viewportBounds.width, m_viewportBounds.height) *
                                  0.17f));
        if (!BuildProjectedRotationRingHandle(selectedEntity,
                                              EditorViewportManipulatorHandle::AxisX,
                                              anchor,
                                              ringWorldRadius,
                                              ringHitRadius))
        {
            AddFallbackRotationRingHandle(EditorViewportManipulatorHandle::AxisX,
                                          anchor,
                                          fallbackRingRadius * 0.82f,
                                          ringHitRadius,
                                          GetHandleWorldAxis(EditorViewportManipulatorHandle::AxisX,
                                                             selectedEntity));
        }
        if (!BuildProjectedRotationRingHandle(selectedEntity,
                                              EditorViewportManipulatorHandle::AxisY,
                                              anchor,
                                              ringWorldRadius,
                                              ringHitRadius))
        {
            AddFallbackRotationRingHandle(EditorViewportManipulatorHandle::AxisY,
                                          anchor,
                                          fallbackRingRadius,
                                          ringHitRadius,
                                          GetHandleWorldAxis(EditorViewportManipulatorHandle::AxisY,
                                                             selectedEntity));
        }
        if (!BuildProjectedRotationRingHandle(selectedEntity,
                                              EditorViewportManipulatorHandle::AxisZ,
                                              anchor,
                                              ringWorldRadius,
                                              ringHitRadius))
        {
            AddFallbackRotationRingHandle(EditorViewportManipulatorHandle::AxisZ,
                                          anchor,
                                          fallbackRingRadius * 1.18f,
                                          ringHitRadius,
                                          GetHandleWorldAxis(EditorViewportManipulatorHandle::AxisZ,
                                                             selectedEntity));
        }

        m_lastStats.anchor = anchor;
        m_lastStats.handleCount = m_handleCount;
        return;
    }

    if (!BuildProjectedAxisHandle(selectedEntity,
                                  EditorViewportManipulatorHandle::AxisX,
                                  anchor,
                                  axisGap,
                                  axisLength,
                                  axisThickness))
    {
        AddRectHandle(EditorViewportManipulatorHandle::AxisX,
                      UI::Rect(anchor.x + axisGap,
                               anchor.y - axisThickness * 0.5f,
                               axisLength,
                               axisThickness),
                      GetHandleWorldAxis(EditorViewportManipulatorHandle::AxisX,
                                         selectedEntity));
    }
    if (!BuildProjectedAxisHandle(selectedEntity,
                                  EditorViewportManipulatorHandle::AxisY,
                                  anchor,
                                  axisGap,
                                  axisLength,
                                  axisThickness))
    {
        AddRectHandle(EditorViewportManipulatorHandle::AxisY,
                      UI::Rect(anchor.x - axisThickness * 0.5f,
                               anchor.y - axisGap - axisLength,
                               axisThickness,
                               axisLength),
                      GetHandleWorldAxis(EditorViewportManipulatorHandle::AxisY,
                                         selectedEntity));
    }
    if (!BuildProjectedAxisHandle(selectedEntity,
                                  EditorViewportManipulatorHandle::AxisZ,
                                  anchor,
                                  axisGap,
                                  axisLength,
                                  axisThickness))
    {
        const float hitRadius = std::max(axisThickness * 1.25f, 8.0f);
        const Vec2 screenStart(anchor.x + axisGap * 0.75f,
                               anchor.y + axisGap * 0.75f);
        const Vec2 screenEnd(screenStart.x + axisLength * 0.72f,
                             screenStart.y + axisLength * 0.72f);
        AddRectHandle(EditorViewportManipulatorHandle::AxisZ,
                      SegmentBounds(screenStart, screenEnd, hitRadius + 6.0f),
                      GetHandleWorldAxis(EditorViewportManipulatorHandle::AxisZ,
                                         selectedEntity),
                      true,
                      screenStart,
                      screenEnd,
                      hitRadius,
                      false);
    }

    const float planeGap = 0.08f;
    const float planeSize = 0.18f;
    const float fallbackPlaneSize = 16.0f;
    if (!BuildProjectedPlaneHandle(selectedEntity,
                                   EditorViewportManipulatorHandle::PlaneXY,
                                   planeGap,
                                   planeSize))
    {
        AddPlaneHandle(EditorViewportManipulatorHandle::PlaneXY,
                       UI::Rect(anchor.x + 22.0f,
                                anchor.y - 38.0f,
                                fallbackPlaneSize,
                                fallbackPlaneSize),
                       GetHandlePlaneNormal(EditorViewportManipulatorHandle::PlaneXY,
                                            selectedEntity));
    }
    if (!BuildProjectedPlaneHandle(selectedEntity,
                                   EditorViewportManipulatorHandle::PlaneXZ,
                                   planeGap,
                                   planeSize))
    {
        AddPlaneHandle(EditorViewportManipulatorHandle::PlaneXZ,
                       UI::Rect(anchor.x + 22.0f,
                                anchor.y + 20.0f,
                                fallbackPlaneSize,
                                fallbackPlaneSize),
                       GetHandlePlaneNormal(EditorViewportManipulatorHandle::PlaneXZ,
                                            selectedEntity));
    }
    if (!BuildProjectedPlaneHandle(selectedEntity,
                                   EditorViewportManipulatorHandle::PlaneYZ,
                                   planeGap,
                                   planeSize))
    {
        AddPlaneHandle(EditorViewportManipulatorHandle::PlaneYZ,
                       UI::Rect(anchor.x - 38.0f,
                                anchor.y + 20.0f,
                                fallbackPlaneSize,
                                fallbackPlaneSize),
                       GetHandlePlaneNormal(EditorViewportManipulatorHandle::PlaneYZ,
                                            selectedEntity));
    }

    m_lastStats.anchor = anchor;
    m_lastStats.handleCount = m_handleCount;
}

bool EditorViewportManipulatorModel::HandleInput(EditorContext& context,
                                                 const UI::UIInputState& input)
{
    if (input.WasMouseButtonPressed(UI::UIMouseButton::Left) &&
        BeginDrag(context, input.current.mousePosition))
    {
        return true;
    }

    if (m_activeHandle == EditorViewportManipulatorHandle::None)
    {
        return false;
    }

    if (input.WasMouseButtonReleased(UI::UIMouseButton::Left))
    {
        EndDrag(context, input.current.mousePosition);
        return true;
    }

    if (input.current.IsMouseButtonDown(UI::UIMouseButton::Left))
    {
        UpdateDrag(input.current.mousePosition);
        return true;
    }

    CancelDrag();
    return true;
}

bool EditorViewportManipulatorModel::BeginDrag(EditorContext& context,
                                               const Vec2& screenPosition)
{
    SceneEntity* selectedEntity = context.GetSelectedEntity();
    if (!selectedEntity || !IsSupportedMode())
    {
        return false;
    }

    const EditorViewportManipulatorHandle handle = HitTest(screenPosition);
    if (handle == EditorViewportManipulatorHandle::None)
    {
        return false;
    }

    m_activeHandle = handle;
    m_dragEntity = selectedEntity;
    m_dragStartScreen = screenPosition;
    m_dragStartPosition = selectedEntity->GetPosition();
    m_dragStartWorldPosition = selectedEntity->GetWorldPosition();
    m_dragStartRotation = selectedEntity->GetRotation();
    m_dragStartWorldRotation = selectedEntity->GetWorldRotation();
    m_dragStartScale = selectedEntity->GetScale();
    m_dragCameraFrame = m_cameraFrame;
    m_dragUsesRayPlaneConstraint = ConfigureRayPlaneDrag(screenPosition);
    if (m_toolState.mode == EditorContext::GizmoMode::Rotate)
    {
        PublishRotationStats(m_dragStartRotation, 0.0f);
    }
    else if (m_toolState.mode == EditorContext::GizmoMode::Scale)
    {
        PublishScaleStats(m_dragStartScale);
    }
    else
    {
        PublishDragStats(Vec3(0.0f));
    }
    return true;
}

bool EditorViewportManipulatorModel::UpdateDrag(const Vec2& screenPosition)
{
    if (m_activeHandle == EditorViewportManipulatorHandle::None ||
        !m_dragEntity)
    {
        return false;
    }

    if (m_toolState.mode == EditorContext::GizmoMode::Rotate)
    {
        PublishRotationStats(ComputeDraggedRotation(screenPosition),
                             ComputeRotationAngle(screenPosition));
    }
    else if (m_toolState.mode == EditorContext::GizmoMode::Scale)
    {
        PublishScaleStats(ComputeDraggedScale(screenPosition));
    }
    else
    {
        PublishDragStats(ComputeDragDelta(screenPosition));
    }
    return true;
}

bool EditorViewportManipulatorModel::EndDrag(EditorContext& context,
                                             const Vec2& screenPosition)
{
    if (m_activeHandle == EditorViewportManipulatorHandle::None ||
        !m_dragEntity)
    {
        return false;
    }

    Vec3 committedDelta(0.0f);
    Vec3 committedScale = m_dragStartScale;
    Quat committedRotation = m_dragStartRotation;
    float committedAngle = 0.0f;
    bool changed = false;

    if (m_toolState.mode == EditorContext::GizmoMode::Rotate)
    {
        committedAngle = ComputeRotationAngle(screenPosition);
        committedRotation = ComputeDraggedRotation(screenPosition);
        changed = std::abs(committedAngle) > RVX_MANIPULATOR_EPSILON;
        if (changed)
        {
            context.SetEntityTransformUndoable(m_dragEntity,
                                               m_dragStartPosition,
                                               committedRotation,
                                               m_dragStartScale,
                                               "Rotate Entity");
        }
    }
    else if (m_toolState.mode == EditorContext::GizmoMode::Scale)
    {
        committedScale = ComputeDraggedScale(screenPosition);
        committedDelta = committedScale - m_dragStartScale;
        changed = length(committedDelta) > RVX_MANIPULATOR_EPSILON;
        if (changed)
        {
            context.SetEntityTransformUndoable(m_dragEntity,
                                               m_dragStartPosition,
                                               m_dragStartRotation,
                                               committedScale,
                                               "Scale Entity");
        }
    }
    else
    {
        committedDelta = ComputeDragDelta(screenPosition);
        changed = length(committedDelta) > RVX_MANIPULATOR_EPSILON;
        if (changed)
        {
            context.SetEntityTransformUndoable(m_dragEntity,
                                               ComputeDraggedLocalPosition(committedDelta),
                                               m_dragStartRotation,
                                               m_dragStartScale,
                                               "Move Entity");
        }
    }

    m_lastStats.dragging = false;
    m_lastStats.activeHandle = EditorViewportManipulatorHandle::None;
    m_lastStats.previewDelta = Vec3(0.0f);
    m_lastStats.committedDelta = committedDelta;
    m_lastStats.previewScale = committedScale;
    m_lastStats.committedScale = committedScale;
    m_lastStats.previewRotation = committedRotation;
    m_lastStats.committedRotation = committedRotation;
    m_lastStats.previewAngleRadians = committedAngle;
    m_lastStats.committedAngleRadians = committedAngle;
    m_lastStats.appliedTransform = changed;

    m_activeHandle = EditorViewportManipulatorHandle::None;
    m_dragEntity = nullptr;
    m_dragUsesRayPlaneConstraint = false;
    m_dragPlaneValid = false;
    m_dragConstraintAxis = Vec3(0.0f);
    m_dragConstraintPlaneNormal = Vec3(0.0f);
    return true;
}

void EditorViewportManipulatorModel::CancelDrag()
{
    const Vec3 canceledScale = m_dragEntity ? m_dragStartScale : m_lastStats.previewScale;
    const Quat canceledRotation =
        m_dragEntity ? m_dragStartRotation : m_lastStats.previewRotation;
    m_activeHandle = EditorViewportManipulatorHandle::None;
    m_dragEntity = nullptr;
    m_dragUsesRayPlaneConstraint = false;
    m_dragPlaneValid = false;
    m_dragConstraintAxis = Vec3(0.0f);
    m_dragConstraintPlaneNormal = Vec3(0.0f);
    m_lastStats.dragging = false;
    m_lastStats.activeHandle = EditorViewportManipulatorHandle::None;
    m_lastStats.usesRayPlaneConstraint = false;
    m_lastStats.dragPlaneValid = false;
    m_lastStats.previewDelta = Vec3(0.0f);
    m_lastStats.previewScale = canceledScale;
    m_lastStats.previewRotation = canceledRotation;
    m_lastStats.previewAngleRadians = 0.0f;
}

const EditorViewportManipulatorHandleInfo* EditorViewportManipulatorModel::GetHandle(
    uint32 index) const
{
    if (index >= m_handleCount)
    {
        return nullptr;
    }

    return &m_handles[index];
}

const EditorViewportManipulatorHandleInfo* EditorViewportManipulatorModel::FindHandle(
    EditorViewportManipulatorHandle handle) const
{
    for (uint32 index = 0; index < m_handleCount; ++index)
    {
        if (m_handles[index].handle == handle)
        {
            return &m_handles[index];
        }
    }

    return nullptr;
}

EditorViewportManipulatorHandle EditorViewportManipulatorModel::HitTest(
    const Vec2& screenPosition) const
{
    for (uint32 index = 0; index < m_handleCount; ++index)
    {
        const EditorViewportManipulatorHandleInfo& info = m_handles[index];
        if (!info.enabled || !info.bounds.Contains(screenPosition))
        {
            continue;
        }

        if (info.hitRing)
        {
            if (DistanceToRingSamples(screenPosition,
                                      info.screenRingSamples,
                                      info.screenRingSampleCount) <= info.hitRadius)
            {
                return info.handle;
            }
            continue;
        }

        if (!info.hitSegment)
        {
            if (info.hitPolygon &&
                PointInConvexQuad(screenPosition, info.screenCorners))
            {
                return info.handle;
            }

            if (info.hitPolygon)
            {
                continue;
            }

            return info.handle;
        }

        if (DistanceToSegment(screenPosition,
                              info.screenStart,
                              info.screenEnd) <= info.hitRadius)
        {
            return info.handle;
        }
    }

    return EditorViewportManipulatorHandle::None;
}

bool EditorViewportManipulatorModel::BuildProjectedRotationRingHandle(
    SceneEntity* selectedEntity,
    EditorViewportManipulatorHandle handle,
    const Vec2& anchor,
    float ringWorldRadius,
    float ringHitRadius)
{
    if (!selectedEntity || !m_cameraFrame.valid)
    {
        return false;
    }

    const Vec3 axis = GetHandleWorldAxis(handle, selectedEntity);
    if (length(axis) <= RVX_MANIPULATOR_EPSILON)
    {
        return false;
    }

    const Vec3 reference =
        std::abs(dot(axis, Vec3(0.0f, 1.0f, 0.0f))) < 0.92f
            ? Vec3(0.0f, 1.0f, 0.0f)
            : Vec3(1.0f, 0.0f, 0.0f);
    Vec3 ringAxisA = cross(axis, reference);
    if (length(ringAxisA) <= RVX_MANIPULATOR_EPSILON)
    {
        return false;
    }
    ringAxisA = normalize(ringAxisA);
    Vec3 ringAxisB = cross(axis, ringAxisA);
    if (length(ringAxisB) <= RVX_MANIPULATOR_EPSILON)
    {
        return false;
    }
    ringAxisB = normalize(ringAxisB);

    std::array<Vec2, RVX_EDITOR_MANIPULATOR_RING_SAMPLE_COUNT> samples{};
    const Vec3 worldAnchor = selectedEntity->GetWorldPosition();
    for (uint32 index = 0; index < RVX_EDITOR_MANIPULATOR_RING_SAMPLE_COUNT; ++index)
    {
        const float t =
            static_cast<float>(index) /
            static_cast<float>(RVX_EDITOR_MANIPULATOR_RING_SAMPLE_COUNT);
        const float angle = t * RVX_MANIPULATOR_PI * 2.0f;
        const Vec3 worldPoint =
            worldAnchor +
            (ringAxisA * std::cos(angle) + ringAxisB * std::sin(angle)) *
                ringWorldRadius;
        const EditorViewportProjectedPoint projected =
            EditorViewportProjection::ProjectWorldPoint(m_cameraFrame, worldPoint);
        if (!projected.valid)
        {
            return false;
        }
        samples[index] = projected.screenPosition;
    }

    const UI::Rect bounds =
        RingBounds(samples, RVX_EDITOR_MANIPULATOR_RING_SAMPLE_COUNT);
    if (bounds.width <= 12.0f || bounds.height <= 12.0f)
    {
        return false;
    }

    float ringRadius = 0.0f;
    for (const Vec2& sample : samples)
    {
        ringRadius = std::max(ringRadius, length(sample - anchor));
    }

    AddRingHandle(handle,
                  bounds.Expand(ringHitRadius + 4.0f),
                  axis,
                  anchor,
                  ringRadius,
                  ringHitRadius,
                  samples,
                  RVX_EDITOR_MANIPULATOR_RING_SAMPLE_COUNT,
                  true);
    ++m_lastStats.projectedRingHandleCount;
    return true;
}

bool EditorViewportManipulatorModel::BuildProjectedPlaneHandle(
    SceneEntity* selectedEntity,
    EditorViewportManipulatorHandle handle,
    float planeGap,
    float planeSize)
{
    if (!selectedEntity || !m_cameraFrame.valid)
    {
        return false;
    }

    Vec3 axisA(0.0f);
    Vec3 axisB(0.0f);
    Vec3 normal(0.0f);
    if (!TryGetPlaneAxes(handle, selectedEntity, axisA, axisB, normal))
    {
        return false;
    }

    const Vec3 worldAnchor = selectedEntity->GetWorldPosition();
    const std::array<Vec3, 4> worldCorners = {
        worldAnchor + axisA * planeGap + axisB * planeGap,
        worldAnchor + axisA * (planeGap + planeSize) + axisB * planeGap,
        worldAnchor + axisA * (planeGap + planeSize) + axisB * (planeGap + planeSize),
        worldAnchor + axisA * planeGap + axisB * (planeGap + planeSize)
    };

    std::array<Vec2, 4> screenCorners{};
    for (uint32 index = 0; index < 4; ++index)
    {
        const EditorViewportProjectedPoint projected =
            EditorViewportProjection::ProjectWorldPoint(m_cameraFrame, worldCorners[index]);
        if (!projected.valid)
        {
            return false;
        }
        screenCorners[index] = projected.screenPosition;
    }

    if (PolygonArea(screenCorners) <= 12.0f)
    {
        return false;
    }

    AddPlaneHandle(handle,
                   PointBounds(screenCorners).Expand(4.0f),
                   normal,
                   screenCorners,
                   true,
                   true);
    ++m_lastStats.projectedPlaneHandleCount;
    return true;
}

bool EditorViewportManipulatorModel::BuildProjectedAxisHandle(
    SceneEntity* selectedEntity,
    EditorViewportManipulatorHandle handle,
    const Vec2& anchor,
    float axisGap,
    float axisLength,
    float axisThickness)
{
    if (!selectedEntity || !m_cameraFrame.valid)
    {
        return false;
    }

    const Vec3 axis = GetHandleWorldAxis(handle, selectedEntity);
    if (length(axis) <= RVX_MANIPULATOR_EPSILON)
    {
        return false;
    }

    const EditorViewportProjectedPoint projectedAxis =
        EditorViewportProjection::ProjectWorldPoint(m_cameraFrame,
                                                    selectedEntity->GetWorldPosition() +
                                                        axis);
    if (!projectedAxis.valid)
    {
        return false;
    }

    const Vec2 projectedDelta = projectedAxis.screenPosition - anchor;
    const float projectedLength = length(projectedDelta);
    if (projectedLength <= RVX_MANIPULATOR_EPSILON)
    {
        return false;
    }

    const Vec2 screenDirection = projectedDelta / projectedLength;
    const Vec2 screenStart = anchor + screenDirection * axisGap;
    const Vec2 screenEnd = anchor + screenDirection * (axisGap + axisLength);
    const float hitRadius = std::max(axisThickness * 1.25f, 8.0f);
    const UI::Rect bounds = SegmentBounds(screenStart, screenEnd, hitRadius + 6.0f);
    AddRectHandle(handle,
                  bounds,
                  axis,
                  true,
                  screenStart,
                  screenEnd,
                  hitRadius,
                  true);
    ++m_lastStats.projectedSegmentHandleCount;
    return true;
}

void EditorViewportManipulatorModel::AddRectHandle(
    EditorViewportManipulatorHandle handle,
    const UI::Rect& bounds,
    const Vec3& axis,
    bool hitSegment,
    const Vec2& screenStart,
    const Vec2& screenEnd,
    float hitRadius,
    bool usesProjectedSegment)
{
    if (m_handleCount >= m_handles.size())
    {
        return;
    }

    EditorViewportManipulatorHandleInfo& info = m_handles[m_handleCount++];
    info = {};
    info.handle = handle;
    info.bounds = bounds;
    info.screenStart = screenStart;
    info.screenEnd = screenEnd;
    info.axis = axis;
    info.hitRadius = hitRadius;
    info.enabled = true;
    info.hitSegment = hitSegment;
    info.usesProjectedSegment = usesProjectedSegment;
}

void EditorViewportManipulatorModel::AddPlaneHandle(
    EditorViewportManipulatorHandle handle,
    const UI::Rect& bounds,
    const Vec3& planeNormal,
    const std::array<Vec2, 4>& screenCorners,
    bool hitPolygon,
    bool usesProjectedPlane)
{
    if (m_handleCount >= m_handles.size())
    {
        return;
    }

    EditorViewportManipulatorHandleInfo& info = m_handles[m_handleCount++];
    info = {};
    info.handle = handle;
    info.bounds = bounds;
    info.screenCorners = screenCorners;
    info.planeNormal = planeNormal;
    info.enabled = true;
    info.hitPolygon = hitPolygon;
    info.usesProjectedPlane = usesProjectedPlane;
}

void EditorViewportManipulatorModel::AddRingHandle(
    EditorViewportManipulatorHandle handle,
    const UI::Rect& bounds,
    const Vec3& axis,
    const Vec2& screenCenter,
    float ringRadius,
    float hitRadius,
    const std::array<Vec2, RVX_EDITOR_MANIPULATOR_RING_SAMPLE_COUNT>& samples,
    uint32 sampleCount,
    bool usesProjectedRing)
{
    if (m_handleCount >= m_handles.size())
    {
        return;
    }

    EditorViewportManipulatorHandleInfo& info = m_handles[m_handleCount++];
    info = {};
    info.handle = handle;
    info.bounds = bounds;
    info.axis = axis;
    info.screenCenter = screenCenter;
    info.ringRadius = ringRadius;
    info.hitRadius = hitRadius;
    info.screenRingSamples = samples;
    info.screenRingSampleCount = std::min<uint32>(
        sampleCount,
        RVX_EDITOR_MANIPULATOR_RING_SAMPLE_COUNT);
    info.enabled = true;
    info.hitRing = true;
    info.usesProjectedRing = usesProjectedRing;
}

void EditorViewportManipulatorModel::AddFallbackRotationRingHandle(
    EditorViewportManipulatorHandle handle,
    const Vec2& anchor,
    float ringRadius,
    float hitRadius,
    const Vec3& axis)
{
    std::array<Vec2, RVX_EDITOR_MANIPULATOR_RING_SAMPLE_COUNT> samples{};
    for (uint32 index = 0; index < RVX_EDITOR_MANIPULATOR_RING_SAMPLE_COUNT; ++index)
    {
        const float t =
            static_cast<float>(index) /
            static_cast<float>(RVX_EDITOR_MANIPULATOR_RING_SAMPLE_COUNT);
        const float angle = t * RVX_MANIPULATOR_PI * 2.0f;
        samples[index] = anchor + Vec2(std::cos(angle), std::sin(angle)) * ringRadius;
    }

    AddRingHandle(handle,
                  RingBounds(samples, RVX_EDITOR_MANIPULATOR_RING_SAMPLE_COUNT)
                      .Expand(hitRadius + 4.0f),
                  axis,
                  anchor,
                  ringRadius,
                  hitRadius,
                  samples,
                  RVX_EDITOR_MANIPULATOR_RING_SAMPLE_COUNT,
                  false);
}

Vec3 EditorViewportManipulatorModel::ComputeDragDelta(const Vec2& screenPosition) const
{
    if (m_dragUsesRayPlaneConstraint && m_dragPlaneValid)
    {
        const EditorViewportScreenRay ray =
            EditorViewportProjection::BuildScreenRay(m_dragCameraFrame, screenPosition);
        if (ray.valid)
        {
            const EditorViewportRayPlaneHit hit =
                EditorViewportProjection::IntersectRayPlane(ray.ray,
                                                            m_dragPlanePoint,
                                                            m_dragPlaneNormal);
            if (hit.valid)
            {
                Vec3 delta = hit.position - m_dragStartHitWorld;
                if (length(m_dragConstraintAxis) > RVX_MANIPULATOR_EPSILON)
                {
                    delta = m_dragConstraintAxis * dot(delta, m_dragConstraintAxis);
                }
                else if (length(m_dragConstraintPlaneNormal) > RVX_MANIPULATOR_EPSILON)
                {
                    delta -= m_dragConstraintPlaneNormal *
                             dot(delta, m_dragConstraintPlaneNormal);
                }
                return ApplySnap(delta);
            }
        }
    }

    return ComputePixelFallbackDragDelta(screenPosition);
}

Vec3 EditorViewportManipulatorModel::ComputePixelFallbackDragDelta(
    const Vec2& screenPosition) const
{
    const Vec2 pixelDelta = screenPosition - m_dragStartScreen;
    Vec3 delta(0.0f);

    switch (m_activeHandle)
    {
        case EditorViewportManipulatorHandle::Center:
            delta = Vec3(pixelDelta.x, -pixelDelta.y, 0.0f) * m_worldUnitsPerPixel;
            break;
        case EditorViewportManipulatorHandle::AxisX:
            delta = Vec3(pixelDelta.x, 0.0f, 0.0f) * m_worldUnitsPerPixel;
            break;
        case EditorViewportManipulatorHandle::AxisY:
            delta = Vec3(0.0f, -pixelDelta.y, 0.0f) * m_worldUnitsPerPixel;
            break;
        case EditorViewportManipulatorHandle::AxisZ:
            delta = Vec3(0.0f, 0.0f, pixelDelta.x + pixelDelta.y) *
                    (m_worldUnitsPerPixel * 0.5f);
            break;
        case EditorViewportManipulatorHandle::PlaneXY:
            delta = Vec3(pixelDelta.x, -pixelDelta.y, 0.0f) * m_worldUnitsPerPixel;
            break;
        case EditorViewportManipulatorHandle::PlaneXZ:
            delta = Vec3(pixelDelta.x, 0.0f, -pixelDelta.y) * m_worldUnitsPerPixel;
            break;
        case EditorViewportManipulatorHandle::PlaneYZ:
            delta = Vec3(0.0f, -pixelDelta.x, -pixelDelta.y) * m_worldUnitsPerPixel;
            break;
        case EditorViewportManipulatorHandle::None:
            break;
    }

    return ApplySnap(delta);
}

Vec3 EditorViewportManipulatorModel::ApplySnap(const Vec3& delta) const
{
    if (!m_toolState.snapEnabled ||
        m_toolState.snapValue <= RVX_MANIPULATOR_EPSILON)
    {
        return delta;
    }

    const float snapValue = m_toolState.snapValue;
    if (length(m_dragConstraintAxis) > RVX_MANIPULATOR_EPSILON)
    {
        return m_dragConstraintAxis * SnapComponent(dot(delta, m_dragConstraintAxis), snapValue);
    }

    if (length(m_dragConstraintPlaneNormal) > RVX_MANIPULATOR_EPSILON)
    {
        Vec3 snapped(SnapComponent(delta.x, snapValue),
                     SnapComponent(delta.y, snapValue),
                     SnapComponent(delta.z, snapValue));
        return snapped - m_dragConstraintPlaneNormal *
                             dot(snapped, m_dragConstraintPlaneNormal);
    }

    return Vec3(SnapComponent(delta.x, snapValue),
                SnapComponent(delta.y, snapValue),
                SnapComponent(delta.z, snapValue));
}

Vec3 EditorViewportManipulatorModel::ComputeDraggedScale(const Vec2& screenPosition) const
{
    const Vec2 pixelDelta = screenPosition - m_dragStartScreen;
    const Vec3 delta = ComputeDragDelta(screenPosition);
    float scalar = 0.0f;

    switch (m_activeHandle)
    {
        case EditorViewportManipulatorHandle::Center:
            scalar = (pixelDelta.x - pixelDelta.y) * m_worldUnitsPerPixel;
            break;
        case EditorViewportManipulatorHandle::AxisX:
        case EditorViewportManipulatorHandle::AxisY:
        case EditorViewportManipulatorHandle::AxisZ:
        {
            const Vec3 axis = GetHandleWorldAxis(m_activeHandle, m_dragEntity);
            if (length(axis) > RVX_MANIPULATOR_EPSILON)
            {
                scalar = dot(delta, axis);
            }
            break;
        }
        case EditorViewportManipulatorHandle::PlaneXY:
        case EditorViewportManipulatorHandle::PlaneXZ:
        case EditorViewportManipulatorHandle::PlaneYZ:
        {
            Vec3 axisA(0.0f);
            Vec3 axisB(0.0f);
            Vec3 normal(0.0f);
            if (TryGetPlaneAxes(m_activeHandle, m_dragEntity, axisA, axisB, normal))
            {
                scalar = (dot(delta, axisA) + dot(delta, axisB)) * 0.5f;
            }
            else
            {
                scalar = (pixelDelta.x - pixelDelta.y) *
                         (m_worldUnitsPerPixel * 0.5f);
            }
            break;
        }
        case EditorViewportManipulatorHandle::None:
            break;
    }

    const float factor = std::max(0.001f, 1.0f + scalar);
    Vec3 scale = m_dragStartScale;
    switch (m_activeHandle)
    {
        case EditorViewportManipulatorHandle::Center:
            scale *= factor;
            break;
        case EditorViewportManipulatorHandle::AxisX:
            scale.x *= factor;
            break;
        case EditorViewportManipulatorHandle::AxisY:
            scale.y *= factor;
            break;
        case EditorViewportManipulatorHandle::AxisZ:
            scale.z *= factor;
            break;
        case EditorViewportManipulatorHandle::PlaneXY:
            scale.x *= factor;
            scale.y *= factor;
            break;
        case EditorViewportManipulatorHandle::PlaneXZ:
            scale.x *= factor;
            scale.z *= factor;
            break;
        case EditorViewportManipulatorHandle::PlaneYZ:
            scale.y *= factor;
            scale.z *= factor;
            break;
        case EditorViewportManipulatorHandle::None:
            break;
    }

    return ApplyScaleSnap(scale);
}

Vec3 EditorViewportManipulatorModel::ApplyScaleSnap(const Vec3& scale) const
{
    if (!m_toolState.snapEnabled ||
        m_toolState.snapValue <= RVX_MANIPULATOR_EPSILON)
    {
        return ClampScale(scale);
    }

    const float snapValue = m_toolState.snapValue;
    Vec3 snapped = scale;
    switch (m_activeHandle)
    {
        case EditorViewportManipulatorHandle::Center:
            snapped.x = SnapComponent(snapped.x, snapValue);
            snapped.y = SnapComponent(snapped.y, snapValue);
            snapped.z = SnapComponent(snapped.z, snapValue);
            break;
        case EditorViewportManipulatorHandle::AxisX:
            snapped.x = SnapComponent(snapped.x, snapValue);
            break;
        case EditorViewportManipulatorHandle::AxisY:
            snapped.y = SnapComponent(snapped.y, snapValue);
            break;
        case EditorViewportManipulatorHandle::AxisZ:
            snapped.z = SnapComponent(snapped.z, snapValue);
            break;
        case EditorViewportManipulatorHandle::PlaneXY:
            snapped.x = SnapComponent(snapped.x, snapValue);
            snapped.y = SnapComponent(snapped.y, snapValue);
            break;
        case EditorViewportManipulatorHandle::PlaneXZ:
            snapped.x = SnapComponent(snapped.x, snapValue);
            snapped.z = SnapComponent(snapped.z, snapValue);
            break;
        case EditorViewportManipulatorHandle::PlaneYZ:
            snapped.y = SnapComponent(snapped.y, snapValue);
            snapped.z = SnapComponent(snapped.z, snapValue);
            break;
        case EditorViewportManipulatorHandle::None:
            break;
    }

    return ClampScale(snapped);
}

Vec3 EditorViewportManipulatorModel::ClampScale(const Vec3& scale) const
{
    constexpr float RVX_MIN_SCALE = 0.001f;
    return Vec3(std::max(scale.x, RVX_MIN_SCALE),
                std::max(scale.y, RVX_MIN_SCALE),
                std::max(scale.z, RVX_MIN_SCALE));
}

float EditorViewportManipulatorModel::ComputeRotationAngle(const Vec2& screenPosition) const
{
    const EditorViewportManipulatorHandleInfo* activeHandle = FindHandle(m_activeHandle);
    if (activeHandle && activeHandle->hitRing)
    {
        const Vec2 startVector = m_dragStartScreen - activeHandle->screenCenter;
        const Vec2 currentVector = screenPosition - activeHandle->screenCenter;
        if (length(startVector) > RVX_MANIPULATOR_EPSILON &&
            length(currentVector) > RVX_MANIPULATOR_EPSILON)
        {
            const float angleRadians =
                std::atan2(Cross2D(startVector, currentVector),
                           dot(startVector, currentVector));
            return ApplyRotationSnap(angleRadians);
        }
    }

    const Vec2 pixelDelta = screenPosition - m_dragStartScreen;
    float angleRadians = 0.0f;

    switch (m_activeHandle)
    {
        case EditorViewportManipulatorHandle::Center:
            angleRadians = (pixelDelta.x - pixelDelta.y) * m_worldUnitsPerPixel;
            break;
        case EditorViewportManipulatorHandle::AxisX:
        case EditorViewportManipulatorHandle::AxisY:
        case EditorViewportManipulatorHandle::AxisZ:
        {
            const Vec3 axis = GetRotationAxis(m_activeHandle, m_dragEntity);
            if (length(axis) > RVX_MANIPULATOR_EPSILON)
            {
                angleRadians = dot(ComputeDragDelta(screenPosition), axis);
            }
            break;
        }
        case EditorViewportManipulatorHandle::PlaneXY:
        case EditorViewportManipulatorHandle::PlaneXZ:
        case EditorViewportManipulatorHandle::PlaneYZ:
            angleRadians = (pixelDelta.x - pixelDelta.y) * m_worldUnitsPerPixel;
            break;
        case EditorViewportManipulatorHandle::None:
            break;
    }

    return ApplyRotationSnap(angleRadians);
}

float EditorViewportManipulatorModel::ApplyRotationSnap(float angleRadians) const
{
    if (!m_toolState.snapEnabled ||
        m_toolState.snapValue <= RVX_MANIPULATOR_EPSILON)
    {
        return angleRadians;
    }

    return SnapComponent(angleRadians, m_toolState.snapValue);
}

Vec3 EditorViewportManipulatorModel::GetRotationAxis(
    EditorViewportManipulatorHandle handle,
    const SceneEntity* entity) const
{
    Vec3 axis(0.0f);
    switch (handle)
    {
        case EditorViewportManipulatorHandle::Center:
            axis = m_dragCameraFrame.valid ? GetCameraForward() : Vec3(0.0f, 0.0f, 1.0f);
            break;
        case EditorViewportManipulatorHandle::AxisX:
        case EditorViewportManipulatorHandle::AxisY:
        case EditorViewportManipulatorHandle::AxisZ:
            axis = GetHandleWorldAxis(handle, entity);
            break;
        case EditorViewportManipulatorHandle::PlaneXY:
        case EditorViewportManipulatorHandle::PlaneXZ:
        case EditorViewportManipulatorHandle::PlaneYZ:
            axis = GetHandlePlaneNormal(handle, entity);
            break;
        case EditorViewportManipulatorHandle::None:
            break;
    }

    if (length(axis) <= RVX_MANIPULATOR_EPSILON)
    {
        return Vec3(0.0f);
    }
    return normalize(axis);
}

Quat EditorViewportManipulatorModel::ComputeDraggedRotation(
    const Vec2& screenPosition) const
{
    const Vec3 axis = GetRotationAxis(m_activeHandle, m_dragEntity);
    if (length(axis) <= RVX_MANIPULATOR_EPSILON)
    {
        return m_dragStartRotation;
    }

    const float angleRadians = ComputeRotationAngle(screenPosition);
    const Quat deltaRotation = normalize(QuatFromAxisAngle(axis, angleRadians));
    const Quat worldRotation = normalize(deltaRotation * m_dragStartWorldRotation);
    return ComputeDraggedLocalRotation(worldRotation);
}

bool EditorViewportManipulatorModel::ConfigureRayPlaneDrag(const Vec2& screenPosition)
{
    m_dragPlaneValid = false;
    m_dragConstraintAxis = GetHandleWorldAxis(m_activeHandle, m_dragEntity);
    m_dragConstraintPlaneNormal = GetHandlePlaneNormal(m_activeHandle, m_dragEntity);

    if (!m_dragCameraFrame.valid || !m_dragEntity)
    {
        return false;
    }

    const EditorViewportScreenRay ray =
        EditorViewportProjection::BuildScreenRay(m_dragCameraFrame, screenPosition);
    if (!ray.valid)
    {
        return false;
    }

    m_dragPlanePoint = m_dragStartWorldPosition;
    if (length(m_dragConstraintPlaneNormal) > RVX_MANIPULATOR_EPSILON)
    {
        m_dragPlaneNormal = m_dragConstraintPlaneNormal;
    }
    else if (length(m_dragConstraintAxis) > RVX_MANIPULATOR_EPSILON)
    {
        Vec3 planeNormal = ray.ray.direction -
                           m_dragConstraintAxis * dot(ray.ray.direction,
                                                      m_dragConstraintAxis);
        if (length(planeNormal) <= RVX_MANIPULATOR_EPSILON)
        {
            planeNormal = GetCameraForward() -
                          m_dragConstraintAxis * dot(GetCameraForward(),
                                                     m_dragConstraintAxis);
        }
        if (length(planeNormal) <= RVX_MANIPULATOR_EPSILON)
        {
            return false;
        }

        m_dragPlaneNormal = normalize(planeNormal);
    }
    else
    {
        m_dragPlaneNormal = GetCameraForward();
        if (length(m_dragPlaneNormal) <= RVX_MANIPULATOR_EPSILON)
        {
            m_dragPlaneNormal = ray.ray.direction;
        }
    }

    const EditorViewportRayPlaneHit hit =
        EditorViewportProjection::IntersectRayPlane(ray.ray,
                                                    m_dragPlanePoint,
                                                    m_dragPlaneNormal);
    if (!hit.valid)
    {
        return false;
    }

    m_dragStartHitWorld = hit.position;
    m_dragPlaneValid = true;
    return true;
}

Vec3 EditorViewportManipulatorModel::GetHandleWorldAxis(
    EditorViewportManipulatorHandle handle,
    const SceneEntity* entity) const
{
    Vec3 axis(0.0f);
    switch (handle)
    {
        case EditorViewportManipulatorHandle::AxisX:
            axis = Vec3(1.0f, 0.0f, 0.0f);
            break;
        case EditorViewportManipulatorHandle::AxisY:
            axis = Vec3(0.0f, 1.0f, 0.0f);
            break;
        case EditorViewportManipulatorHandle::AxisZ:
            axis = Vec3(0.0f, 0.0f, 1.0f);
            break;
        case EditorViewportManipulatorHandle::PlaneXY:
        case EditorViewportManipulatorHandle::PlaneXZ:
        case EditorViewportManipulatorHandle::PlaneYZ:
        case EditorViewportManipulatorHandle::Center:
        case EditorViewportManipulatorHandle::None:
            return Vec3(0.0f);
    }

    if (m_toolState.space == EditorContext::GizmoSpace::Local && entity)
    {
        axis = Vec3(entity->GetWorldMatrix() * Vec4(axis, 0.0f));
    }

    if (length(axis) <= RVX_MANIPULATOR_EPSILON)
    {
        return Vec3(0.0f);
    }
    return normalize(axis);
}

Vec3 EditorViewportManipulatorModel::GetHandlePlaneNormal(
    EditorViewportManipulatorHandle handle,
    const SceneEntity* entity) const
{
    Vec3 axisA(0.0f);
    Vec3 axisB(0.0f);
    Vec3 normal(0.0f);
    if (!TryGetPlaneAxes(handle, entity, axisA, axisB, normal))
    {
        return Vec3(0.0f);
    }
    return normal;
}

bool EditorViewportManipulatorModel::TryGetPlaneAxes(
    EditorViewportManipulatorHandle handle,
    const SceneEntity* entity,
    Vec3& axisA,
    Vec3& axisB,
    Vec3& normal) const
{
    switch (handle)
    {
        case EditorViewportManipulatorHandle::PlaneXY:
            axisA = GetHandleWorldAxis(EditorViewportManipulatorHandle::AxisX, entity);
            axisB = GetHandleWorldAxis(EditorViewportManipulatorHandle::AxisY, entity);
            break;
        case EditorViewportManipulatorHandle::PlaneXZ:
            axisA = GetHandleWorldAxis(EditorViewportManipulatorHandle::AxisX, entity);
            axisB = GetHandleWorldAxis(EditorViewportManipulatorHandle::AxisZ, entity);
            break;
        case EditorViewportManipulatorHandle::PlaneYZ:
            axisA = GetHandleWorldAxis(EditorViewportManipulatorHandle::AxisY, entity);
            axisB = GetHandleWorldAxis(EditorViewportManipulatorHandle::AxisZ, entity);
            break;
        case EditorViewportManipulatorHandle::Center:
        case EditorViewportManipulatorHandle::AxisX:
        case EditorViewportManipulatorHandle::AxisY:
        case EditorViewportManipulatorHandle::AxisZ:
        case EditorViewportManipulatorHandle::None:
            return false;
    }

    if (length(axisA) <= RVX_MANIPULATOR_EPSILON ||
        length(axisB) <= RVX_MANIPULATOR_EPSILON)
    {
        return false;
    }

    normal = cross(axisA, axisB);
    if (length(normal) <= RVX_MANIPULATOR_EPSILON)
    {
        return false;
    }

    axisA = normalize(axisA);
    axisB = normalize(axisB);
    normal = normalize(normal);
    return true;
}

Vec3 EditorViewportManipulatorModel::GetCameraForward() const
{
    const Mat4 invView = inverse(m_dragCameraFrame.viewMatrix);
    Vec3 forward = Vec3(invView * Vec4(0.0f, 0.0f, -1.0f, 0.0f));
    if (length(forward) <= RVX_MANIPULATOR_EPSILON)
    {
        return Vec3(0.0f, 0.0f, -1.0f);
    }
    return normalize(forward);
}

Vec3 EditorViewportManipulatorModel::ComputeDraggedLocalPosition(
    const Vec3& worldDelta) const
{
    if (!m_dragEntity)
    {
        return m_dragStartPosition + worldDelta;
    }

    SceneEntity* parent = m_dragEntity->GetParent();
    if (!parent)
    {
        return m_dragStartPosition + worldDelta;
    }

    Vec4 localPosition =
        inverse(parent->GetWorldMatrix()) *
        Vec4(m_dragStartWorldPosition + worldDelta, 1.0f);
    if (std::abs(localPosition.w) > RVX_MANIPULATOR_EPSILON)
    {
        localPosition /= localPosition.w;
    }
    return Vec3(localPosition);
}

Quat EditorViewportManipulatorModel::ComputeDraggedLocalRotation(
    const Quat& worldRotation) const
{
    if (!m_dragEntity)
    {
        return normalize(worldRotation);
    }

    SceneEntity* parent = m_dragEntity->GetParent();
    if (!parent)
    {
        return normalize(worldRotation);
    }

    return normalize(inverse(parent->GetWorldRotation()) * worldRotation);
}

void EditorViewportManipulatorModel::PublishDragStats(const Vec3& delta)
{
    m_lastStats.dragging = m_activeHandle != EditorViewportManipulatorHandle::None;
    m_lastStats.activeHandle = m_activeHandle;
    m_lastStats.usesRayPlaneConstraint = m_dragUsesRayPlaneConstraint;
    m_lastStats.dragPlaneValid = m_dragPlaneValid;
    m_lastStats.previewDelta = delta;
    m_lastStats.committedDelta = Vec3(0.0f);
    m_lastStats.previewScale = m_dragStartScale;
    m_lastStats.committedScale = Vec3(1.0f);
    m_lastStats.previewRotation = m_dragStartRotation;
    m_lastStats.committedRotation = Quat(1.0f, 0.0f, 0.0f, 0.0f);
    m_lastStats.previewAngleRadians = 0.0f;
    m_lastStats.committedAngleRadians = 0.0f;
    m_lastStats.appliedTransform = false;
}

void EditorViewportManipulatorModel::PublishScaleStats(const Vec3& scale)
{
    m_lastStats.dragging = m_activeHandle != EditorViewportManipulatorHandle::None;
    m_lastStats.activeHandle = m_activeHandle;
    m_lastStats.usesRayPlaneConstraint = m_dragUsesRayPlaneConstraint;
    m_lastStats.dragPlaneValid = m_dragPlaneValid;
    m_lastStats.previewDelta = scale - m_dragStartScale;
    m_lastStats.committedDelta = Vec3(0.0f);
    m_lastStats.previewScale = scale;
    m_lastStats.committedScale = Vec3(1.0f);
    m_lastStats.previewRotation = m_dragStartRotation;
    m_lastStats.committedRotation = Quat(1.0f, 0.0f, 0.0f, 0.0f);
    m_lastStats.previewAngleRadians = 0.0f;
    m_lastStats.committedAngleRadians = 0.0f;
    m_lastStats.appliedTransform = false;
}

void EditorViewportManipulatorModel::PublishRotationStats(const Quat& rotation,
                                                          float angleRadians)
{
    m_lastStats.dragging = m_activeHandle != EditorViewportManipulatorHandle::None;
    m_lastStats.activeHandle = m_activeHandle;
    m_lastStats.usesRayPlaneConstraint = m_dragUsesRayPlaneConstraint;
    m_lastStats.dragPlaneValid = m_dragPlaneValid;
    m_lastStats.previewDelta = Vec3(0.0f);
    m_lastStats.committedDelta = Vec3(0.0f);
    m_lastStats.previewScale = m_dragStartScale;
    m_lastStats.committedScale = Vec3(1.0f);
    m_lastStats.previewRotation = rotation;
    m_lastStats.committedRotation = Quat(1.0f, 0.0f, 0.0f, 0.0f);
    m_lastStats.previewAngleRadians = angleRadians;
    m_lastStats.committedAngleRadians = 0.0f;
    m_lastStats.appliedTransform = false;
}

} // namespace RVX::Editor
