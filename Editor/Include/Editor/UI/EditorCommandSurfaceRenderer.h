/**
 * @file EditorCommandSurfaceRenderer.h
 * @brief Native editor command surface widget builder
 */

#pragma once

#include "Core/Types.h"
#include "Editor/UI/EditorCommandSurfaceModel.h"

#include <string>
#include <vector>

namespace RVX::UI
{
    class UIContext;
}

namespace RVX::Editor
{

class EditorUIHost;
class EditorPopupLayer;

struct EditorCommandToolbarOverflowEntry
{
    EditorCommandSurfaceItem item;
    std::string label;
    std::string iconName;
    std::string shortcutText;
    bool enabled = false;
};

struct EditorCommandSurfaceRenderStats
{
    uint32 menuCount = 0;
    uint32 menuItemCount = 0;
    uint32 menuShortcutItemCount = 0;
    uint32 menuCheckedItemCount = 0;
    uint32 menuDisabledReasonItemCount = 0;
    uint32 openMenuItemCount = 0;
    uint32 dropdownCount = 0;
    uint32 toolbarCount = 0;
    uint32 toolbarItemCount = 0;
    uint32 toolbarIconItemCount = 0;
    uint32 toolbarVisibleIconItemCount = 0;
    uint32 toolbarResolvedIconItemCount = 0;
    uint32 toolbarKnownIconItemCount = 0;
    uint32 toolbarUnknownIconItemCount = 0;
    uint32 toolbarEmptyIconItemCount = 0;
    uint32 toolbarTextFallbackRiskCount = 0;
    uint32 toolbarOverflowItemCount = 0;
    uint32 toolbarOverflowIndicatorCount = 0;
    uint32 toolbarOverflowIndicatorClippedCount = 0;
    uint32 toolbarTitleHiddenCount = 0;
    uint32 toolbarSeparatorCollapsedCount = 0;
    uint32 toolbarOverflowMenuItemCount = 0;
    uint32 toolbarOverflowMenuIconCount = 0;
    uint32 toolbarOverflowMenuTextFallbackRiskCount = 0;
    uint32 toolbarOverflowMenuVisibleItemCount = 0;
    uint32 toolbarOverflowMenuMaxVisibleItemCount = 0;
    uint32 toolbarOverflowMenuScrollOffsetRows = 0;
    int32 toolbarOverflowMenuSelectedItemIndex = -1;
    uint32 statusItemCount = 0;
    uint32 statusLeftItemCount = 0;
    uint32 statusCenterItemCount = 0;
    uint32 statusRightItemCount = 0;
    uint32 executableItemCount = 0;
    float menuBarHeight = 0.0f;
    float toolbarHeight = 0.0f;
    float statusBarHeight = 0.0f;
    float topReservedHeight = 0.0f;
    float bottomReservedHeight = 0.0f;
};

struct EditorCommandSurfaceRenderDesc
{
    UI::UIContext* ui = nullptr;
    EditorUIHost* host = nullptr;
    const EditorCommandRegistry* commandRegistry = nullptr;
    const EditorCommandSurfaceModel* surfaceModel = nullptr;
    EditorPopupLayer* popupLayer = nullptr;
    bool buildMenuBar = true;
    bool buildToolbars = true;
    bool buildStatusBar = true;
};

class EditorCommandSurfaceRenderer
{
public:
    bool Build(const EditorCommandSurfaceRenderDesc& desc);
    void Clear(UI::UIContext& ui);

    bool IsMenuOpen(const std::string& menuId) const { return m_openMenuId == menuId; }
    const std::string& GetOpenMenuId() const { return m_openMenuId; }
    bool IsToolbarOverflowOpen() const { return m_toolbarOverflowOpen; }
    std::string GetOpenMenuRootWidgetName() const;
    std::string GetToolbarOverflowRootWidgetName() const;
    int32 GetOpenMenuSelectedItemIndex() const { return m_openMenuSelectedItemIndex; }
    bool OpenMenu(const std::string& menuId,
                  const EditorCommandRegistry& registry,
                  const EditorCommandSurfaceModel& surfaceModel);
    void CloseOpenMenu();
    void CloseToolbarOverflow();

    const EditorCommandSurfaceRenderStats& GetLastBuildStats() const
    {
        return m_lastBuildStats;
    }

private:
    bool HandleOpenMenuKeyDown(uint32 keyCode,
                               EditorUIHost& host,
                               const EditorCommandRegistry& registry,
                               const EditorCommandSurfaceModel& surfaceModel);
    void NormalizeOpenMenuSelection(const EditorMenuSurface& menu,
                                    const EditorCommandRegistry& registry,
                                    const EditorCommandSurfaceModel& surfaceModel);
    void MoveOpenMenuSelection(int32 delta,
                               const EditorMenuSurface& menu,
                               const EditorCommandRegistry& registry,
                               const EditorCommandSurfaceModel& surfaceModel);
    bool MoveOpenMenuSurface(int32 delta,
                             const EditorCommandRegistry& registry,
                             const EditorCommandSurfaceModel& surfaceModel);
    bool ExecuteOpenMenuSelection(EditorUIHost& host,
                                  const EditorMenuSurface& menu,
                                  const EditorCommandRegistry& registry,
                                  const EditorCommandSurfaceModel& surfaceModel);
    bool HandleToolbarOverflowKeyDown(uint32 keyCode, EditorUIHost& host);

    EditorCommandSurfaceRenderStats m_lastBuildStats;
    std::string m_openMenuId;
    int32 m_openMenuSelectedItemIndex = -1;
    bool m_toolbarOverflowOpen = false;
    int32 m_toolbarOverflowSelectedItemIndex = -1;
    uint32 m_toolbarOverflowScrollOffsetRows = 0;
    std::vector<EditorCommandToolbarOverflowEntry> m_toolbarOverflowEntries;
};

} // namespace RVX::Editor
