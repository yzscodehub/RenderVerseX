/**
 * @file EditorLayoutPersistenceService.cpp
 * @brief Native editor workspace layout persistence lifecycle implementation.
 */

#include "Editor/EditorLayoutPersistenceService.h"

#include "Editor/EditorSettings.h"
#include "Editor/UI/EditorUIHost.h"
#include "Editor/UI/IEditorUIBackend.h"

#include <system_error>
#include <utility>

namespace RVX::Editor
{
namespace
{
    constexpr const char* RVX_EDITOR_DEFAULT_LAYOUT_FILENAME =
        "EditorLayout.layout";
} // namespace

std::filesystem::path EditorLayoutPersistenceService::ResolveDefaultLayoutPath(
    const EditorSettingsService& settingsService)
{
    return ResolveDefaultLayoutPath(settingsService.GetSettingsPath());
}

std::filesystem::path EditorLayoutPersistenceService::ResolveDefaultLayoutPath(
    const std::filesystem::path& settingsPath)
{
    std::filesystem::path basePath =
        settingsPath.empty() ? EditorSettingsService::ResolveDefaultSettingsPath()
                             : settingsPath;
    if (basePath.has_parent_path())
    {
        return basePath.parent_path() / RVX_EDITOR_DEFAULT_LAYOUT_FILENAME;
    }
    return std::filesystem::path(RVX_EDITOR_DEFAULT_LAYOUT_FILENAME);
}

EditorLayoutPersistenceResult
EditorLayoutPersistenceService::LoadStartupLayout(
    const EditorLayoutPersistenceDesc& desc)
{
    const std::filesystem::path layoutPath = ResolveLayoutPath(desc);
    if (!desc.enabled)
    {
        m_stats.lastLoad = Skip(layoutPath);
        return m_stats.lastLoad;
    }
    if (layoutPath.empty())
    {
        m_stats.lastLoad = Skip(layoutPath);
        return m_stats.lastLoad;
    }
    if (!desc.uiBackend || !desc.uiBackend->GetHost())
    {
        m_stats.lastLoad = Skip(layoutPath);
        return m_stats.lastLoad;
    }

    std::error_code ec;
    const bool exists = std::filesystem::exists(layoutPath, ec);
    if (ec)
    {
        m_stats.lastLoad =
            Fail(layoutPath,
                 false,
                 "Could not inspect editor layout file: " + ec.message());
        return m_stats.lastLoad;
    }
    if (!exists)
    {
        m_stats.lastLoad = Skip(layoutPath, true);
        return m_stats.lastLoad;
    }

    std::string error;
    if (!desc.uiBackend->GetHost()->LoadLayout(layoutPath, &error))
    {
        m_stats.lastLoad = Fail(layoutPath, true, std::move(error));
        return m_stats.lastLoad;
    }

    m_stats.lastLoad = Complete(layoutPath, true, true);
    return m_stats.lastLoad;
}

EditorLayoutPersistenceResult
EditorLayoutPersistenceService::SaveShutdownLayout(
    const EditorLayoutPersistenceDesc& desc)
{
    const std::filesystem::path layoutPath = ResolveLayoutPath(desc);
    if (!desc.enabled)
    {
        m_stats.lastSave = Skip(layoutPath);
        return m_stats.lastSave;
    }
    if (layoutPath.empty())
    {
        m_stats.lastSave = Skip(layoutPath);
        return m_stats.lastSave;
    }
    if (!desc.uiBackend || !desc.uiBackend->GetHost())
    {
        m_stats.lastSave = Skip(layoutPath);
        return m_stats.lastSave;
    }

    std::string error;
    if (!desc.uiBackend->GetHost()->SaveLayout(layoutPath, &error))
    {
        m_stats.lastSave = Fail(layoutPath, true, std::move(error));
        return m_stats.lastSave;
    }

    m_stats.lastSave = Complete(layoutPath, true, true);
    return m_stats.lastSave;
}

std::filesystem::path EditorLayoutPersistenceService::ResolveLayoutPath(
    const EditorLayoutPersistenceDesc& desc)
{
    if (!desc.layoutPathOverride.empty())
    {
        return desc.layoutPathOverride;
    }
    if (desc.settingsService)
    {
        return ResolveDefaultLayoutPath(*desc.settingsService);
    }
    return {};
}

EditorLayoutPersistenceResult EditorLayoutPersistenceService::Complete(
    std::filesystem::path path,
    bool attempted,
    bool succeeded)
{
    EditorLayoutPersistenceResult result;
    result.completed = true;
    result.attempted = attempted;
    result.succeeded = succeeded;
    result.layoutPath = std::move(path);
    return result;
}

EditorLayoutPersistenceResult EditorLayoutPersistenceService::Skip(
    std::filesystem::path path,
    bool missingLayout)
{
    EditorLayoutPersistenceResult result;
    result.completed = true;
    result.skipped = true;
    result.missingLayout = missingLayout;
    result.layoutPath = std::move(path);
    return result;
}

EditorLayoutPersistenceResult EditorLayoutPersistenceService::Fail(
    std::filesystem::path path,
    bool attempted,
    std::string error)
{
    EditorLayoutPersistenceResult result;
    result.completed = false;
    result.attempted = attempted;
    result.layoutPath = std::move(path);
    result.error = std::move(error);
    return result;
}

} // namespace RVX::Editor
