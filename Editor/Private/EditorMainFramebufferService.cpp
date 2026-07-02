/**
 * @file EditorMainFramebufferService.cpp
 * @brief Editor main-framebuffer helper implementation
 */

#include "Editor/EditorMainFramebufferService.h"
#include "Render/Context/RenderContext.h"
#include "RHI/RHIDevice.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>

namespace RVX::Editor
{

bool EditorMainFramebufferService::CanUseOpenGLDefaultFramebuffer(
    RenderContext* renderContext) const
{
    return renderContext &&
           renderContext->IsInitialized() &&
           renderContext->GetDevice() &&
           renderContext->GetDevice()->GetBackendType() == RHIBackendType::OpenGL;
}

EditorMainFramebufferClearResult
EditorMainFramebufferService::ClearDefaultFramebuffer(
    const EditorMainFramebufferClearDesc& desc) const
{
    EditorMainFramebufferClearResult result;
    result.attempted = true;
    result.openGLDefaultFramebufferAvailable =
        CanUseOpenGLDefaultFramebuffer(desc.renderContext);
    if (!result.openGLDefaultFramebufferAvailable)
    {
        result.fallbackReason =
            "OpenGL main framebuffer is unavailable until the Editor owns an RHI swap chain";
        return result;
    }
    if (!desc.window)
    {
        result.fallbackReason = "Main framebuffer window unavailable";
        return result;
    }

    int displayW = 0;
    int displayH = 0;
    glfwGetFramebufferSize(desc.window, &displayW, &displayH);
    if (displayW <= 0 || displayH <= 0)
    {
        result.fallbackReason = "Main framebuffer has invalid dimensions";
        return result;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, displayW, displayH);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    result.cleared = true;
    result.width = static_cast<uint32>(displayW);
    result.height = static_cast<uint32>(displayH);
    return result;
}

EditorMainFramebufferPresentResult EditorMainFramebufferService::Present(
    const EditorMainFramebufferPresentDesc& desc) const
{
    EditorMainFramebufferPresentResult result;
    result.attempted = true;
    if (desc.useMainSwapChain)
    {
        if (!desc.renderContext)
        {
            result.fallbackReason = "RenderContext unavailable";
            return result;
        }

        desc.renderContext->Present();
        result.presented = true;
        result.mainSwapChainPresented = true;
        return result;
    }

    if (!desc.window)
    {
        result.fallbackReason = "Main framebuffer window unavailable";
        return result;
    }

    glfwSwapBuffers(desc.window);
    result.presented = true;
    result.glfwSwapBuffersUsed = true;
    return result;
}

} // namespace RVX::Editor
