/**
 * @file EditorUIBootstrapService.cpp
 * @brief Native editor UI startup dependency assembly service.
 */

#include "Editor/EditorUIBootstrapService.h"

#include "Core/Log.h"
#include "Editor/EditorAutomationScenarioService.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorDocument.h"
#include "Editor/EditorDocumentActionService.h"
#include "Editor/EditorDocumentCommandService.h"
#include "Editor/EditorDocumentFileCommandService.h"
#include "Editor/EditorFileDialog.h"
#include "Editor/EditorNativePanelRegistrationService.h"
#include "Editor/EditorNotificationService.h"
#include "Editor/EditorOperationService.h"
#include "Editor/EditorPendingAction.h"
#include "Editor/EditorRunLoopService.h"
#include "Editor/EditorSelectionService.h"
#include "Editor/EditorSettings.h"
#include "Editor/EditorShortcutProfileLifecycleService.h"
#include "Editor/EditorUnsavedChanges.h"
#include "Editor/EditorViewportToolService.h"
#include "Editor/UI/EditorCommandRegistry.h"
#include "Editor/UI/EditorCommandSurfaceModel.h"
#include "Editor/UI/EditorShortcutProfileService.h"
#include "Editor/UI/EditorUIBackendFactory.h"
#include "Editor/UI/IEditorUIBackend.h"
#include "UI/UIRenderer.h"

#include <memory>
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

    EditorUIBackendType ResolveBootstrapBackendType(
        const EditorUIBootstrapDesc& desc,
        const EditorSettingsService& settings)
    {
        if (desc.overrideUIBackendType)
        {
            return desc.uiBackendType;
        }

        return settings.GetUIBackendType();
    }

    UI::UIFontFallbackChainDesc BuildFontFallbackDesc(
        const EditorSettingsService& settings)
    {
        UI::UIFontFallbackChainDesc fontDesc;
        fontDesc.appendDefaultSystemFonts =
            settings.ShouldAppendDefaultSystemFonts();
        for (const std::filesystem::path& fontPath : settings.GetUIFontPaths())
        {
            if (!fontPath.empty())
            {
                fontDesc.fontPaths.push_back(fontPath.string());
            }
        }
        return fontDesc;
    }
} // namespace

EditorUIBootstrapResult
EditorUIBootstrapService::Bootstrap(const EditorUIBootstrapDesc& desc) const
{
    std::string missingDependency;
    if (!Require(desc.settingsService,
                 "Editor settings service storage",
                 missingDependency) ||
        !Require(desc.documentSession,
                 "Editor document session storage",
                 missingDependency) ||
        !Require(desc.fileDialogService,
                 "Editor file dialog service storage",
                 missingDependency) ||
        !Require(desc.shortcutProfileService,
                 "Editor shortcut profile service storage",
                 missingDependency) ||
        !Require(desc.unsavedChangesGuard,
                 "Editor unsaved changes guard storage",
                 missingDependency) ||
        !Require(desc.pendingActions,
                 "Editor pending action queue storage",
                 missingDependency) ||
        !Require(desc.editorUIBackend,
                 "Editor UI backend storage",
                 missingDependency) ||
        !Require(desc.runLoopService,
                 "Editor run-loop service",
                 missingDependency) ||
        !Require(desc.shortcutProfileLifecycleService,
                 "Editor shortcut profile lifecycle service",
                 missingDependency) ||
        !Require(desc.automationScenarioService,
                 "Editor automation scenario service",
                 missingDependency) ||
        !Require(desc.documentActionService,
                 "Editor document action service",
                 missingDependency) ||
        !Require(desc.commandExecutionService,
                 "Editor command execution service",
                 missingDependency) ||
        !Require(desc.documentCommandService,
                 "Editor document command service",
                 missingDependency) ||
        !Require(desc.operationService,
                 "Editor operation service",
                 missingDependency) ||
        !Require(desc.selectionService,
                 "Editor selection service",
                 missingDependency) ||
        !Require(desc.documentFileCommandService,
                 "Editor document file command service",
                 missingDependency) ||
        !Require(desc.commandBindingService,
                 "Editor command binding service",
                 missingDependency) ||
        !Require(desc.layoutPersistenceService,
                 "Editor layout persistence service",
                 missingDependency) ||
        !Require(desc.viewportToolService,
                 "Editor viewport tool service",
                 missingDependency) ||
        !Require(desc.notificationService,
                 "Editor notification service",
                 missingDependency) ||
        !Require(desc.nativePanelRegistrationService,
                 "Editor native panel registration service",
                 missingDependency))
    {
        return Fail(std::move(missingDependency));
    }

    EditorUIBootstrapResult result;
    if (!*desc.settingsService)
    {
        *desc.settingsService =
            desc.settingsPathOverride.empty()
                ? std::make_unique<EditorSettingsService>()
                : std::make_unique<EditorSettingsService>(
                      desc.settingsPathOverride);
        result.settingsLoadAttempted = true;
        result.settingsLoaded = (*desc.settingsService)->Load();
        if (!result.settingsLoaded)
        {
            RVX_CORE_WARN("Editor settings load failed: {}",
                          (*desc.settingsService)->GetLastError());
        }
    }
    else
    {
        result.settingsLoaded = true;
    }

    EditorSettingsService* settings = desc.settingsService->get();
    std::string fontConfigurationError;
    result.fontConfigurationApplied =
        UI::UIFontFallbackChain::ConfigureDefault(
            BuildFontFallbackDesc(*settings),
            &fontConfigurationError);
    result.fontDiagnostics =
        UI::UIFontFallbackChain::GetDefaultDiagnostics();
    if (!result.fontConfigurationApplied)
    {
        RVX_CORE_WARN("Editor font configuration failed: {}",
                      fontConfigurationError);
    }
    else
    {
        RVX_CORE_INFO("Editor font chain configured: {} fonts, {} missing core glyphs",
                      result.fontDiagnostics.fontCount,
                      result.fontDiagnostics.missingCoreCodepointCount);
    }

    if (!*desc.documentSession)
    {
        EditorContext& context =
            desc.context ? *desc.context : EditorContext::Get();
        *desc.documentSession =
            std::make_unique<EditorDocumentSession>(context);
    }
    if (!*desc.fileDialogService)
    {
        *desc.fileDialogService =
            std::make_unique<EditorFileDialogService>();
    }
    if (!*desc.shortcutProfileService)
    {
        *desc.shortcutProfileService =
            std::make_unique<EditorShortcutProfileService>(settings);
    }
    else
    {
        (*desc.shortcutProfileService)->SetSettingsService(settings);
    }
    if (!desc.shortcutProfilePathOverride.empty())
    {
        (*desc.shortcutProfileService)
            ->SetProfilePathOverride(desc.shortcutProfilePathOverride);
    }

    desc.runLoopService->Configure({desc.maxFrames, desc.screenshotPath});
    desc.shortcutProfileLifecycleService->SetSettingsService(settings);
    desc.shortcutProfileLifecycleService->SetShortcutProfileService(
        desc.shortcutProfileService->get());
    desc.automationScenarioService->Configure(desc.automationScenario,
                                              desc.automationTarget,
                                              desc.automationConfirmOverwrite);
    result.automationSettingsPrepared =
        desc.automationScenarioService->PrepareSettings(settings);
    if (!result.automationSettingsPrepared)
    {
        return Fail("Editor automation scenario settings preparation failed");
    }
    desc.shortcutProfileLifecycleService->ConfigureAutosave();

    if (!*desc.unsavedChangesGuard)
    {
        *desc.unsavedChangesGuard =
            std::make_unique<EditorUnsavedChangesGuard>(
                **desc.documentSession,
                **desc.fileDialogService);
    }
    if (!*desc.pendingActions)
    {
        *desc.pendingActions = std::make_unique<EditorPendingActionQueue>();
    }

    desc.documentActionService->SetDocumentSession(
        desc.documentSession->get());
    desc.documentActionService->SetUnsavedChangesGuard(
        desc.unsavedChangesGuard->get());
    desc.documentActionService->SetPendingActions(
        desc.pendingActions->get());
    desc.commandExecutionService->SetNotificationService(
        desc.notificationService);
    desc.commandExecutionService->SetRefreshCallback(
        desc.refreshDocumentCommandStates);
    desc.documentCommandService->SetCommandExecutionService(
        desc.commandExecutionService);
    desc.documentCommandService->SetDocumentActionService(
        desc.documentActionService);
    desc.operationService->SetDocumentSession(desc.documentSession->get());
    desc.operationService->SetSelectionService(desc.selectionService);
    desc.documentFileCommandService->SetCommandExecutionService(
        desc.commandExecutionService);
    desc.documentFileCommandService->SetDocumentActionService(
        desc.documentActionService);
    desc.documentFileCommandService->SetDocumentSession(
        desc.documentSession->get());
    desc.documentFileCommandService->SetFileDialogService(
        desc.fileDialogService->get());
    desc.documentFileCommandService->SetNotificationService(
        desc.notificationService);
    desc.documentFileCommandService->SetOperationService(
        desc.operationService);
    desc.documentFileCommandService->SetSettingsService(settings);
    desc.documentFileCommandService->SetRefreshCallback(
        desc.refreshDocumentCommandStates);
    desc.commandBindingService->SetCommandExecutionService(
        desc.commandExecutionService);
    desc.commandBindingService->SetOperationService(desc.operationService);
    desc.commandBindingService->SetViewportToolService(
        desc.viewportToolService);
    desc.commandBindingService->SetSettingsService(settings);
    desc.commandBindingService->SetLayoutPersistenceService(
        desc.layoutPersistenceService);
    desc.commandBindingService->SetNotificationService(
        desc.notificationService);

    EditorUIBackendFactoryDesc backendDesc;
    backendDesc.type = ResolveBootstrapBackendType(desc, *settings);
    backendDesc.renderDevice = desc.renderDevice;
    backendDesc.surfaceWidth = desc.surfaceWidth;
    backendDesc.surfaceHeight = desc.surfaceHeight;
    backendDesc.scaleFactor =
        settings->GetUIScaleFactor() * desc.surfaceScaleFactor;
    backendDesc.debugName = desc.backendDebugName;

    RVX_CORE_INFO("Initializing editor UI backend '{}'",
                  ToString(backendDesc.type));

    EditorUIBackendFactoryResult backendResult =
        CreateEditorUIBackend(backendDesc);
    if (!backendResult)
    {
        std::string error = "Failed to create editor UI backend '";
        error += ToString(backendDesc.type);
        error += "': ";
        error += backendResult.error;
        return Fail(std::move(error));
    }
    *desc.editorUIBackend = std::move(backendResult.backend);
    result.backendCreated = true;

    IEditorUIBackend* uiBackend = desc.editorUIBackend->get();
    desc.commandBindingService->SetUIBackend(uiBackend);
    desc.nativePanelRegistrationService->SetUIBackend(uiBackend);
    desc.nativePanelRegistrationService->SetSelectionService(
        desc.selectionService);
    desc.nativePanelRegistrationService->SetViewportToolService(
        desc.viewportToolService);
    desc.nativePanelRegistrationService->SetSettingsService(settings);
    desc.nativePanelRegistrationService->SetShortcutProfileService(
        desc.shortcutProfileService->get());
    desc.nativePanelRegistrationService->SetLegacyImGuiDebugExposure(
        desc.exposeLegacyImGuiDebugCommands);
    desc.nativePanelRegistrationService->SetLegacyImGuiDebugState(
        desc.showDemoWindow,
        desc.showMetricsWindow);

    desc.commandBindingService->SetCallbacks(desc.commandCallbacks);

    EditorCommandRegistry* commandRegistry =
        uiBackend ? uiBackend->GetCommandRegistry() : nullptr;
    result.commandCatalogRegistered =
        commandRegistry &&
        desc.commandBindingService->RegisterBuiltInCommands(*commandRegistry);
    if (!result.commandCatalogRegistered)
    {
        RVX_CORE_WARN("Editor command catalog registration was incomplete");
    }
    if (commandRegistry)
    {
        desc.shortcutProfileLifecycleService->LoadStartupProfile(
            *commandRegistry);
    }
    if (EditorCommandSurfaceModel* surfaceModel =
            uiBackend ? uiBackend->GetCommandSurfaceModel() : nullptr)
    {
        result.commandSurfacesInitialized =
            desc.commandBindingService->InitializeCommandSurfaces(
                *surfaceModel);
    }
    if (desc.refreshDocumentCommandStates)
    {
        desc.refreshDocumentCommandStates();
    }

    EditorLayoutPersistenceDesc layoutDesc;
    layoutDesc.settingsService = settings;
    layoutDesc.uiBackend = uiBackend;
    result.layoutLoadResult =
        desc.layoutPersistenceService->LoadStartupLayout(layoutDesc);
    if (!result.layoutLoadResult)
    {
        RVX_CORE_WARN("Editor layout load failed: {}",
                      result.layoutLoadResult.error);
    }

    result.initialized = true;
    return result;
}

EditorUIBootstrapResult EditorUIBootstrapService::Fail(std::string error)
{
    EditorUIBootstrapResult result;
    result.error = std::move(error);
    return result;
}

} // namespace RVX::Editor
