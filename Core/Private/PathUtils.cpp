/**
 * @file PathUtils.cpp
 * @brief Process and workspace path discovery helpers
 */

#include "Core/PathUtils.h"

#include <cstdlib>
#include <string>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <vector>
#elif defined(__linux__)
#include <unistd.h>
#include <vector>
#endif

namespace RVX
{
namespace
{
    std::filesystem::path NormalizePath(const std::filesystem::path& path)
    {
        if (path.empty())
        {
            return {};
        }

        std::error_code ec;
        std::filesystem::path normalized =
            std::filesystem::weakly_canonical(path, ec);
        if (!ec && !normalized.empty())
        {
            return normalized;
        }

        normalized = std::filesystem::absolute(path, ec);
        if (!ec && !normalized.empty())
        {
            return normalized.lexically_normal();
        }

        return path.lexically_normal();
    }

    void AddUniquePath(std::vector<std::filesystem::path>& paths,
                       const std::filesystem::path& path)
    {
        const std::filesystem::path normalized = NormalizePath(path);
        if (normalized.empty())
        {
            return;
        }

        for (const std::filesystem::path& existing : paths)
        {
            if (existing == normalized)
            {
                return;
            }
        }
        paths.push_back(normalized);
    }

    void AddEnvironmentPath(std::vector<std::filesystem::path>& paths,
                            const char* variableName)
    {
#if defined(_WIN32)
        char* value = nullptr;
        std::size_t valueLength = 0;
        if (_dupenv_s(&value, &valueLength, variableName) == 0 && value)
        {
            if (valueLength > 1)
            {
                AddUniquePath(paths, std::filesystem::path(value));
            }
            std::free(value);
        }
#else
        if (const char* value = std::getenv(variableName))
        {
            if (*value != '\0')
            {
                AddUniquePath(paths, std::filesystem::path(value));
            }
        }
#endif
    }

    std::filesystem::path GetExecutablePathPlatform()
    {
#if defined(_WIN32)
        std::wstring buffer(260, L'\0');
        for (;;)
        {
            const DWORD length = GetModuleFileNameW(
                nullptr,
                buffer.data(),
                static_cast<DWORD>(buffer.size()));
            if (length == 0)
            {
                return {};
            }
            if (length < buffer.size())
            {
                buffer.resize(length);
                return std::filesystem::path(buffer);
            }
            buffer.resize(buffer.size() * 2);
        }
#elif defined(__APPLE__)
        uint32_t size = 0;
        _NSGetExecutablePath(nullptr, &size);
        if (size == 0)
        {
            return {};
        }

        std::vector<char> buffer(size + 1, '\0');
        if (_NSGetExecutablePath(buffer.data(), &size) != 0)
        {
            return {};
        }
        return std::filesystem::path(buffer.data());
#elif defined(__linux__)
        std::vector<char> buffer(1024, '\0');
        for (;;)
        {
            const ssize_t length =
                readlink("/proc/self/exe", buffer.data(), buffer.size());
            if (length < 0)
            {
                return {};
            }
            if (static_cast<std::size_t>(length) < buffer.size())
            {
                return std::filesystem::path(
                    std::string(buffer.data(),
                                static_cast<std::size_t>(length)));
            }
            buffer.resize(buffer.size() * 2);
        }
#else
        return {};
#endif
    }

    std::filesystem::path NormalizeSearchStart(
        const std::filesystem::path& start)
    {
        std::filesystem::path normalized = NormalizePath(start);
        if (normalized.empty())
        {
            return {};
        }

        std::error_code ec;
        if (std::filesystem::is_regular_file(normalized, ec))
        {
            normalized = normalized.parent_path();
        }
        return normalized;
    }
} // namespace

std::filesystem::path GetExecutablePath()
{
    return NormalizePath(GetExecutablePathPlatform());
}

std::filesystem::path GetExecutableDirectory()
{
    const std::filesystem::path executablePath = GetExecutablePath();
    if (executablePath.empty())
    {
        return {};
    }
    return executablePath.parent_path();
}

std::filesystem::path FindAncestorContaining(
    const std::filesystem::path& start,
    const std::filesystem::path& marker)
{
    if (marker.empty())
    {
        return {};
    }

    std::filesystem::path current = NormalizeSearchStart(start);
    while (!current.empty())
    {
        std::error_code ec;
        if (std::filesystem::exists(current / marker, ec) && !ec)
        {
            return current;
        }

        const std::filesystem::path parent = current.parent_path();
        if (parent.empty() || parent == current)
        {
            break;
        }
        current = parent;
    }

    return {};
}

std::vector<std::filesystem::path> BuildWorkspaceSearchRoots()
{
    std::vector<std::filesystem::path> paths;
    AddEnvironmentPath(paths, "RVX_WORKSPACE_ROOT");
    AddEnvironmentPath(paths, "RVX_ENGINE_ROOT");

    std::error_code ec;
    AddUniquePath(paths, std::filesystem::current_path(ec));
    AddUniquePath(paths, GetExecutableDirectory());
    return paths;
}

std::filesystem::path FindWorkspaceRoot()
{
    const std::filesystem::path workspaceMarker =
        std::filesystem::path("Render") / "Shaders" / "DefaultLit.hlsl";
    for (const std::filesystem::path& root : BuildWorkspaceSearchRoots())
    {
        const std::filesystem::path workspace =
            FindAncestorContaining(root, workspaceMarker);
        if (!workspace.empty())
        {
            return workspace;
        }
    }
    return {};
}

std::filesystem::path ResolveWorkspaceRelativePath(
    const std::filesystem::path& relativePath)
{
    if (relativePath.empty() || relativePath.is_absolute())
    {
        return NormalizePath(relativePath);
    }

    const std::filesystem::path workspaceRoot = FindWorkspaceRoot();
    if (!workspaceRoot.empty())
    {
        return NormalizePath(workspaceRoot / relativePath);
    }

    std::error_code ec;
    return NormalizePath(std::filesystem::current_path(ec) / relativePath);
}
} // namespace RVX
