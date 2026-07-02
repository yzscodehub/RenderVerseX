/**
 * @file EditorServiceRegistrationService.cpp
 * @brief Editor service registry population boundary implementation
 */

#include "Editor/EditorServiceRegistrationService.h"
#include "Editor/EditorAutomationScenarioService.h"
#include "Editor/EditorBeginFrameService.h"
#include "Editor/EditorCommandBindingService.h"
#include "Editor/EditorCommandExecutionService.h"
#include "Editor/EditorDocument.h"
#include "Editor/EditorDocumentActionService.h"
#include "Editor/EditorDocumentCommandService.h"
#include "Editor/EditorDocumentFileCommandService.h"
#include "Editor/EditorEndFrameService.h"
#include "Editor/EditorFileDialog.h"
#include "Editor/EditorFrameCoordinator.h"
#include "Editor/EditorFrameLifecycleService.h"
#include "Editor/EditorFrameSubmissionService.h"
#include "Editor/EditorInputBridgeService.h"
#include "Editor/EditorLegacyDebugUIService.h"
#include "Editor/EditorLayoutPersistenceService.h"
#include "Editor/EditorMainFramePresentationService.h"
#include "Editor/EditorMainFramebufferService.h"
#include "Editor/EditorMainSwapChainService.h"
#include "Editor/EditorNativePanelRegistrationService.h"
#include "Editor/EditorNativeUIFrameService.h"
#include "Editor/EditorNativeUIRenderStatsService.h"
#include "Editor/EditorNativeUISubmissionService.h"
#include "Editor/EditorNotificationService.h"
#include "Editor/EditorOperationService.h"
#include "Editor/EditorPendingAction.h"
#include "Editor/EditorRenderBootstrapService.h"
#include "Editor/EditorRenderFrameService.h"
#include "Editor/EditorRenderShutdownService.h"
#include "Editor/EditorRunLoopService.h"
#include "Editor/EditorScreenshotRequestService.h"
#include "Editor/EditorScreenshotService.h"
#include "Editor/EditorSelectionService.h"
#include "Editor/EditorServiceRegistry.h"
#include "Editor/EditorSettings.h"
#include "Editor/EditorShortcutProfileLifecycleService.h"
#include "Editor/EditorUpdateService.h"
#include "Editor/EditorUIBootstrapService.h"
#include "Editor/EditorUIShutdownService.h"
#include "Editor/EditorUnsavedChanges.h"
#include "Editor/EditorViewportRenderService.h"
#include "Editor/EditorViewportToolService.h"
#include "Editor/EditorWindowFrameService.h"
#include "Editor/EditorWindowLifecycleService.h"
#include "Editor/UI/EditorShortcutProfileService.h"
#include "Editor/UI/EditorInputBridge.h"
#include "Editor/UI/IEditorUIBackend.h"
#include "Render/Context/RenderContext.h"
#include "Render/Renderer/SceneRenderer.h"

namespace RVX::Editor
{

namespace
{
    template <typename ServiceT>
    void RegisterIfAvailable(EditorServiceRegistry& registry,
                             ServiceT* service,
                             const char* debugName)
    {
        if (service)
        {
            registry.Register(*service, debugName);
        }
    }
} // namespace

EditorServiceRegistrationResult
EditorServiceRegistrationService::RegisterCoreServices(
    const EditorCoreServiceRegistrationDesc& desc) const
{
    EditorServiceRegistrationResult result;
    if (!desc.registry)
    {
        return result;
    }

    EditorServiceRegistry& registry = *desc.registry;
    registry.Clear();
    result.registryReady = true;

    RegisterIfAvailable(registry,
                        desc.serviceRegistrationService,
                        "EditorServiceRegistrationService");
    RegisterIfAvailable(registry, desc.frameCoordinator, "EditorFrameCoordinator");
    RegisterIfAvailable(registry,
                        desc.frameLifecycleService,
                        "EditorFrameLifecycleService");
    RegisterIfAvailable(registry,
                        desc.frameSubmissionService,
                        "EditorFrameSubmissionService");
    RegisterIfAvailable(registry,
                        desc.mainFramePresentationService,
                        "EditorMainFramePresentationService");
    RegisterIfAvailable(registry,
                        desc.mainFramebufferService,
                        "EditorMainFramebufferService");
    RegisterIfAvailable(registry,
                        desc.mainSwapChainService,
                        "EditorMainSwapChainService");
    RegisterIfAvailable(registry,
                        desc.nativeUIFrameService,
                        "EditorNativeUIFrameService");
    RegisterIfAvailable(registry,
                        desc.nativeUIRenderStatsService,
                        "EditorNativeUIRenderStatsService");
    RegisterIfAvailable(registry,
                        desc.nativePanelRegistrationService,
                        "EditorNativePanelRegistrationService");
    RegisterIfAvailable(registry,
                        desc.nativeUISubmissionService,
                        "EditorNativeUISubmissionService");
    RegisterIfAvailable(registry,
                        desc.beginFrameService,
                        "EditorBeginFrameService");
    RegisterIfAvailable(registry,
                        desc.commandBindingService,
                        "EditorCommandBindingService");
    RegisterIfAvailable(registry,
                        desc.commandExecutionService,
                        "EditorCommandExecutionService");
    RegisterIfAvailable(registry,
                        desc.documentActionService,
                        "EditorDocumentActionService");
    RegisterIfAvailable(registry,
                        desc.documentCommandService,
                        "EditorDocumentCommandService");
    RegisterIfAvailable(registry,
                        desc.documentFileCommandService,
                        "EditorDocumentFileCommandService");
    RegisterIfAvailable(registry,
                        desc.endFrameService,
                        "EditorEndFrameService");
    RegisterIfAvailable(registry,
                        desc.inputBridgeService,
                        "EditorInputBridgeService");
    RegisterIfAvailable(registry,
                        desc.legacyDebugUIService,
                        "EditorLegacyDebugUIService");
    RegisterIfAvailable(registry,
                        desc.layoutPersistenceService,
                        "EditorLayoutPersistenceService");
    RegisterIfAvailable(registry,
                        desc.notificationService,
                        "EditorNotificationService");
    RegisterIfAvailable(registry, desc.operationService, "EditorOperationService");
    RegisterIfAvailable(registry,
                        desc.renderBootstrapService,
                        "EditorRenderBootstrapService");
    RegisterIfAvailable(registry,
                        desc.renderFrameService,
                        "EditorRenderFrameService");
    RegisterIfAvailable(registry,
                        desc.renderShutdownService,
                        "EditorRenderShutdownService");
    RegisterIfAvailable(registry, desc.runLoopService, "EditorRunLoopService");
    RegisterIfAvailable(registry,
                        desc.screenshotRequestService,
                        "EditorScreenshotRequestService");
    RegisterIfAvailable(registry,
                        desc.screenshotService,
                        "EditorScreenshotService");
    RegisterIfAvailable(registry, desc.selectionService, "EditorSelectionService");
    RegisterIfAvailable(registry,
                        desc.automationScenarioService,
                        "EditorAutomationScenarioService");
    RegisterIfAvailable(registry,
                        desc.shortcutProfileLifecycleService,
                        "EditorShortcutProfileLifecycleService");
    RegisterIfAvailable(registry, desc.updateService, "EditorUpdateService");
    RegisterIfAvailable(registry,
                        desc.uiBootstrapService,
                        "EditorUIBootstrapService");
    RegisterIfAvailable(registry,
                        desc.uiShutdownService,
                        "EditorUIShutdownService");
    RegisterIfAvailable(registry,
                        desc.viewportRenderService,
                        "EditorViewportRenderService");
    RegisterIfAvailable(registry,
                        desc.viewportToolService,
                        "EditorViewportToolService");
    RegisterIfAvailable(registry,
                        desc.windowFrameService,
                        "EditorWindowFrameService");
    RegisterIfAvailable(registry,
                        desc.windowLifecycleService,
                        "EditorWindowLifecycleService");

    RegisterIfAvailable(registry, desc.renderContext, "RenderContext");
    RegisterIfAvailable(registry, desc.sceneRenderer, "SceneRenderer");
    RegisterIfAvailable(registry, desc.editorUIBackend, "EditorUIBackend");
    RegisterIfAvailable(registry, desc.editorInputBridge, "EditorInputBridge");
    RegisterIfAvailable(registry, desc.documentSession, "EditorDocumentSession");
    RegisterIfAvailable(registry,
                        desc.fileDialogService,
                        "EditorFileDialogService");
    RegisterIfAvailable(registry, desc.settingsService, "EditorSettingsService");
    RegisterIfAvailable(registry,
                        desc.shortcutProfileService,
                        "EditorShortcutProfileService");
    RegisterIfAvailable(registry,
                        desc.unsavedChangesGuard,
                        "EditorUnsavedChangesGuard");
    RegisterIfAvailable(registry,
                        desc.pendingActions,
                        "EditorPendingActionQueue");

    result.registeredCount = registry.GetServiceCount();
    return result;
}

void EditorServiceRegistrationService::ClearRegistry(
    EditorServiceRegistry& registry) const
{
    registry.Clear();
}

} // namespace RVX::Editor
