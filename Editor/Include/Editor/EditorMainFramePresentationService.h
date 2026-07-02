/**
 * @file EditorMainFramePresentationService.h
 * @brief Editor main-frame presentation policy helper
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

class EditorMainFramebufferService;

struct EditorMainFramePrepareDesc
{
    GLFWwindow* window = nullptr;
    RenderContext* renderContext = nullptr;
    const EditorMainFramebufferService* mainFramebufferService = nullptr;
    bool mainSwapChainReady = false;
    bool debugImGuiMainFramebufferRendered = false;
};

struct EditorMainFramePrepareResult
{
    bool defaultFramebufferClearAttempted = false;
    bool defaultFramebufferClearedWithoutImGui = false;
    bool defaultFramebufferClearSkipped = false;
    std::string fallbackReason;
};

struct EditorMainFramePresentDesc
{
    GLFWwindow* window = nullptr;
    RenderContext* renderContext = nullptr;
    const EditorMainFramebufferService* mainFramebufferService = nullptr;
    bool useMainSwapChain = false;
};

struct EditorMainFramePresentResult
{
    bool attempted = false;
    bool presented = false;
    bool mainSwapChainPresented = false;
    bool glfwSwapBuffersUsed = false;
    std::string fallbackReason;
};

class EditorMainFramePresentationService
{
public:
    // =========================================================================
    // Public Methods
    // =========================================================================
    EditorMainFramePrepareResult PrepareMainFramebuffer(
        const EditorMainFramePrepareDesc& desc) const;

    EditorMainFramePresentResult PresentMainFramebuffer(
        const EditorMainFramePresentDesc& desc) const;
};

} // namespace RVX::Editor
