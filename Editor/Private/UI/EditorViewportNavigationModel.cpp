/**
 * @file EditorViewportNavigationModel.cpp
 * @brief Native viewport navigation HUD model implementation
 */

#include "Editor/UI/EditorViewportNavigationModel.h"

#include <algorithm>
#include <cmath>

namespace RVX::Editor
{
namespace
{
    constexpr float RVX_VIEWPORT_NAVIGATION_EPSILON = 0.0001f;

    Vec2 FallbackAxisDirection(EditorViewportNavigationAxis axis)
    {
        switch (axis)
        {
            case EditorViewportNavigationAxis::X:
                return Vec2(1.0f, 0.16f);
            case EditorViewportNavigationAxis::Y:
                return Vec2(0.05f, -1.0f);
            case EditorViewportNavigationAxis::Z:
                return Vec2(-0.68f, 0.54f);
        }
        return Vec2(1.0f, 0.0f);
    }

    EditorViewportNavigationAxisVisual BuildAxisVisual(
        EditorViewportNavigationAxis axis,
        const EditorViewportNavigationHUDDesc& desc,
        const Vec2& center)
    {
        EditorViewportNavigationAxisVisual visual;
        visual.axis = axis;
        visual.label = EditorViewportNavigationModel::GetAxisName(axis);
        visual.worldDirection = EditorViewportNavigationModel::GetAxisDirection(axis);

        const Vec4 viewDirection =
            desc.cameraFrame.viewMatrix * Vec4(visual.worldDirection, 0.0f);
        Vec2 projectedDirection(viewDirection.x, -viewDirection.y);
        const float projectedLength = length(projectedDirection);
        if (projectedLength > RVX_VIEWPORT_NAVIGATION_EPSILON)
        {
            visual.direction = projectedDirection / projectedLength;
        }
        else
        {
            visual.direction = normalize(FallbackAxisDirection(axis));
            visual.usedProjectionFallback = true;
        }

        visual.depth = viewDirection.z;
        visual.endPoint = center + visual.direction * std::max(0.0f, desc.axisLength);
        const float hitSize = std::max(8.0f, desc.hitSize);
        visual.hitRect = UI::Rect(visual.endPoint.x - hitSize * 0.5f,
                                  visual.endPoint.y - hitSize * 0.5f,
                                  hitSize,
                                  hitSize);
        visual.valid = desc.axisLength > 0.0f;
        return visual;
    }
} // namespace

const EditorViewportNavigationHUDState& EditorViewportNavigationModel::BuildHUD(
    const EditorViewportNavigationHUDDesc& desc)
{
    m_hudState = {};
    m_lastStats.hudBuilt = false;
    m_lastStats.axisCount = 0;

    if (!desc.cameraFrame.valid ||
        desc.localBounds.width <= 0.0f ||
        desc.localBounds.height <= 0.0f ||
        desc.axisLength <= 0.0f)
    {
        return m_hudState;
    }

    m_hudState.valid = true;
    m_hudState.center = Vec2(desc.localBounds.width * 0.48f,
                             desc.localBounds.height * 0.58f);

    m_hudState.axes[0] =
        BuildAxisVisual(EditorViewportNavigationAxis::X, desc, m_hudState.center);
    m_hudState.axes[1] =
        BuildAxisVisual(EditorViewportNavigationAxis::Y, desc, m_hudState.center);
    m_hudState.axes[2] =
        BuildAxisVisual(EditorViewportNavigationAxis::Z, desc, m_hudState.center);

    for (const EditorViewportNavigationAxisVisual& axis : m_hudState.axes)
    {
        if (axis.valid)
        {
            ++m_hudState.axisCount;
        }
    }

    m_lastStats.hudBuilt = m_hudState.valid;
    m_lastStats.axisCount = m_hudState.axisCount;
    return m_hudState;
}

const EditorViewportNavigationAxisVisual* EditorViewportNavigationModel::FindAxis(
    EditorViewportNavigationAxis axis) const
{
    const auto it = std::find_if(m_hudState.axes.begin(),
                                 m_hudState.axes.end(),
                                 [axis](const EditorViewportNavigationAxisVisual& visual) {
                                     return visual.valid && visual.axis == axis;
                                 });
    return it != m_hudState.axes.end() ? &(*it) : nullptr;
}

const EditorViewportNavigationAxisVisual* EditorViewportNavigationModel::HitTestAxis(
    const Vec2& localPoint) const
{
    for (auto it = m_hudState.axes.rbegin(); it != m_hudState.axes.rend(); ++it)
    {
        if (it->valid && it->hitRect.Contains(localPoint))
        {
            return &(*it);
        }
    }
    return nullptr;
}

EditorViewportNavigationViewTarget EditorViewportNavigationModel::ActivateAxis(
    EditorViewportNavigationAxis axis,
    const Vec3& cameraPosition,
    const Vec3& cameraTarget)
{
    EditorViewportNavigationViewTarget target;
    target.axis = axis;
    target.target = cameraTarget;
    target.directionFromTarget = GetAxisDirection(axis);
    target.distance = length(cameraPosition - cameraTarget);
    if (!std::isfinite(target.distance) || target.distance <= RVX_VIEWPORT_NAVIGATION_EPSILON)
    {
        target.distance = 10.0f;
    }
    target.cameraMode = axis == EditorViewportNavigationAxis::Y
                            ? ViewportCameraMode::TopDown
                            : ViewportCameraMode::Orbit;
    target.valid = true;

    ++m_lastStats.activationCount;
    m_lastStats.lastActivatedAxis = axis;
    return target;
}

const char* EditorViewportNavigationModel::GetAxisName(
    EditorViewportNavigationAxis axis)
{
    switch (axis)
    {
        case EditorViewportNavigationAxis::X:
            return "X";
        case EditorViewportNavigationAxis::Y:
            return "Y";
        case EditorViewportNavigationAxis::Z:
            return "Z";
    }
    return "X";
}

Vec3 EditorViewportNavigationModel::GetAxisDirection(
    EditorViewportNavigationAxis axis)
{
    switch (axis)
    {
        case EditorViewportNavigationAxis::X:
            return Vec3(1.0f, 0.0f, 0.0f);
        case EditorViewportNavigationAxis::Y:
            return Vec3(0.0f, 1.0f, 0.0f);
        case EditorViewportNavigationAxis::Z:
            return Vec3(0.0f, 0.0f, 1.0f);
    }
    return Vec3(1.0f, 0.0f, 0.0f);
}

} // namespace RVX::Editor
