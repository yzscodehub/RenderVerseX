/**
 * @file EditorMainFramebufferService.h
 * @brief Editor main-framebuffer fallback clear and present helper
 */

#pragma once

#include "Core/Types.h"

#include <string>

struct GLFWwindow;

namespace RVX
{
    class RenderContext;
}

namespace RVX::Editor
{

struct EditorMainFramebufferClearDesc
{
    GLFWwindow* window = nullptr;
    RenderContext* renderContext = nullptr;
};

struct EditorMainFramebufferClearResult
{
    bool attempted = false;
    bool openGLDefaultFramebufferAvailable = false;
    bool cleared = false;
    uint32 width = 0;
    uint32 height = 0;
    std::string fallbackReason;
};

struct EditorMainFramebufferPresentDesc
{
    GLFWwindow* window = nullptr;
    RenderContext* renderContext = nullptr;
    bool useMainSwapChain = false;
};

struct EditorMainFramebufferPresentResult
{
    bool attempted = false;
    bool presented = false;
    bool mainSwapChainPresented = false;
    bool glfwSwapBuffersUsed = false;
    std::string fallbackReason;
};

class EditorMainFramebufferService
{
public:
    // =========================================================================
    // Public Methods
    // =========================================================================
    bool CanUseOpenGLDefaultFramebuffer(RenderContext* renderContext) const;

    EditorMainFramebufferClearResult ClearDefaultFramebuffer(
        const EditorMainFramebufferClearDesc& desc) const;

    EditorMainFramebufferPresentResult Present(
        const EditorMainFramebufferPresentDesc& desc) const;
};

} // namespace RVX::Editor
