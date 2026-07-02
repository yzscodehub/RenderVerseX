/**
 * @file EditorWindowFrameService.h
 * @brief Editor platform window per-frame state service.
 */

#pragma once

#include "Core/Types.h"

#include <string>

struct GLFWwindow;

namespace RVX::Editor
{

enum class EditorWindowCursorMode : uint8
{
    Normal = 0,
    Hidden,
    Locked
};

struct EditorWindowMetricsDesc
{
    GLFWwindow* window = nullptr;
};

struct EditorWindowMetricsResult
{
    bool captured = false;
    int windowWidth = 0;
    int windowHeight = 0;
    int framebufferWidth = 0;
    int framebufferHeight = 0;
    float contentScaleX = 1.0f;
    float contentScaleY = 1.0f;
    float surfaceScaleFactor = 1.0f;
    std::string error;

    explicit operator bool() const { return captured; }
};

struct EditorWindowFrameDesc
{
    GLFWwindow* window = nullptr;
    bool pollEvents = true;
    bool clearCloseRequest = true;
};

struct EditorWindowFrameResult
{
    bool processed = false;
    bool eventsPolled = false;
    bool closeRequested = false;
    bool closeRequestCleared = false;
    int windowWidth = 0;
    int windowHeight = 0;
    int framebufferWidth = 0;
    int framebufferHeight = 0;
    float contentScaleX = 1.0f;
    float contentScaleY = 1.0f;
    float surfaceScaleFactor = 1.0f;
    double currentTime = 0.0;
    std::string error;

    explicit operator bool() const { return processed; }
};

struct EditorWindowCursorDesc
{
    GLFWwindow* window = nullptr;
    EditorWindowCursorMode mode = EditorWindowCursorMode::Normal;
};

struct EditorWindowCursorResult
{
    bool applied = false;
    EditorWindowCursorMode requestedMode = EditorWindowCursorMode::Normal;
    int glfwCursorMode = 0;
    std::string error;

    explicit operator bool() const { return applied; }
};

/**
 * @brief Captures GLFW-backed editor window frame state.
 */
class EditorWindowFrameService
{
public:
    // =========================================================================
    // Public Methods
    // =========================================================================

    EditorWindowMetricsResult CaptureMetrics(
        const EditorWindowMetricsDesc& desc) const;
    EditorWindowFrameResult BeginFrame(
        const EditorWindowFrameDesc& desc) const;
    EditorWindowCursorResult ApplyCursorMode(
        const EditorWindowCursorDesc& desc) const;
    double GetTime() const;

private:
    static EditorWindowMetricsResult FailMetrics(std::string error);
    static EditorWindowFrameResult FailFrame(std::string error);
    static EditorWindowCursorResult FailCursor(EditorWindowCursorMode mode,
                                               std::string error);
};

} // namespace RVX::Editor
