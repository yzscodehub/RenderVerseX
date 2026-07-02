/**
 * @file EditorBeginFrameService.h
 * @brief Editor begin-frame orchestration boundary.
 */

#pragma once

#include "Core/Types.h"
#include "Editor/EditorNativeUIFrameService.h"
#include "Editor/EditorWindowFrameService.h"

#include <functional>
#include <string>

struct GLFWwindow;

namespace RVX::Editor
{

class EditorInputBridge;
class EditorInputBridgeService;
class IEditorUIBackend;

struct EditorBeginFrameDesc
{
    GLFWwindow* window = nullptr;
    IEditorUIBackend* editorUIBackend = nullptr;
    EditorWindowFrameService* windowFrameService = nullptr;
    EditorNativeUIFrameService* nativeUIFrameService = nullptr;
    const EditorInputBridgeService* inputBridgeService = nullptr;
    EditorInputBridge* inputBridge = nullptr;
    std::function<bool()> handleCloseRequested;
    double lastFrameTime = 0.0;
    float totalTime = 0.0f;
    float scaleFactor = 1.0f;
};

struct EditorBeginFrameResult
{
    EditorWindowFrameResult windowFrameResult;
    EditorNativeUIBeginFrameResult nativeUIBeginFrameResult;
    bool completed = false;
    bool windowFrameProcessed = false;
    bool closeRequested = false;
    bool closeRequestHandled = false;
    bool continueAfterCloseRequest = true;
    bool nativeUIBeginAttempted = false;
    bool debugFrameActiveAfterBegin = false;
    int windowWidth = 0;
    int windowHeight = 0;
    float scaleFactor = 1.0f;
    float deltaTime = 0.0f;
    double lastFrameTime = 0.0;
    float totalTime = 0.0f;
    std::string error;

    explicit operator bool() const { return completed; }
};

class EditorBeginFrameService
{
public:
    // =========================================================================
    // Public Methods
    // =========================================================================
    EditorBeginFrameResult BeginFrame(
        const EditorBeginFrameDesc& desc) const;

private:
    static EditorBeginFrameResult Fail(std::string error);
};

} // namespace RVX::Editor
