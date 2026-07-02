/**
 * @file EditorInputBridge.h
 * @brief Native editor input snapshot bridge
 */

#pragma once

#include "Core/Types.h"
#include "UI/UIContext.h"

#include <array>
#include <string>

struct GLFWwindow;

namespace RVX::Editor
{

struct EditorInputBridgeCaptureState
{
    bool wantsMouseCapture = false;
    bool wantsKeyboardCapture = false;
};

struct EditorInputBridgeStats
{
    bool attached = false;
    uint32 textByteCount = 0;
    uint32 mouseButtonEdgeCount = 0;
    Vec2 scrollDelta{0.0f};
};

/**
 * @brief Builds native editor UI input snapshots from the platform window.
 *
 * This keeps the native UI input path independent from ImGui. While the debug
 * ImGui bridge still exists, GLFW callbacks are chained so both systems can
 * receive text and scroll events.
 */
class EditorInputBridge
{
public:
    // =========================================================================
    // Lifecycle
    // =========================================================================
    bool Attach(GLFWwindow* window);
    void Detach();
    bool IsAttached() const { return m_window != nullptr; }

    // =========================================================================
    // Snapshot
    // =========================================================================
    UI::UIInputSnapshot BuildSnapshot(
        const EditorInputBridgeCaptureState& captureState = {});

    // =========================================================================
    // Event Queue
    // =========================================================================
    void QueueScroll(double xOffset, double yOffset);
    void QueueCharacter(uint32 codepoint);
    void QueueMouseButton(int button, bool pressed);

    // =========================================================================
    // Utilities
    // =========================================================================
    static uint32 MapGLFWKeyToUIKey(int glfwKey);
    static void AppendCodepointUTF8(std::string& output, uint32 codepoint);

    const EditorInputBridgeStats& GetLastBuildStats() const
    {
        return m_lastBuildStats;
    }

private:
    void PollWindowState(UI::UIInputSnapshot& input) const;

    GLFWwindow* m_window = nullptr;
    Vec2 m_pendingScrollDelta{0.0f};
    std::array<bool, UI::RVX_UI_MOUSE_BUTTON_COUNT> m_pendingMouseButtonPresses{};
    std::array<bool, UI::RVX_UI_MOUSE_BUTTON_COUNT> m_pendingMouseButtonReleases{};
    std::string m_pendingTextInput;
    EditorInputBridgeStats m_lastBuildStats;
};

} // namespace RVX::Editor
