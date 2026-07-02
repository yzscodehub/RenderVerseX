/**
 * @file EditorUIShutdownService.h
 * @brief Editor UI backend and session service shutdown boundary.
 */

#pragma once

#include "Editor/EditorLayoutPersistenceService.h"

#include <memory>
#include <string>

namespace RVX::Editor
{

class EditorDocumentSession;
class EditorFileDialogService;
class EditorNativePanelRegistrationService;
class EditorNotificationService;
class EditorPendingActionQueue;
class EditorSettingsService;
class EditorShortcutProfileService;
class EditorShortcutProfileLifecycleService;
class EditorUnsavedChangesGuard;
class IEditorUIBackend;

struct EditorUIShutdownDesc
{
    std::unique_ptr<IEditorUIBackend>* editorUIBackend = nullptr;
    EditorNotificationService* notificationService = nullptr;
    EditorLayoutPersistenceService* layoutPersistenceService = nullptr;
    EditorNativePanelRegistrationService* nativePanelRegistrationService =
        nullptr;
    EditorShortcutProfileLifecycleService* shortcutProfileLifecycleService =
        nullptr;
    std::unique_ptr<EditorPendingActionQueue>* pendingActions = nullptr;
    std::unique_ptr<EditorUnsavedChangesGuard>* unsavedChangesGuard = nullptr;
    std::unique_ptr<EditorShortcutProfileService>* shortcutProfileService =
        nullptr;
    std::unique_ptr<EditorSettingsService>* settingsService = nullptr;
    std::unique_ptr<EditorFileDialogService>* fileDialogService = nullptr;
    std::unique_ptr<EditorDocumentSession>* documentSession = nullptr;

    bool shutdownBackend = true;
    bool detachServiceBindings = true;
    bool releaseSessionServices = true;
};

struct EditorUIShutdownResult
{
    bool completed = false;
    EditorLayoutPersistenceResult layoutSaveResult;
    bool backendShutdown = false;
    bool serviceBindingsDetached = false;
    bool pendingActionsReleased = false;
    bool unsavedChangesGuardReleased = false;
    bool shortcutProfileServiceReleased = false;
    bool settingsServiceReleased = false;
    bool fileDialogServiceReleased = false;
    bool documentSessionReleased = false;
    std::string error;

    explicit operator bool() const { return completed; }
};

/**
 * @brief Owns shutdown of editor UI backend bindings and session services.
 */
class EditorUIShutdownService
{
public:
    // =========================================================================
    // Public Methods
    // =========================================================================

    EditorUIShutdownResult Shutdown(const EditorUIShutdownDesc& desc) const;

private:
    static EditorUIShutdownResult Fail(std::string error);
    static bool Validate(const EditorUIShutdownDesc& desc, std::string& error);
};

} // namespace RVX::Editor
