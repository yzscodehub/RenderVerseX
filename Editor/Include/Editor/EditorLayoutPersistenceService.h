/**
 * @file EditorLayoutPersistenceService.h
 * @brief Native editor workspace layout persistence lifecycle service.
 */

#pragma once

#include <filesystem>
#include <string>

namespace RVX::Editor
{

class EditorSettingsService;
class IEditorUIBackend;

struct EditorLayoutPersistenceDesc
{
    EditorSettingsService* settingsService = nullptr;
    IEditorUIBackend* uiBackend = nullptr;
    std::filesystem::path layoutPathOverride;
    bool enabled = true;
};

struct EditorLayoutPersistenceResult
{
    bool completed = false;
    bool attempted = false;
    bool succeeded = false;
    bool skipped = false;
    bool missingLayout = false;
    std::filesystem::path layoutPath;
    std::string error;

    explicit operator bool() const { return completed; }
};

struct EditorLayoutPersistenceStats
{
    EditorLayoutPersistenceResult lastLoad;
    EditorLayoutPersistenceResult lastSave;
};

/**
 * @brief Loads and saves the native editor dock layout around UI lifetime.
 */
class EditorLayoutPersistenceService
{
public:
    // =========================================================================
    // Path Resolution
    // =========================================================================

    static std::filesystem::path ResolveDefaultLayoutPath(
        const EditorSettingsService& settingsService);
    static std::filesystem::path ResolveDefaultLayoutPath(
        const std::filesystem::path& settingsPath);

    // =========================================================================
    // Persistence
    // =========================================================================

    EditorLayoutPersistenceResult LoadStartupLayout(
        const EditorLayoutPersistenceDesc& desc);
    EditorLayoutPersistenceResult SaveShutdownLayout(
        const EditorLayoutPersistenceDesc& desc);

    const EditorLayoutPersistenceStats& GetStats() const { return m_stats; }

private:
    static std::filesystem::path ResolveLayoutPath(
        const EditorLayoutPersistenceDesc& desc);
    static EditorLayoutPersistenceResult Complete(
        std::filesystem::path path,
        bool attempted,
        bool succeeded);
    static EditorLayoutPersistenceResult Skip(std::filesystem::path path,
                                              bool missingLayout = false);
    static EditorLayoutPersistenceResult Fail(std::filesystem::path path,
                                              bool attempted,
                                              std::string error);

    EditorLayoutPersistenceStats m_stats;
};

} // namespace RVX::Editor
