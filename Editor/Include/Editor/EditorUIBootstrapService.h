/**
 * @file EditorUIBootstrapService.h
 * @brief Native editor UI startup dependency assembly service.
 */

#pragma once

#include "Core/Types.h"
#include "Editor/EditorAutomation.h"
#include "Editor/EditorCommandBindingService.h"
#include "Editor/EditorLayoutPersistenceService.h"
#include "Editor/UI/EditorUIBackendCatalog.h"
#include "UI/UIRenderer.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace RVX
{
class IRHIDevice;
} // namespace RVX

namespace RVX::Editor
{

class EditorAutomationScenarioService;
class EditorCommandExecutionService;
class EditorContext;
class EditorDocumentActionService;
class EditorDocumentCommandService;
class EditorDocumentFileCommandService;
class EditorDocumentSession;
class EditorFileDialogService;
class EditorNativePanelRegistrationService;
class EditorNotificationService;
class EditorOperationService;
class EditorPendingActionQueue;
class EditorRunLoopService;
class EditorSelectionService;
class EditorSettingsService;
class EditorShortcutProfileLifecycleService;
class EditorShortcutProfileService;
class EditorUnsavedChangesGuard;
class EditorViewportToolService;
class IEditorUIBackend;

struct EditorUIBootstrapDesc
{
    EditorContext* context = nullptr;

    std::unique_ptr<EditorSettingsService>* settingsService = nullptr;
    std::unique_ptr<EditorDocumentSession>* documentSession = nullptr;
    std::unique_ptr<EditorFileDialogService>* fileDialogService = nullptr;
    std::unique_ptr<EditorShortcutProfileService>* shortcutProfileService =
        nullptr;
    std::unique_ptr<EditorUnsavedChangesGuard>* unsavedChangesGuard = nullptr;
    std::unique_ptr<EditorPendingActionQueue>* pendingActions = nullptr;
    std::unique_ptr<IEditorUIBackend>* editorUIBackend = nullptr;

    EditorRunLoopService* runLoopService = nullptr;
    EditorShortcutProfileLifecycleService* shortcutProfileLifecycleService =
        nullptr;
    EditorAutomationScenarioService* automationScenarioService = nullptr;
    EditorDocumentActionService* documentActionService = nullptr;
    EditorCommandExecutionService* commandExecutionService = nullptr;
    EditorDocumentCommandService* documentCommandService = nullptr;
    EditorOperationService* operationService = nullptr;
    EditorSelectionService* selectionService = nullptr;
    EditorDocumentFileCommandService* documentFileCommandService = nullptr;
    EditorCommandBindingService* commandBindingService = nullptr;
    EditorLayoutPersistenceService* layoutPersistenceService = nullptr;
    EditorViewportToolService* viewportToolService = nullptr;
    EditorNotificationService* notificationService = nullptr;
    EditorNativePanelRegistrationService* nativePanelRegistrationService =
        nullptr;

    std::function<void()> refreshDocumentCommandStates;
    EditorCommandBindingCallbacks commandCallbacks;

    std::filesystem::path settingsPathOverride;
    std::filesystem::path shortcutProfilePathOverride;
    uint32 maxFrames = 0;
    std::string screenshotPath;
    EditorAutomationScenarioType automationScenario =
        EditorAutomationScenarioType::None;
    std::string automationTarget;
    bool automationConfirmOverwrite = false;

    EditorUIBackendType uiBackendType = GetDefaultEditorUIBackendType();
    bool overrideUIBackendType = false;
    IRHIDevice* renderDevice = nullptr;
    uint32 surfaceWidth = 0;
    uint32 surfaceHeight = 0;
    float surfaceScaleFactor = 1.0f;
    std::string backendDebugName = "RenderVerseX.EditorUI";

    bool exposeLegacyImGuiDebugCommands = false;
    bool* showDemoWindow = nullptr;
    bool* showMetricsWindow = nullptr;
};

struct EditorUIBootstrapResult
{
    bool initialized = false;
    bool settingsLoadAttempted = false;
    bool settingsLoaded = false;
    bool automationSettingsPrepared = false;
    bool fontConfigurationApplied = false;
    UI::UIFontFallbackChainDiagnostics fontDiagnostics;
    bool backendCreated = false;
    bool commandCatalogRegistered = false;
    bool commandSurfacesInitialized = false;
    EditorLayoutPersistenceResult layoutLoadResult;
    std::string error;

    explicit operator bool() const { return initialized; }
};

/**
 * @brief Builds native editor UI runtime dependencies and backend surfaces.
 */
class EditorUIBootstrapService
{
public:
    // =========================================================================
    // Lifecycle
    // =========================================================================

    EditorUIBootstrapResult Bootstrap(
        const EditorUIBootstrapDesc& desc) const;

private:
    static EditorUIBootstrapResult Fail(std::string error);
};

} // namespace RVX::Editor
