/**
 * @file EditorLegacyDebugUIService.cpp
 * @brief Legacy/debug ImGui shell orchestration boundary implementation.
 */

#include "Editor/EditorLegacyDebugUIService.h"

#include "Editor/EditorNativeUIRenderStatsService.h"
#include "Editor/UI/EditorShellPolicy.h"

#if RVX_EDITOR_ENABLE_LEGACY_IMGUI
#include "Editor/UI/DebugImGuiLayer.h"
#include <imgui.h>
#endif

namespace RVX::Editor
{

EditorLegacyDebugUIFrameResult EditorLegacyDebugUIService::RenderFrame(
    const EditorLegacyDebugUIFrameDesc& desc) const
{
    EditorLegacyDebugUIFrameResult result;
    result.completed = true;
    result.shellPolicyAvailable = desc.shellDecision != nullptr;
    result.visibleLegacyPanelCount = desc.visibleLegacyPanelCount;

    if (desc.nativeUIRenderStatsService && desc.nativeUIStats)
    {
        desc.nativeUIRenderStatsService->SetVisibleLegacyPanelCount(
            *desc.nativeUIStats,
            desc.visibleLegacyPanelCount);
        if (desc.shellDecision)
        {
            desc.nativeUIRenderStatsService->ApplyShellPolicyDecision(
                *desc.nativeUIStats,
                *desc.shellDecision);
        }
    }

#if RVX_EDITOR_ENABLE_LEGACY_IMGUI
    const EditorShellPolicyDecision decision =
        desc.shellDecision ? *desc.shellDecision : EditorShellPolicyDecision{};
    if (decision.runDebugImGuiFrame && desc.debugImGuiLayer)
    {
        desc.debugImGuiLayer->BeginFrame();
        result.debugFrameActive = true;
        result.debugFrameBegun = true;
        if (desc.nativeUIRenderStatsService && desc.nativeUIStats)
        {
            desc.nativeUIRenderStatsService->MarkDebugImGuiFrameBegun(
                *desc.nativeUIStats);
        }
    }

    if (decision.drawLegacyDockSpace && result.debugFrameActive &&
        desc.drawDockSpace)
    {
        desc.drawDockSpace();
        result.legacyDockSpaceDrawn = true;
        if (desc.nativeUIRenderStatsService && desc.nativeUIStats)
        {
            desc.nativeUIRenderStatsService->MarkLegacyDockSpaceDrawn(
                *desc.nativeUIStats);
        }
    }
    if (decision.drawLegacyMainMenu && result.debugFrameActive &&
        desc.drawMainMenuBar)
    {
        desc.drawMainMenuBar();
        result.legacyMainMenuDrawn = true;
        if (desc.nativeUIRenderStatsService && desc.nativeUIStats)
        {
            desc.nativeUIRenderStatsService->MarkLegacyMainMenuDrawn(
                *desc.nativeUIStats);
        }
    }
    if (decision.drawLegacyStatusBar && result.debugFrameActive &&
        desc.drawStatusBar)
    {
        desc.drawStatusBar();
        result.legacyStatusBarDrawn = true;
        if (desc.nativeUIRenderStatsService && desc.nativeUIStats)
        {
            desc.nativeUIRenderStatsService->MarkLegacyStatusBarDrawn(
                *desc.nativeUIStats);
        }
    }

    if (result.debugFrameActive && decision.showLegacyDemoWindow &&
        desc.showDemoWindow)
    {
        ImGui::ShowDemoWindow(desc.showDemoWindow);
        result.legacyDemoWindowDrawn = true;
    }
    if (result.debugFrameActive && decision.showLegacyMetricsWindow &&
        desc.showMetricsWindow)
    {
        ImGui::ShowMetricsWindow(desc.showMetricsWindow);
        result.legacyMetricsWindowDrawn = true;
    }
#else
    if (desc.nativeUIRenderStatsService && desc.nativeUIStats)
    {
        desc.nativeUIRenderStatsService->MarkNativeOnlyDefaultFramebufferClear(
            *desc.nativeUIStats);
        result.nativeOnlyDefaultFramebufferClearMarked = true;
    }
#endif

    if (desc.drawPanels)
    {
        desc.drawPanels();
        result.legacyPanelsDrawn = true;
    }

    return result;
}

EditorLegacyDebugUIMainFramebufferResult
EditorLegacyDebugUIService::RenderMainFramebuffer(
    const EditorLegacyDebugUIMainFramebufferDesc& desc) const
{
    EditorLegacyDebugUIMainFramebufferResult result;
    result.completed = true;
    result.debugFrameActive = desc.debugFrameActive;

#if RVX_EDITOR_ENABLE_LEGACY_IMGUI
    result.debugLayerAvailable = desc.debugImGuiLayer != nullptr;
    if (desc.debugFrameActive && desc.debugImGuiLayer)
    {
        desc.debugImGuiLayer->RenderMainFramebuffer();
        result.rendered = true;
    }
#endif

    return result;
}

} // namespace RVX::Editor
