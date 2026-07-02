/**
 * @file EditorShellPolicy.cpp
 * @brief Native editor shell ownership policy implementation
 */

#include "Editor/UI/EditorShellPolicy.h"

namespace RVX::Editor
{

EditorShellPolicyDecision EditorShellPolicy::Evaluate(
    const EditorShellPolicyState& state) const
{
    EditorShellPolicyDecision decision;
    decision.useNativeMainShell = ShouldUseNativeMainShell(state);
    decision.drawLegacyDockSpace = ShouldDrawLegacyDockSpace(state);
    decision.drawLegacyMainMenu = !decision.useNativeMainShell;
    decision.drawLegacyStatusBar = !decision.useNativeMainShell;
    decision.showLegacyDemoWindow = state.legacyDemoWindowVisible;
    decision.showLegacyMetricsWindow = state.legacyMetricsWindowVisible;
    decision.runDebugImGuiFrame =
        decision.drawLegacyDockSpace ||
        decision.drawLegacyMainMenu ||
        decision.drawLegacyStatusBar ||
        decision.showLegacyDemoWindow ||
        decision.showLegacyMetricsWindow;
    return decision;
}

bool EditorShellPolicy::ShouldUseNativeMainShell(
    const EditorShellPolicyState& state) const
{
    return state.editorUIBackendReady &&
           state.nativeFrameRecorded &&
           state.nativeMenuCount > 0 &&
           state.nativeToolbarCount > 0 &&
           state.nativeStatusItemCount > 0 &&
           state.nativeDockspaceTopReservedHeight > 0.0f &&
           state.nativeDockspaceBottomReservedHeight > 0.0f;
}

bool EditorShellPolicy::HasVisibleLegacyPanels(
    const EditorShellPolicyState& state) const
{
    return state.visibleLegacyPanelCount > 0;
}

bool EditorShellPolicy::ShouldDrawLegacyDockSpace(
    const EditorShellPolicyState& state) const
{
    if (!ShouldUseNativeMainShell(state))
    {
        return true;
    }

    return HasVisibleLegacyPanels(state) ||
           state.legacyDemoWindowVisible ||
           state.legacyMetricsWindowVisible;
}

bool EditorShellPolicy::ShouldRunDebugImGuiFrame(
    const EditorShellPolicyState& state) const
{
    return ShouldDrawLegacyDockSpace(state);
}

} // namespace RVX::Editor
