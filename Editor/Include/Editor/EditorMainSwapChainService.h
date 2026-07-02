/**
 * @file EditorMainSwapChainService.h
 * @brief Editor main-window swap-chain lifecycle helper
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

struct EditorMainSwapChainEnsureDesc
{
    GLFWwindow* window = nullptr;
    RenderContext* renderContext = nullptr;
};

struct EditorMainSwapChainEnsureResult
{
    bool ready = false;
    bool created = false;
    bool resized = false;
    uint32 width = 0;
    uint32 height = 0;
    void* windowHandle = nullptr;
    std::string fallbackReason;
};

class EditorMainSwapChainService
{
public:
    // =========================================================================
    // Public Methods
    // =========================================================================
    bool HasSwapChain(RenderContext* renderContext) const;

    void* ResolveWindowHandle(GLFWwindow* window,
                              RenderContext* renderContext) const;

    EditorMainSwapChainEnsureResult Ensure(
        const EditorMainSwapChainEnsureDesc& desc) const;
};

} // namespace RVX::Editor
