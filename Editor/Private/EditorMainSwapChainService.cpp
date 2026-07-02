/**
 * @file EditorMainSwapChainService.cpp
 * @brief Editor main-window swap-chain lifecycle helper implementation
 */

#include "Editor/EditorMainSwapChainService.h"
#include "Render/Context/RenderContext.h"
#include "RHI/RHIDevice.h"

#include <GLFW/glfw3.h>
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

namespace RVX::Editor
{

bool EditorMainSwapChainService::HasSwapChain(RenderContext* renderContext) const
{
    return renderContext &&
           renderContext->IsInitialized() &&
           renderContext->HasSwapChain();
}

void* EditorMainSwapChainService::ResolveWindowHandle(
    GLFWwindow* window,
    RenderContext* renderContext) const
{
    if (!window ||
        !renderContext ||
        !renderContext->IsInitialized() ||
        !renderContext->GetDevice())
    {
        return nullptr;
    }

    const RHIBackendType backend = renderContext->GetDevice()->GetBackendType();
    if (backend == RHIBackendType::OpenGL)
    {
        return window;
    }

#ifdef _WIN32
    if (backend == RHIBackendType::DX11 ||
        backend == RHIBackendType::DX12 ||
        backend == RHIBackendType::Vulkan)
    {
        return glfwGetWin32Window(window);
    }
#endif

    return nullptr;
}

EditorMainSwapChainEnsureResult EditorMainSwapChainService::Ensure(
    const EditorMainSwapChainEnsureDesc& desc) const
{
    EditorMainSwapChainEnsureResult result;
    if (!desc.window ||
        !desc.renderContext ||
        !desc.renderContext->IsInitialized() ||
        !desc.renderContext->GetDevice())
    {
        result.fallbackReason = "Main window or RenderContext unavailable";
        return result;
    }

    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetFramebufferSize(desc.window, &framebufferWidth, &framebufferHeight);
    if (framebufferWidth <= 0 || framebufferHeight <= 0)
    {
        result.fallbackReason =
            "Main window framebuffer has invalid dimensions";
        return result;
    }

    result.width = static_cast<uint32>(framebufferWidth);
    result.height = static_cast<uint32>(framebufferHeight);
    if (desc.renderContext->HasSwapChain())
    {
        if (RHISwapChain* swapChain = desc.renderContext->GetSwapChain())
        {
            if (swapChain->GetWidth() != result.width ||
                swapChain->GetHeight() != result.height)
            {
                desc.renderContext->ResizeSwapChain(result.width,
                                                    result.height);
                result.resized = true;
            }
        }
        result.ready = true;
        return result;
    }

    result.windowHandle =
        ResolveWindowHandle(desc.window, desc.renderContext);
    if (!result.windowHandle)
    {
        result.fallbackReason =
            "Main window native handle is unavailable for the active RHI backend";
        return result;
    }

    if (!desc.renderContext->CreateSwapChain(result.windowHandle,
                                             result.width,
                                             result.height))
    {
        result.fallbackReason =
            "RenderContext failed to create the Editor main-window swap chain";
        return result;
    }

    result.created = true;
    result.ready = true;
    return result;
}

} // namespace RVX::Editor
