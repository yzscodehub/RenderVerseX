/**
 * @file EditorShortcutProfileService.h
 * @brief Editor shortcut profile storage and registry apply service.
 */

#pragma once

#include "Core/Types.h"
#include "Editor/UI/EditorShortcutProfile.h"

#include <filesystem>
#include <string>
#include <vector>

namespace RVX::Editor
{

class EditorSettingsService;

enum class EditorShortcutProfileOperationStatus : uint8
{
    None = 0,
    Exported,
    Imported,
    NotFound,
    ExportFailed,
    ImportFailed,
    MissingCommand,
    Conflict
};

struct EditorShortcutProfileOperationResult
{
    EditorShortcutProfileOperationStatus status =
        EditorShortcutProfileOperationStatus::None;
    std::filesystem::path path;
    uint32 entryCount = 0;
    uint32 appliedCount = 0;
    uint32 missingCommandCount = 0;
    uint32 conflictCount = 0;
    std::vector<std::string> missingCommandIds;
    std::string error;
    std::string statusText;

    bool Succeeded() const
    {
        return status == EditorShortcutProfileOperationStatus::Exported ||
               status == EditorShortcutProfileOperationStatus::Imported;
    }
};

struct EditorShortcutProfileAutosaveState
{
    bool enabled = false;
    bool dirty = false;
    float debounceSeconds = 0.75f;
    float elapsedSinceEditSeconds = 0.0f;
    uint32 dirtyGeneration = 0;
    uint32 savedGeneration = 0;
    EditorShortcutProfileOperationResult lastResult;
};

/**
 * @brief Owns shortcut profile file location and safe registry application.
 */
class EditorShortcutProfileService
{
public:
    explicit EditorShortcutProfileService(
        EditorSettingsService* settingsService = nullptr);

    void SetSettingsService(EditorSettingsService* settingsService)
    {
        m_settingsService = settingsService;
    }

    void SetProfilePathOverride(std::filesystem::path path)
    {
        m_profilePathOverride = std::move(path);
    }

    const std::filesystem::path& GetProfilePathOverride() const
    {
        return m_profilePathOverride;
    }

    std::filesystem::path ResolveProfilePath() const;

    EditorShortcutProfileOperationResult SaveCurrentProfile(
        const EditorCommandRegistry& registry);
    EditorShortcutProfileOperationResult LoadAndApplyProfile(
        EditorCommandRegistry& registry);
    EditorShortcutProfileOperationResult LoadAndApplyProfileIfPresent(
        EditorCommandRegistry& registry);
    EditorShortcutProfileOperationResult ApplyProfile(
        EditorCommandRegistry& registry,
        const EditorShortcutProfile& profile,
        std::filesystem::path sourcePath = {});

    void SetAutosaveEnabled(bool enabled);
    void SetAutosaveDebounceSeconds(float seconds);
    void MarkProfileDirty();
    void ClearProfileDirty();
    bool IsProfileDirty() const { return m_autosaveState.dirty; }
    const EditorShortcutProfileAutosaveState& GetAutosaveState() const
    {
        return m_autosaveState;
    }
    EditorShortcutProfileOperationResult TickAutosave(
        float deltaSeconds,
        const EditorCommandRegistry& registry);
    EditorShortcutProfileOperationResult FlushPendingAutosave(
        const EditorCommandRegistry& registry);

private:
    static std::string CountLabel(uint32 count,
                                  const char* singular,
                                  const char* plural);
    void RecordOperationResult(
        const EditorShortcutProfileOperationResult& result);
    void MarkProfileClean();

    EditorSettingsService* m_settingsService = nullptr;
    std::filesystem::path m_profilePathOverride;
    EditorShortcutProfileAutosaveState m_autosaveState;
};

} // namespace RVX::Editor
