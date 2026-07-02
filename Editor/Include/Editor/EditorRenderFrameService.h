/**
 * @file EditorRenderFrameService.h
 * @brief Editor render-stage orchestration boundary.
 */

#pragma once

#include "Core/Types.h"
#include "Editor/EditorLegacyDebugUIService.h"
#include "Editor/EditorNativeUIFrameService.h"
#include "Editor/UI/EditorShellPolicy.h"

#include <functional>
#include <string>

namespace RVX::UI
{
class UIRenderer;
} // namespace RVX::UI

namespace RVX::Editor
{

class DebugImGuiLayer;
class EditorAutomationScenarioService;
class EditorDocumentSession;
class EditorLegacyDebugUIService;
class EditorNativeUIFrameService;
class EditorNativeUIRenderStatsService;
class EditorSettingsService;
class EditorShortcutProfileService;
class IEditorUIBackend;
struct EditorNativeUIRenderStats;

struct EditorRenderFrameDesc
{
    IEditorUIBackend* editorUIBackend = nullptr;
    UI::UIRenderer* editorUIRenderer = nullptr;
    EditorNativeUIFrameService* nativeUIFrameService = nullptr;
    EditorNativeUIRenderStatsService* nativeUIRenderStatsService = nullptr;
    EditorNativeUIRenderStats* nativeUIStats = nullptr;
    EditorLegacyDebugUIService* legacyDebugUIService = nullptr;
    EditorAutomationScenarioService* automationScenarioService = nullptr;
    EditorDocumentSession* documentSession = nullptr;
    EditorSettingsService* settingsService = nullptr;
    EditorShortcutProfileService* shortcutProfileService = nullptr;
    DebugImGuiLayer* debugImGuiLayer = nullptr;
    bool* showDemoWindow = nullptr;
    bool* showMetricsWindow = nullptr;
    std::function<void()> refreshNativeViewCommands;
    std::function<void()> refreshDocumentCommands;
    std::function<EditorShellPolicyDecision()> evaluateShellPolicy;
    std::function<uint32()> countVisibleLegacyPanels;
    std::function<void()> drawDockSpace;
    std::function<void()> drawMainMenuBar;
    std::function<void()> drawStatusBar;
    std::function<void()> drawPanels;
};

struct EditorRenderFrameResult
{
    bool completed = false;
    bool panelBuildAttempted = false;
    bool nativeUIRecordAttempted = false;
    bool nativeUIStatsApplied = false;
    bool shellPolicyEvaluated = false;
    bool legacyDebugUIFrameAttempted = false;
    bool requestStop = false;
    bool debugFrameActive = false;
    uint32 visibleLegacyPanelCount = 0;
    EditorNativeUIPanelBuildResult panelBuildResult;
    EditorNativeUIFrameResult nativeUIFrameResult;
    EditorShellPolicyDecision shellDecision;
    EditorLegacyDebugUIFrameResult legacyUIFrameResult;
    std::string error;

    explicit operator bool() const { return completed; }
};

class EditorRenderFrameService
{
public:
    // =========================================================================
    // Public Methods
    // =========================================================================

    EditorRenderFrameResult RenderFrame(
        const EditorRenderFrameDesc& desc) const;

private:
    static EditorRenderFrameResult Fail(std::string error);
};

} // namespace RVX::Editor
