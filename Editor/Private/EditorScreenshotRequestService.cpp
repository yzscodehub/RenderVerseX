/**
 * @file EditorScreenshotRequestService.cpp
 * @brief Editor screenshot request orchestration service implementation
 */

#include "Editor/EditorScreenshotRequestService.h"
#include "Editor/EditorRunLoopService.h"
#include "Editor/EditorScreenshotService.h"

namespace RVX::Editor
{

EditorScreenshotRequestResult
EditorScreenshotRequestService::CapturePendingMainFramebufferScreenshot(
    const EditorScreenshotRequestDesc& desc) const
{
    EditorScreenshotRequestResult result;
    if (!desc.runLoopService)
    {
        return result;
    }

    result.path = desc.runLoopService->ConsumePendingScreenshotPath();
    if (result.path.empty())
    {
        return result;
    }

    result.requestConsumed = true;
    if (desc.screenshotService)
    {
        EditorMainFramebufferScreenshotDesc screenshotDesc;
        screenshotDesc.path = result.path;
        screenshotDesc.window = desc.window;
        screenshotDesc.renderContext = desc.renderContext;
        screenshotDesc.mainFramebufferService = desc.mainFramebufferService;
        result.captureSucceeded =
            desc.screenshotService->CaptureMainFramebuffer(screenshotDesc);
    }

    desc.runLoopService->RecordScreenshotResult(result.captureSucceeded);
    return result;
}

} // namespace RVX::Editor
