/**
 * @file EditorCommandBindingService.cpp
 * @brief Editor command catalog binding service implementation.
 */

#include "Editor/EditorCommandBindingService.h"
#include "Editor/EditorLayoutPersistenceService.h"
#include "Editor/EditorNotificationService.h"
#include "Editor/EditorOperationService.h"
#include "Editor/EditorSettings.h"
#include "Editor/EditorViewportToolService.h"
#include "Editor/UI/EditorCommandCatalog.h"
#include "Editor/UI/EditorCommandRegistry.h"
#include "Editor/UI/EditorCommandSurfaceModel.h"
#include "Editor/UI/EditorUIHost.h"
#include "Editor/UI/IEditorUIBackend.h"

#include <utility>

namespace RVX::Editor
{

bool EditorCommandBindingService::RegisterBuiltInCommands(
    EditorCommandRegistry& registry)
{
    EditorCommandCatalogCallbacks callbacks;
    callbacks.newScene = [this]() {
        ExecutePlan("New Scene", m_callbacks.newScene);
    };
    callbacks.openScene = [this]() {
        ExecutePlan("Open Scene", m_callbacks.openScene);
    };
    callbacks.saveScene = [this]() {
        ExecutePlan("Save Scene", m_callbacks.saveScene);
    };
    callbacks.saveSceneAs = [this]() {
        ExecutePlan("Save Scene As", m_callbacks.saveSceneAs);
    };
    callbacks.exit = [this]() {
        ExecutePlan("Exit", m_callbacks.exit);
    };
    callbacks.undo = [this]() {
        ExecuteOperation("Undo", [this]() {
            return m_operations ? m_operations->Undo()
                                : EditorOperationResult{
                                      EditorOperationStatus::Unavailable,
                                      "Undo failed: operation service is unavailable"};
        });
    };
    callbacks.redo = [this]() {
        ExecuteOperation("Redo", [this]() {
            return m_operations ? m_operations->Redo()
                                : EditorOperationResult{
                                      EditorOperationStatus::Unavailable,
                                      "Redo failed: operation service is unavailable"};
        });
    };
    callbacks.deleteSelection = [this]() {
        ExecuteOperation("Delete Selection", [this]() {
            return m_operations ? m_operations->DeleteSelection()
                                : EditorOperationResult{
                                      EditorOperationStatus::Unavailable,
                                      "Delete Selection failed: operation service is unavailable"};
        });
    };
    callbacks.preferences = [this]() {
        if (m_callbacks.preferences)
        {
            m_callbacks.preferences();
        }
    };
    callbacks.resetLayout = [this]() {
        ExecuteViewportToolCommand(
            "Reset Layout",
            [this]() {
                if (m_uiBackend)
                {
                    m_uiBackend->ResetLayout();
                    if (EditorCommandRegistry* commandRegistry =
                            m_uiBackend->GetCommandRegistry())
                    {
                        commandRegistry->SetCommandChecked(
                            EditorCommandIds::ViewToggleBottomDrawer,
                            false);
                    }
                }
            },
            "Editor layout reset");
    };
    callbacks.toggleBottomDrawer = [this]() {
        if (!m_uiBackend || !m_uiBackend->GetHost())
        {
            return;
        }

        EditorUIHost* host = m_uiBackend->GetHost();
        host->ToggleBottomDrawerCollapsed();
        if (EditorCommandRegistry* commandRegistry =
                m_uiBackend->GetCommandRegistry())
        {
            commandRegistry->SetCommandChecked(
                EditorCommandIds::ViewToggleBottomDrawer,
                host->IsBottomDrawerCollapsed());
        }
    };
    callbacks.commandPalette = [this]() {
        if (m_uiBackend)
        {
            m_uiBackend->OpenCommandPalette();
        }
    };
    callbacks.saveLayout = [this]() {
        ExecuteSaveLayoutCommand();
    };
    callbacks.reloadLayout = [this]() {
        ExecuteReloadLayoutCommand();
    };
    callbacks.createEmpty = [this]() {
        ExecuteOperation("Create Empty", [this]() {
            return m_operations
                       ? m_operations->CreateEmptyEntity("Entity")
                       : EditorOperationResult{
                             EditorOperationStatus::Unavailable,
                             "Create Empty failed: operation service is unavailable"};
        });
    };
    callbacks.gizmoTranslate = [this]() {
        ExecuteViewportToolCommand(
            "Translate Tool",
            [this]() {
                if (m_viewportTools)
                {
                    m_viewportTools->SetMode(EditorContext::GizmoMode::Translate);
                }
            },
            "Viewport tool set to Translate");
    };
    callbacks.gizmoRotate = [this]() {
        ExecuteViewportToolCommand(
            "Rotate Tool",
            [this]() {
                if (m_viewportTools)
                {
                    m_viewportTools->SetMode(EditorContext::GizmoMode::Rotate);
                }
            },
            "Viewport tool set to Rotate");
    };
    callbacks.gizmoScale = [this]() {
        ExecuteViewportToolCommand(
            "Scale Tool",
            [this]() {
                if (m_viewportTools)
                {
                    m_viewportTools->SetMode(EditorContext::GizmoMode::Scale);
                }
            },
            "Viewport tool set to Scale");
    };

    m_stats.catalogRegistered =
        RegisterEditorCommandCatalog(registry, callbacks);
    if (m_uiBackend && m_uiBackend->GetHost())
    {
        m_uiBackend->GetHost()->RefreshLayoutCommandStates();
    }
    m_stats.registeredCommandCount =
        static_cast<uint32>(registry.GetCommandCount());
    return m_stats.catalogRegistered;
}

bool EditorCommandBindingService::InitializeCommandSurfaces(
    EditorCommandSurfaceModel& surfaceModel)
{
    surfaceModel.BuildDefaultEditorSurfaces();
    if (m_notifications)
    {
        m_notifications->SetCommandSurfaceModel(&surfaceModel);
        m_notifications->SetMirrorToConsole(true);
        m_notifications->PublishInfo("Editor", "Ready");
    }
    m_stats.commandSurfacesInitialized = true;
    return true;
}

void EditorCommandBindingService::ExecutePlan(
    std::string title,
    const std::function<EditorCommandActionResult()>& action)
{
    if (!m_commands)
    {
        return;
    }

    m_commands->ExecutePlan({std::move(title), action});
}

void EditorCommandBindingService::ExecuteOperation(
    std::string title,
    std::function<EditorOperationResult()> operation)
{
    if (!m_commands)
    {
        return;
    }

    m_commands->ExecuteOperation(std::move(title), std::move(operation));
}

void EditorCommandBindingService::ExecuteViewportToolCommand(
    std::string title,
    std::function<void()> action,
    std::string successMessage)
{
    if (!m_commands)
    {
        return;
    }

    EditorCommandActionOptions options;
    options.refreshDocumentCommands = false;
    m_commands->ExecuteVoid(std::move(title),
                            std::move(action),
                            std::move(successMessage),
                            options);
}

void EditorCommandBindingService::ExecuteSaveLayoutCommand()
{
    if (!m_commands)
    {
        return;
    }

    EditorCommandActionOptions options;
    options.refreshDocumentCommands = false;
    m_commands->ExecutePlan(
        {"Save Layout",
         [this]() {
             if (!m_layoutPersistenceService)
             {
                 return m_commands->MakeUnavailable(
                     "Save Layout",
                     "Editor layout persistence service is unavailable");
             }

             const EditorLayoutPersistenceResult result =
                 m_layoutPersistenceService->SaveShutdownLayout(
                     BuildLayoutPersistenceDesc());
             return BuildLayoutPersistenceResult(
                 "Save Layout",
                 result,
                 "Editor layout saved",
                 "No editor layout was available to save");
         },
         options});
}

void EditorCommandBindingService::ExecuteReloadLayoutCommand()
{
    if (!m_commands)
    {
        return;
    }

    EditorCommandActionOptions options;
    options.refreshDocumentCommands = false;
    m_commands->ExecutePlan(
        {"Reload Saved Layout",
         [this]() {
             if (!m_layoutPersistenceService)
             {
                 return m_commands->MakeUnavailable(
                     "Reload Saved Layout",
                     "Editor layout persistence service is unavailable");
             }

             const EditorLayoutPersistenceResult result =
                 m_layoutPersistenceService->LoadStartupLayout(
                     BuildLayoutPersistenceDesc());
             return BuildLayoutPersistenceResult(
                 "Reload Saved Layout",
                 result,
                 "Saved editor layout reloaded",
                 "No saved editor layout was found");
         },
         options});
}

EditorCommandActionResult
EditorCommandBindingService::BuildLayoutPersistenceResult(
    std::string title,
    const EditorLayoutPersistenceResult& result,
    std::string successMessage,
    std::string missingLayoutMessage) const
{
    if (!m_commands)
    {
        return {};
    }

    if (result.succeeded)
    {
        return m_commands->MakeSucceeded(std::move(title),
                                         std::move(successMessage));
    }
    if (result.skipped && result.missingLayout)
    {
        return m_commands->MakeDeferred(std::move(title),
                                        std::move(missingLayoutMessage));
    }
    if (result.skipped)
    {
        std::string message =
            result.layoutPath.empty()
                ? "Editor layout path is unavailable"
                : "Native editor layout backend is unavailable";
        return m_commands->MakeUnavailable(std::move(title),
                                           std::move(message));
    }

    std::string message =
        result.error.empty() ? "Editor layout operation failed" : result.error;
    return m_commands->MakeFailed(std::move(title), std::move(message));
}

EditorLayoutPersistenceDesc
EditorCommandBindingService::BuildLayoutPersistenceDesc() const
{
    EditorLayoutPersistenceDesc desc;
    desc.settingsService = m_settingsService;
    desc.uiBackend = m_uiBackend;
    return desc;
}

} // namespace RVX::Editor
