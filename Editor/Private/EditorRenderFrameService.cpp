/**
 * @file EditorRenderFrameService.cpp
 * @brief Editor render-stage orchestration boundary implementation.
 */

#include "Editor/EditorRenderFrameService.h"

#include "Editor/EditorNativeUIRenderStatsService.h"

#include <utility>

namespace RVX::Editor
{

EditorRenderFrameResult EditorRenderFrameService::RenderFrame(
    const EditorRenderFrameDesc& desc) const
{
    if (!desc.nativeUIFrameService)
    {
        return Fail("Editor native UI frame service is unavailable");
    }
    if (!desc.legacyDebugUIService)
    {
        return Fail("Editor legacy debug UI service is unavailable");
    }

    EditorRenderFrameResult result;

    EditorNativeUIPanelBuildDesc panelBuildDesc;
    panelBuildDesc.backend = desc.editorUIBackend;
    panelBuildDesc.automationScenarioService = desc.automationScenarioService;
    panelBuildDesc.documentSession = desc.documentSession;
    panelBuildDesc.settingsService = desc.settingsService;
    panelBuildDesc.shortcutProfileService = desc.shortcutProfileService;
    panelBuildDesc.refreshNativeViewCommands =
        desc.refreshNativeViewCommands;
    panelBuildDesc.refreshDocumentCommands = desc.refreshDocumentCommands;
    result.panelBuildResult =
        desc.nativeUIFrameService->BuildPanels(panelBuildDesc);
    result.panelBuildAttempted = true;
    result.requestStop = result.panelBuildResult.requestStop;
    if (!result.panelBuildResult)
    {
        result.error = "Native editor UI panel build failed";
        return result;
    }

    EditorNativeUIFrameDesc nativeUIFrameDesc;
    nativeUIFrameDesc.backend = desc.editorUIBackend;
    nativeUIFrameDesc.renderer = desc.editorUIRenderer;
    result.nativeUIFrameResult =
        desc.nativeUIFrameService->RecordFrame(nativeUIFrameDesc);
    result.nativeUIRecordAttempted = true;

    if (desc.nativeUIRenderStatsService && desc.nativeUIStats)
    {
        *desc.nativeUIStats =
            desc.nativeUIRenderStatsService->FromFrameResult(
                result.nativeUIFrameResult);
        result.nativeUIStatsApplied = true;
    }

    if (desc.evaluateShellPolicy)
    {
        result.shellDecision = desc.evaluateShellPolicy();
        result.shellPolicyEvaluated = true;
    }

    result.visibleLegacyPanelCount = desc.countVisibleLegacyPanels
                                         ? desc.countVisibleLegacyPanels()
                                         : 0u;

    EditorLegacyDebugUIFrameDesc legacyUIDesc;
    legacyUIDesc.shellDecision =
        result.shellPolicyEvaluated ? &result.shellDecision : nullptr;
    legacyUIDesc.nativeUIRenderStatsService =
        desc.nativeUIRenderStatsService;
    legacyUIDesc.nativeUIStats = desc.nativeUIStats;
    legacyUIDesc.debugImGuiLayer = desc.debugImGuiLayer;
    legacyUIDesc.showDemoWindow = desc.showDemoWindow;
    legacyUIDesc.showMetricsWindow = desc.showMetricsWindow;
    legacyUIDesc.visibleLegacyPanelCount = result.visibleLegacyPanelCount;
    legacyUIDesc.drawDockSpace = desc.drawDockSpace;
    legacyUIDesc.drawMainMenuBar = desc.drawMainMenuBar;
    legacyUIDesc.drawStatusBar = desc.drawStatusBar;
    legacyUIDesc.drawPanels = desc.drawPanels;
    result.legacyUIFrameResult =
        desc.legacyDebugUIService->RenderFrame(legacyUIDesc);
    result.legacyDebugUIFrameAttempted = true;
    result.debugFrameActive = result.legacyUIFrameResult.debugFrameActive;
    if (!result.legacyUIFrameResult)
    {
        result.error = result.legacyUIFrameResult.error;
        return result;
    }

    result.completed = true;
    return result;
}

EditorRenderFrameResult EditorRenderFrameService::Fail(std::string error)
{
    EditorRenderFrameResult result;
    result.error = std::move(error);
    return result;
}

} // namespace RVX::Editor
