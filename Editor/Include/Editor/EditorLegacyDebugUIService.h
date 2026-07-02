/**
 * @file EditorLegacyDebugUIService.h
 * @brief Legacy/debug ImGui shell orchestration boundary.
 */

#pragma once

#include "Core/Types.h"

#include <functional>
#include <string>

namespace RVX::Editor
{

class DebugImGuiLayer;
class EditorNativeUIRenderStatsService;
struct EditorNativeUIRenderStats;
struct EditorShellPolicyDecision;

struct EditorLegacyDebugUIFrameDesc
{
    const EditorShellPolicyDecision* shellDecision = nullptr;
    EditorNativeUIRenderStatsService* nativeUIRenderStatsService = nullptr;
    EditorNativeUIRenderStats* nativeUIStats = nullptr;
    DebugImGuiLayer* debugImGuiLayer = nullptr;
    bool* showDemoWindow = nullptr;
    bool* showMetricsWindow = nullptr;
    uint32 visibleLegacyPanelCount = 0;
    std::function<void()> drawDockSpace;
    std::function<void()> drawMainMenuBar;
    std::function<void()> drawStatusBar;
    std::function<void()> drawPanels;
};

struct EditorLegacyDebugUIFrameResult
{
    bool completed = false;
    bool shellPolicyAvailable = false;
    bool debugFrameActive = false;
    bool debugFrameBegun = false;
    bool legacyDockSpaceDrawn = false;
    bool legacyMainMenuDrawn = false;
    bool legacyStatusBarDrawn = false;
    bool legacyDemoWindowDrawn = false;
    bool legacyMetricsWindowDrawn = false;
    bool legacyPanelsDrawn = false;
    bool nativeOnlyDefaultFramebufferClearMarked = false;
    uint32 visibleLegacyPanelCount = 0;
    std::string error;

    explicit operator bool() const { return completed; }
};

struct EditorLegacyDebugUIMainFramebufferDesc
{
    DebugImGuiLayer* debugImGuiLayer = nullptr;
    bool debugFrameActive = false;
};

struct EditorLegacyDebugUIMainFramebufferResult
{
    bool completed = false;
    bool debugFrameActive = false;
    bool debugLayerAvailable = false;
    bool rendered = false;
    std::string error;

    explicit operator bool() const { return completed; }
};

class EditorLegacyDebugUIService
{
public:
    // =========================================================================
    // Public Methods
    // =========================================================================
    EditorLegacyDebugUIFrameResult RenderFrame(
        const EditorLegacyDebugUIFrameDesc& desc) const;

    EditorLegacyDebugUIMainFramebufferResult RenderMainFramebuffer(
        const EditorLegacyDebugUIMainFramebufferDesc& desc) const;
};

} // namespace RVX::Editor
