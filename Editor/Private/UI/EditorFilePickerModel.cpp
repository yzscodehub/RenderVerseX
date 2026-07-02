/**
 * @file EditorFilePickerModel.cpp
 * @brief Reusable native editor file picker filesystem model implementation
 */

#include "Editor/UI/EditorFilePickerModel.h"

#include <algorithm>
#include <cctype>
#include <system_error>
#include <utility>

namespace RVX::Editor
{
namespace
{
    std::string ToLowerAscii(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    }

    std::string NormalizeExtension(std::string extension)
    {
        if (!extension.empty() && extension.front() != '.')
        {
            extension.insert(extension.begin(), '.');
        }
        return extension;
    }

    std::filesystem::path NormalizePath(const std::filesystem::path& path)
    {
        std::error_code ec;
        std::filesystem::path absolute = std::filesystem::absolute(path, ec);
        if (ec)
        {
            absolute = path;
        }
        return absolute.lexically_normal();
    }

    bool WildcardMatch(const std::string& pattern, const std::string& text)
    {
        size_t patternIndex = 0;
        size_t textIndex = 0;
        size_t starIndex = std::string::npos;
        size_t matchIndex = 0;

        while (textIndex < text.size())
        {
            if (patternIndex < pattern.size() &&
                (pattern[patternIndex] == '?' ||
                 pattern[patternIndex] == text[textIndex]))
            {
                ++patternIndex;
                ++textIndex;
                continue;
            }

            if (patternIndex < pattern.size() && pattern[patternIndex] == '*')
            {
                starIndex = patternIndex++;
                matchIndex = textIndex;
                continue;
            }

            if (starIndex != std::string::npos)
            {
                patternIndex = starIndex + 1u;
                textIndex = ++matchIndex;
                continue;
            }

            return false;
        }

        while (patternIndex < pattern.size() && pattern[patternIndex] == '*')
        {
            ++patternIndex;
        }

        return patternIndex == pattern.size();
    }

    bool PatternMatchesPath(const std::string& pattern,
                            const std::filesystem::path& path)
    {
        const std::string normalizedPattern = ToLowerAscii(pattern);
        if (normalizedPattern.empty() ||
            normalizedPattern == "*" ||
            normalizedPattern == "*.*")
        {
            return true;
        }

        const std::string fileName = ToLowerAscii(path.filename().string());
        const std::string extension = ToLowerAscii(path.extension().string());
        if (normalizedPattern.rfind("*.", 0u) == 0u &&
            normalizedPattern.find_first_of("*?", 1u) == std::string::npos)
        {
            return extension == NormalizeExtension(normalizedPattern.substr(2u));
        }

        return WildcardMatch(normalizedPattern, fileName);
    }

    bool FilterMatchesPath(const EditorFilePickerFilter& filter,
                           const std::filesystem::path& path)
    {
        if (filter.patterns.empty())
        {
            return true;
        }

        for (const std::string& pattern : filter.patterns)
        {
            if (PatternMatchesPath(pattern, path))
            {
                return true;
            }
        }
        return false;
    }

    bool EntrySortLess(const EditorFilePickerEntry& lhs,
                       const EditorFilePickerEntry& rhs)
    {
        if (lhs.directory != rhs.directory)
        {
            return lhs.directory;
        }

        const std::string lhsName = ToLowerAscii(lhs.displayName);
        const std::string rhsName = ToLowerAscii(rhs.displayName);
        if (lhsName != rhsName)
        {
            return lhsName < rhsName;
        }
        return lhs.path.string() < rhs.path.string();
    }
}

bool EditorFilePickerModel::Open(EditorFilePickerDesc desc)
{
    m_currentDirectory.clear();
    m_entries.clear();
    m_visibleEntries.clear();
    m_backDirectoryStack.clear();
    m_forwardDirectoryStack.clear();
    m_recentDirectories.clear();
    m_breadcrumbs.clear();

    m_desc = std::move(desc);
    SeedRecentDirectories(m_desc.recentDirectories);
    m_activeFilterIndex = m_desc.activeFilterIndex;
    if (m_activeFilterIndex >= static_cast<uint32>(m_desc.filters.size()))
    {
        m_activeFilterIndex = 0;
    }

    m_typedFileName = m_desc.defaultFileName;
    if (!m_desc.initialPath.empty() && !m_desc.initialPath.filename().empty())
    {
        m_typedFileName = m_desc.initialPath.filename().string();
        m_selectedPath = NormalizePath(m_desc.initialPath);
    }
    else
    {
        m_selectedPath.clear();
    }

    std::filesystem::path directory = m_desc.initialDirectory;
    if (directory.empty() && !m_desc.initialPath.empty())
    {
        directory = m_desc.initialPath.parent_path();
    }
    if (directory.empty())
    {
        std::error_code ec;
        directory = std::filesystem::current_path(ec);
        if (ec)
        {
            return Fail("Failed to query current directory");
        }
    }

    return LoadDirectory(NormalizePath(directory));
}

bool EditorFilePickerModel::Refresh()
{
    if (m_currentDirectory.empty())
    {
        return Fail("No current directory is selected");
    }
    return LoadDirectory(m_currentDirectory);
}

bool EditorFilePickerModel::SetCurrentDirectory(
    const std::filesystem::path& directory)
{
    if (directory.empty())
    {
        return Fail("Cannot open an empty directory path");
    }

    const std::filesystem::path targetDirectory = NormalizePath(directory);
    const std::filesystem::path previousDirectory = m_currentDirectory;
    const bool shouldPushHistory =
        !previousDirectory.empty() && targetDirectory != previousDirectory;
    if (!LoadDirectory(targetDirectory))
    {
        return false;
    }

    if (shouldPushHistory)
    {
        m_backDirectoryStack.push_back(previousDirectory);
        m_forwardDirectoryStack.clear();
    }
    m_selectedPath.clear();
    m_typedFileName.clear();
    UpdateStats(m_stats.directoryLoaded, {});
    return true;
}

bool EditorFilePickerModel::EnterDirectory(const std::filesystem::path& directory)
{
    const std::filesystem::path target =
        directory.is_absolute() ? directory : m_currentDirectory / directory;
    return SetCurrentDirectory(target);
}

bool EditorFilePickerModel::EnterSelectedDirectory()
{
    if (m_selectedPath.empty())
    {
        return false;
    }

    std::error_code ec;
    if (!std::filesystem::is_directory(m_selectedPath, ec) || ec)
    {
        return false;
    }

    return SetCurrentDirectory(m_selectedPath);
}

bool EditorFilePickerModel::GoToParentDirectory()
{
    if (m_currentDirectory.empty())
    {
        return false;
    }

    const std::filesystem::path parent = m_currentDirectory.parent_path();
    if (parent.empty() || parent == m_currentDirectory)
    {
        return false;
    }

    return SetCurrentDirectory(parent);
}

bool EditorFilePickerModel::GoBackDirectory()
{
    if (m_backDirectoryStack.empty())
    {
        return false;
    }

    const std::filesystem::path targetDirectory = m_backDirectoryStack.back();
    const std::filesystem::path previousDirectory = m_currentDirectory;
    if (!LoadDirectory(targetDirectory))
    {
        return false;
    }

    m_backDirectoryStack.pop_back();
    if (!previousDirectory.empty() && previousDirectory != targetDirectory)
    {
        m_forwardDirectoryStack.push_back(previousDirectory);
    }
    m_selectedPath.clear();
    m_typedFileName.clear();
    UpdateStats(m_stats.directoryLoaded, {});
    return true;
}

bool EditorFilePickerModel::GoForwardDirectory()
{
    if (m_forwardDirectoryStack.empty())
    {
        return false;
    }

    const std::filesystem::path targetDirectory = m_forwardDirectoryStack.back();
    const std::filesystem::path previousDirectory = m_currentDirectory;
    if (!LoadDirectory(targetDirectory))
    {
        return false;
    }

    m_forwardDirectoryStack.pop_back();
    if (!previousDirectory.empty() && previousDirectory != targetDirectory)
    {
        m_backDirectoryStack.push_back(previousDirectory);
    }
    m_selectedPath.clear();
    m_typedFileName.clear();
    UpdateStats(m_stats.directoryLoaded, {});
    return true;
}

bool EditorFilePickerModel::EnterRecentDirectory(uint32 recentIndex)
{
    if (recentIndex >= static_cast<uint32>(m_recentDirectories.size()))
    {
        return FailPreserveState("Recent directory index is out of range");
    }

    return SetCurrentDirectory(m_recentDirectories[recentIndex]);
}

bool EditorFilePickerModel::EnterBreadcrumb(uint32 breadcrumbIndex)
{
    if (breadcrumbIndex >= static_cast<uint32>(m_breadcrumbs.size()))
    {
        return FailPreserveState("Breadcrumb index is out of range");
    }

    return SetCurrentDirectory(m_breadcrumbs[breadcrumbIndex].path);
}

bool EditorFilePickerModel::SetActiveFilterIndex(uint32 index)
{
    if (!m_desc.filters.empty() && index >= static_cast<uint32>(m_desc.filters.size()))
    {
        return false;
    }

    if (m_desc.filters.empty() && index != 0u)
    {
        return false;
    }

    if (m_activeFilterIndex == index)
    {
        return false;
    }

    m_activeFilterIndex = index;
    RebuildVisibleEntries();
    UpdateStats(m_stats.directoryLoaded, {});
    return true;
}

bool EditorFilePickerModel::SelectEntry(uint32 visibleIndex)
{
    if (visibleIndex >= static_cast<uint32>(m_visibleEntries.size()))
    {
        return FailPreserveState("File picker selection index is out of range");
    }

    const EditorFilePickerEntry& entry = m_visibleEntries[visibleIndex];
    m_selectedPath = entry.path;
    m_typedFileName = entry.file ? entry.displayName : std::string{};
    UpdateStats(m_stats.directoryLoaded, {});
    return true;
}

void EditorFilePickerModel::ClearSelection()
{
    m_selectedPath.clear();
    m_typedFileName.clear();
    UpdateStats(m_stats.directoryLoaded, {});
}

bool EditorFilePickerModel::SetTypedFileName(std::string fileName)
{
    if (m_typedFileName == fileName)
    {
        return false;
    }

    m_typedFileName = std::move(fileName);
    m_selectedPath.clear();
    UpdateStats(m_stats.directoryLoaded, {});
    return true;
}

std::filesystem::path EditorFilePickerModel::ResolveAcceptedPath() const
{
    std::filesystem::path path;
    if (!m_typedFileName.empty())
    {
        path = std::filesystem::path(m_typedFileName);
        if (!path.is_absolute())
        {
            path = m_currentDirectory / path;
        }
    }
    else
    {
        path = m_selectedPath;
    }

    if (path.empty())
    {
        return {};
    }

    if (m_desc.mode == EditorFilePickerMode::SaveFile &&
        !m_desc.defaultExtension.empty() &&
        path.extension().empty())
    {
        path += NormalizeExtension(m_desc.defaultExtension);
    }

    return NormalizePath(path);
}

bool EditorFilePickerModel::CanAcceptSelection() const
{
    const std::filesystem::path path = ResolveAcceptedPath();
    if (path.empty())
    {
        return false;
    }

    if (!m_desc.filters.empty() &&
        !FilterMatchesPath(m_desc.filters[m_activeFilterIndex], path))
    {
        return false;
    }

    std::error_code ec;
    if (m_desc.mode == EditorFilePickerMode::OpenFile)
    {
        return std::filesystem::is_regular_file(path, ec) && !ec;
    }

    if (std::filesystem::is_directory(path, ec) && !ec)
    {
        return false;
    }

    if (!m_desc.allowCreateFileInSaveMode &&
        !std::filesystem::is_regular_file(path, ec))
    {
        return false;
    }

    const std::filesystem::path parent = path.parent_path();
    return parent.empty() || std::filesystem::is_directory(parent, ec);
}

bool EditorFilePickerModel::LoadDirectory(const std::filesystem::path& directory)
{
    const std::filesystem::path targetDirectory = NormalizePath(directory);
    const bool preserveExistingDirectory =
        !m_currentDirectory.empty() && targetDirectory != m_currentDirectory;

    std::error_code ec;
    if (!std::filesystem::is_directory(targetDirectory, ec) || ec)
    {
        const std::string error =
            "File picker directory does not exist: " + targetDirectory.string();
        return preserveExistingDirectory ? FailPreserveState(error) : Fail(error);
    }

    std::filesystem::directory_iterator iterator(targetDirectory, ec);
    if (ec)
    {
        const std::string error =
            "Failed to enumerate directory: " + targetDirectory.string();
        return preserveExistingDirectory ? FailPreserveState(error) : Fail(error);
    }

    std::vector<EditorFilePickerEntry> loadedEntries;
    for (const std::filesystem::directory_entry& directoryEntry : iterator)
    {
        std::error_code entryEc;
        const bool isDirectory = directoryEntry.is_directory(entryEc);
        const bool isFile = !isDirectory && directoryEntry.is_regular_file(entryEc);
        if (entryEc || (!isDirectory && !isFile))
        {
            continue;
        }
        if ((isDirectory && !m_desc.showDirectories) ||
            (isFile && !m_desc.showFiles))
        {
            continue;
        }

        EditorFilePickerEntry entry;
        entry.path = NormalizePath(directoryEntry.path());
        entry.displayName = entry.path.filename().string();
        entry.directory = isDirectory;
        entry.file = isFile;
        entry.matchesActiveFilter =
            !isFile || m_desc.filters.empty() ||
            FilterMatchesPath(m_desc.filters[m_activeFilterIndex], entry.path);
        if (isFile)
        {
            const uintmax_t fileSize = directoryEntry.file_size(entryEc);
            entry.sizeBytes = entryEc ? 0u : static_cast<uint64>(fileSize);
        }
        loadedEntries.push_back(std::move(entry));
    }

    m_currentDirectory = targetDirectory;
    m_entries = std::move(loadedEntries);
    std::stable_sort(m_entries.begin(), m_entries.end(), EntrySortLess);
    RebuildVisibleEntries();
    RebuildBreadcrumbs();
    AddRecentDirectory(m_currentDirectory);
    UpdateStats(true, {});
    return true;
}

void EditorFilePickerModel::AddRecentDirectory(
    const std::filesystem::path& directory)
{
    if (directory.empty() || m_desc.maxRecentDirectories == 0u)
    {
        return;
    }

    const std::filesystem::path normalizedDirectory = NormalizePath(directory);
    std::error_code ec;
    if (!std::filesystem::is_directory(normalizedDirectory, ec) || ec)
    {
        return;
    }

    m_recentDirectories.erase(
        std::remove(m_recentDirectories.begin(),
                    m_recentDirectories.end(),
                    normalizedDirectory),
        m_recentDirectories.end());
    m_recentDirectories.insert(m_recentDirectories.begin(), normalizedDirectory);

    const size_t maxRecentDirectories =
        static_cast<size_t>(m_desc.maxRecentDirectories);
    if (m_recentDirectories.size() > maxRecentDirectories)
    {
        m_recentDirectories.resize(maxRecentDirectories);
    }
}

void EditorFilePickerModel::SeedRecentDirectories(
    const std::vector<std::filesystem::path>& directories)
{
    m_recentDirectories.clear();
    for (auto it = directories.rbegin(); it != directories.rend(); ++it)
    {
        AddRecentDirectory(*it);
    }
}

void EditorFilePickerModel::RebuildBreadcrumbs()
{
    m_breadcrumbs.clear();
    if (m_currentDirectory.empty())
    {
        return;
    }

    const std::filesystem::path normalizedDirectory =
        NormalizePath(m_currentDirectory);
    std::filesystem::path cumulative = normalizedDirectory.root_path();
    if (!cumulative.empty())
    {
        EditorFilePickerBreadcrumb breadcrumb;
        breadcrumb.path = NormalizePath(cumulative);
        breadcrumb.displayName = cumulative.string();
        m_breadcrumbs.push_back(std::move(breadcrumb));
    }

    for (const std::filesystem::path& part : normalizedDirectory.relative_path())
    {
        if (part.empty())
        {
            continue;
        }

        cumulative = cumulative.empty() ? part : cumulative / part;
        EditorFilePickerBreadcrumb breadcrumb;
        breadcrumb.path = NormalizePath(cumulative);
        breadcrumb.displayName = part.string();
        m_breadcrumbs.push_back(std::move(breadcrumb));
    }
}

void EditorFilePickerModel::RebuildVisibleEntries()
{
    m_visibleEntries.clear();
    for (EditorFilePickerEntry& entry : m_entries)
    {
        entry.matchesActiveFilter =
            !entry.file || m_desc.filters.empty() ||
            FilterMatchesPath(m_desc.filters[m_activeFilterIndex], entry.path);
        if (entry.directory || entry.matchesActiveFilter)
        {
            m_visibleEntries.push_back(entry);
        }
    }
}

void EditorFilePickerModel::UpdateStats(bool directoryLoaded, std::string error)
{
    m_stats = {};
    m_stats.directoryLoaded = directoryLoaded;
    m_stats.canGoBackDirectory = CanGoBackDirectory();
    m_stats.canGoForwardDirectory = CanGoForwardDirectory();
    m_stats.activeFilterIndex = m_activeFilterIndex;
    m_stats.currentDirectory = m_currentDirectory;
    m_stats.selectedPath = m_selectedPath;
    m_stats.error = std::move(error);
    m_stats.entryCount = static_cast<uint32>(m_entries.size());
    m_stats.visibleEntryCount = static_cast<uint32>(m_visibleEntries.size());
    m_stats.breadcrumbCount = static_cast<uint32>(m_breadcrumbs.size());
    m_stats.recentDirectoryCount =
        static_cast<uint32>(m_recentDirectories.size());

    for (const EditorFilePickerEntry& entry : m_entries)
    {
        if (entry.directory)
        {
            ++m_stats.directoryCount;
            continue;
        }

        if (entry.file)
        {
            ++m_stats.fileCount;
            if (entry.matchesActiveFilter)
            {
                ++m_stats.visibleFileCount;
            }
            else
            {
                ++m_stats.hiddenFileCount;
            }
        }
    }
}

bool EditorFilePickerModel::Fail(std::string error)
{
    m_entries.clear();
    m_visibleEntries.clear();
    m_selectedPath.clear();
    UpdateStats(false, std::move(error));
    return false;
}

bool EditorFilePickerModel::FailPreserveState(std::string error)
{
    UpdateStats(m_stats.directoryLoaded, std::move(error));
    return false;
}

} // namespace RVX::Editor
