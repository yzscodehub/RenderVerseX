/**
 * @file EditorWindowLifecycleService.cpp
 * @brief Editor platform window creation and shutdown service.
 */

#include "Editor/EditorWindowLifecycleService.h"

#include "Core/Log.h"

#include <GLFW/glfw3.h>

#include <utility>

namespace RVX::Editor
{

EditorWindowCreateResult
EditorWindowLifecycleService::CreateWindow(
    const EditorWindowCreateDesc& desc) const
{
    if (desc.width <= 0 || desc.height <= 0)
    {
        return FailCreate("Editor window dimensions are invalid");
    }
    if (!desc.title || desc.title[0] == '\0')
    {
        return FailCreate("Editor window title is unavailable");
    }

    EditorWindowCreateResult result;
    if (!glfwInit())
    {
        result.error = "Failed to initialize GLFW";
        return result;
    }
    result.glfwInitialized = true;

    glfwDefaultWindowHints();
    if (desc.backendType == RHIBackendType::OpenGL)
    {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
    }
    else
    {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    }
    if (desc.maximized)
    {
        glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);
    }

    GLFWwindow* window =
        glfwCreateWindow(desc.width, desc.height, desc.title, nullptr, nullptr);
    if (!window)
    {
        glfwTerminate();
        result.glfwTerminated = true;
        result.error = "Failed to create GLFW window";
        return result;
    }
    result.windowCreated = true;
    result.window = window;

    if (desc.backendType == RHIBackendType::OpenGL)
    {
        glfwMakeContextCurrent(window);
        glfwSwapInterval(desc.vsync ? 1 : 0);
        result.openGLContextCurrent = true;
    }

    glfwGetWindowSize(window, &result.width, &result.height);
    RVX_CORE_INFO("Window created: {}x{}", result.width, result.height);
    result.created = true;
    return result;
}

EditorWindowDestroyResult
EditorWindowLifecycleService::DestroyWindow(
    const EditorWindowDestroyDesc& desc) const
{
    if (!desc.window)
    {
        return FailDestroy("Editor window storage is unavailable");
    }

    EditorWindowDestroyResult result;
    if (*desc.window)
    {
        glfwDestroyWindow(*desc.window);
        *desc.window = nullptr;
        result.windowDestroyed = true;
    }
    if (desc.terminateGlfw)
    {
        glfwTerminate();
        result.glfwTerminated = true;
    }

    result.completed = true;
    return result;
}

EditorWindowCreateResult
EditorWindowLifecycleService::FailCreate(std::string error)
{
    EditorWindowCreateResult result;
    result.error = std::move(error);
    return result;
}

EditorWindowDestroyResult
EditorWindowLifecycleService::FailDestroy(std::string error)
{
    EditorWindowDestroyResult result;
    result.error = std::move(error);
    return result;
}

} // namespace RVX::Editor
