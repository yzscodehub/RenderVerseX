/**
 * @file EditorViewportInteractionModel.h
 * @brief Native viewport input routing and navigation capture model
 */

#pragma once

#include "Core/Types.h"
#include "UI/UIContext.h"
#include "UI/UITypes.h"

namespace RVX::Editor
{

enum class EditorViewportInteractionAction : uint8
{
    None = 0,
    Orbit,
    Pan,
    Zoom
};

enum class EditorViewportCursorMode : uint8
{
    Normal = 0,
    Hidden,
    Locked
};

struct EditorViewportInteractionDesc
{
    UI::Rect viewportBounds;
    bool navigationEnabled = true;
    bool cursorCaptureEnabled = true;
    bool cursorLockEnabled = true;
    bool hideCursorWhileNavigating = true;
};

struct EditorViewportInteractionState
{
    bool valid = false;
    bool hovered = false;
    bool focused = false;
    bool navigating = false;
    bool navigationStarted = false;
    bool navigationEnded = false;
    bool routesCameraInput = false;
    bool blocksSceneTools = false;
    bool requestsMouseCapture = false;
    bool requestsCursorHidden = false;
    bool requestsCursorLock = false;
    EditorViewportInteractionAction action = EditorViewportInteractionAction::None;
    EditorViewportCursorMode cursorMode = EditorViewportCursorMode::Normal;
    UI::UIMouseButton captureButton = UI::UIMouseButton::Left;
    UI::Rect viewportBounds;
    Vec2 mousePosition{0.0f};
    Vec2 localMousePosition{0.0f};
    Vec2 dragStart{0.0f};
    Vec2 dragDelta{0.0f};
};

struct EditorViewportInteractionStats
{
    uint32 updateCount = 0;
    uint32 navigationBeginCount = 0;
    uint32 navigationEndCount = 0;
    bool hovered = false;
    bool focused = false;
    bool navigating = false;
    bool routesCameraInput = false;
    bool blocksSceneTools = false;
    bool requestsMouseCapture = false;
    bool requestsCursorHidden = false;
    bool requestsCursorLock = false;
    EditorViewportInteractionAction action = EditorViewportInteractionAction::None;
    EditorViewportCursorMode cursorMode = EditorViewportCursorMode::Normal;
};

class EditorViewportInteractionModel
{
public:
    EditorViewportInteractionModel() = default;

    const EditorViewportInteractionState& Update(
        const UI::UIInputState& input,
        const EditorViewportInteractionDesc& desc);

    void Reset();

    const EditorViewportInteractionState& GetState() const { return m_state; }
    const EditorViewportInteractionStats& GetLastStats() const { return m_stats; }

    static const char* GetActionName(EditorViewportInteractionAction action);
    static const char* GetCursorModeName(EditorViewportCursorMode mode);

private:
    bool IsCaptureButtonDown(const UI::UIInputState& input) const;
    void BeginNavigation(UI::UIMouseButton button, const Vec2& mousePosition);
    void EndNavigation();

    EditorViewportInteractionState m_state;
    EditorViewportInteractionStats m_stats;
    bool m_hasNavigationCapture = false;
    UI::UIMouseButton m_captureButton = UI::UIMouseButton::Left;
    Vec2 m_dragStart{0.0f};
};

} // namespace RVX::Editor
