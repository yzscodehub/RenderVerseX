/**
 * @file EditorNativeUIFrameService.h
 * @brief Native editor UI frame recording helper
 */

#pragma once

#include "Core/Types.h"
#include "Editor/EditorInputBridgeService.h"
#include "Editor/EditorWindowFrameService.h"

#include <functional>
#include <string>

namespace RVX::UI
{
    class UIRenderer;
}

namespace RVX::Editor
{

class EditorAutomationScenarioService;
class EditorDocumentSession;
class EditorSettingsService;
class EditorShortcutProfileLifecycleService;
class EditorShortcutProfileService;
class IEditorUIBackend;

struct EditorNativeUIBeginFrameDesc
{
    IEditorUIBackend* backend = nullptr;
    const EditorWindowFrameResult* windowFrame = nullptr;
    const EditorInputBridgeService* inputBridgeService = nullptr;
    EditorInputBridge* inputBridge = nullptr;
    float deltaTime = 0.0f;
    float scaleFactor = 1.0f;
};

struct EditorNativeUIBeginFrameResult
{
    bool completed = false;
    bool backendAvailable = false;
    bool inputPrepared = false;
    bool frameBegun = false;
    uint32 surfaceWidth = 0;
    uint32 surfaceHeight = 0;
    EditorInputBridgeFrameResult inputResult;
    std::string error;

    explicit operator bool() const { return completed; }
};

struct EditorNativeUIUpdateDesc
{
    IEditorUIBackend* backend = nullptr;
    EditorShortcutProfileLifecycleService* shortcutProfileLifecycleService =
        nullptr;
    float deltaTime = 0.0f;
};

struct EditorNativeUIUpdateResult
{
    bool completed = false;
    bool backendAvailable = false;
    bool updated = false;
    bool commandRegistryAvailable = false;
    bool shortcutAutosaveTicked = false;

    explicit operator bool() const { return completed; }
};

struct EditorNativeUIPanelBuildDesc
{
    IEditorUIBackend* backend = nullptr;
    EditorAutomationScenarioService* automationScenarioService = nullptr;
    EditorDocumentSession* documentSession = nullptr;
    EditorSettingsService* settingsService = nullptr;
    EditorShortcutProfileService* shortcutProfileService = nullptr;
    std::function<void()> refreshNativeViewCommands;
    std::function<void()> refreshDocumentCommands;
};

struct EditorNativeUIPanelBuildResult
{
    bool completed = false;
    bool backendAvailable = false;
    bool nativeViewCommandsRefreshed = false;
    bool panelsBuilt = false;
    bool automationScenarioActive = false;
    bool automationAdvanced = false;
    bool automationComplete = false;
    bool automationFailed = false;
    bool requestStop = false;

    explicit operator bool() const { return completed; }
};

struct EditorNativeUIFrameDesc
{
    IEditorUIBackend* backend = nullptr;
    UI::UIRenderer* renderer = nullptr;
};

struct EditorNativeUIFrameResult
{
    bool hostReady = false;
    bool rendererReady = false;
    bool frameRecorded = false;
    uint32 commandCount = 0;
    uint32 rectCount = 0;
    uint32 textCount = 0;
    uint32 imageCount = 0;
    uint32 vertexCount = 0;
    uint32 indexCount = 0;
    uint32 menuCount = 0;
    uint32 toolbarCount = 0;
    uint32 statusItemCount = 0;
    uint32 executableItemCount = 0;
    float menuBarHeight = 0.0f;
    float toolbarHeight = 0.0f;
    float statusBarHeight = 0.0f;
    float dockspaceTopReservedHeight = 0.0f;
    float dockspaceBottomReservedHeight = 0.0f;
    uint64 frameCount = 0;
};

struct EditorNativeUIEndFrameDesc
{
    IEditorUIBackend* backend = nullptr;
};

struct EditorNativeUIEndFrameResult
{
    bool completed = false;
    bool backendAvailable = false;
    bool frameEnded = false;

    explicit operator bool() const { return completed; }
};

class EditorNativeUIFrameService
{
public:
    // =========================================================================
    // Public Methods
    // =========================================================================
    EditorNativeUIBeginFrameResult BeginFrame(
        const EditorNativeUIBeginFrameDesc& desc) const;
    EditorNativeUIUpdateResult Update(
        const EditorNativeUIUpdateDesc& desc) const;
    EditorNativeUIPanelBuildResult BuildPanels(
        const EditorNativeUIPanelBuildDesc& desc) const;
    EditorNativeUIFrameResult RecordFrame(
        const EditorNativeUIFrameDesc& desc) const;
    EditorNativeUIEndFrameResult EndFrame(
        const EditorNativeUIEndFrameDesc& desc) const;
};

} // namespace RVX::Editor
