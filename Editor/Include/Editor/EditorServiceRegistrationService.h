/**
 * @file EditorServiceRegistrationService.h
 * @brief Editor service registry population boundary
 */

#pragma once

#include "Core/Types.h"

namespace RVX
{
    class RenderContext;
    class SceneRenderer;
}

namespace RVX::Editor
{

class EditorAutomationScenarioService;
class EditorBeginFrameService;
class EditorCommandBindingService;
class EditorCommandExecutionService;
class EditorDocumentActionService;
class EditorDocumentCommandService;
class EditorDocumentFileCommandService;
class EditorDocumentSession;
class EditorEndFrameService;
class EditorFileDialogService;
class EditorFrameCoordinator;
class EditorFrameLifecycleService;
class EditorFrameSubmissionService;
class EditorInputBridge;
class EditorInputBridgeService;
class EditorLegacyDebugUIService;
class EditorLayoutPersistenceService;
class EditorMainFramePresentationService;
class EditorMainFramebufferService;
class EditorMainSwapChainService;
class EditorNativePanelRegistrationService;
class EditorNativeUIFrameService;
class EditorNativeUIRenderStatsService;
class EditorNativeUISubmissionService;
class EditorNotificationService;
class EditorOperationService;
class EditorPendingActionQueue;
class EditorRenderBootstrapService;
class EditorRenderFrameService;
class EditorRenderShutdownService;
class EditorRunLoopService;
class EditorScreenshotRequestService;
class EditorScreenshotService;
class EditorSelectionService;
class EditorServiceRegistrationService;
class EditorServiceRegistry;
class EditorSettingsService;
class EditorShortcutProfileService;
class EditorShortcutProfileLifecycleService;
class EditorUpdateService;
class EditorUIBootstrapService;
class EditorUIShutdownService;
class EditorUnsavedChangesGuard;
class EditorViewportRenderService;
class EditorViewportToolService;
class EditorWindowFrameService;
class EditorWindowLifecycleService;
class IEditorUIBackend;

struct EditorCoreServiceRegistrationDesc
{
    EditorServiceRegistry* registry = nullptr;
    EditorServiceRegistrationService* serviceRegistrationService = nullptr;
    EditorFrameCoordinator* frameCoordinator = nullptr;
    EditorFrameLifecycleService* frameLifecycleService = nullptr;
    EditorFrameSubmissionService* frameSubmissionService = nullptr;
    EditorMainFramePresentationService* mainFramePresentationService = nullptr;
    EditorMainFramebufferService* mainFramebufferService = nullptr;
    EditorMainSwapChainService* mainSwapChainService = nullptr;
    EditorNativeUIFrameService* nativeUIFrameService = nullptr;
    EditorNativeUIRenderStatsService* nativeUIRenderStatsService = nullptr;
    EditorNativePanelRegistrationService* nativePanelRegistrationService =
        nullptr;
    EditorNativeUISubmissionService* nativeUISubmissionService = nullptr;
    EditorBeginFrameService* beginFrameService = nullptr;
    EditorCommandBindingService* commandBindingService = nullptr;
    EditorCommandExecutionService* commandExecutionService = nullptr;
    EditorDocumentActionService* documentActionService = nullptr;
    EditorDocumentCommandService* documentCommandService = nullptr;
    EditorDocumentFileCommandService* documentFileCommandService = nullptr;
    EditorEndFrameService* endFrameService = nullptr;
    EditorInputBridgeService* inputBridgeService = nullptr;
    EditorLegacyDebugUIService* legacyDebugUIService = nullptr;
    EditorLayoutPersistenceService* layoutPersistenceService = nullptr;
    EditorNotificationService* notificationService = nullptr;
    EditorOperationService* operationService = nullptr;
    EditorRenderBootstrapService* renderBootstrapService = nullptr;
    EditorRenderFrameService* renderFrameService = nullptr;
    EditorRenderShutdownService* renderShutdownService = nullptr;
    EditorRunLoopService* runLoopService = nullptr;
    EditorScreenshotRequestService* screenshotRequestService = nullptr;
    EditorScreenshotService* screenshotService = nullptr;
    EditorSelectionService* selectionService = nullptr;
    EditorAutomationScenarioService* automationScenarioService = nullptr;
    EditorShortcutProfileLifecycleService* shortcutProfileLifecycleService =
        nullptr;
    EditorUpdateService* updateService = nullptr;
    EditorUIBootstrapService* uiBootstrapService = nullptr;
    EditorUIShutdownService* uiShutdownService = nullptr;
    EditorViewportRenderService* viewportRenderService = nullptr;
    EditorViewportToolService* viewportToolService = nullptr;
    EditorWindowFrameService* windowFrameService = nullptr;
    EditorWindowLifecycleService* windowLifecycleService = nullptr;

    RenderContext* renderContext = nullptr;
    SceneRenderer* sceneRenderer = nullptr;
    IEditorUIBackend* editorUIBackend = nullptr;
    EditorInputBridge* editorInputBridge = nullptr;
    EditorDocumentSession* documentSession = nullptr;
    EditorFileDialogService* fileDialogService = nullptr;
    EditorSettingsService* settingsService = nullptr;
    EditorShortcutProfileService* shortcutProfileService = nullptr;
    EditorUnsavedChangesGuard* unsavedChangesGuard = nullptr;
    EditorPendingActionQueue* pendingActions = nullptr;
};

struct EditorServiceRegistrationResult
{
    uint32 registeredCount = 0;
    bool registryReady = false;
};

class EditorServiceRegistrationService
{
public:
    // =========================================================================
    // Public Methods
    // =========================================================================
    EditorServiceRegistrationResult RegisterCoreServices(
        const EditorCoreServiceRegistrationDesc& desc) const;

    void ClearRegistry(EditorServiceRegistry& registry) const;
};

} // namespace RVX::Editor
