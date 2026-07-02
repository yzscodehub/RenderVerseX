/**
 * @file EditorShortcutProfileLifecycleService.h
 * @brief Editor shortcut profile startup and autosave lifecycle service.
 */

#pragma once

#include "Core/Types.h"
#include "Editor/UI/EditorShortcutProfileService.h"

namespace RVX::Editor
{

class EditorCommandRegistry;
class EditorSettingsService;
class EditorShortcutProfileService;

/**
 * @brief Coordinates shortcut profile startup loading and autosave ticking.
 */
class EditorShortcutProfileLifecycleService
{
public:
    // =========================================================================
    // Dependencies
    // =========================================================================

    void SetSettingsService(EditorSettingsService* settings)
    {
        m_settings = settings;
    }

    void SetShortcutProfileService(EditorShortcutProfileService* shortcutProfiles)
    {
        m_shortcutProfiles = shortcutProfiles;
    }

    // =========================================================================
    // Lifecycle
    // =========================================================================

    bool ConfigureAutosave();
    const EditorShortcutProfileOperationResult& LoadStartupProfile(
        EditorCommandRegistry& registry);
    EditorShortcutProfileOperationResult TickAutosave(
        float deltaTime,
        const EditorCommandRegistry& registry);

    // =========================================================================
    // Diagnostics
    // =========================================================================

    bool WasStartupLoadAttempted() const
    {
        return m_startupLoadAttempted;
    }

    const EditorShortcutProfileOperationResult& GetStartupLoadResult() const
    {
        return m_startupLoadResult;
    }

private:
    void LogStartupLoadResult() const;
    static void LogAutosaveResult(
        const EditorShortcutProfileOperationResult& result);

    EditorSettingsService* m_settings = nullptr;
    EditorShortcutProfileService* m_shortcutProfiles = nullptr;
    EditorShortcutProfileOperationResult m_startupLoadResult;
    bool m_startupLoadAttempted = false;
};

} // namespace RVX::Editor
