/**
 * @file EditorFilePickerDialog.h
 * @brief Native editor file picker dialog service
 */

#pragma once

#include "Core/Types.h"
#include "Editor/UI/EditorFilePickerModel.h"
#include "Editor/UI/EditorPickerFilterModel.h"
#include "Editor/UI/EditorPickerListModel.h"
#include "Editor/UI/EditorPickerListView.h"
#include "UI/UITypes.h"

#include <filesystem>
#include <functional>
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

struct EditorFilePickerDialogResult
{
    bool accepted = false;
    std::filesystem::path path;
    std::string error;
};

using EditorFilePickerDialogResultCallback =
    std::function<void(const EditorFilePickerDialogResult&, EditorUIHost&)>;

struct EditorFilePickerDialogDesc
{
    std::string id = "FilePicker";
    std::string title;
    EditorFilePickerDesc picker;
    std::string acceptButtonText;
    std::string cancelButtonText = "Cancel";
    EditorFilePickerDialogResultCallback onResult;
    float minWidth = 520.0f;
    float maxWidth = 840.0f;
    bool closeOnEscape = true;
    bool focusOnOpen = true;
};

struct EditorFilePickerDialogStats
{
    bool open = false;
    bool canAccept = false;
    bool canGoBackDirectory = false;
    bool canGoForwardDirectory = false;
    uint32 breadcrumbCount = 0;
    uint32 recentDirectoryCount = 0;
    uint32 entryCount = 0;
    uint32 visibleEntryCount = 0;
    uint32 filteredEntryCount = 0;
    uint32 directoryCount = 0;
    uint32 fileCount = 0;
    int32 selectedItemIndex = -1;
    std::filesystem::path currentDirectory;
    std::filesystem::path acceptedPath;
    std::string filterText;
    std::string pathText;
    std::string typedFileName;
    std::string error;
    UI::Rect overlayBounds;
    UI::Rect dialogBounds;
    EditorFilePickerStats modelStats;
    EditorPickerFilterStats filterStats;
    EditorPickerListViewStats listStats;
};

/**
 * @brief Builds a native modal file picker through the editor popup layer.
 */
class EditorFilePickerDialog
{
public:
    // =========================================================================
    // Lifecycle
    // =========================================================================
    void Open(EditorFilePickerDialogDesc desc);
    void Close();
    void Clear();

    // =========================================================================
    // State
    // =========================================================================
    bool IsOpen() const { return m_open; }
    const std::string& GetOpenDialogId() const { return m_desc.id; }
    const std::string& GetFilterText() const { return m_filterText; }
    std::string GetRootWidgetName() const;
    std::string GetFocusWidgetName() const;
    bool WantsKeyboardFocus() const { return m_open && m_desc.focusOnOpen; }

    const EditorFilePickerModel& GetModel() const { return m_model; }

    // =========================================================================
    // Build
    // =========================================================================
    void Build(UI::UIContext& ui, EditorPopupLayer& popupLayer, EditorUIHost& host);
    bool HandleKeyDown(uint32 keyCode, EditorUIHost& host);

    // =========================================================================
    // Actions
    // =========================================================================
    bool Accept(EditorUIHost& host);
    bool Cancel(EditorUIHost& host);

    const EditorFilePickerDialogStats& GetLastBuildStats() const
    {
        return m_lastBuildStats;
    }

private:
    void RebuildEntryItems();
    bool SelectVisibleEntry(uint32 listIndex);
    bool ActivateSelectedEntry(EditorUIHost& host);
    bool GoToRecentDirectory(uint32 recentIndex);
    bool GoToBreadcrumb(uint32 breadcrumbIndex);
    bool GoBackDirectory();
    bool GoForwardDirectory();
    bool GoToParentDirectory();
    bool RefreshDirectory();
    bool SubmitPathText();
    void ResetPathTextFromModel();
    void SetDialogError(std::string error);
    std::string MakeWidgetPrefix() const;
    std::string MakeListWidgetName() const;

    EditorFilePickerDialogDesc m_desc;
    EditorFilePickerModel m_model;
    EditorPickerFilterModel m_filterModel;
    EditorPickerListModel m_listModel;
    EditorPickerListView m_listView;
    EditorFilePickerDialogStats m_lastBuildStats;
    std::vector<uint32> m_visibleEntryIndices;
    std::string m_filterText;
    std::string m_pathText;
    std::string m_error;
    bool m_open = false;
};

} // namespace RVX::Editor
