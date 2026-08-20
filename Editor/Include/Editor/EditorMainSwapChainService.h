/**
 * @file EditorMainSwapChainService.h
 * @brief Editor main-window swap-chain lifecycle helper
 */

#pragma once

#include "Core/Types.h"
#include "HAL/Window/NativeSurface.h"
#include "RHI/RHIDefinitions.h"

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
    HAL::NativeSurfaceDesc surface;
    std::string fallbackReason;
};

class EditorMainSwapChainService
{
public:
    // =========================================================================
    // Public Methods
    // =========================================================================
    bool HasSwapChain(RenderContext* renderContext) const;

    HAL::NativeSurfaceDesc CaptureSurface(GLFWwindow* window,
                                          RHIBackendType backend) const;

    EditorMainSwapChainEnsureResult Ensure(
        const EditorMainSwapChainEnsureDesc& desc) const;
};

} // namespace RVX::Editor
