#pragma once

#include "Core/Types.h"

namespace RVX
{
    /**
     * @brief Top-level application mode shared by runtime, editor, preview, and PIE worlds.
     */
    enum class AppMode : uint8
    {
        Runtime = 0,
        Editor,
        Preview,
        PlayInEditor
    };

    /**
     * @brief Stable mode traits used to keep editor shells and runtime worlds separated.
     */
    struct AppModeTraits
    {
        AppMode mode = AppMode::Runtime;
        std::string_view name;
        bool editorShellVisible = false;
        bool authoringToolsEnabled = false;
        bool runtimeExecutionEnabled = true;
        bool previewWorld = false;
        bool sourceAssetAccessAllowed = false;
        bool cookedRuntimeArtifactsRequired = true;
    };

    constexpr std::string_view ToString(AppMode mode)
    {
        switch (mode)
        {
        case AppMode::Runtime:
            return "Runtime";
        case AppMode::Editor:
            return "Editor";
        case AppMode::Preview:
            return "Preview";
        case AppMode::PlayInEditor:
            return "PlayInEditor";
        }

        return "Unknown";
    }

    constexpr AppModeTraits GetAppModeTraits(AppMode mode)
    {
        switch (mode)
        {
        case AppMode::Runtime:
            return {
                AppMode::Runtime,
                "Runtime",
                false,
                false,
                true,
                false,
                false,
                true,
            };
        case AppMode::Editor:
            return {
                AppMode::Editor,
                "Editor",
                true,
                true,
                false,
                false,
                true,
                false,
            };
        case AppMode::Preview:
            return {
                AppMode::Preview,
                "Preview",
                true,
                true,
                false,
                true,
                true,
                false,
            };
        case AppMode::PlayInEditor:
            return {
                AppMode::PlayInEditor,
                "PlayInEditor",
                true,
                false,
                true,
                false,
                false,
                true,
            };
        }

        return {};
    }

    constexpr bool IsEditorShellMode(AppMode mode)
    {
        return GetAppModeTraits(mode).editorShellVisible;
    }

    constexpr bool IsRuntimeExecutionMode(AppMode mode)
    {
        return GetAppModeTraits(mode).runtimeExecutionEnabled;
    }

    constexpr bool AllowsSourceAssetAccess(AppMode mode)
    {
        return GetAppModeTraits(mode).sourceAssetAccessAllowed;
    }

} // namespace RVX
