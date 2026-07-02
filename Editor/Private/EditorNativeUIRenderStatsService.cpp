/**
 * @file EditorNativeUIRenderStatsService.cpp
 * @brief Native editor UI render-stat mapping helper implementation
 */

#include "Editor/EditorNativeUIRenderStatsService.h"

namespace RVX::Editor
{

EditorNativeUIRenderStats EditorNativeUIRenderStatsService::FromFrameResult(
    const EditorNativeUIFrameResult& result) const
{
    EditorNativeUIRenderStats stats;
    stats.hostReady = result.hostReady;
    stats.rendererReady = result.rendererReady;
    stats.frameRecorded = result.frameRecorded;
    stats.commandCount = result.commandCount;
    stats.rectCount = result.rectCount;
    stats.textCount = result.textCount;
    stats.imageCount = result.imageCount;
    stats.vertexCount = result.vertexCount;
    stats.indexCount = result.indexCount;
    stats.menuCount = result.menuCount;
    stats.toolbarCount = result.toolbarCount;
    stats.statusItemCount = result.statusItemCount;
    stats.executableItemCount = result.executableItemCount;
    stats.menuBarHeight = result.menuBarHeight;
    stats.toolbarHeight = result.toolbarHeight;
    stats.statusBarHeight = result.statusBarHeight;
    stats.dockspaceTopReservedHeight = result.dockspaceTopReservedHeight;
    stats.dockspaceBottomReservedHeight =
        result.dockspaceBottomReservedHeight;
    stats.frameCount = result.frameCount;
    return stats;
}

void EditorNativeUIRenderStatsService::SetVisibleLegacyPanelCount(
    EditorNativeUIRenderStats& stats,
    uint32 count) const
{
    stats.visibleLegacyPanelCount = count;
}

void EditorNativeUIRenderStatsService::ApplyShellPolicyDecision(
    EditorNativeUIRenderStats& stats,
    const EditorShellPolicyDecision& decision) const
{
    stats.shellPolicyEvaluated = true;
    stats.nativeCommandSurfacesOwnMainShell = decision.useNativeMainShell;
    stats.legacyDockSpaceRequested = decision.drawLegacyDockSpace;
    stats.legacyMainMenuRequested = decision.drawLegacyMainMenu;
    stats.legacyStatusBarRequested = decision.drawLegacyStatusBar;
    stats.debugImGuiFrameRequested = decision.runDebugImGuiFrame;
}

void EditorNativeUIRenderStatsService::MarkDebugImGuiFrameBegun(
    EditorNativeUIRenderStats& stats) const
{
    stats.debugImGuiFrameBegun = true;
}

void EditorNativeUIRenderStatsService::MarkLegacyDockSpaceDrawn(
    EditorNativeUIRenderStats& stats) const
{
    stats.legacyDockSpaceDrawn = true;
}

void EditorNativeUIRenderStatsService::MarkLegacyMainMenuDrawn(
    EditorNativeUIRenderStats& stats) const
{
    stats.legacyMainMenuDrawn = true;
}

void EditorNativeUIRenderStatsService::MarkLegacyStatusBarDrawn(
    EditorNativeUIRenderStats& stats) const
{
    stats.legacyStatusBarDrawn = true;
}

void EditorNativeUIRenderStatsService::MarkNativeOnlyDefaultFramebufferClear(
    EditorNativeUIRenderStats& stats) const
{
    stats.defaultFramebufferClearedWithoutImGui = true;
}

void EditorNativeUIRenderStatsService::ApplyMainSwapChainResult(
    EditorNativeUIRenderStats& stats,
    const EditorMainSwapChainEnsureResult& result) const
{
    stats.mainSwapChainReady = result.ready;
    if (result.ready)
    {
        stats.mainSwapChainWidth = result.width;
        stats.mainSwapChainHeight = result.height;
        stats.mainFramebufferFallbackReason.clear();
        return;
    }
    if (!result.fallbackReason.empty())
    {
        stats.mainFramebufferFallbackReason = result.fallbackReason;
    }
}

void EditorNativeUIRenderStatsService::SetDebugImGuiMainFramebufferRendered(
    EditorNativeUIRenderStats& stats,
    bool rendered) const
{
    stats.debugImGuiMainFramebufferRendered = rendered;
}

void EditorNativeUIRenderStatsService::ApplyMainFramePrepareResult(
    EditorNativeUIRenderStats& stats,
    const EditorMainFramePrepareResult& result) const
{
    stats.defaultFramebufferClearedWithoutImGui =
        result.defaultFramebufferClearedWithoutImGui;
    stats.defaultFramebufferClearSkipped =
        result.defaultFramebufferClearSkipped;
    if (!result.fallbackReason.empty())
    {
        stats.mainFramebufferFallbackReason = result.fallbackReason;
    }
}

void EditorNativeUIRenderStatsService::ApplyMainFramePresentResult(
    EditorNativeUIRenderStats& stats,
    const EditorMainFramePresentResult& result) const
{
    stats.mainSwapChainPresented = result.mainSwapChainPresented;
    if (!result.presented && !result.fallbackReason.empty())
    {
        stats.mainFramebufferFallbackReason = result.fallbackReason;
    }
}

void EditorNativeUIRenderStatsService::ApplySubmissionResult(
    EditorNativeUIRenderStats& stats,
    const EditorNativeUISubmissionResult& result) const
{
    stats.submitAttempted = result.attempted;
    stats.submitTargetReady = result.targetReady;
    stats.pipelineReady = result.pipelineReady;
    stats.submitted = result.submitted;
    stats.defaultFramebufferTarget = result.defaultFramebufferTarget;
    stats.submittedToMainSwapChain = result.submittedToMainSwapChain;
    stats.submittedInEditorRHIFrame = result.submittedInEditorRHIFrame;
    stats.drawCallCount = result.drawCallCount;
    stats.submitFallbackReason = result.fallbackReason;
}

void EditorNativeUIRenderStatsService::MarkGraphicsContextUnavailable(
    EditorNativeUIRenderStats& stats,
    const std::string& reason) const
{
    if (!stats.frameRecorded)
    {
        return;
    }

    stats.submitAttempted = true;
    stats.submitTargetReady = true;
    stats.submitFallbackReason = reason;
}

} // namespace RVX::Editor
