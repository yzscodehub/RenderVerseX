/**
 * @file EditorSettings.h
 * @brief Persistent user editor settings.
 */

#pragma once

#include "Core/Types.h"
#include "Editor/UI/EditorUIBackendCatalog.h"

#include <filesystem>
#include <string>
#include <vector>

namespace RVX::Editor
{

inline constexpr float RVX_EDITOR_DEFAULT_UI_SCALE_FACTOR = 1.25f;

struct EditorFilePickerSettings
{
    std::vector<std::filesystem::path> recentSceneDirectories;
    uint32 maxRecentSceneDirectories = 6;
};

struct EditorUISettings
{
    EditorUIBackendType backendType = GetDefaultEditorUIBackendType();
    float scaleFactor = RVX_EDITOR_DEFAULT_UI_SCALE_FACTOR;
    std::vector<std::filesystem::path> fontPaths;
    bool appendDefaultSystemFonts = true;
};

struct EditorShortcutProfileSettings
{
    bool autosaveEnabled = false;
    float autosaveDebounceSeconds = 0.75f;
};

struct EditorSettings
{
    uint32 schemaVersion = 1;
    EditorUISettings ui;
    EditorFilePickerSettings filePicker;
    EditorShortcutProfileSettings shortcuts;
};

/**
 * @brief Loads and saves user-level editor settings.
 */
class EditorSettingsService
{
public:
    EditorSettingsService();
    explicit EditorSettingsService(std::filesystem::path settingsPath);

    void SetSettingsPath(std::filesystem::path settingsPath);
    const std::filesystem::path& GetSettingsPath() const { return m_settingsPath; }

    bool Load();
    bool Save() const;

    const EditorSettings& GetSettings() const { return m_settings; }
    const std::string& GetLastError() const { return m_lastError; }
    EditorUIBackendType GetUIBackendType() const
    {
        return m_settings.ui.backendType;
    }

    float GetUIScaleFactor() const
    {
        return m_settings.ui.scaleFactor;
    }

    const std::vector<std::filesystem::path>& GetUIFontPaths() const
    {
        return m_settings.ui.fontPaths;
    }

    bool ShouldAppendDefaultSystemFonts() const
    {
        return m_settings.ui.appendDefaultSystemFonts;
    }

    const std::vector<std::filesystem::path>& GetRecentSceneDirectories() const
    {
        return m_settings.filePicker.recentSceneDirectories;
    }

    uint32 GetMaxRecentSceneDirectories() const
    {
        return m_settings.filePicker.maxRecentSceneDirectories;
    }

    bool IsShortcutProfileAutosaveEnabled() const
    {
        return m_settings.shortcuts.autosaveEnabled;
    }

    float GetShortcutProfileAutosaveDebounceSeconds() const
    {
        return m_settings.shortcuts.autosaveDebounceSeconds;
    }

    void SetUIBackendType(EditorUIBackendType type);
    void SetUIScaleFactor(float scaleFactor);
    void SetUIFontPaths(std::vector<std::filesystem::path> fontPaths);
    void SetAppendDefaultSystemFonts(bool appendDefaultSystemFonts);
    void SetMaxRecentSceneDirectories(uint32 maxDirectories);
    void SetShortcutProfileAutosaveEnabled(bool enabled);
    void SetShortcutProfileAutosaveDebounceSeconds(float seconds);
    bool AddRecentSceneDirectory(const std::filesystem::path& directory);
    bool AddRecentScenePath(const std::filesystem::path& scenePath);

    static std::filesystem::path ResolveDefaultSettingsPath();

private:
    static bool AddRecentDirectory(EditorSettings& settings,
                                   const std::filesystem::path& directory,
                                   std::string* error);

    bool Fail(std::string error) const;
    void ClearError() const;

    std::filesystem::path m_settingsPath;
    EditorSettings m_settings;
    mutable std::string m_lastError;
};

} // namespace RVX::Editor
