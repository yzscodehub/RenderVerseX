/**
 * @file EditorShortcutProfileService.cpp
 * @brief Editor shortcut profile storage and registry apply service implementation.
 */

#include "Editor/UI/EditorShortcutProfileService.h"

#include "Editor/EditorSettings.h"

#include <system_error>
#include <utility>

namespace RVX::Editor
{

EditorShortcutProfileService::EditorShortcutProfileService(
    EditorSettingsService* settingsService)
    : m_settingsService(settingsService)
{
}

std::filesystem::path EditorShortcutProfileService::ResolveProfilePath() const
{
    if (!m_profilePathOverride.empty())
    {
        return m_profilePathOverride;
    }

    const std::filesystem::path settingsPath =
        m_settingsService ? m_settingsService->GetSettingsPath()
                          : EditorSettingsService::ResolveDefaultSettingsPath();
    if (settingsPath.has_parent_path())
    {
        return settingsPath.parent_path() / "EditorShortcuts.shortcuts";
    }

    return std::filesystem::path("EditorShortcuts.shortcuts");
}

EditorShortcutProfileOperationResult
EditorShortcutProfileService::SaveCurrentProfile(
    const EditorCommandRegistry& registry)
{
    EditorShortcutProfileOperationResult result;
    result.path = ResolveProfilePath();

    const EditorShortcutProfile profile =
        EditorShortcutProfile::CaptureFromRegistry(registry);
    result.entryCount = static_cast<uint32>(profile.GetEntryCount());

    std::string error;
    if (!EditorShortcutProfileSerializer::SaveToFile(profile,
                                                     result.path,
                                                     &error))
    {
        result.status = EditorShortcutProfileOperationStatus::ExportFailed;
        result.error = error;
        result.statusText = "Shortcut export failed: " + error;
        RecordOperationResult(result);
        return result;
    }

    result.status = EditorShortcutProfileOperationStatus::Exported;
    result.statusText = "Exported " +
                        CountLabel(result.entryCount,
                                   "shortcut",
                                   "shortcuts") +
                        ".";
    MarkProfileClean();
    RecordOperationResult(result);
    return result;
}

EditorShortcutProfileOperationResult
EditorShortcutProfileService::LoadAndApplyProfile(
    EditorCommandRegistry& registry)
{
    EditorShortcutProfileOperationResult result;
    result.path = ResolveProfilePath();

    EditorShortcutProfile importedProfile;
    std::string error;
    if (!EditorShortcutProfileSerializer::LoadFromFile(result.path,
                                                       importedProfile,
                                                       &error))
    {
        result.status = EditorShortcutProfileOperationStatus::ImportFailed;
        result.error = error;
        result.statusText = "Shortcut import failed: " + error;
        RecordOperationResult(result);
        return result;
    }

    return ApplyProfile(registry, importedProfile, result.path);
}

EditorShortcutProfileOperationResult
EditorShortcutProfileService::LoadAndApplyProfileIfPresent(
    EditorCommandRegistry& registry)
{
    EditorShortcutProfileOperationResult result;
    result.path = ResolveProfilePath();

    std::error_code ec;
    if (!std::filesystem::exists(result.path, ec))
    {
        if (ec)
        {
            result.status = EditorShortcutProfileOperationStatus::ImportFailed;
            result.error =
                "Could not inspect shortcut profile file: " + ec.message();
            result.statusText = "Shortcut import failed: " + result.error;
            RecordOperationResult(result);
            return result;
        }

        result.status = EditorShortcutProfileOperationStatus::NotFound;
        result.statusText =
            "Shortcut profile not found; using default shortcuts.";
        RecordOperationResult(result);
        return result;
    }

    return LoadAndApplyProfile(registry);
}

EditorShortcutProfileOperationResult
EditorShortcutProfileService::ApplyProfile(
    EditorCommandRegistry& registry,
    const EditorShortcutProfile& profile,
    std::filesystem::path sourcePath)
{
    EditorShortcutProfileOperationResult result;
    result.path = sourcePath.empty() ? ResolveProfilePath()
                                     : std::move(sourcePath);
    result.entryCount = static_cast<uint32>(profile.GetEntryCount());

    EditorShortcutProfile previousProfile =
        EditorShortcutProfile::CaptureFromRegistry(registry);

    EditorShortcutProfileApplyResult applyResult;
    profile.ApplyToRegistry(registry, &applyResult);
    result.appliedCount = applyResult.appliedCount;
    result.missingCommandCount = applyResult.missingCommandCount;
    result.missingCommandIds = std::move(applyResult.missingCommandIds);

    const std::vector<EditorCommandShortcutConflict> conflicts =
        registry.GetShortcutConflicts();
    result.conflictCount = static_cast<uint32>(conflicts.size());

    if (!applyResult.Succeeded() || !conflicts.empty())
    {
        previousProfile.ApplyToRegistry(registry, nullptr);
        if (!applyResult.Succeeded())
        {
            result.status = EditorShortcutProfileOperationStatus::MissingCommand;
            result.statusText =
                "Shortcut import rejected: " +
                CountLabel(result.missingCommandCount,
                           "missing command",
                           "missing commands") +
                ".";
            RecordOperationResult(result);
            return result;
        }

        result.status = EditorShortcutProfileOperationStatus::Conflict;
        result.statusText =
            "Shortcut import rejected: " +
            CountLabel(result.conflictCount, "conflict", "conflicts") + ".";
        RecordOperationResult(result);
        return result;
    }

    result.status = EditorShortcutProfileOperationStatus::Imported;
    result.statusText =
        "Imported " +
        CountLabel(result.appliedCount, "shortcut", "shortcuts") + ".";
    MarkProfileClean();
    RecordOperationResult(result);
    return result;
}

void EditorShortcutProfileService::SetAutosaveEnabled(bool enabled)
{
    m_autosaveState.enabled = enabled;
    if (!enabled)
    {
        m_autosaveState.elapsedSinceEditSeconds = 0.0f;
    }
}

void EditorShortcutProfileService::SetAutosaveDebounceSeconds(float seconds)
{
    m_autosaveState.debounceSeconds = seconds > 0.0f ? seconds : 0.0f;
}

void EditorShortcutProfileService::MarkProfileDirty()
{
    m_autosaveState.dirty = true;
    m_autosaveState.elapsedSinceEditSeconds = 0.0f;
    ++m_autosaveState.dirtyGeneration;
}

void EditorShortcutProfileService::ClearProfileDirty()
{
    MarkProfileClean();
}

EditorShortcutProfileOperationResult
EditorShortcutProfileService::TickAutosave(
    float deltaSeconds,
    const EditorCommandRegistry& registry)
{
    if (!m_autosaveState.enabled || !m_autosaveState.dirty)
    {
        return {};
    }

    if (deltaSeconds > 0.0f)
    {
        m_autosaveState.elapsedSinceEditSeconds += deltaSeconds;
    }
    if (m_autosaveState.elapsedSinceEditSeconds <
        m_autosaveState.debounceSeconds)
    {
        return {};
    }

    return FlushPendingAutosave(registry);
}

EditorShortcutProfileOperationResult
EditorShortcutProfileService::FlushPendingAutosave(
    const EditorCommandRegistry& registry)
{
    if (!m_autosaveState.dirty)
    {
        return {};
    }

    EditorShortcutProfileOperationResult result =
        SaveCurrentProfile(registry);
    if (!result.Succeeded())
    {
        m_autosaveState.elapsedSinceEditSeconds = 0.0f;
    }
    return result;
}

std::string EditorShortcutProfileService::CountLabel(uint32 count,
                                                     const char* singular,
                                                     const char* plural)
{
    return std::to_string(count) + " " + (count == 1u ? singular : plural);
}

void EditorShortcutProfileService::RecordOperationResult(
    const EditorShortcutProfileOperationResult& result)
{
    m_autosaveState.lastResult = result;
}

void EditorShortcutProfileService::MarkProfileClean()
{
    m_autosaveState.dirty = false;
    m_autosaveState.elapsedSinceEditSeconds = 0.0f;
    m_autosaveState.savedGeneration = m_autosaveState.dirtyGeneration;
}

} // namespace RVX::Editor
