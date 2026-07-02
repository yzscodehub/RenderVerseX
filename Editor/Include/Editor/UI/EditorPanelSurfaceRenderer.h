/**
 * @file EditorPanelSurfaceRenderer.h
 * @brief Native editor panel chrome and dock area layout renderer
 */

#pragma once

#include "Core/Types.h"
#include "Editor/UI/EditorDockingModel.h"
#include "Editor/UI/EditorUIPanel.h"
#include "UI/UITypes.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace RVX::UI
{
    class Panel;
    class UIContext;
}

namespace RVX::Editor
{

class EditorUIHost;

struct EditorPanelSurfaceRenderStats
{
    uint32 panelFrameCount = 0;
    uint32 visiblePanelCount = 0;
    uint32 closablePanelCount = 0;
    uint32 leftPanelCount = 0;
    uint32 rightPanelCount = 0;
    uint32 bottomPanelCount = 0;
    uint32 centerPanelCount = 0;
    uint32 floatingPanelCount = 0;
    uint32 splitterHandleCount = 0;
    uint32 floatingResizeHandleCount = 0;
    uint32 dockDropPreviewCount = 0;
    uint32 dockTabStackCount = 0;
    uint32 dockTabCount = 0;
    uint32 inactiveDockTabCount = 0;
    uint32 dockTabStackDropPreviewCount = 0;
    uint32 dockTabReorderHandleCount = 0;
    uint32 dockTabAccentCount = 0;
    uint32 panelChromeIconButtonCount = 0;
    uint32 bottomDrawerToggleCount = 0;
    float titleBarHeight = 0.0f;
    float chromeTabHeight = 0.0f;
    float chromeFrameBorderWidth = 0.0f;
    float leftAreaWidth = 0.0f;
    float rightAreaWidth = 0.0f;
    float bottomAreaHeight = 0.0f;
    bool bottomAreaCollapsed = false;
    UI::Rect workArea;
};

struct EditorPanelSurfaceRenderDesc
{
    UI::UIContext* ui = nullptr;
    EditorUIHost* host = nullptr;
    const EditorDockingModel* dockingModel = nullptr;
    const std::unordered_map<std::string, std::string>* panelTitles = nullptr;
    float topReservedHeight = 0.0f;
    float bottomReservedHeight = 0.0f;
};

struct EditorPanelSurfaceFrame
{
    UI::Panel* panelContainer = nullptr;
    UI::Panel* contentContainer = nullptr;
    UI::Rect panelBounds;
    UI::Rect contentBounds;
    bool built = false;
};

struct EditorPanelSurfaceTabStackDropTarget
{
    std::string stackId;
    EditorUIPanelDockArea area = EditorUIPanelDockArea::Floating;
    UI::Rect bounds;
};

struct EditorPanelSurfaceDockTabTarget
{
    std::string stackId;
    std::string panelId;
    UI::Rect bounds;
};

class EditorPanelSurfaceRenderer
{
public:
    void Begin(const EditorPanelSurfaceRenderDesc& desc);
    EditorPanelSurfaceFrame BuildPanelFrame(const EditorUIPanelDesc& panelDesc);
    void End();
    void Clear(UI::UIContext& ui);

    const EditorPanelSurfaceRenderStats& GetLastBuildStats() const
    {
        return m_lastBuildStats;
    }

private:
    struct AreaLayout
    {
        UI::Rect bounds;
        uint32 count = 0;
    };

    void CountVisiblePlacements();
    void ConfigureAreaLayouts();
    void BuildPanelRects();
    void BuildLinearAreaPanelRects(EditorUIPanelDockArea area,
                                   const AreaLayout& layout,
                                   bool horizontal);
    void BuildFloatingPanelRects();
    void AddSplitterHandles();
    void AddDockDropPreviewOverlay();

    EditorPanelSurfaceRenderDesc m_desc;
    EditorPanelSurfaceRenderStats m_lastBuildStats;
    UI::Panel* m_rootPanel = nullptr;
    AreaLayout m_leftArea;
    AreaLayout m_rightArea;
    AreaLayout m_bottomArea;
    AreaLayout m_centerArea;
    AreaLayout m_floatingArea;
    std::unordered_map<std::string, UI::Rect> m_panelRects;
    std::vector<EditorPanelSurfaceTabStackDropTarget> m_tabStackDropTargets;
    std::vector<EditorPanelSurfaceDockTabTarget> m_dockTabTargets;
};

} // namespace RVX::Editor
