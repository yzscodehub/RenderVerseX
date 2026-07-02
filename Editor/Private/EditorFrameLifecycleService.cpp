/**
 * @file EditorFrameLifecycleService.cpp
 * @brief Editor end-frame lifecycle orchestration service implementation
 */

#include "Editor/EditorFrameLifecycleService.h"
#include "Editor/EditorFrameSubmissionService.h"
#include "Editor/EditorNativeUIRenderStatsService.h"
#include "Editor/EditorScreenshotRequestService.h"

namespace RVX::Editor
{

EditorFrameLifecycleResult EditorFrameLifecycleService::CompleteFrame(
    const EditorFrameLifecycleDesc& desc) const
{
    EditorFrameLifecycleResult result;

    if (desc.nativeUIRenderStatsService && desc.nativeUIStats)
    {
        desc.nativeUIRenderStatsService->SetDebugImGuiMainFramebufferRendered(
            *desc.nativeUIStats,
            desc.debugImGuiMainFramebufferRendered);
    }

    if (desc.mainFramePresentationService)
    {
        EditorMainFramePrepareDesc prepareDesc;
        prepareDesc.window = desc.window;
        prepareDesc.renderContext = desc.renderContext;
        prepareDesc.mainFramebufferService = desc.mainFramebufferService;
        prepareDesc.mainSwapChainReady = desc.mainSwapChainReady;
        prepareDesc.debugImGuiMainFramebufferRendered =
            desc.debugImGuiMainFramebufferRendered;
        result.prepareResult =
            desc.mainFramePresentationService->PrepareMainFramebuffer(
                prepareDesc);
    }
    if (desc.nativeUIRenderStatsService && desc.nativeUIStats)
    {
        desc.nativeUIRenderStatsService->ApplyMainFramePrepareResult(
            *desc.nativeUIStats,
            result.prepareResult);
    }

    if (desc.frameSubmissionService && desc.frameCoordinator)
    {
        EditorFrameSubmissionDesc submissionDesc;
        submissionDesc.renderContext = desc.renderContext;
        submissionDesc.sceneRenderer = desc.sceneRenderer;
        submissionDesc.runtimeUIRenderer = desc.runtimeUIRenderer;
        submissionDesc.editorUIRenderer = desc.editorUIRenderer;
        submissionDesc.sceneManager = desc.sceneManager;
        submissionDesc.editorUIBackend = desc.editorUIBackend;
        submissionDesc.viewportRenderService = desc.viewportRenderService;
        submissionDesc.nativeUISubmissionService =
            desc.nativeUISubmissionService;
        submissionDesc.nativeUIRenderStatsService =
            desc.nativeUIRenderStatsService;
        submissionDesc.viewportStats = desc.viewportStats;
        submissionDesc.nativeUIStats = desc.nativeUIStats;
        submissionDesc.legacyPanels = desc.legacyPanels;
        submissionDesc.includeLegacyPanels = desc.includeLegacyPanels;
        submissionDesc.hasMainWindowSwapChain = desc.mainSwapChainReady;
        desc.frameSubmissionService->SetFrameDesc(submissionDesc);

        EditorFrameCoordinatorDesc frameDesc;
        frameDesc.renderContext = desc.renderContext;
        frameDesc.client = desc.frameSubmissionService;
        result.frameCoordinatorStats = desc.frameCoordinator->SubmitFrame(
            frameDesc);
        result.frameCoordinatorSubmitted = true;
    }

    if (desc.screenshotRequestService)
    {
        EditorScreenshotRequestDesc screenshotDesc;
        screenshotDesc.runLoopService = desc.runLoopService;
        screenshotDesc.screenshotService = desc.screenshotService;
        screenshotDesc.mainFramebufferService = desc.mainFramebufferService;
        screenshotDesc.window = desc.window;
        screenshotDesc.renderContext = desc.renderContext;
        result.screenshotRequestResult =
            desc.screenshotRequestService
                ->CapturePendingMainFramebufferScreenshot(screenshotDesc);
    }

    if (desc.mainFramePresentationService)
    {
        EditorMainFramePresentDesc presentDesc;
        presentDesc.window = desc.window;
        presentDesc.renderContext = desc.renderContext;
        presentDesc.mainFramebufferService = desc.mainFramebufferService;
        presentDesc.useMainSwapChain = desc.mainSwapChainReady;
        result.presentResult =
            desc.mainFramePresentationService->PresentMainFramebuffer(
                presentDesc);
    }
    if (desc.nativeUIRenderStatsService && desc.nativeUIStats)
    {
        desc.nativeUIRenderStatsService->ApplyMainFramePresentResult(
            *desc.nativeUIStats,
            result.presentResult);
    }

    return result;
}

} // namespace RVX::Editor
