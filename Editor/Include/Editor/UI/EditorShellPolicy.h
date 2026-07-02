/**
 * @file EditorShellPolicy.h
 * @brief Native editor shell ownership policy
 */

#pragma once

#include "Core/Types.h"

namespace RVX::Editor
{

struct EditorShellPolicyState
{
    bool editorUIBackendReady = false;
    bool nativeFrameRecorded = false;
    uint32 nativeMenuCount = 0;
    uint32 nativeToolbarCount = 0;
    uint32 nativeStatusItemCount = 0;
    float nativeDockspaceTopReservedHeight = 0.0f;
    float nativeDockspaceBottomReservedHeight = 0.0f;
    uint32 visibleLegacyPanelCount = 0;
    bool legacyDemoWindowVisible = false;
    bool legacyMetricsWindowVisible = false;
};

struct EditorShellPolicyDecision
{
    bool useNativeMainShell = false;
    bool drawLegacyDockSpace = false;
    bool drawLegacyMainMenu = false;
    bool drawLegacyStatusBar = false;
    bool runDebugImGuiFrame = false;
    bool showLegacyDemoWindow = false;
    bool showLegacyMetricsWindow = false;
};

class EditorShellPolicy
{
public:
    // =========================================================================
    // Public Methods
    // =========================================================================
    EditorShellPolicyDecision Evaluate(const EditorShellPolicyState& state) const;
    bool ShouldUseNativeMainShell(const EditorShellPolicyState& state) const;
    bool HasVisibleLegacyPanels(const EditorShellPolicyState& state) const;
    bool ShouldDrawLegacyDockSpace(const EditorShellPolicyState& state) const;
    bool ShouldRunDebugImGuiFrame(const EditorShellPolicyState& state) const;
};

} // namespace RVX::Editor
