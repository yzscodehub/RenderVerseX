#pragma once

/**
 * @file PathUtils.h
 * @brief Process and workspace path discovery helpers
 */

#include <filesystem>
#include <vector>

namespace RVX
{
    /**
     * @brief Get the absolute path of the running executable when available.
     */
    std::filesystem::path GetExecutablePath();

    /**
     * @brief Get the directory that contains the running executable.
     */
    std::filesystem::path GetExecutableDirectory();

    /**
     * @brief Walk upward from a path until a marker file or directory exists.
     *
     * @param start Directory or file path used as the search starting point.
     * @param marker Relative marker path to probe inside each ancestor.
     * @return Ancestor directory containing the marker, or empty path.
     */
    std::filesystem::path FindAncestorContaining(
        const std::filesystem::path& start,
        const std::filesystem::path& marker);

    /**
     * @brief Build candidate roots used for workspace-relative asset lookup.
     */
    std::vector<std::filesystem::path> BuildWorkspaceSearchRoots();

    /**
     * @brief Locate the RenderVerseX workspace root using stable source markers.
     */
    std::filesystem::path FindWorkspaceRoot();

    /**
     * @brief Resolve a relative path against the workspace root when possible.
     */
    std::filesystem::path ResolveWorkspaceRelativePath(
        const std::filesystem::path& relativePath);
} // namespace RVX
