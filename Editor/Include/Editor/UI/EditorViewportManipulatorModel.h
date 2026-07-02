/**
 * @file EditorViewportManipulatorModel.h
 * @brief Native viewport transform manipulator model
 */

#pragma once

#include "Editor/UI/EditorViewportProjection.h"
#include "Editor/UI/EditorViewportToolModel.h"
#include "UI/UITypes.h"

#include <array>

namespace RVX
{
class SceneEntity;

namespace UI
{
struct UIInputState;
}
} // namespace RVX

namespace RVX::Editor
{

inline constexpr uint32 RVX_EDITOR_MANIPULATOR_RING_SAMPLE_COUNT = 32;

enum class EditorViewportManipulatorHandle : uint8
{
    None = 0,
    Center,
    AxisX,
    AxisY,
    AxisZ,
    PlaneXY,
    PlaneXZ,
    PlaneYZ
};

struct EditorViewportManipulatorBuildDesc
{
    UI::Rect viewportBounds;
    EditorViewportToolState toolState;
    EditorViewportCameraFrame cameraFrame;
    float worldUnitsPerPixel = 0.01f;
};

struct EditorViewportManipulatorHandleInfo
{
    EditorViewportManipulatorHandle handle = EditorViewportManipulatorHandle::None;
    UI::Rect bounds;
    Vec2 screenStart{0.0f};
    Vec2 screenEnd{0.0f};
    std::array<Vec2, 4> screenCorners{};
    std::array<Vec2, RVX_EDITOR_MANIPULATOR_RING_SAMPLE_COUNT> screenRingSamples{};
    uint32 screenRingSampleCount = 0;
    Vec2 screenCenter{0.0f};
    Vec3 axis{0.0f};
    Vec3 planeNormal{0.0f};
    float ringRadius = 0.0f;
    float hitRadius = 0.0f;
    bool enabled = false;
    bool hitSegment = false;
    bool hitPolygon = false;
    bool hitRing = false;
    bool usesProjectedSegment = false;
    bool usesProjectedPlane = false;
    bool usesProjectedRing = false;
};

struct EditorViewportManipulatorStats
{
    bool built = false;
    bool hasSelection = false;
    bool dragging = false;
    bool unsupportedMode = false;
    bool appliedTransform = false;
    bool anchorProjected = false;
    bool anchorVisible = false;
    bool usesRayPlaneConstraint = false;
    bool dragPlaneValid = false;
    uint32 projectedSegmentHandleCount = 0;
    uint32 projectedPlaneHandleCount = 0;
    uint32 projectedRingHandleCount = 0;
    bool usesLegacyGizmoAdapter = false;
    uint32 handleCount = 0;
    EditorViewportManipulatorHandle activeHandle = EditorViewportManipulatorHandle::None;
    EditorViewportToolState toolState;
    UI::Rect viewportBounds;
    Vec2 anchor{0.0f};
    Vec3 previewDelta{0.0f};
    Vec3 committedDelta{0.0f};
    Vec3 previewScale{1.0f};
    Vec3 committedScale{1.0f};
    Quat previewRotation{1.0f, 0.0f, 0.0f, 0.0f};
    Quat committedRotation{1.0f, 0.0f, 0.0f, 0.0f};
    float previewAngleRadians = 0.0f;
    float committedAngleRadians = 0.0f;
};

class EditorViewportManipulatorModel
{
public:
    EditorViewportManipulatorModel() = default;

    void Build(EditorContext& context, const EditorViewportManipulatorBuildDesc& desc);
    bool HandleInput(EditorContext& context, const UI::UIInputState& input);

    bool BeginDrag(EditorContext& context, const Vec2& screenPosition);
    bool UpdateDrag(const Vec2& screenPosition);
    bool EndDrag(EditorContext& context, const Vec2& screenPosition);
    void CancelDrag();

    uint32 GetHandleCount() const { return m_handleCount; }
    const EditorViewportManipulatorHandleInfo* GetHandle(uint32 index) const;
    const EditorViewportManipulatorHandleInfo* FindHandle(
        EditorViewportManipulatorHandle handle) const;

    const EditorViewportManipulatorStats& GetLastStats() const { return m_lastStats; }

private:
    bool IsSupportedMode() const;
    EditorViewportManipulatorHandle HitTest(const Vec2& screenPosition) const;
    bool BuildProjectedAxisHandle(SceneEntity* selectedEntity,
                                  EditorViewportManipulatorHandle handle,
                                  const Vec2& anchor,
                                  float axisGap,
                                  float axisLength,
                                  float axisThickness);
    bool BuildProjectedRotationRingHandle(SceneEntity* selectedEntity,
                                          EditorViewportManipulatorHandle handle,
                                          const Vec2& anchor,
                                          float ringWorldRadius,
                                          float ringHitRadius);
    bool BuildProjectedPlaneHandle(SceneEntity* selectedEntity,
                                   EditorViewportManipulatorHandle handle,
                                   float planeGap,
                                   float planeSize);
    void AddRectHandle(EditorViewportManipulatorHandle handle,
                       const UI::Rect& bounds,
                       const Vec3& axis,
                       bool hitSegment = false,
                       const Vec2& screenStart = Vec2(0.0f),
                       const Vec2& screenEnd = Vec2(0.0f),
                       float hitRadius = 0.0f,
                       bool usesProjectedSegment = false);
    void AddPlaneHandle(EditorViewportManipulatorHandle handle,
                        const UI::Rect& bounds,
                        const Vec3& planeNormal,
                        const std::array<Vec2, 4>& screenCorners = {},
                        bool hitPolygon = false,
                        bool usesProjectedPlane = false);
    void AddRingHandle(EditorViewportManipulatorHandle handle,
                       const UI::Rect& bounds,
                       const Vec3& axis,
                       const Vec2& screenCenter,
                       float ringRadius,
                       float hitRadius,
                       const std::array<Vec2, RVX_EDITOR_MANIPULATOR_RING_SAMPLE_COUNT>& samples,
                       uint32 sampleCount,
                       bool usesProjectedRing);
    void AddFallbackRotationRingHandle(EditorViewportManipulatorHandle handle,
                                       const Vec2& anchor,
                                       float ringRadius,
                                       float hitRadius,
                                       const Vec3& axis);
    Vec3 GetHandleWorldAxis(EditorViewportManipulatorHandle handle,
                            const SceneEntity* entity) const;
    Vec3 GetHandlePlaneNormal(EditorViewportManipulatorHandle handle,
                              const SceneEntity* entity) const;
    bool TryGetPlaneAxes(EditorViewportManipulatorHandle handle,
                         const SceneEntity* entity,
                         Vec3& axisA,
                         Vec3& axisB,
                         Vec3& normal) const;
    Vec3 ComputeDragDelta(const Vec2& screenPosition) const;
    Vec3 ComputePixelFallbackDragDelta(const Vec2& screenPosition) const;
    Vec3 ApplySnap(const Vec3& delta) const;
    Vec3 ComputeDraggedScale(const Vec2& screenPosition) const;
    Vec3 ApplyScaleSnap(const Vec3& scale) const;
    Vec3 ClampScale(const Vec3& scale) const;
    float ComputeRotationAngle(const Vec2& screenPosition) const;
    float ApplyRotationSnap(float angleRadians) const;
    Vec3 GetRotationAxis(EditorViewportManipulatorHandle handle,
                         const SceneEntity* entity) const;
    Quat ComputeDraggedRotation(const Vec2& screenPosition) const;
    bool ConfigureRayPlaneDrag(const Vec2& screenPosition);
    Vec3 GetCameraForward() const;
    Vec3 ComputeDraggedLocalPosition(const Vec3& worldDelta) const;
    Quat ComputeDraggedLocalRotation(const Quat& worldRotation) const;
    void PublishDragStats(const Vec3& delta);
    void PublishScaleStats(const Vec3& scale);
    void PublishRotationStats(const Quat& rotation, float angleRadians);

    std::array<EditorViewportManipulatorHandleInfo, 7> m_handles{};
    uint32 m_handleCount = 0;
    EditorViewportManipulatorStats m_lastStats;
    EditorViewportToolState m_toolState;
    UI::Rect m_viewportBounds;
    EditorViewportCameraFrame m_cameraFrame;
    float m_worldUnitsPerPixel = 0.01f;

    EditorViewportManipulatorHandle m_activeHandle = EditorViewportManipulatorHandle::None;
    SceneEntity* m_dragEntity = nullptr;
    Vec2 m_dragStartScreen{0.0f};
    Vec3 m_dragStartPosition{0.0f};
    Vec3 m_dragStartWorldPosition{0.0f};
    Quat m_dragStartRotation{1.0f, 0.0f, 0.0f, 0.0f};
    Quat m_dragStartWorldRotation{1.0f, 0.0f, 0.0f, 0.0f};
    Vec3 m_dragStartScale{1.0f};
    EditorViewportCameraFrame m_dragCameraFrame;
    bool m_dragUsesRayPlaneConstraint = false;
    bool m_dragPlaneValid = false;
    Vec3 m_dragPlanePoint{0.0f};
    Vec3 m_dragPlaneNormal{0.0f, 0.0f, 1.0f};
    Vec3 m_dragConstraintAxis{0.0f};
    Vec3 m_dragConstraintPlaneNormal{0.0f};
    Vec3 m_dragStartHitWorld{0.0f};
};

} // namespace RVX::Editor
