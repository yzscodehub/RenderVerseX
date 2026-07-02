/**
 * @file EditorCommandPalette.h
 * @brief Native editor command palette service
 */

#pragma once

#include "Core/Types.h"
#include "Editor/UI/EditorPickerFilterModel.h"
#include "Editor/UI/EditorPickerListModel.h"
#include "Editor/UI/EditorPickerListView.h"
#include "UI/UITypes.h"

#include <string>
#include <vector>

namespace RVX::UI
{
    class UIContext;
}

namespace RVX::Editor
{

class EditorPopupLayer;
class EditorUIHost;

struct EditorCommandPaletteStats
{
    bool open = false;
    uint32 commandCount = 0;
    uint32 resultCount = 0;
    uint32 executableItemCount = 0;
    uint32 shortcutMetadataItemCount = 0;
    uint32 disabledReasonItemCount = 0;
    int32 selectedItemIndex = -1;
    UI::Rect bounds;
    EditorPickerFilterStats filterStats;
    EditorPickerListViewStats pickerStats;
};

/**
 * @brief Builds a searchable native command palette through the popup layer.
 */
class EditorCommandPalette
{
public:
    // =========================================================================
    // Lifecycle
    // =========================================================================
    void Open(std::string initialFilter = {});
    void Close();
    void Clear();

    // =========================================================================
    // State
    // =========================================================================
    bool IsOpen() const { return m_open; }
    const std::string& GetFilterText() const { return m_filterText; }
    std::string GetRootWidgetName() const;
    std::string GetFocusWidgetName() const;
    bool WantsKeyboardFocus() const { return m_open; }

    // =========================================================================
    // Build
    // =========================================================================
    void Build(UI::UIContext& ui, EditorPopupLayer& popupLayer, EditorUIHost& host);
    bool HandleKeyDown(uint32 keyCode, EditorUIHost& host);

    // =========================================================================
    // Stats
    // =========================================================================
    const EditorCommandPaletteStats& GetLastBuildStats() const
    {
        return m_lastBuildStats;
    }

private:
    void RebuildCommandItems(const EditorUIHost& host);
    bool ExecuteSelectedCommand(EditorUIHost& host);
    bool IsVisibleCommandExecutable(int32 visibleIndex, const EditorUIHost& host) const;

    std::string m_filterText;
    std::vector<std::string> m_visibleCommandIds;
    EditorPickerFilterModel m_filterModel;
    EditorPickerListModel m_listModel;
    EditorPickerListView m_listView;
    EditorCommandPaletteStats m_lastBuildStats;
    bool m_open = false;
};

} // namespace RVX::Editor
