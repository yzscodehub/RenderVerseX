/**
 * @file EditorViewportNavigationModel.h
 * @brief Native viewport navigation HUD model
 */

#pragma once

#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "Editor/Panels/Viewport.h"
#include "Editor/UI/EditorViewportProjection.h"
#include "UI/UITypes.h"

#include <array>

namespace RVX::Editor
{

enum class EditorViewportNavigationAxis : uint8
{
    X = 0,
    Y,
    Z
};

struct EditorViewportNavigationAxisVisual
{
    EditorViewportNavigationAxis axis = EditorViewportNavigationAxis::X;
    const char* label = "X";
    Vec3 worldDirection{1.0f, 0.0f, 0.0f};
    Vec2 direction{1.0f, 0.0f};
    Vec2 endPoint{0.0f};
    UI::Rect hitRect;
    float depth = 0.0f;
    bool valid = false;
    bool usedProjectionFallback = false;
};

struct EditorViewportNavigationHUDDesc
{
    EditorViewportCameraFrame cameraFrame;
    UI::Rect localBounds;
    float axisLength = 32.0f;
    float hitSize = 24.0f;
};

struct EditorViewportNavigationHUDState
{
    bool valid = false;
    Vec2 center{0.0f};
    uint32 axisCount = 0;
    std::array<EditorViewportNavigationAxisVisual, 3> axes{};
};

struct EditorViewportNavigationViewTarget
{
    bool valid = false;
    EditorViewportNavigationAxis axis = EditorViewportNavigationAxis::X;
    ViewportCameraMode cameraMode = ViewportCameraMode::Orbit;
    Vec3 directionFromTarget{1.0f, 0.0f, 0.0f};
    Vec3 target{0.0f};
    float distance = 10.0f;
};

struct EditorViewportNavigationStats
{
    bool hudBuilt = false;
    uint32 axisCount = 0;
    uint32 activationCount = 0;
    EditorViewportNavigationAxis lastActivatedAxis = EditorViewportNavigationAxis::X;
};

class EditorViewportNavigationModel
{
public:
    EditorViewportNavigationModel() = default;

    const EditorViewportNavigationHUDState& BuildHUD(
        const EditorViewportNavigationHUDDesc& desc);

    const EditorViewportNavigationHUDState& GetHUDState() const { return m_hudState; }
    const EditorViewportNavigationStats& GetLastStats() const { return m_lastStats; }

    const EditorViewportNavigationAxisVisual* FindAxis(
        EditorViewportNavigationAxis axis) const;
    const EditorViewportNavigationAxisVisual* HitTestAxis(
        const Vec2& localPoint) const;

    EditorViewportNavigationViewTarget ActivateAxis(
        EditorViewportNavigationAxis axis,
        const Vec3& cameraPosition,
        const Vec3& cameraTarget);

    static const char* GetAxisName(EditorViewportNavigationAxis axis);
    static Vec3 GetAxisDirection(EditorViewportNavigationAxis axis);

private:
    EditorViewportNavigationHUDState m_hudState;
    EditorViewportNavigationStats m_lastStats;
};

} // namespace RVX::Editor
