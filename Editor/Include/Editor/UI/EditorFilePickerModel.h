/**
 * @file EditorFilePickerModel.h
 * @brief Reusable native editor file picker filesystem model
 */

#pragma once

#include "Core/Types.h"

#include <filesystem>
#include <string>
#include <vector>

namespace RVX::Editor
{

enum class EditorFilePickerMode : uint8
{
    OpenFile = 0,
    SaveFile,
};

struct EditorFilePickerFilter
{
    std::string displayName;
    std::vector<std::string> patterns;
};

struct EditorFilePickerDesc
{
    EditorFilePickerMode mode = EditorFilePickerMode::OpenFile;
    std::filesystem::path initialDirectory;
    std::filesystem::path initialPath;
    std::string defaultFileName;
    std::string defaultExtension;
    std::vector<EditorFilePickerFilter> filters;
    std::vector<std::filesystem::path> recentDirectories;
    uint32 activeFilterIndex = 0;
    uint32 maxRecentDirectories = 6;
    bool showDirectories = true;
    bool showFiles = true;
    bool allowCreateFileInSaveMode = true;
};

struct EditorFilePickerEntry
{
    std::filesystem::path path;
    std::string displayName;
    bool directory = false;
    bool file = false;
    bool matchesActiveFilter = false;
    uint64 sizeBytes = 0;
};

struct EditorFilePickerBreadcrumb
{
    std::filesystem::path path;
    std::string displayName;
};

struct EditorFilePickerStats
{
    bool directoryLoaded = false;
    bool canGoBackDirectory = false;
    bool canGoForwardDirectory = false;
    uint32 entryCount = 0;
    uint32 directoryCount = 0;
    uint32 fileCount = 0;
    uint32 visibleEntryCount = 0;
    uint32 visibleFileCount = 0;
    uint32 hiddenFileCount = 0;
    uint32 breadcrumbCount = 0;
    uint32 recentDirectoryCount = 0;
    uint32 activeFilterIndex = 0;
    std::filesystem::path currentDirectory;
    std::filesystem::path selectedPath;
    std::string error;
};

class EditorFilePickerModel
{
public:
    // =========================================================================
    // Session
    // =========================================================================
    bool Open(EditorFilePickerDesc desc);
    bool Refresh();

    const EditorFilePickerDesc& GetDesc() const { return m_desc; }
    const EditorFilePickerStats& GetLastBuildStats() const { return m_stats; }
    const std::string& GetLastError() const { return m_stats.error; }

    // =========================================================================
    // Directory
    // =========================================================================
    const std::filesystem::path& GetCurrentDirectory() const
    {
        return m_currentDirectory;
    }

    bool SetCurrentDirectory(const std::filesystem::path& directory);
    bool EnterDirectory(const std::filesystem::path& directory);
    bool EnterSelectedDirectory();
    bool GoToParentDirectory();
    bool GoBackDirectory();
    bool GoForwardDirectory();
    bool EnterRecentDirectory(uint32 recentIndex);
    bool EnterBreadcrumb(uint32 breadcrumbIndex);
    bool CanGoBackDirectory() const { return !m_backDirectoryStack.empty(); }
    bool CanGoForwardDirectory() const { return !m_forwardDirectoryStack.empty(); }
    const std::vector<std::filesystem::path>& GetRecentDirectories() const
    {
        return m_recentDirectories;
    }
    const std::vector<EditorFilePickerBreadcrumb>& GetBreadcrumbs() const
    {
        return m_breadcrumbs;
    }

    // =========================================================================
    // Filter
    // =========================================================================
    bool SetActiveFilterIndex(uint32 index);
    uint32 GetActiveFilterIndex() const { return m_activeFilterIndex; }

    // =========================================================================
    // Selection
    // =========================================================================
    const std::vector<EditorFilePickerEntry>& GetEntries() const
    {
        return m_entries;
    }

    const std::vector<EditorFilePickerEntry>& GetVisibleEntries() const
    {
        return m_visibleEntries;
    }

    bool SelectEntry(uint32 visibleIndex);
    void ClearSelection();

    const std::filesystem::path& GetSelectedPath() const { return m_selectedPath; }

    // =========================================================================
    // Filename
    // =========================================================================
    bool SetTypedFileName(std::string fileName);
    const std::string& GetTypedFileName() const { return m_typedFileName; }

    std::filesystem::path ResolveAcceptedPath() const;
    bool CanAcceptSelection() const;

private:
    bool LoadDirectory(const std::filesystem::path& directory);
    void AddRecentDirectory(const std::filesystem::path& directory);
    void SeedRecentDirectories(const std::vector<std::filesystem::path>& directories);
    void RebuildBreadcrumbs();
    void RebuildVisibleEntries();
    void UpdateStats(bool directoryLoaded, std::string error);
    bool Fail(std::string error);
    bool FailPreserveState(std::string error);

    EditorFilePickerDesc m_desc;
    std::filesystem::path m_currentDirectory;
    std::filesystem::path m_selectedPath;
    std::string m_typedFileName;
    uint32 m_activeFilterIndex = 0;
    std::vector<std::filesystem::path> m_backDirectoryStack;
    std::vector<std::filesystem::path> m_forwardDirectoryStack;
    std::vector<std::filesystem::path> m_recentDirectories;
    std::vector<EditorFilePickerBreadcrumb> m_breadcrumbs;
    std::vector<EditorFilePickerEntry> m_entries;
    std::vector<EditorFilePickerEntry> m_visibleEntries;
    EditorFilePickerStats m_stats;
};

} // namespace RVX::Editor
