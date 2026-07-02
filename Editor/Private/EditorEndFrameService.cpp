/**
 * @file EditorEndFrameService.cpp
 * @brief Editor end-frame orchestration boundary implementation.
 */

#include "Editor/EditorEndFrameService.h"

#include "Editor/EditorNativeUIRenderStatsService.h"

namespace RVX::Editor
{

EditorEndFrameResult EditorEndFrameService::EndFrame(
    const EditorEndFrameDesc& desc) const
{
    EditorEndFrameResult result;
    result.completed = true;

    if (desc.nativeUIFrameService)
    {
        EditorNativeUIEndFrameDesc nativeUIEndDesc;
        nativeUIEndDesc.backend = desc.editorUIBackend;
        result.nativeUIEndFrameResult =
            desc.nativeUIFrameService->EndFrame(nativeUIEndDesc);
    }

    if (desc.mainSwapChainService)
    {
        EditorMainSwapChainEnsureDesc swapChainDesc;
        swapChainDesc.window = desc.window;
        swapChainDesc.renderContext = desc.renderContext;
        result.mainSwapChainResult =
            desc.mainSwapChainService->Ensure(swapChainDesc);
    }
    if (desc.nativeUIRenderStatsService && desc.nativeUIStats)
    {
        desc.nativeUIRenderStatsService->ApplyMainSwapChainResult(
            *desc.nativeUIStats,
            result.mainSwapChainResult);
    }

    if (desc.legacyDebugUIService)
    {
        EditorLegacyDebugUIMainFramebufferDesc debugUIDesc;
        debugUIDesc.debugFrameActive = desc.debugFrameActive;
        debugUIDesc.debugImGuiLayer = desc.debugImGuiLayer;
        result.debugUIMainFramebufferResult =
            desc.legacyDebugUIService->RenderMainFramebuffer(debugUIDesc);
    }

    if (desc.frameLifecycleService)
    {
        EditorFrameLifecycleDesc frameLifecycleDesc;
        frameLifecycleDesc.window = desc.window;
        frameLifecycleDesc.renderContext = desc.renderContext;
        frameLifecycleDesc.sceneRenderer = desc.sceneRenderer;
        frameLifecycleDesc.runtimeUIRenderer = desc.runtimeUIRenderer;
        frameLifecycleDesc.editorUIRenderer = desc.editorUIRenderer;
        frameLifecycleDesc.sceneManager = desc.sceneManager;
        frameLifecycleDesc.editorUIBackend = desc.editorUIBackend;
        frameLifecycleDesc.frameCoordinator = desc.frameCoordinator;
        frameLifecycleDesc.frameSubmissionService = desc.frameSubmissionService;
        frameLifecycleDesc.mainFramePresentationService =
            desc.mainFramePresentationService;
        frameLifecycleDesc.mainFramebufferService =
            desc.mainFramebufferService;
        frameLifecycleDesc.nativeUISubmissionService =
            desc.nativeUISubmissionService;
        frameLifecycleDesc.nativeUIRenderStatsService =
            desc.nativeUIRenderStatsService;
        frameLifecycleDesc.viewportRenderService = desc.viewportRenderService;
        frameLifecycleDesc.runLoopService = desc.runLoopService;
        frameLifecycleDesc.screenshotRequestService =
            desc.screenshotRequestService;
        frameLifecycleDesc.screenshotService = desc.screenshotService;
        frameLifecycleDesc.viewportStats = desc.viewportStats;
        frameLifecycleDesc.nativeUIStats = desc.nativeUIStats;
        frameLifecycleDesc.legacyPanels = desc.legacyPanels;
        frameLifecycleDesc.includeLegacyPanels = desc.includeLegacyPanels;
        frameLifecycleDesc.mainSwapChainReady =
            result.mainSwapChainResult.ready;
        frameLifecycleDesc.debugImGuiMainFramebufferRendered =
            result.debugUIMainFramebufferResult.rendered;

        result.frameLifecycleResult =
            desc.frameLifecycleService->CompleteFrame(frameLifecycleDesc);
        result.frameLifecycleCompleted = true;
    }

    return result;
}

} // namespace RVX::Editor
