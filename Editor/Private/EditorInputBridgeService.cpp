/**
 * @file EditorInputBridgeService.cpp
 * @brief Editor input bridge lifecycle and frame input service implementation.
 */

#include "Editor/EditorInputBridgeService.h"

#include "Editor/UI/EditorInputBridge.h"
#include "Editor/UI/IEditorUIBackend.h"

#include <utility>

namespace RVX::Editor
{
namespace
{
    Vec2 ComputeMouseCoordinateScale(const EditorInputBridgeFrameDesc& desc)
    {
        if (desc.windowWidth <= 0 ||
            desc.windowHeight <= 0 ||
            desc.framebufferWidth <= 0 ||
            desc.framebufferHeight <= 0)
        {
            return Vec2(1.0f, 1.0f);
        }

        return Vec2(static_cast<float>(desc.framebufferWidth) /
                        static_cast<float>(desc.windowWidth),
                    static_cast<float>(desc.framebufferHeight) /
                        static_cast<float>(desc.windowHeight));
    }
} // namespace

EditorInputBridgeAttachResult
EditorInputBridgeService::Attach(
    const EditorInputBridgeAttachDesc& desc) const
{
    if (!desc.bridge)
    {
        return FailAttach("Editor input bridge storage is unavailable");
    }
    if (!desc.window)
    {
        return FailAttach("Editor window is unavailable");
    }

    EditorInputBridgeAttachResult result;
    if (!*desc.bridge)
    {
        *desc.bridge = std::make_unique<EditorInputBridge>();
        result.bridgeCreated = true;
    }
    result.bridgeAvailable = true;

    if (!(*desc.bridge)->Attach(desc.window))
    {
        if (result.bridgeCreated)
        {
            desc.bridge->reset();
            result.bridgeAvailable = false;
        }
        result.error = "Failed to attach native editor input bridge";
        return result;
    }

    result.attached = true;
    return result;
}

EditorInputBridgeShutdownResult
EditorInputBridgeService::Shutdown(
    const EditorInputBridgeShutdownDesc& desc) const
{
    if (!desc.bridge)
    {
        return FailShutdown("Editor input bridge storage is unavailable");
    }

    EditorInputBridgeShutdownResult result;
    if (*desc.bridge)
    {
        result.bridgeDetached = (*desc.bridge)->IsAttached();
        (*desc.bridge)->Detach();
        desc.bridge->reset();
        result.bridgeReleased = true;
    }

    result.completed = true;
    return result;
}

EditorInputBridgeFrameResult
EditorInputBridgeService::PrepareFrameInput(
    const EditorInputBridgeFrameDesc& desc) const
{
    if (!desc.editorUIBackend)
    {
        return FailFrame("Editor UI backend is unavailable");
    }

    EditorInputBridgeFrameResult result;
    result.backendAvailable = true;

    if (desc.bridge)
    {
        result.bridgeAvailable = true;
        EditorInputBridgeCaptureState captureState;
        captureState.wantsMouseCapture =
            desc.editorUIBackend->WantsMouseCapture();
        captureState.wantsKeyboardCapture =
            desc.editorUIBackend->WantsKeyboardCapture();

        result.input = desc.bridge->BuildSnapshot(captureState);
        result.mouseCoordinateScale = ComputeMouseCoordinateScale(desc);
        result.input.mousePosition.x *= result.mouseCoordinateScale.x;
        result.input.mousePosition.y *= result.mouseCoordinateScale.y;
        const EditorInputBridgeStats& stats = desc.bridge->GetLastBuildStats();
        result.textByteCount = stats.textByteCount;
        result.scrollDelta = stats.scrollDelta;
    }

    result.prepared = true;
    return result;
}

EditorInputBridgeAttachResult
EditorInputBridgeService::FailAttach(std::string error)
{
    EditorInputBridgeAttachResult result;
    result.error = std::move(error);
    return result;
}

EditorInputBridgeShutdownResult
EditorInputBridgeService::FailShutdown(std::string error)
{
    EditorInputBridgeShutdownResult result;
    result.error = std::move(error);
    return result;
}

EditorInputBridgeFrameResult
EditorInputBridgeService::FailFrame(std::string error)
{
    EditorInputBridgeFrameResult result;
    result.error = std::move(error);
    return result;
}

} // namespace RVX::Editor
