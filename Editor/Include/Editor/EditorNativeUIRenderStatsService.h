/**
 * @file EditorNativeUIRenderStatsService.h
 * @brief Native editor UI render-stat mapping helper
 */

#pragma once

#include "Core/Types.h"
#include "Editor/EditorMainFramePresentationService.h"
#include "Editor/EditorMainSwapChainService.h"
#include "Editor/EditorNativeUIFrameService.h"
#include "Editor/EditorNativeUISubmissionService.h"
#include "Editor/UI/EditorShellPolicy.h"

#include <string>

namespace RVX::Editor
{

struct EditorNativeUIRenderStats
{
    bool hostReady = false;
    bool rendererReady = false;
    bool frameRecorded = false;
    bool submitAttempted = false;
    bool submitTargetReady = false;
    bool pipelineReady = false;
    bool submitted = false;
    bool defaultFramebufferTarget = false;
    bool shellPolicyEvaluated = false;
    bool nativeCommandSurfacesOwnMainShell = false;
    bool legacyDockSpaceRequested = false;
    bool legacyMainMenuRequested = false;
    bool legacyStatusBarRequested = false;
    bool debugImGuiFrameRequested = false;
    bool legacyDockSpaceDrawn = false;
    bool legacyMainMenuDrawn = false;
    bool legacyStatusBarDrawn = false;
    bool debugImGuiFrameBegun = false;
    bool debugImGuiMainFramebufferRendered = false;
    bool defaultFramebufferClearedWithoutImGui = false;
    bool defaultFramebufferClearSkipped = false;
    bool mainSwapChainReady = false;
    bool mainSwapChainPresented = false;
    bool submittedToMainSwapChain = false;
    bool submittedInEditorRHIFrame = false;
    uint32 visibleLegacyPanelCount = 0;
    uint32 commandCount = 0;
    uint32 rectCount = 0;
    uint32 textCount = 0;
    uint32 imageCount = 0;
    uint32 vertexCount = 0;
    uint32 indexCount = 0;
    uint32 drawCallCount = 0;
    uint32 menuCount = 0;
    uint32 toolbarCount = 0;
    uint32 statusItemCount = 0;
    uint32 executableItemCount = 0;
    float menuBarHeight = 0.0f;
    float toolbarHeight = 0.0f;
    float statusBarHeight = 0.0f;
    float dockspaceTopReservedHeight = 0.0f;
    float dockspaceBottomReservedHeight = 0.0f;
    uint32 mainSwapChainWidth = 0;
    uint32 mainSwapChainHeight = 0;
    uint64 frameCount = 0;
    std::string mainFramebufferFallbackReason;
    std::string submitFallbackReason;
};

class EditorNativeUIRenderStatsService
{
public:
    // =========================================================================
    // Frame Stats
    // =========================================================================
    EditorNativeUIRenderStats FromFrameResult(
        const EditorNativeUIFrameResult& result) const;

    void SetVisibleLegacyPanelCount(EditorNativeUIRenderStats& stats,
                                    uint32 count) const;

    void ApplyShellPolicyDecision(
        EditorNativeUIRenderStats& stats,
        const EditorShellPolicyDecision& decision) const;

    void MarkDebugImGuiFrameBegun(EditorNativeUIRenderStats& stats) const;
    void MarkLegacyDockSpaceDrawn(EditorNativeUIRenderStats& stats) const;
    void MarkLegacyMainMenuDrawn(EditorNativeUIRenderStats& stats) const;
    void MarkLegacyStatusBarDrawn(EditorNativeUIRenderStats& stats) const;
    void MarkNativeOnlyDefaultFramebufferClear(
        EditorNativeUIRenderStats& stats) const;

    // =========================================================================
    // Main-Frame Stats
    // =========================================================================
    void ApplyMainSwapChainResult(
        EditorNativeUIRenderStats& stats,
        const EditorMainSwapChainEnsureResult& result) const;

    void SetDebugImGuiMainFramebufferRendered(
        EditorNativeUIRenderStats& stats,
        bool rendered) const;

    void ApplyMainFramePrepareResult(
        EditorNativeUIRenderStats& stats,
        const EditorMainFramePrepareResult& result) const;

    void ApplyMainFramePresentResult(
        EditorNativeUIRenderStats& stats,
        const EditorMainFramePresentResult& result) const;

    // =========================================================================
    // Submission Stats
    // =========================================================================
    void ApplySubmissionResult(
        EditorNativeUIRenderStats& stats,
        const EditorNativeUISubmissionResult& result) const;

    void MarkGraphicsContextUnavailable(EditorNativeUIRenderStats& stats,
                                        const std::string& reason) const;
};

} // namespace RVX::Editor
