/**
 * @file EditorBeginFrameService.cpp
 * @brief Editor begin-frame orchestration boundary implementation.
 */

#include "Editor/EditorBeginFrameService.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace RVX::Editor
{
namespace
{
    float SanitizeScale(float scale, float fallback)
    {
        if (!std::isfinite(scale) || scale <= 0.0f)
        {
            scale = fallback;
        }
        return std::max(0.01f, scale);
    }
}

EditorBeginFrameResult EditorBeginFrameService::BeginFrame(
    const EditorBeginFrameDesc& desc) const
{
    if (!desc.windowFrameService)
    {
        return Fail("Editor window frame service is unavailable");
    }

    EditorWindowFrameDesc windowFrameDesc;
    windowFrameDesc.window = desc.window;

    EditorBeginFrameResult result;
    result.windowFrameResult =
        desc.windowFrameService->BeginFrame(windowFrameDesc);
    result.windowFrameProcessed = static_cast<bool>(result.windowFrameResult);
    if (!result.windowFrameProcessed)
    {
        result.error = result.windowFrameResult.error;
        return result;
    }

    result.closeRequested = result.windowFrameResult.closeRequested;
    if (result.closeRequested && desc.handleCloseRequested)
    {
        result.closeRequestHandled = true;
        result.continueAfterCloseRequest = desc.handleCloseRequested();
        if (!result.continueAfterCloseRequest)
        {
            result.completed = true;
            return result;
        }
    }

    result.windowWidth = result.windowFrameResult.windowWidth;
    result.windowHeight = result.windowFrameResult.windowHeight;
    result.scaleFactor =
        SanitizeScale(desc.scaleFactor, 1.0f) *
        SanitizeScale(result.windowFrameResult.surfaceScaleFactor, 1.0f);
    result.deltaTime = static_cast<float>(
        result.windowFrameResult.currentTime - desc.lastFrameTime);
    result.lastFrameTime = result.windowFrameResult.currentTime;
    result.totalTime = desc.totalTime + result.deltaTime;
    result.debugFrameActiveAfterBegin = false;

    if (desc.nativeUIFrameService)
    {
        EditorNativeUIBeginFrameDesc nativeUIBeginDesc;
        nativeUIBeginDesc.backend = desc.editorUIBackend;
        nativeUIBeginDesc.windowFrame = &result.windowFrameResult;
        nativeUIBeginDesc.inputBridgeService = desc.inputBridgeService;
        nativeUIBeginDesc.inputBridge = desc.inputBridge;
        nativeUIBeginDesc.deltaTime = result.deltaTime;
        nativeUIBeginDesc.scaleFactor = result.scaleFactor;
        result.nativeUIBeginFrameResult =
            desc.nativeUIFrameService->BeginFrame(nativeUIBeginDesc);
        result.nativeUIBeginAttempted = true;
    }

    result.completed = true;
    return result;
}

EditorBeginFrameResult EditorBeginFrameService::Fail(std::string error)
{
    EditorBeginFrameResult result;
    result.error = std::move(error);
    return result;
}

} // namespace RVX::Editor
