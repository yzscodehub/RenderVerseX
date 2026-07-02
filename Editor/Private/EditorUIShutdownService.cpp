/**
 * @file EditorUIShutdownService.cpp
 * @brief Editor UI backend and session service shutdown boundary.
 */

#include "Editor/EditorUIShutdownService.h"

#include "Editor/EditorDocument.h"
#include "Editor/EditorFileDialog.h"
#include "Editor/EditorNativePanelRegistrationService.h"
#include "Editor/EditorNotificationService.h"
#include "Editor/EditorPendingAction.h"
#include "Editor/EditorSettings.h"
#include "Editor/EditorShortcutProfileLifecycleService.h"
#include "Editor/EditorUnsavedChanges.h"
#include "Editor/UI/EditorShortcutProfileService.h"
#include "Editor/UI/IEditorUIBackend.h"

#include <utility>

namespace RVX::Editor
{
namespace
{
    template <typename T>
    bool Require(T* value, const char* name, std::string& error)
    {
        if (value)
        {
            return true;
        }

        error = name;
        error += " is unavailable";
        return false;
    }
} // namespace

EditorUIShutdownResult
EditorUIShutdownService::Shutdown(const EditorUIShutdownDesc& desc) const
{
    std::string missingDependency;
    if (!Validate(desc, missingDependency))
    {
        return Fail(std::move(missingDependency));
    }

    EditorUIShutdownResult result;
    EditorLayoutPersistenceDesc layoutDesc;
    layoutDesc.settingsService =
        desc.settingsService ? desc.settingsService->get() : nullptr;
    layoutDesc.uiBackend =
        desc.editorUIBackend ? desc.editorUIBackend->get() : nullptr;
    result.layoutSaveResult =
        desc.layoutPersistenceService->SaveShutdownLayout(layoutDesc);

    if (desc.shutdownBackend && *desc.editorUIBackend)
    {
        (*desc.editorUIBackend)->Shutdown();
        desc.editorUIBackend->reset();
        result.backendShutdown = true;
    }

    if (desc.detachServiceBindings)
    {
        desc.notificationService->SetCommandSurfaceModel(nullptr);
        desc.nativePanelRegistrationService->SetUIBackend(nullptr);
        desc.nativePanelRegistrationService->SetSelectionService(nullptr);
        desc.nativePanelRegistrationService->SetViewportToolService(nullptr);
        desc.nativePanelRegistrationService->SetSettingsService(nullptr);
        desc.nativePanelRegistrationService->SetShortcutProfileService(nullptr);
        desc.nativePanelRegistrationService->SetLegacyImGuiDebugState(nullptr,
                                                                      nullptr);
        desc.shortcutProfileLifecycleService->SetShortcutProfileService(
            nullptr);
        desc.shortcutProfileLifecycleService->SetSettingsService(nullptr);
        result.serviceBindingsDetached = true;
    }

    if (desc.releaseSessionServices)
    {
        result.pendingActionsReleased = desc.pendingActions->get() != nullptr;
        desc.pendingActions->reset();
        result.unsavedChangesGuardReleased =
            desc.unsavedChangesGuard->get() != nullptr;
        desc.unsavedChangesGuard->reset();
        result.shortcutProfileServiceReleased =
            desc.shortcutProfileService->get() != nullptr;
        desc.shortcutProfileService->reset();
        result.settingsServiceReleased = desc.settingsService->get() != nullptr;
        desc.settingsService->reset();
        result.fileDialogServiceReleased =
            desc.fileDialogService->get() != nullptr;
        desc.fileDialogService->reset();
        result.documentSessionReleased =
            desc.documentSession->get() != nullptr;
        desc.documentSession->reset();
    }

    result.completed = true;
    return result;
}

EditorUIShutdownResult EditorUIShutdownService::Fail(std::string error)
{
    EditorUIShutdownResult result;
    result.error = std::move(error);
    return result;
}

bool EditorUIShutdownService::Validate(const EditorUIShutdownDesc& desc,
                                       std::string& error)
{
    return Require(desc.editorUIBackend,
                   "Editor UI backend storage",
                   error) &&
           Require(desc.notificationService,
                   "Editor notification service",
                   error) &&
           Require(desc.layoutPersistenceService,
                   "Editor layout persistence service",
                   error) &&
           Require(desc.nativePanelRegistrationService,
                   "Editor native panel registration service",
                   error) &&
           Require(desc.shortcutProfileLifecycleService,
                   "Editor shortcut profile lifecycle service",
                   error) &&
           Require(desc.pendingActions,
                   "Editor pending action queue storage",
                   error) &&
           Require(desc.unsavedChangesGuard,
                   "Editor unsaved changes guard storage",
                   error) &&
           Require(desc.shortcutProfileService,
                   "Editor shortcut profile service storage",
                   error) &&
           Require(desc.settingsService,
                   "Editor settings service storage",
                   error) &&
           Require(desc.fileDialogService,
                   "Editor file dialog service storage",
                   error) &&
           Require(desc.documentSession,
                   "Editor document session storage",
                   error);
}

} // namespace RVX::Editor
