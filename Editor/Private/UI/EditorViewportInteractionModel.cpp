/**
 * @file EditorViewportInteractionModel.cpp
 * @brief Native viewport input routing and navigation capture implementation
 */

#include "Editor/UI/EditorViewportInteractionModel.h"

namespace RVX::Editor
{
namespace
{
    bool IsUsableBounds(const UI::Rect& bounds)
    {
        return bounds.width > 0.0f && bounds.height > 0.0f;
    }

    EditorViewportInteractionAction ActionForButton(UI::UIMouseButton button)
    {
        switch (button)
        {
            case UI::UIMouseButton::Right:
                return EditorViewportInteractionAction::Orbit;
            case UI::UIMouseButton::Middle:
                return EditorViewportInteractionAction::Pan;
            default:
                return EditorViewportInteractionAction::None;
        }
    }
} // namespace

const EditorViewportInteractionState& EditorViewportInteractionModel::Update(
    const UI::UIInputState& input,
    const EditorViewportInteractionDesc& desc)
{
    ++m_stats.updateCount;

    const bool boundsValid = IsUsableBounds(desc.viewportBounds);
    const bool hovered =
        boundsValid && desc.viewportBounds.Contains(input.current.mousePosition);

    m_state = {};
    m_state.valid = boundsValid;
    m_state.hovered = hovered;
    m_state.focused = hovered || m_hasNavigationCapture;
    m_state.viewportBounds = desc.viewportBounds;
    m_state.mousePosition = input.current.mousePosition;
    m_state.localMousePosition =
        boundsValid
            ? Vec2(input.current.mousePosition.x - desc.viewportBounds.x,
                   input.current.mousePosition.y - desc.viewportBounds.y)
            : Vec2(0.0f);

    if (m_hasNavigationCapture && !IsCaptureButtonDown(input))
    {
        EndNavigation();
        m_state.navigationEnded = true;
    }

    if (!m_hasNavigationCapture && desc.navigationEnabled && hovered)
    {
        if (input.WasMouseButtonPressed(UI::UIMouseButton::Right))
        {
            BeginNavigation(UI::UIMouseButton::Right, input.current.mousePosition);
            m_state.navigationStarted = true;
        }
        else if (input.WasMouseButtonPressed(UI::UIMouseButton::Middle))
        {
            BeginNavigation(UI::UIMouseButton::Middle, input.current.mousePosition);
            m_state.navigationStarted = true;
        }
    }

    if (m_hasNavigationCapture)
    {
        m_state.focused = true;
        m_state.navigating = true;
        m_state.routesCameraInput = true;
        m_state.blocksSceneTools = true;
        m_state.requestsMouseCapture = desc.cursorCaptureEnabled;
        if (desc.cursorCaptureEnabled && desc.cursorLockEnabled)
        {
            m_state.requestsCursorLock = true;
            m_state.requestsCursorHidden = true;
            m_state.cursorMode = EditorViewportCursorMode::Locked;
        }
        else if (desc.cursorCaptureEnabled && desc.hideCursorWhileNavigating)
        {
            m_state.requestsCursorHidden = true;
            m_state.cursorMode = EditorViewportCursorMode::Hidden;
        }
        m_state.captureButton = m_captureButton;
        m_state.action = ActionForButton(m_captureButton);
        m_state.dragStart = m_dragStart;
        m_state.dragDelta = input.current.mousePosition - m_dragStart;
    }
    else if (desc.navigationEnabled && hovered &&
             (input.current.scrollDelta.x != 0.0f ||
              input.current.scrollDelta.y != 0.0f))
    {
        m_state.routesCameraInput = true;
        m_state.action = EditorViewportInteractionAction::Zoom;
    }

    m_stats.hovered = m_state.hovered;
    m_stats.focused = m_state.focused;
    m_stats.navigating = m_state.navigating;
    m_stats.routesCameraInput = m_state.routesCameraInput;
    m_stats.blocksSceneTools = m_state.blocksSceneTools;
    m_stats.requestsMouseCapture = m_state.requestsMouseCapture;
    m_stats.requestsCursorHidden = m_state.requestsCursorHidden;
    m_stats.requestsCursorLock = m_state.requestsCursorLock;
    m_stats.action = m_state.action;
    m_stats.cursorMode = m_state.cursorMode;
    return m_state;
}

void EditorViewportInteractionModel::Reset()
{
    m_state = {};
    m_stats = {};
    m_hasNavigationCapture = false;
    m_captureButton = UI::UIMouseButton::Left;
    m_dragStart = Vec2(0.0f);
}

const char* EditorViewportInteractionModel::GetActionName(
    EditorViewportInteractionAction action)
{
    switch (action)
    {
        case EditorViewportInteractionAction::None:
            return "None";
        case EditorViewportInteractionAction::Orbit:
            return "Orbit";
        case EditorViewportInteractionAction::Pan:
            return "Pan";
        case EditorViewportInteractionAction::Zoom:
            return "Zoom";
    }
    return "None";
}

const char* EditorViewportInteractionModel::GetCursorModeName(
    EditorViewportCursorMode mode)
{
    switch (mode)
    {
        case EditorViewportCursorMode::Normal:
            return "Normal";
        case EditorViewportCursorMode::Hidden:
            return "Hidden";
        case EditorViewportCursorMode::Locked:
            return "Locked";
    }
    return "Normal";
}

bool EditorViewportInteractionModel::IsCaptureButtonDown(
    const UI::UIInputState& input) const
{
    return m_hasNavigationCapture &&
           input.current.IsMouseButtonDown(m_captureButton);
}

void EditorViewportInteractionModel::BeginNavigation(UI::UIMouseButton button,
                                                     const Vec2& mousePosition)
{
    m_hasNavigationCapture = true;
    m_captureButton = button;
    m_dragStart = mousePosition;
    ++m_stats.navigationBeginCount;
}

void EditorViewportInteractionModel::EndNavigation()
{
    m_hasNavigationCapture = false;
    m_captureButton = UI::UIMouseButton::Left;
    m_dragStart = Vec2(0.0f);
    ++m_stats.navigationEndCount;
}

} // namespace RVX::Editor
