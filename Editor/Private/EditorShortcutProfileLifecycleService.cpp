/**
 * @file EditorShortcutProfileLifecycleService.cpp
 * @brief Editor shortcut profile lifecycle service implementation.
 */

#include "Editor/EditorShortcutProfileLifecycleService.h"

#include "Core/Log.h"
#include "Editor/EditorSettings.h"
#include "Editor/UI/EditorCommandRegistry.h"
#include "Editor/UI/EditorShortcutProfileService.h"

#include <string>

namespace RVX::Editor
{

bool EditorShortcutProfileLifecycleService::ConfigureAutosave()
{
    if (!m_shortcutProfiles || !m_settings)
    {
        return false;
    }

    m_shortcutProfiles->SetAutosaveEnabled(
        m_settings->IsShortcutProfileAutosaveEnabled());
    m_shortcutProfiles->SetAutosaveDebounceSeconds(
        m_settings->GetShortcutProfileAutosaveDebounceSeconds());
    return true;
}

const EditorShortcutProfileOperationResult&
EditorShortcutProfileLifecycleService::LoadStartupProfile(
    EditorCommandRegistry& registry)
{
    m_startupLoadAttempted = false;
    m_startupLoadResult = {};
    if (!m_shortcutProfiles)
    {
        return m_startupLoadResult;
    }

    m_startupLoadAttempted = true;
    m_startupLoadResult =
        m_shortcutProfiles->LoadAndApplyProfileIfPresent(registry);
    LogStartupLoadResult();
    return m_startupLoadResult;
}

EditorShortcutProfileOperationResult
EditorShortcutProfileLifecycleService::TickAutosave(
    float deltaTime,
    const EditorCommandRegistry& registry)
{
    if (!m_shortcutProfiles)
    {
        return {};
    }

    EditorShortcutProfileOperationResult result =
        m_shortcutProfiles->TickAutosave(deltaTime, registry);
    LogAutosaveResult(result);
    return result;
}

void EditorShortcutProfileLifecycleService::LogStartupLoadResult() const
{
    const std::string pathText =
        m_startupLoadResult.path.empty()
            ? std::string("<unset>")
            : m_startupLoadResult.path.string();
    switch (m_startupLoadResult.status)
    {
        case EditorShortcutProfileOperationStatus::Imported:
        case EditorShortcutProfileOperationStatus::NotFound:
            RVX_CORE_INFO("{} Path: {}",
                          m_startupLoadResult.statusText,
                          pathText);
            break;
        case EditorShortcutProfileOperationStatus::ImportFailed:
        case EditorShortcutProfileOperationStatus::MissingCommand:
        case EditorShortcutProfileOperationStatus::Conflict:
            RVX_CORE_WARN("{} Path: {}",
                          m_startupLoadResult.statusText,
                          pathText);
            break;
        case EditorShortcutProfileOperationStatus::None:
        case EditorShortcutProfileOperationStatus::Exported:
        case EditorShortcutProfileOperationStatus::ExportFailed:
            break;
    }
}

void EditorShortcutProfileLifecycleService::LogAutosaveResult(
    const EditorShortcutProfileOperationResult& result)
{
    if (result.status == EditorShortcutProfileOperationStatus::None)
    {
        return;
    }

    if (result.Succeeded())
    {
        RVX_CORE_INFO("Shortcut profile autosave: {}", result.statusText);
    }
    else
    {
        RVX_CORE_WARN("Shortcut profile autosave: {}", result.statusText);
    }
}

} // namespace RVX::Editor
