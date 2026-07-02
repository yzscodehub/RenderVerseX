/**
 * @file EditorWindowFrameService.cpp
 * @brief Editor platform window per-frame state service implementation.
 */

#include "Editor/EditorWindowFrameService.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace RVX::Editor
{
namespace
{
    constexpr float RVX_EDITOR_MIN_SURFACE_SCALE = 1.0f;
    constexpr float RVX_EDITOR_MAX_SURFACE_SCALE = 3.0f;

    float SanitizeScale(float scale)
    {
        if (!std::isfinite(scale) || scale <= 0.0f)
        {
            return 1.0f;
        }
        return std::clamp(scale,
                          RVX_EDITOR_MIN_SURFACE_SCALE,
                          RVX_EDITOR_MAX_SURFACE_SCALE);
    }

    float ComputeSurfaceScaleFactor(int windowWidth,
                                    int windowHeight,
                                    int framebufferWidth,
                                    int framebufferHeight,
                                    float contentScaleX,
                                    float contentScaleY)
    {
        float scale = std::max(SanitizeScale(contentScaleX),
                               SanitizeScale(contentScaleY));
        if (windowWidth > 0 && framebufferWidth > 0)
        {
            scale = std::max(
                scale,
                SanitizeScale(static_cast<float>(framebufferWidth) /
                              static_cast<float>(windowWidth)));
        }
        if (windowHeight > 0 && framebufferHeight > 0)
        {
            scale = std::max(
                scale,
                SanitizeScale(static_cast<float>(framebufferHeight) /
                              static_cast<float>(windowHeight)));
        }
        return SanitizeScale(scale);
    }

    int ToGLFWCursorMode(EditorWindowCursorMode mode)
    {
        switch (mode)
        {
            case EditorWindowCursorMode::Normal:
                return GLFW_CURSOR_NORMAL;
            case EditorWindowCursorMode::Hidden:
                return GLFW_CURSOR_HIDDEN;
            case EditorWindowCursorMode::Locked:
                return GLFW_CURSOR_DISABLED;
        }
        return GLFW_CURSOR_NORMAL;
    }
}

EditorWindowMetricsResult
EditorWindowFrameService::CaptureMetrics(
    const EditorWindowMetricsDesc& desc) const
{
    if (!desc.window)
    {
        return FailMetrics("Editor window is unavailable");
    }

    EditorWindowMetricsResult result;
    glfwGetWindowSize(desc.window, &result.windowWidth, &result.windowHeight);
    glfwGetFramebufferSize(desc.window,
                           &result.framebufferWidth,
                           &result.framebufferHeight);
    glfwGetWindowContentScale(desc.window,
                              &result.contentScaleX,
                              &result.contentScaleY);
    result.surfaceScaleFactor =
        ComputeSurfaceScaleFactor(result.windowWidth,
                                  result.windowHeight,
                                  result.framebufferWidth,
                                  result.framebufferHeight,
                                  result.contentScaleX,
                                  result.contentScaleY);
    result.captured = true;
    return result;
}

EditorWindowFrameResult
EditorWindowFrameService::BeginFrame(
    const EditorWindowFrameDesc& desc) const
{
    if (!desc.window)
    {
        return FailFrame("Editor window is unavailable");
    }

    EditorWindowFrameResult result;
    if (desc.pollEvents)
    {
        glfwPollEvents();
        result.eventsPolled = true;
    }

    if (glfwWindowShouldClose(desc.window))
    {
        result.closeRequested = true;
        if (desc.clearCloseRequest)
        {
            glfwSetWindowShouldClose(desc.window, GLFW_FALSE);
            result.closeRequestCleared = true;
        }
    }

    glfwGetWindowSize(desc.window, &result.windowWidth, &result.windowHeight);
    glfwGetFramebufferSize(desc.window,
                           &result.framebufferWidth,
                           &result.framebufferHeight);
    glfwGetWindowContentScale(desc.window,
                              &result.contentScaleX,
                              &result.contentScaleY);
    result.surfaceScaleFactor =
        ComputeSurfaceScaleFactor(result.windowWidth,
                                  result.windowHeight,
                                  result.framebufferWidth,
                                  result.framebufferHeight,
                                  result.contentScaleX,
                                  result.contentScaleY);
    result.currentTime = glfwGetTime();
    result.processed = true;
    return result;
}

double EditorWindowFrameService::GetTime() const
{
    return glfwGetTime();
}

EditorWindowCursorResult EditorWindowFrameService::ApplyCursorMode(
    const EditorWindowCursorDesc& desc) const
{
    if (!desc.window)
    {
        return FailCursor(desc.mode, "Editor window is unavailable");
    }

    EditorWindowCursorResult result;
    result.requestedMode = desc.mode;
    result.glfwCursorMode = ToGLFWCursorMode(desc.mode);
    glfwSetInputMode(desc.window, GLFW_CURSOR, result.glfwCursorMode);
    result.applied = true;
    return result;
}

EditorWindowMetricsResult
EditorWindowFrameService::FailMetrics(std::string error)
{
    EditorWindowMetricsResult result;
    result.error = std::move(error);
    return result;
}

EditorWindowFrameResult
EditorWindowFrameService::FailFrame(std::string error)
{
    EditorWindowFrameResult result;
    result.error = std::move(error);
    return result;
}

EditorWindowCursorResult
EditorWindowFrameService::FailCursor(EditorWindowCursorMode mode,
                                     std::string error)
{
    EditorWindowCursorResult result;
    result.requestedMode = mode;
    result.glfwCursorMode = ToGLFWCursorMode(mode);
    result.error = std::move(error);
    return result;
}

} // namespace RVX::Editor
