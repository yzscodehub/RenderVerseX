/**
 * @file EditorInputBridgeService.h
 * @brief Editor input bridge lifecycle and frame input service.
 */

#pragma once

#include "Core/Types.h"
#include "UI/UIContext.h"

#include <memory>
#include <string>

struct GLFWwindow;

namespace RVX::Editor
{

class EditorInputBridge;
class IEditorUIBackend;

struct EditorInputBridgeAttachDesc
{
    std::unique_ptr<EditorInputBridge>* bridge = nullptr;
    GLFWwindow* window = nullptr;
};

struct EditorInputBridgeAttachResult
{
    bool attached = false;
    bool bridgeCreated = false;
    bool bridgeAvailable = false;
    std::string error;

    explicit operator bool() const { return attached; }
};

struct EditorInputBridgeShutdownDesc
{
    std::unique_ptr<EditorInputBridge>* bridge = nullptr;
};

struct EditorInputBridgeShutdownResult
{
    bool completed = false;
    bool bridgeDetached = false;
    bool bridgeReleased = false;
    std::string error;

    explicit operator bool() const { return completed; }
};

struct EditorInputBridgeFrameDesc
{
    EditorInputBridge* bridge = nullptr;
    IEditorUIBackend* editorUIBackend = nullptr;
    int32 windowWidth = 0;
    int32 windowHeight = 0;
    int32 framebufferWidth = 0;
    int32 framebufferHeight = 0;
};

struct EditorInputBridgeFrameResult
{
    bool prepared = false;
    bool backendAvailable = false;
    bool bridgeAvailable = false;
    uint32 textByteCount = 0;
    Vec2 mouseCoordinateScale{1.0f, 1.0f};
    Vec2 scrollDelta{0.0f};
    UI::UIInputSnapshot input;
    std::string error;

    explicit operator bool() const { return prepared; }
};

/**
 * @brief Owns native editor input bridge startup, shutdown, and frame input.
 */
class EditorInputBridgeService
{
public:
    // =========================================================================
    // Public Methods
    // =========================================================================

    EditorInputBridgeAttachResult Attach(
        const EditorInputBridgeAttachDesc& desc) const;
    EditorInputBridgeShutdownResult Shutdown(
        const EditorInputBridgeShutdownDesc& desc) const;
    EditorInputBridgeFrameResult PrepareFrameInput(
        const EditorInputBridgeFrameDesc& desc) const;

private:
    static EditorInputBridgeAttachResult FailAttach(std::string error);
    static EditorInputBridgeShutdownResult FailShutdown(std::string error);
    static EditorInputBridgeFrameResult FailFrame(std::string error);
};

} // namespace RVX::Editor
