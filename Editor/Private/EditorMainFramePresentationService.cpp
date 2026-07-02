/**
 * @file EditorMainFramePresentationService.cpp
 * @brief Editor main-frame presentation policy helper implementation
 */

#include "Editor/EditorMainFramePresentationService.h"
#include "Editor/EditorMainFramebufferService.h"

namespace RVX::Editor
{

EditorMainFramePrepareResult
EditorMainFramePresentationService::PrepareMainFramebuffer(
    const EditorMainFramePrepareDesc& desc) const
{
    EditorMainFramePrepareResult result;
    if (desc.debugImGuiMainFramebufferRendered)
    {
        return result;
    }
    if (desc.mainSwapChainReady)
    {
        result.defaultFramebufferClearSkipped = true;
        return result;
    }
    if (!desc.mainFramebufferService)
    {
        result.defaultFramebufferClearAttempted = true;
        result.defaultFramebufferClearSkipped = true;
        result.fallbackReason = "Main framebuffer service unavailable";
        return result;
    }

    EditorMainFramebufferClearDesc clearDesc;
    clearDesc.window = desc.window;
    clearDesc.renderContext = desc.renderContext;
    const EditorMainFramebufferClearResult clearResult =
        desc.mainFramebufferService->ClearDefaultFramebuffer(clearDesc);
    result.defaultFramebufferClearAttempted = clearResult.attempted;
    result.defaultFramebufferClearedWithoutImGui = clearResult.cleared;
    result.defaultFramebufferClearSkipped = !clearResult.cleared;
    result.fallbackReason = clearResult.fallbackReason;
    return result;
}

EditorMainFramePresentResult
EditorMainFramePresentationService::PresentMainFramebuffer(
    const EditorMainFramePresentDesc& desc) const
{
    EditorMainFramePresentResult result;
    result.attempted = true;
    if (!desc.mainFramebufferService)
    {
        result.fallbackReason = "Main framebuffer service unavailable";
        return result;
    }

    EditorMainFramebufferPresentDesc presentDesc;
    presentDesc.window = desc.window;
    presentDesc.renderContext = desc.renderContext;
    presentDesc.useMainSwapChain = desc.useMainSwapChain;
    const EditorMainFramebufferPresentResult presentResult =
        desc.mainFramebufferService->Present(presentDesc);
    result.attempted = presentResult.attempted;
    result.presented = presentResult.presented;
    result.mainSwapChainPresented = presentResult.mainSwapChainPresented;
    result.glfwSwapBuffersUsed = presentResult.glfwSwapBuffersUsed;
    result.fallbackReason = presentResult.fallbackReason;
    return result;
}

} // namespace RVX::Editor
