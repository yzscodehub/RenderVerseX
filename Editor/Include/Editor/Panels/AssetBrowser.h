/**
 * @file AssetBrowser.h
 * @brief Asset browser panel
 */

#pragma once

#include "Editor/Panels/IEditorPanel.h"
#include "Tools/AssetDatabase.h"
#include <filesystem>
#include <vector>
#include <string>

namespace RVX::Editor
{

/**
 * @brief Asset browser panel for viewing and managing assets
 */
class AssetBrowserPanel : public IEditorPanel
{
public:
    AssetBrowserPanel();
    ~AssetBrowserPanel() override = default;

    // =========================================================================
    // IEditorPanel Interface
    // =========================================================================

    const char* GetName() const override { return "Assets"; }
    const char* GetIcon() const override { return "folder"; }
    void OnInit() override;
    void OnGUI() override;

    // =========================================================================
    // Navigation
    // =========================================================================

    void NavigateTo(const std::filesystem::path& path);
    void NavigateUp();
    void Refresh();

    const std::filesystem::path& GetCurrentPath() const { return m_currentPath; }

private:
    void DrawToolbar();
    void DrawBreadcrumbs();
    void DrawDirectoryTree();
    void DrawDirectoryNode(const std::filesystem::path& path, bool isRoot = false);
    void DrawAssetGrid();
    void DrawAssetList();
    void DrawAssetContextMenu(const std::filesystem::path& path);
    void DrawAssetIcon(const std::filesystem::path& path, bool isDirectory);
    void DrawImportDialog();

    void HandleDragDrop(const std::filesystem::path& path);
    bool IsAssetFile(const std::filesystem::path& path) const;
    const char* GetAssetTypeIcon(const std::filesystem::path& path) const;

    // State
    std::filesystem::path m_rootPath;
    std::filesystem::path m_currentPath;
    std::filesystem::path m_selectedPath;
    std::vector<std::filesystem::path> m_directoryHistory;
    int m_historyIndex = -1;

    // Display options
    std::string m_searchFilter;
    float m_thumbnailSize = 96.0f;
    bool m_showOnlyDirty = false;
    bool m_showHiddenFiles = false;
    bool m_useListView = false;
    bool m_showDirectoryTree = true;

    // Cached directory contents
    std::vector<std::filesystem::path> m_cachedEntries;
    bool m_needsRefresh = true;
};

} // namespace RVX::Editor
