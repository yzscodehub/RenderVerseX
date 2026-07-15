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
#elif defined(__APPLE__)
#define GLFW_EXPOSE_NATIVE_COCOA
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

NativeSurfaceDesc EditorMainSwapChainService::CaptureSurface(
    GLFWwindow* window,
    RHIBackendType backend,
    RHIFormat preferredFormat,
    bool vsync) const
{
    NativeSurfaceDesc surface;
    if (!window)
    {
        return surface;
    }

    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
    float xScale = 1.0f;
    float yScale = 1.0f;
    glfwGetWindowContentScale(window, &xScale, &yScale);

#ifdef _WIN32
    surface.platform = NativeSurfacePlatform::Win32;
    surface.nativeWindow =
        reinterpret_cast<uintptr_t>(glfwGetWin32Window(window));
#elif defined(__APPLE__)
    surface.platform = NativeSurfacePlatform::Cocoa;
    surface.nativeWindow =
        reinterpret_cast<uintptr_t>(glfwGetCocoaWindow(window));
#else
    surface.platform = NativeSurfacePlatform::GLFW;
#endif
    surface.backendWindow = reinterpret_cast<uintptr_t>(window);
    surface.width = framebufferWidth > 0
        ? static_cast<uint32>(framebufferWidth)
        : 0;
    surface.height = framebufferHeight > 0
        ? static_cast<uint32>(framebufferHeight)
        : 0;
    surface.contentScale = xScale;
    surface.preferredFormat = preferredFormat;
    surface.vsync = vsync;
    surface.generation = 1;

    if (!surface.IsValidFor(backend))
    {
        return surface;
    }
    return surface;
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

    const RHIBackendType backend =
        desc.renderContext->GetDevice()->GetBackendType();
    result.surface = CaptureSurface(desc.window,
                                    backend,
                                    RHIFormat::BGRA8_UNORM,
                                    desc.renderContext->GetConfig().vsync);
    if (!result.surface.IsValidFor(backend))
    {
        result.fallbackReason =
            "Main window native handle is unavailable for the active RHI backend";
        return result;
    }

    if (!desc.renderContext->CreateSwapChain(result.surface))
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
