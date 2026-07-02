/**
 * @file EditorWindowLifecycleService.h
 * @brief Editor platform window creation and shutdown service.
 */

#pragma once

#include "Core/Types.h"
#include "RHI/RHIDefinitions.h"

#include <string>

struct GLFWwindow;

namespace RVX::Editor
{

struct EditorWindowCreateDesc
{
    int width = 1920;
    int height = 1080;
    const char* title = "RenderVerseX Editor";
    RHIBackendType backendType = RHIBackendType::None;
    bool maximized = true;
    bool vsync = true;
};

struct EditorWindowCreateResult
{
    bool created = false;
    bool glfwInitialized = false;
    bool windowCreated = false;
    bool openGLContextCurrent = false;
    bool glfwTerminated = false;
    GLFWwindow* window = nullptr;
    int width = 0;
    int height = 0;
    std::string error;

    explicit operator bool() const { return created; }
};

struct EditorWindowDestroyDesc
{
    GLFWwindow** window = nullptr;
    bool terminateGlfw = true;
};

struct EditorWindowDestroyResult
{
    bool completed = false;
    bool windowDestroyed = false;
    bool glfwTerminated = false;
    std::string error;

    explicit operator bool() const { return completed; }
};

/**
 * @brief Owns platform window startup and shutdown details for the editor.
 */
class EditorWindowLifecycleService
{
public:
    // =========================================================================
    // Public Methods
    // =========================================================================

    EditorWindowCreateResult CreateWindow(
        const EditorWindowCreateDesc& desc) const;
    EditorWindowDestroyResult DestroyWindow(
        const EditorWindowDestroyDesc& desc) const;

private:
    static EditorWindowCreateResult FailCreate(std::string error);
    static EditorWindowDestroyResult FailDestroy(std::string error);
};

} // namespace RVX::Editor
