/**
 * @file EditorCommandBindingService.h
 * @brief Editor command catalog binding service.
 */

#pragma once

#include "Core/Types.h"
#include "Editor/EditorCommandExecutionService.h"

#include <functional>
#include <string>
#include <utility>

namespace RVX::Editor
{

class EditorCommandRegistry;
class EditorCommandSurfaceModel;
class EditorLayoutPersistenceService;
class EditorNotificationService;
class EditorOperationService;
class EditorSettingsService;
class EditorViewportToolService;
class IEditorUIBackend;
struct EditorLayoutPersistenceDesc;
struct EditorLayoutPersistenceResult;

struct EditorCommandBindingCallbacks
{
    std::function<EditorCommandActionResult()> newScene;
    std::function<EditorCommandActionResult()> openScene;
    std::function<EditorCommandActionResult()> saveScene;
    std::function<EditorCommandActionResult()> saveSceneAs;
    std::function<EditorCommandActionResult()> exit;
    std::function<void()> preferences;
};

struct EditorCommandBindingStats
{
    bool catalogRegistered = false;
    bool commandSurfacesInitialized = false;
    uint32 registeredCommandCount = 0;
};

/**
 * @brief Binds built-in editor commands to shared command services.
 */
class EditorCommandBindingService
{
public:
    // =========================================================================
    // Dependencies
    // =========================================================================

    void SetCommandExecutionService(EditorCommandExecutionService* commands)
    {
        m_commands = commands;
    }
    void SetOperationService(EditorOperationService* operations)
    {
        m_operations = operations;
    }
    void SetViewportToolService(EditorViewportToolService* viewportTools)
    {
        m_viewportTools = viewportTools;
    }
    void SetUIBackend(IEditorUIBackend* uiBackend)
    {
        m_uiBackend = uiBackend;
    }
    void SetSettingsService(EditorSettingsService* settingsService)
    {
        m_settingsService = settingsService;
    }
    void SetLayoutPersistenceService(
        EditorLayoutPersistenceService* layoutPersistenceService)
    {
        m_layoutPersistenceService = layoutPersistenceService;
    }
    void SetNotificationService(EditorNotificationService* notifications)
    {
        m_notifications = notifications;
    }
    void SetCallbacks(EditorCommandBindingCallbacks callbacks)
    {
        m_callbacks = std::move(callbacks);
    }

    // =========================================================================
    // Binding
    // =========================================================================

    bool RegisterBuiltInCommands(EditorCommandRegistry& registry);
    bool InitializeCommandSurfaces(EditorCommandSurfaceModel& surfaceModel);

    const EditorCommandBindingStats& GetStats() const { return m_stats; }

private:
    void ExecutePlan(std::string title,
                     const std::function<EditorCommandActionResult()>& action);
    void ExecuteOperation(std::string title,
                          std::function<EditorOperationResult()> operation);
    void ExecuteViewportToolCommand(std::string title,
                                    std::function<void()> action,
                                    std::string successMessage);
    void ExecuteSaveLayoutCommand();
    void ExecuteReloadLayoutCommand();
    EditorCommandActionResult BuildLayoutPersistenceResult(
        std::string title,
        const EditorLayoutPersistenceResult& result,
        std::string successMessage,
        std::string missingLayoutMessage) const;
    EditorLayoutPersistenceDesc BuildLayoutPersistenceDesc() const;

    EditorCommandExecutionService* m_commands = nullptr;
    EditorOperationService* m_operations = nullptr;
    EditorViewportToolService* m_viewportTools = nullptr;
    IEditorUIBackend* m_uiBackend = nullptr;
    EditorSettingsService* m_settingsService = nullptr;
    EditorLayoutPersistenceService* m_layoutPersistenceService = nullptr;
    EditorNotificationService* m_notifications = nullptr;
    EditorCommandBindingCallbacks m_callbacks;
    EditorCommandBindingStats m_stats;
};

} // namespace RVX::Editor
