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
    void OnNativeInput(const UI::UIInputState& input) override;

    // =========================================================================
    // Navigation
    // =========================================================================

    void NavigateTo(const std::filesystem::path& path);
    void NavigateUp();
    void Refresh();

    const std::filesystem::path& GetCurrentPath() const { return m_currentPath; }
    Tools::AssetGUID ResolveAssetGuid(const std::filesystem::path& path) const;
    bool SelectAssetPath(const std::filesystem::path& path);

    static std::string MakeAssetDatabasePath(const std::filesystem::path& path,
                                             const std::filesystem::path& rootPath);
    static const char* GetAssetGuidDragDropPayloadType();

private:
    struct DirectoryRowHit
    {
        std::filesystem::path path;
        UI::Rect bounds;
        UI::Rect toggleBounds;
        bool hasToggle = false;
    };

    struct AssetItemHit
    {
        std::filesystem::path path;
        UI::Rect bounds;
        bool isDirectory = false;
    };

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
    void HandleNativeAssetClick(const std::filesystem::path& path,
                                bool isDirectory,
                                bool doubleClick);
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
    std::vector<DirectoryRowHit> m_directoryRowHits;
    std::vector<AssetItemHit> m_assetItemHits;
    bool m_needsRefresh = true;
};

} // namespace RVX::Editor
