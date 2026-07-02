/**
 * @file EditorScreenshotRequestService.h
 * @brief Editor screenshot request orchestration service
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
class EditorRunLoopService;
class EditorScreenshotService;

struct EditorScreenshotRequestDesc
{
    EditorRunLoopService* runLoopService = nullptr;
    EditorScreenshotService* screenshotService = nullptr;
    const EditorMainFramebufferService* mainFramebufferService = nullptr;
    GLFWwindow* window = nullptr;
    RenderContext* renderContext = nullptr;
};

struct EditorScreenshotRequestResult
{
    bool requestConsumed = false;
    bool captureSucceeded = false;
    std::string path;
};

class EditorScreenshotRequestService
{
public:
    // =========================================================================
    // Public Methods
    // =========================================================================
    EditorScreenshotRequestResult CapturePendingMainFramebufferScreenshot(
        const EditorScreenshotRequestDesc& desc) const;
};

} // namespace RVX::Editor
