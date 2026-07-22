/**
 * @file EditorApplication.cpp
 * @brief Editor application implementation
 */

#include "Editor/EditorApplication.h"
#include "Editor/EditorDocument.h"
#include "Editor/EditorFileDialog.h"
#include "Editor/EditorPendingAction.h"
#include "Editor/EditorSettings.h"
#include "Editor/EditorTheme.h"
#include "Editor/EditorUnsavedChanges.h"
#include "Editor/UI/EditorCommandCatalog.h"
#include "Editor/UI/EditorCommandRegistry.h"
#include "Editor/UI/EditorCommandSurfaceModel.h"
#include "Editor/UI/EditorFilePickerDialog.h"
#include "Editor/UI/EditorInputBridge.h"
#include "Editor/UI/EditorModalDialog.h"
#include "Editor/UI/EditorShortcutProfileService.h"
#include "Editor/UI/IEditorUIBackend.h"
#if RVX_EDITOR_ENABLE_LEGACY_IMGUI
#include "Editor/UI/DebugImGuiLayer.h"
#endif
#include "Editor/Panels/IEditorPanel.h"
#include "Editor/Panels/Viewport.h"
#include "Editor/Panels/Console.h"
#include "Editor/Panels/NativeViewport.h"
#include "Core/Log.h"
#include "Render/Context/RenderContext.h"
#include "Render/Renderer/SceneRenderer.h"
#include "Scene/ComponentFactory.h"
#include "UI/UIContext.h"
#include "UI/UIRenderer.h"

#if RVX_EDITOR_ENABLE_LEGACY_IMGUI
#include <imgui.h>
#include <imgui_internal.h>
#endif

#include <filesystem>
#include <memory>
#include <utility>

namespace RVX::Editor
{
namespace
{
    constexpr const char* RVX_EDITOR_UNSAVED_CHANGES_MODAL_ID =
        "UnsavedChanges";
    constexpr const char* RVX_EDITOR_OVERWRITE_SCENE_MODAL_ID =
        "OverwriteScene";
    constexpr const char* RVX_EDITOR_OPEN_SCENE_PICKER_ID = "OpenScene";
    constexpr const char* RVX_EDITOR_SAVE_SCENE_PICKER_ID = "SaveScene";

    EditorFilePickerFilter ToFilePickerFilter(
        const EditorFileDialogFilter& filter)
    {
        EditorFilePickerFilter pickerFilter;
        pickerFilter.displayName = filter.displayName;
        pickerFilter.patterns = filter.patterns;
        return pickerFilter;
    }

    EditorFilePickerDialogDesc MakeSceneFilePickerDialogDesc(
        EditorFilePickerMode mode,
        const EditorFileDialogDesc& fileDialogDesc)
    {
        EditorFilePickerDialogDesc dialog;
        dialog.id = mode == EditorFilePickerMode::SaveFile
                        ? RVX_EDITOR_SAVE_SCENE_PICKER_ID
                        : RVX_EDITOR_OPEN_SCENE_PICKER_ID;
        dialog.title = fileDialogDesc.title;
        dialog.acceptButtonText =
            mode == EditorFilePickerMode::SaveFile ? "Save" : "Open";
        dialog.picker.mode = mode;
        dialog.picker.initialDirectory = fileDialogDesc.initialDirectory;
        dialog.picker.initialPath = fileDialogDesc.defaultPath;
        dialog.picker.defaultFileName =
            fileDialogDesc.defaultPath.filename().string();
        dialog.picker.defaultExtension = fileDialogDesc.defaultExtension;
        dialog.picker.filters.reserve(fileDialogDesc.filters.size());
        for (const EditorFileDialogFilter& filter : fileDialogDesc.filters)
        {
            dialog.picker.filters.push_back(ToFilePickerFilter(filter));
        }
        return dialog;
    }

    NativeViewportPanel* FindNativeViewportPanel(IEditorUIBackend* backend)
    {
        if (!backend)
        {
            return nullptr;
        }

        return dynamic_cast<NativeViewportPanel*>(
            backend->GetPanel(NativeViewportPanel::PanelId()));
    }

    EditorWindowCursorMode ToWindowCursorMode(EditorUICursorMode mode)
    {
        switch (mode)
        {
            case EditorUICursorMode::Normal:
                return EditorWindowCursorMode::Normal;
            case EditorUICursorMode::Hidden:
                return EditorWindowCursorMode::Hidden;
            case EditorUICursorMode::Locked:
                return EditorWindowCursorMode::Locked;
        }
        return EditorWindowCursorMode::Normal;
    }

}

EditorApplication::EditorApplication() = default;

EditorApplication::~EditorApplication()
{
    Shutdown();
}

bool EditorApplication::Initialize()
{
    RVX_CORE_INFO("Initializing EditorApplication...");
    ComponentFactory::RegisterDefaults();

    if (!InitializeWindow())
    {
        RVX_CORE_ERROR("Failed to initialize window");
        return false;
    }

    if (!InitializeRHI())
    {
        RVX_CORE_WARN("Editor RHI initialization failed; viewport will use placeholder rendering");
    }

    if (!InitializeEditorUI())
    {
        RVX_CORE_ERROR("Failed to initialize native editor UI host");
        return false;
    }

#if RVX_EDITOR_ENABLE_LEGACY_IMGUI
    if (!InitializeImGui())
    {
        RVX_CORE_ERROR("Failed to initialize ImGui");
        return false;
    }
#endif
    EditorInputBridgeAttachDesc inputBridgeDesc;
    inputBridgeDesc.bridge = &m_editorInputBridge;
    inputBridgeDesc.window = m_window;
    const EditorInputBridgeAttachResult inputBridgeResult =
        m_inputBridgeService.Attach(inputBridgeDesc);
    if (!inputBridgeResult)
    {
        RVX_CORE_ERROR("Failed to initialize native editor input bridge: {}",
                       inputBridgeResult.error);
        return false;
    }
    RegisterCoreEditorServices();

    if (!InitializePanels())
    {
        RVX_CORE_ERROR("Failed to initialize panels");
        return false;
    }

    m_running = true;
    m_lastFrameTime = m_windowFrameService.GetTime();

    RVX_CORE_INFO("EditorApplication initialized successfully");
    return true;
}

bool EditorApplication::InitializeWindow()
{
    EditorWindowCreateDesc desc;
    desc.width = m_windowWidth;
    desc.height = m_windowHeight;
    desc.title = m_windowTitle.c_str();
    desc.backendType = m_renderRuntimeAdapter.ResolveDefaultBackend();
    desc.maximized = m_runConfig.startMaximized;

    const EditorWindowCreateResult result =
        m_windowLifecycleService.CreateWindow(desc);
    if (!result)
    {
        RVX_CORE_ERROR("Failed to create editor window: {}", result.error);
        return false;
    }

    m_window = result.window;
    m_windowWidth = result.width;
    m_windowHeight = result.height;
    return true;
}

bool EditorApplication::InitializeRHI()
{
    EditorRenderRuntimeAdapterConfig config;
    config.backendType = m_renderRuntimeAdapter.ResolveDefaultBackend();
    const EditorRenderRuntimeAdapterStartResult result =
        m_renderRuntimeAdapter.Start(m_window, config);
    if (!result)
    {
        RVX_CORE_ERROR("Editor render runtime adapter failed: {}",
                       result.runtime.message);
        return false;
    }

    RVX_CORE_WARN(
        "Editor viewport/native UI rendering is unavailable during the M1 "
        "architecture cut; render runtime diagnostics remain available");
    return true;
}
#if RVX_EDITOR_ENABLE_LEGACY_IMGUI
bool EditorApplication::InitializeImGui()
{
    m_debugImGuiLayer = std::make_unique<DebugImGuiLayer>();
    const bool automatedRun = m_runLoopService.HasAutomatedRunRequest() ||
                              m_automationScenarioService.HasScenario();
    DebugImGuiLayerDesc desc;
    desc.automatedRun = automatedRun;
    return m_debugImGuiLayer->Initialize(m_window, desc);
}
#endif

bool EditorApplication::InitializeEditorUI()
{
#if RVX_EDITOR_M1_RENDER_ADAPTER
    RVX_CORE_WARN(
        "Editor native UI bootstrap is unavailable during the M1 "
        "architecture cut");
    return false;
#else
    EditorCommandBindingCallbacks commandCallbacks;
    commandCallbacks.newScene = [this]() {
        return HandleFileNewScene();
    };
    commandCallbacks.openScene = [this]() {
        return HandleFileOpenScene();
    };
    commandCallbacks.saveScene = [this]() {
        return HandleFileSaveScene();
    };
    commandCallbacks.saveSceneAs = [this]() {
        return HandleFileSaveSceneAs();
    };
    commandCallbacks.exit = [this]() {
        return HandleFileExit();
    };
    commandCallbacks.preferences = [this]() {
        m_nativePanelRegistrationService.OpenPreferencesPanel();
    };

    EditorWindowMetricsDesc windowMetricsDesc;
    windowMetricsDesc.window = m_window;
    const EditorWindowMetricsResult windowMetrics =
        m_windowFrameService.CaptureMetrics(windowMetricsDesc);
    if (!windowMetrics)
    {
        RVX_CORE_ERROR("Failed to capture editor window metrics: {}",
                       windowMetrics.error);
        return false;
    }

    EditorUIBootstrapDesc bootstrapDesc;
    bootstrapDesc.context = &EditorContext::Get();
    bootstrapDesc.settingsService = &m_settingsService;
    bootstrapDesc.documentSession = &m_documentSession;
    bootstrapDesc.fileDialogService = &m_fileDialogService;
    bootstrapDesc.shortcutProfileService = &m_shortcutProfileService;
    bootstrapDesc.unsavedChangesGuard = &m_unsavedChangesGuard;
    bootstrapDesc.pendingActions = &m_pendingActions;
    bootstrapDesc.editorUIBackend = &m_editorUIBackend;
    bootstrapDesc.runLoopService = &m_runLoopService;
    bootstrapDesc.shortcutProfileLifecycleService =
        &m_shortcutProfileLifecycleService;
    bootstrapDesc.automationScenarioService = &m_automationScenarioService;
    bootstrapDesc.documentActionService = &m_documentActionService;
    bootstrapDesc.commandExecutionService = &m_commandExecutionService;
    bootstrapDesc.documentCommandService = &m_documentCommandService;
    bootstrapDesc.operationService = &m_operationService;
    bootstrapDesc.selectionService = &m_selectionService;
    bootstrapDesc.documentFileCommandService = &m_documentFileCommandService;
    bootstrapDesc.commandBindingService = &m_commandBindingService;
    bootstrapDesc.layoutPersistenceService = &m_layoutPersistenceService;
    bootstrapDesc.viewportToolService = &m_viewportToolService;
    bootstrapDesc.notificationService = &m_notificationService;
    bootstrapDesc.nativePanelRegistrationService =
        &m_nativePanelRegistrationService;
    bootstrapDesc.refreshDocumentCommandStates = [this]() {
        RefreshDocumentCommandStates();
    };
    bootstrapDesc.commandCallbacks = std::move(commandCallbacks);
    bootstrapDesc.settingsPathOverride = m_runConfig.settingsPathOverride;
    bootstrapDesc.shortcutProfilePathOverride =
        m_runConfig.autoShortcutProfilePath;
    bootstrapDesc.maxFrames = m_runConfig.maxFrames;
    bootstrapDesc.screenshotPath = m_runConfig.screenshotPath;
    bootstrapDesc.automationScenario = m_runConfig.automationScenario;
    bootstrapDesc.automationTarget = m_runConfig.automationTarget;
    bootstrapDesc.automationConfirmOverwrite =
        m_runConfig.automationConfirmOverwrite;
    bootstrapDesc.uiBackendType = m_runConfig.uiBackendType;
    bootstrapDesc.overrideUIBackendType =
        m_runConfig.overrideUIBackendType;
    bootstrapDesc.renderDevice = GetRenderDevice();
    bootstrapDesc.surfaceWidth =
        static_cast<uint32>(std::max(0, windowMetrics.framebufferWidth));
    bootstrapDesc.surfaceHeight =
        static_cast<uint32>(std::max(0, windowMetrics.framebufferHeight));
    bootstrapDesc.surfaceScaleFactor = windowMetrics.surfaceScaleFactor;
    bootstrapDesc.backendDebugName = "RenderVerseX.EditorUI";
    bootstrapDesc.exposeLegacyImGuiDebugCommands =
        m_runConfig.exposeLegacyImGuiDebugCommands;
    bootstrapDesc.showDemoWindow = &m_showDemoWindow;
    bootstrapDesc.showMetricsWindow = &m_showMetricsWindow;

    const EditorUIBootstrapResult bootstrapResult =
        m_uiBootstrapService.Bootstrap(bootstrapDesc);
    if (!bootstrapResult)
    {
        RVX_CORE_ERROR("Failed to initialize editor UI bootstrap: {}",
                       bootstrapResult.error);
        return false;
    }

    if (m_editorContextChangeCallbackId == 0)
    {
        m_editorContextChangeCallbackId =
            EditorContext::Get().AddChangeCallback(
                [this](const EditorContextChangeEvent& event) {
                    HandleEditorContextChanged(event);
                });
    }

    if (m_documentSession && !m_documentSession->HasActiveScene())
    {
        if (!m_documentSession->NewScene())
        {
            RVX_CORE_ERROR("Failed to create startup editor scene: {}",
                           m_documentSession->GetLastError());
            return false;
        }
        RefreshDocumentCommandStates();
    }

    RegisterCoreEditorServices();
    return true;
#endif
}

void EditorApplication::RegisterCoreEditorServices()
{
    EditorCoreServiceRegistrationDesc desc;
    desc.registry = &m_services;
    desc.serviceRegistrationService = &m_serviceRegistrationService;
    desc.frameCoordinator = &m_frameCoordinator;
    desc.frameLifecycleService = &m_frameLifecycleService;
    desc.frameSubmissionService = &m_frameSubmissionService;
    desc.mainFramePresentationService = &m_mainFramePresentationService;
    desc.mainFramebufferService = &m_mainFramebufferService;
    desc.mainSwapChainService = &m_mainSwapChainService;
    desc.nativeUIFrameService = &m_nativeUIFrameService;
    desc.nativeUIRenderStatsService = &m_nativeUIRenderStatsService;
    desc.nativePanelRegistrationService = &m_nativePanelRegistrationService;
    desc.nativeUISubmissionService = &m_nativeUISubmissionService;
    desc.beginFrameService = &m_beginFrameService;
    desc.commandBindingService = &m_commandBindingService;
    desc.commandExecutionService = &m_commandExecutionService;
    desc.documentActionService = &m_documentActionService;
    desc.documentCommandService = &m_documentCommandService;
    desc.documentFileCommandService = &m_documentFileCommandService;
    desc.endFrameService = &m_endFrameService;
    desc.inputBridgeService = &m_inputBridgeService;
    desc.legacyDebugUIService = &m_legacyDebugUIService;
    desc.layoutPersistenceService = &m_layoutPersistenceService;
    desc.notificationService = &m_notificationService;
    desc.operationService = &m_operationService;
    desc.renderBootstrapService = &m_renderBootstrapService;
    desc.renderFrameService = &m_renderFrameService;
    desc.renderShutdownService = &m_renderShutdownService;
    desc.runLoopService = &m_runLoopService;
    desc.screenshotRequestService = &m_screenshotRequestService;
    desc.screenshotService = &m_screenshotService;
    desc.selectionService = &m_selectionService;
    desc.automationScenarioService = &m_automationScenarioService;
    desc.shortcutProfileLifecycleService = &m_shortcutProfileLifecycleService;
    desc.updateService = &m_updateService;
    desc.uiBootstrapService = &m_uiBootstrapService;
    desc.uiShutdownService = &m_uiShutdownService;
    desc.viewportRenderService = &m_viewportRenderService;
    desc.viewportToolService = &m_viewportToolService;
    desc.windowFrameService = &m_windowFrameService;
    desc.windowLifecycleService = &m_windowLifecycleService;
    desc.renderContext = m_renderContext.get();
    desc.sceneRenderer = m_sceneRenderer.get();
    desc.editorUIBackend = m_editorUIBackend.get();
    desc.editorInputBridge = m_editorInputBridge.get();
    desc.documentSession = m_documentSession.get();
    desc.fileDialogService = m_fileDialogService.get();
    desc.settingsService = m_settingsService.get();
    desc.shortcutProfileService = m_shortcutProfileService.get();
    desc.unsavedChangesGuard = m_unsavedChangesGuard.get();
    desc.pendingActions = m_pendingActions.get();
    m_serviceRegistrationService.RegisterCoreServices(desc);
}

bool EditorApplication::InitializePanels()
{
    if (!m_nativePanelRegistrationService.RegisterStandardNativePanels())
    {
        RVX_CORE_WARN("Native editor panel registration was incomplete");
    }

    RefreshNativeViewCommandStates();
    m_nativePanelRegistrationService.RegisterDebugViewCommands();

    BindRenderDeviceToViewportPanels(GetRenderDevice());

    // Initialize all panels
    for (auto& panel : m_panels)
    {
        panel->OnInit();
    }

    RVX_CORE_INFO("Registered {} editor panels", m_panels.size());
    return true;
}

int EditorApplication::Run()
{
    RVX_CORE_INFO("Entering main loop");

    m_runLoopService.Configure({m_runConfig.maxFrames,
                                m_runConfig.screenshotPath});
    m_runLoopService.BeginRun();
    m_automationScenarioService.ResetRun();

    while (m_running)
    {
        m_runLoopService.BeginFrame();

        BeginFrame();
        if (!m_running)
        {
            break;
        }
        Update(m_deltaTime);
        Render();
        EndFrame();

        m_runLoopService.CompleteFrame();
        if (m_runLoopService.ShouldStopAfterFrame())
        {
            m_running = false;
        }
    }

    m_automationScenarioService.MarkIncompleteBeforeExit();

    return m_runLoopService.BuildExitCode(
        m_automationScenarioService.HasFailed());
}
void EditorApplication::Shutdown()
{
    EditorUIShutdownDesc uiShutdownDesc;
    uiShutdownDesc.editorUIBackend = &m_editorUIBackend;
    uiShutdownDesc.notificationService = &m_notificationService;
    uiShutdownDesc.layoutPersistenceService = &m_layoutPersistenceService;
    uiShutdownDesc.nativePanelRegistrationService =
        &m_nativePanelRegistrationService;
    uiShutdownDesc.shortcutProfileLifecycleService =
        &m_shortcutProfileLifecycleService;
    uiShutdownDesc.pendingActions = &m_pendingActions;
    uiShutdownDesc.unsavedChangesGuard = &m_unsavedChangesGuard;
    uiShutdownDesc.shortcutProfileService = &m_shortcutProfileService;
    uiShutdownDesc.settingsService = &m_settingsService;
    uiShutdownDesc.fileDialogService = &m_fileDialogService;
    uiShutdownDesc.documentSession = &m_documentSession;

    if (!m_window)
    {
        m_serviceRegistrationService.ClearRegistry(m_services);
        const EditorUIShutdownResult uiShutdownResult =
            m_uiShutdownService.Shutdown(uiShutdownDesc);
        if (!uiShutdownResult)
        {
            RVX_CORE_WARN("Editor UI shutdown failed: {}",
                          uiShutdownResult.error);
        }
        if (!uiShutdownResult.layoutSaveResult &&
            !uiShutdownResult.layoutSaveResult.error.empty())
        {
            RVX_CORE_WARN("Editor layout save failed: {}",
                          uiShutdownResult.layoutSaveResult.error);
        }
        return;
    }

    RVX_CORE_INFO("Shutting down editor...");
    m_serviceRegistrationService.ClearRegistry(m_services);

    if (m_editorContextChangeCallbackId != 0)
    {
        EditorContext::Get().RemoveChangeCallback(m_editorContextChangeCallbackId);
        m_editorContextChangeCallbackId = 0;
    }

    BindRenderDeviceToViewportPanels(nullptr);

    // Shutdown panels
    for (auto& panel : m_panels)
    {
        panel->OnShutdown();
    }
    m_panels.clear();

    const RenderShutdownResult renderShutdownResult =
        m_renderRuntimeAdapter.Shutdown();
    if (renderShutdownResult.code != RenderShutdownCode::Completed &&
        renderShutdownResult.code != RenderShutdownCode::AlreadyStopped)
    {
        RVX_CORE_WARN("Editor render runtime shutdown failed: {}",
                      renderShutdownResult.message);
    }
    const EditorUIShutdownResult uiShutdownResult =
        m_uiShutdownService.Shutdown(uiShutdownDesc);
    if (!uiShutdownResult)
    {
        RVX_CORE_WARN("Editor UI shutdown failed: {}",
                      uiShutdownResult.error);
    }
    if (!uiShutdownResult.layoutSaveResult &&
        !uiShutdownResult.layoutSaveResult.error.empty())
    {
        RVX_CORE_WARN("Editor layout save failed: {}",
                      uiShutdownResult.layoutSaveResult.error);
    }

    EditorInputBridgeShutdownDesc inputBridgeShutdownDesc;
    inputBridgeShutdownDesc.bridge = &m_editorInputBridge;
    const EditorInputBridgeShutdownResult inputBridgeShutdownResult =
        m_inputBridgeService.Shutdown(inputBridgeShutdownDesc);
    if (!inputBridgeShutdownResult)
    {
        RVX_CORE_WARN("Editor input bridge shutdown failed: {}",
                      inputBridgeShutdownResult.error);
    }

#if RVX_EDITOR_ENABLE_LEGACY_IMGUI
    if (m_debugImGuiLayer)
    {
        m_debugImGuiLayer->Shutdown();
        m_debugImGuiLayer.reset();
    }
#endif

    EditorWindowCursorDesc cursorDesc;
    cursorDesc.window = m_window;
    cursorDesc.mode = EditorWindowCursorMode::Normal;
    m_windowFrameService.ApplyCursorMode(cursorDesc);

    // Cleanup window
    EditorWindowDestroyDesc windowDestroyDesc;
    windowDestroyDesc.window = &m_window;
    const EditorWindowDestroyResult windowDestroyResult =
        m_windowLifecycleService.DestroyWindow(windowDestroyDesc);
    if (!windowDestroyResult)
    {
        RVX_CORE_WARN("Editor window shutdown failed: {}",
                      windowDestroyResult.error);
    }
}

void EditorApplication::BeginFrame()
{
    EditorBeginFrameDesc beginFrameDesc;
    beginFrameDesc.window = m_window;
    beginFrameDesc.editorUIBackend = m_editorUIBackend.get();
    beginFrameDesc.windowFrameService = &m_windowFrameService;
    beginFrameDesc.nativeUIFrameService = &m_nativeUIFrameService;
    beginFrameDesc.inputBridgeService = &m_inputBridgeService;
    beginFrameDesc.inputBridge = m_editorInputBridge.get();
    beginFrameDesc.lastFrameTime = m_lastFrameTime;
    beginFrameDesc.totalTime = m_totalTime;
    beginFrameDesc.scaleFactor =
        m_settingsService ? m_settingsService->GetUIScaleFactor()
                          : RVX_EDITOR_DEFAULT_UI_SCALE_FACTOR;
    beginFrameDesc.handleCloseRequested = [this]() {
        m_commandExecutionService.ExecutePlan(
            {"Exit",
             [this]() {
                 return HandleFileExit();
             }});
        return m_running;
    };

    const EditorBeginFrameResult beginFrameResult =
        m_beginFrameService.BeginFrame(beginFrameDesc);
    if (!beginFrameResult)
    {
        RVX_CORE_ERROR("Failed to process editor window frame: {}",
                       beginFrameResult.error);
        m_running = false;
        return;
    }

    if (!beginFrameResult.continueAfterCloseRequest)
    {
        return;
    }

    m_windowWidth = beginFrameResult.windowWidth;
    m_windowHeight = beginFrameResult.windowHeight;
    m_deltaTime = beginFrameResult.deltaTime;
    m_lastFrameTime = beginFrameResult.lastFrameTime;
    m_totalTime = beginFrameResult.totalTime;
    m_debugImGuiFrameActive = beginFrameResult.debugFrameActiveAfterBegin;

    if (beginFrameResult.nativeUIBeginAttempted &&
        !beginFrameResult.nativeUIBeginFrameResult)
    {
        RVX_CORE_WARN("Native editor UI begin frame failed: {}",
                      beginFrameResult.nativeUIBeginFrameResult.error);
    }
}
void EditorApplication::Update(float deltaTime)
{
    EditorUpdateDesc updateDesc;
    updateDesc.editorUIBackend = m_editorUIBackend.get();
    updateDesc.nativeUIFrameService = &m_nativeUIFrameService;
    updateDesc.shortcutProfileLifecycleService = &m_shortcutProfileLifecycleService;
    updateDesc.legacyPanels = &m_panels;
    updateDesc.deltaTime = deltaTime;
    const EditorUpdateResult updateResult = m_updateService.Update(updateDesc);
    if (!updateResult)
    {
        RVX_CORE_ERROR("Failed to update editor frame: {}",
                       updateResult.error);
        m_running = false;
    }
}

void EditorApplication::Render()
{
#if RVX_EDITOR_M1_RENDER_ADAPTER
    RVX_CORE_ERROR(
        "Editor frame rendering is unavailable during the M1 architecture cut");
    m_running = false;
    return;
#else
    EditorRenderFrameDesc renderFrameDesc;
    renderFrameDesc.editorUIBackend = m_editorUIBackend.get();
    renderFrameDesc.editorUIRenderer = m_editorUIRenderer.get();
    renderFrameDesc.nativeUIFrameService = &m_nativeUIFrameService;
    renderFrameDesc.nativeUIRenderStatsService = &m_nativeUIRenderStatsService;
    renderFrameDesc.nativeUIStats = &m_nativeUIRenderStats;
    renderFrameDesc.legacyDebugUIService = &m_legacyDebugUIService;
    renderFrameDesc.automationScenarioService = &m_automationScenarioService;
    renderFrameDesc.documentSession = m_documentSession.get();
    renderFrameDesc.settingsService = m_settingsService.get();
    renderFrameDesc.shortcutProfileService = m_shortcutProfileService.get();
    renderFrameDesc.refreshNativeViewCommands = [this]() {
        RefreshNativeViewCommandStates();
    };
    renderFrameDesc.refreshDocumentCommands = [this]() {
        RefreshDocumentCommandStates();
    };
    renderFrameDesc.evaluateShellPolicy = [this]() {
        return EvaluateShellPolicy();
    };
    renderFrameDesc.countVisibleLegacyPanels = [this]() {
        return CountVisibleLegacyPanels();
    };
#if RVX_EDITOR_ENABLE_LEGACY_IMGUI
    renderFrameDesc.debugImGuiLayer = m_debugImGuiLayer.get();
    renderFrameDesc.showDemoWindow = &m_showDemoWindow;
    renderFrameDesc.showMetricsWindow = &m_showMetricsWindow;
    renderFrameDesc.drawDockSpace = [this]() {
        DrawDockSpace();
    };
    renderFrameDesc.drawMainMenuBar = [this]() {
        DrawMainMenuBar();
    };
    renderFrameDesc.drawStatusBar = [this]() {
        DrawStatusBar();
    };
#endif
    renderFrameDesc.drawPanels = [this]() {
        DrawPanels();
    };
    const EditorRenderFrameResult renderFrameResult =
        m_renderFrameService.RenderFrame(renderFrameDesc);
    if (!renderFrameResult)
    {
        RVX_CORE_ERROR("Failed to render editor frame: {}",
                       renderFrameResult.error);
        m_running = false;
        return;
    }
    if (renderFrameResult.requestStop)
    {
        m_running = false;
    }
    m_debugImGuiFrameActive = renderFrameResult.debugFrameActive;

    if (m_window)
    {
        const EditorUICursorRequest cursorRequest =
            m_editorUIBackend ? m_editorUIBackend->GetCursorRequest()
                              : EditorUICursorRequest{};
        EditorWindowCursorDesc cursorDesc;
        cursorDesc.window = m_window;
        cursorDesc.mode = ToWindowCursorMode(cursorRequest.cursorMode);
        const EditorWindowCursorResult cursorResult =
            m_windowFrameService.ApplyCursorMode(cursorDesc);
        if (!cursorResult && cursorRequest.IsActive())
        {
            RVX_CORE_WARN("Failed to apply editor cursor mode: {}",
                          cursorResult.error);
        }
    }
#endif
}

void EditorApplication::EndFrame()
{
#if RVX_EDITOR_M1_RENDER_ADAPTER
    return;
#else
    EditorEndFrameDesc endFrameDesc;
    endFrameDesc.window = m_window;
    endFrameDesc.renderContext = m_renderContext.get();
    endFrameDesc.sceneRenderer = m_sceneRenderer.get();
    endFrameDesc.runtimeUIRenderer = m_runtimeUIRenderer.get();
    endFrameDesc.editorUIRenderer = m_editorUIRenderer.get();
    endFrameDesc.sceneManager =
        EditorContext::Get().GetActiveSceneManager();
    endFrameDesc.editorUIBackend = m_editorUIBackend.get();
    endFrameDesc.nativeUIFrameService = &m_nativeUIFrameService;
    endFrameDesc.mainSwapChainService = &m_mainSwapChainService;
    endFrameDesc.legacyDebugUIService = &m_legacyDebugUIService;
    endFrameDesc.frameLifecycleService = &m_frameLifecycleService;
    endFrameDesc.frameCoordinator = &m_frameCoordinator;
    endFrameDesc.frameSubmissionService = &m_frameSubmissionService;
    endFrameDesc.mainFramePresentationService =
        &m_mainFramePresentationService;
    endFrameDesc.mainFramebufferService = &m_mainFramebufferService;
    endFrameDesc.nativeUISubmissionService = &m_nativeUISubmissionService;
    endFrameDesc.nativeUIRenderStatsService =
        &m_nativeUIRenderStatsService;
    endFrameDesc.viewportRenderService = &m_viewportRenderService;
    endFrameDesc.runLoopService = &m_runLoopService;
    endFrameDesc.screenshotRequestService = &m_screenshotRequestService;
    endFrameDesc.screenshotService = &m_screenshotService;
    endFrameDesc.viewportStats = &m_viewportSceneRenderStats;
    endFrameDesc.nativeUIStats = &m_nativeUIRenderStats;
    endFrameDesc.debugFrameActive = m_debugImGuiFrameActive;
#if RVX_EDITOR_ENABLE_LEGACY_IMGUI
    endFrameDesc.debugImGuiLayer = m_debugImGuiLayer.get();
    endFrameDesc.legacyPanels = &m_panels;
    endFrameDesc.includeLegacyPanels = true;
#endif

    const EditorEndFrameResult result = m_endFrameService.EndFrame(endFrameDesc);
    m_debugImGuiFrameActive = result.debugFrameActiveAfterEnd;
#endif
}

EditorShellPolicyState EditorApplication::BuildShellPolicyState() const
{
    EditorShellPolicyState state;
    state.editorUIBackendReady =
        m_editorUIBackend && m_editorUIBackend->IsInitialized();
    state.nativeFrameRecorded = m_nativeUIRenderStats.frameRecorded;
    state.nativeMenuCount = m_nativeUIRenderStats.menuCount;
    state.nativeToolbarCount = m_nativeUIRenderStats.toolbarCount;
    state.nativeStatusItemCount = m_nativeUIRenderStats.statusItemCount;
    state.nativeDockspaceTopReservedHeight =
        m_nativeUIRenderStats.dockspaceTopReservedHeight;
    state.nativeDockspaceBottomReservedHeight =
        m_nativeUIRenderStats.dockspaceBottomReservedHeight;
    state.visibleLegacyPanelCount = CountVisibleLegacyPanels();
    state.legacyDemoWindowVisible = m_showDemoWindow;
    state.legacyMetricsWindowVisible = m_showMetricsWindow;
    return state;
}

EditorShellPolicyDecision EditorApplication::EvaluateShellPolicy() const
{
    EditorShellPolicyDecision decision =
        m_shellPolicy.Evaluate(BuildShellPolicyState());
#if !RVX_EDITOR_ENABLE_LEGACY_IMGUI
    decision.drawLegacyDockSpace = false;
    decision.drawLegacyMainMenu = false;
    decision.drawLegacyStatusBar = false;
    decision.runDebugImGuiFrame = false;
    decision.showLegacyDemoWindow = false;
    decision.showLegacyMetricsWindow = false;
#endif
    return decision;
}

uint32 EditorApplication::CountVisibleLegacyPanels() const
{
#if RVX_EDITOR_ENABLE_LEGACY_IMGUI
    uint32 visiblePanelCount = 0;
    for (const auto& panel : m_panels)
    {
        if (panel && panel->IsVisible())
        {
            ++visiblePanelCount;
        }
    }
    return visiblePanelCount;
#else
    return 0;
#endif
}

bool EditorApplication::ShouldUseNativeMainShell() const
{
    return EvaluateShellPolicy().useNativeMainShell;
}

bool EditorApplication::HasVisibleLegacyPanels() const
{
    return m_shellPolicy.HasVisibleLegacyPanels(BuildShellPolicyState());
}

bool EditorApplication::ShouldDrawLegacyDockSpace() const
{
    return EvaluateShellPolicy().drawLegacyDockSpace;
}

bool EditorApplication::ShouldRunDebugImGuiFrame() const
{
    return EvaluateShellPolicy().runDebugImGuiFrame;
}

#if RVX_EDITOR_ENABLE_LEGACY_IMGUI
void EditorApplication::DrawMainMenuBar()
{
    auto executeCommand = [this](const std::string& commandId) {
        if (m_editorUIBackend)
        {
            m_editorUIBackend->ExecuteCommand(commandId);
        }
    };

    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("New Scene", "Ctrl+N"))
            {
                executeCommand(EditorCommandIds::FileNewScene);
            }
            if (ImGui::MenuItem("Open Scene", "Ctrl+O"))
            {
                executeCommand(EditorCommandIds::FileOpenScene);
            }
            if (ImGui::MenuItem("Save Scene", "Ctrl+S"))
            {
                executeCommand(EditorCommandIds::FileSaveScene);
            }
            if (ImGui::MenuItem("Save Scene As...", "Ctrl+Shift+S"))
            {
                executeCommand(EditorCommandIds::FileSaveSceneAs);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Open Project..."))
            {
                // TODO: Open project dialog
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4"))
            {
                executeCommand(EditorCommandIds::FileExit);
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Edit"))
        {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, EditorContext::Get().CanUndo()))
            {
                executeCommand(EditorCommandIds::EditUndo);
            }
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, EditorContext::Get().CanRedo()))
            {
                executeCommand(EditorCommandIds::EditRedo);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Cut", "Ctrl+X"))
            {
                // TODO: Cut
            }
            if (ImGui::MenuItem("Copy", "Ctrl+C"))
            {
                // TODO: Copy
            }
            if (ImGui::MenuItem("Paste", "Ctrl+V"))
            {
                // TODO: Paste
            }
            if (ImGui::MenuItem("Delete", "Delete"))
            {
                // TODO: Delete selected
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Preferences..."))
            {
                // TODO: Show preferences
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View"))
        {
            for (auto& panel : m_panels)
            {
                bool visible = panel->IsVisible();
                if (ImGui::MenuItem(panel->GetName(), nullptr, &visible))
                {
                    panel->SetVisible(visible);
                }
            }
            ImGui::Separator();
            ImGui::MenuItem("ImGui Demo", nullptr, &m_showDemoWindow);
            ImGui::MenuItem("ImGui Metrics", nullptr, &m_showMetricsWindow);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("GameObject"))
        {
            if (ImGui::MenuItem("Create Empty", "Ctrl+Shift+N"))
            {
                executeCommand(EditorCommandIds::GameObjectCreateEmpty);
            }
            if (ImGui::BeginMenu("3D Object"))
            {
                if (ImGui::MenuItem("Cube")) {}
                if (ImGui::MenuItem("Sphere")) {}
                if (ImGui::MenuItem("Plane")) {}
                if (ImGui::MenuItem("Cylinder")) {}
                if (ImGui::MenuItem("Capsule")) {}
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Light"))
            {
                if (ImGui::MenuItem("Directional Light")) {}
                if (ImGui::MenuItem("Point Light")) {}
                if (ImGui::MenuItem("Spot Light")) {}
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Audio"))
            {
                if (ImGui::MenuItem("Audio Source")) {}
                if (ImGui::MenuItem("Audio Listener")) {}
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Effects"))
            {
                if (ImGui::MenuItem("Particle System")) {}
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Camera")) {}
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Window"))
        {
            if (ImGui::MenuItem("Reset Layout"))
            {
                executeCommand(EditorCommandIds::ViewResetLayout);
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help"))
        {
            if (ImGui::MenuItem("Documentation"))
            {
                // TODO: Open documentation
            }
            if (ImGui::MenuItem("About RenderVerseX"))
            {
                // TODO: Show about dialog
            }
            ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
    }
}

void EditorApplication::DrawDockSpace()
{
    static ImGuiDockNodeFlags dockspaceFlags = ImGuiDockNodeFlags_None;

    const bool nativeMainShell = ShouldUseNativeMainShell();
    const float topReserved = nativeMainShell
                                  ? m_nativeUIRenderStats.dockspaceTopReservedHeight
                                  : 0.0f;
    const float bottomReserved = nativeMainShell
                                     ? m_nativeUIRenderStats.dockspaceBottomReservedHeight
                                     : 0.0f;
    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDocking;
    if (!nativeMainShell)
    {
        windowFlags |= ImGuiWindowFlags_MenuBar;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 dockspacePos(viewport->WorkPos.x,
                              viewport->WorkPos.y + topReserved);
    const ImVec2 dockspaceSize(viewport->WorkSize.x,
                               std::max(1.0f,
                                        viewport->WorkSize.y -
                                            topReserved -
                                            bottomReserved));

    ImGui::SetNextWindowPos(dockspacePos);
    ImGui::SetNextWindowSize(dockspaceSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    windowFlags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse;
    windowFlags |= ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
    windowFlags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    ImGui::Begin("DockSpace", nullptr, windowFlags);
    ImGui::PopStyleVar(3);

    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_DockingEnable)
    {
        ImGuiID dockspaceId = ImGui::GetID("MainDockSpace");
            const bool automatedRun =
                m_runLoopService.HasAutomatedRunRequest() ||
                m_automationScenarioService.HasScenario();
        static bool automatedLayoutBuilt = false;
        if (automatedRun && !automatedLayoutBuilt)
        {
            ImGui::DockBuilderRemoveNode(dockspaceId);
            ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockspaceId, dockspaceSize);

            ImGuiID dockMain = dockspaceId;
            ImGuiID dockLeft = 0;
            ImGuiID dockRight = 0;
            ImGuiID dockBottom = 0;

            ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Left, 0.18f, &dockLeft, &dockMain);
            ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Right, 0.24f, &dockRight, &dockMain);
            ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Down, 0.24f, &dockBottom, &dockMain);

            ImGui::DockBuilderDockWindow("Hierarchy", dockLeft);
            ImGui::DockBuilderDockWindow("Inspector", dockRight);
            ImGui::DockBuilderDockWindow("Assets", dockBottom);
            ImGui::DockBuilderDockWindow("Console", dockBottom);
            ImGui::DockBuilderDockWindow("Viewport", dockMain);
            ImGui::DockBuilderFinish(dockspaceId);
            automatedLayoutBuilt = true;
        }
        ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), dockspaceFlags);
    }

    ImGui::End();
}

void EditorApplication::DrawStatusBar()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    float statusBarHeight = 24.0f;

    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x,
                                    viewport->WorkPos.y + viewport->WorkSize.y - statusBarHeight));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, statusBarHeight));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoDocking;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 4.0f));
    if (ImGui::Begin("StatusBar", nullptr, flags))
    {
        // Left side: Status message
        if (EditorContext::Get().IsPlaying())
        {
            ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "Playing");
        }
        else
        {
            ImGui::Text("Ready");
        }

        // Right side: FPS and memory
        ImGui::SameLine(ImGui::GetWindowWidth() - 200.0f);
        ImGui::Text("FPS: %.1f (%.2f ms)", 1.0f / m_deltaTime, m_deltaTime * 1000.0f);
    }
    ImGui::End();
    ImGui::PopStyleVar();
}
#endif

void EditorApplication::DrawPanels()
{
#if RVX_EDITOR_ENABLE_LEGACY_IMGUI
    for (auto& panel : m_panels)
    {
        if (panel->IsVisible())
        {
            panel->OnGUI();
        }
    }
#endif

    const UI::UIInputState* nativeInput =
        m_editorUIBackend ? m_editorUIBackend->GetInputState() : nullptr;
    if (nativeInput)
    {
        DispatchNativeInputToPanels(*nativeInput);
    }
}

void EditorApplication::DispatchNativeInputToPanels(const UI::UIInputState& input)
{
    for (auto& panel : m_panels)
    {
        if (panel && panel->IsVisible())
        {
            panel->OnNativeInput(input);
        }
    }
}

void EditorApplication::GetWindowSize(int& width, int& height) const
{
    width = m_windowWidth;
    height = m_windowHeight;
}

IRHIDevice* EditorApplication::GetRenderDevice() const
{
    return nullptr;
}

void EditorApplication::RegisterPanel(std::shared_ptr<IEditorPanel> panel)
{
    if (panel)
    {
        IEditorPanel* panelPtr = panel.get();
#if RVX_EDITOR_ENABLE_LEGACY_IMGUI
        if (auto* viewport = dynamic_cast<ViewportPanel*>(panel.get()))
        {
            viewport->SetRenderDevice(GetRenderDevice());
        }
#endif
        m_panels.push_back(std::move(panel));
        if (panelPtr)
        {
            m_nativePanelRegistrationService.RegisterLegacyPanelViewCommand(
                *panelPtr);
        }
    }
}

EditorCommandActionResult EditorApplication::HandleFileNewScene()
{
    return RequestGuardedDocumentAction(
        EditorCommandIds::FileNewScene,
        "New Scene",
        [this]() {
            return ExecuteFileNewScene();
        });
}

EditorCommandActionResult EditorApplication::HandleFileOpenScene()
{
    if (!m_documentSession || !m_fileDialogService)
    {
        return m_commandExecutionService.MakeUnavailable(
            "Open Scene",
            "Open Scene failed: document or file dialog service is unavailable");
    }

    return RequestGuardedDocumentAction(
        EditorCommandIds::FileOpenScene,
        "Open Scene",
        [this]() {
            return ExecuteFileOpenScene();
        });
}

EditorCommandActionResult EditorApplication::HandleFileExit()
{
    return RequestGuardedDocumentAction(
        EditorCommandIds::FileExit,
        "Exit",
        [this]() {
            return ExecuteFileExit();
        });
}

EditorCommandActionResult EditorApplication::RequestGuardedDocumentAction(
    std::string actionId,
    std::string label,
    std::function<EditorCommandActionResult()> continuation)
{
    EditorCommandActionResult result =
        m_documentCommandService.RequestGuardedAction(
            std::move(actionId),
            std::move(label),
            std::move(continuation),
            m_editorUIBackend != nullptr,
            [this]() {
                OpenUnsavedChangesModalForPendingAction();
            });
    RefreshDocumentCommandStates();
    return result;
}

void EditorApplication::OpenUnsavedChangesModalForPendingAction()
{
    if (!m_editorUIBackend || !m_documentActionService.HasPendingAction())
    {
        return;
    }

    const EditorUnsavedChangesPromptDesc promptDesc =
        m_documentActionService.BuildUnsavedChangesPromptDesc();

    EditorModalDialogDesc dialog;
    dialog.id = RVX_EDITOR_UNSAVED_CHANGES_MODAL_ID;
    dialog.title = promptDesc.title;
    dialog.message = promptDesc.message;
    dialog.severity = EditorModalDialogSeverity::Warning;
    dialog.defaultButtonId = "save";
    dialog.cancelButtonId = "cancel";
    dialog.closeOnOutsideClick = false;
    dialog.buttons.push_back(EditorModalDialogButton::Action(
        "save",
        "Save",
        [this](auto&) {
            ResolvePendingUnsavedChanges(EditorUnsavedChangesChoice::Save);
        },
        true,
        false));
    dialog.buttons.push_back(EditorModalDialogButton::Action(
        "discard",
        "Discard",
        [this](auto&) {
            ResolvePendingUnsavedChanges(EditorUnsavedChangesChoice::Discard);
        },
        true,
        false));
    dialog.buttons.push_back(EditorModalDialogButton::Action(
        "cancel",
        "Cancel",
        [this](auto&) {
            ResolvePendingUnsavedChanges(EditorUnsavedChangesChoice::Cancel);
        },
        true,
        false));

    if (!m_editorUIBackend->OpenModalDialog(std::move(dialog)))
    {
        PublishErrorNotification("Unsaved Changes",
                                 "Unable to open unsaved changes dialog");
        RVX_CORE_WARN("Unable to open unsaved changes dialog");
        m_documentActionService.ResolveUnsavedChanges(
            EditorUnsavedChangesChoice::Cancel,
            false);
    }
}

void EditorApplication::ResolvePendingUnsavedChanges(
    EditorUnsavedChangesChoice choice)
{
    const EditorDocumentActionResult result =
        m_documentActionService.ResolveUnsavedChanges(
            choice,
            m_editorUIBackend != nullptr);
    if (!result.message.empty() &&
        result.status != EditorDocumentActionStatus::Completed)
    {
        PublishWarningNotification("Unsaved Changes", result.message);
        RVX_CORE_WARN("{}", result.message);
    }

    if (result.closeModal && m_editorUIBackend)
    {
        m_editorUIBackend->CloseModalDialog();
    }
    if (result.openSaveSceneAsPicker)
    {
        OpenNativeSaveSceneAsPickerForPendingAction();
    }
    if (result.openUnsavedChangesModal)
    {
        OpenUnsavedChangesModalForPendingAction();
    }
    RefreshDocumentCommandStates();
}

void EditorApplication::PublishInfoNotification(const std::string& title,
                                                const std::string& message)
{
    m_notificationService.PublishInfo(title, message);
}

void EditorApplication::PublishWarningNotification(const std::string& title,
                                                   const std::string& message)
{
    m_notificationService.PublishWarning(title, message);
}

void EditorApplication::PublishErrorNotification(const std::string& title,
                                                 const std::string& message)
{
    m_notificationService.PublishError(title, message);
}

EditorCommandActionResult EditorApplication::ExecuteFileNewScene()
{
    const EditorOperationResult result = m_operationService.NewScene();
    if (!result)
    {
        RVX_CORE_ERROR("{}", result.message);
    }
    RefreshDocumentCommandStates();
    return m_commandExecutionService.BuildOperationResult("New Scene", result);
}

EditorCommandActionResult EditorApplication::ExecuteFileOpenScene()
{
    if (!m_documentSession)
    {
        return m_commandExecutionService.MakeUnavailable(
            "Open Scene",
            "Open Scene failed: document session is unavailable");
    }

    if (m_editorUIBackend)
    {
        return OpenNativeOpenScenePicker();
    }

    return OpenFallbackOpenSceneFileDialog();
}

EditorCommandActionResult EditorApplication::OpenNativeOpenScenePicker()
{
    if (!m_editorUIBackend || !m_documentSession)
    {
        return OpenFallbackOpenSceneFileDialog();
    }

    EditorFileDialogDesc fileDialogDesc =
        EditorFileDialogService::MakeSceneFileDialogDesc();
    const std::filesystem::path preferredDirectory =
        m_documentFileCommandService.GetPreferredSceneDialogDirectory();
    if (!preferredDirectory.empty())
    {
        fileDialogDesc.initialDirectory = preferredDirectory;
    }
    EditorFilePickerDialogDesc dialog = MakeSceneFilePickerDialogDesc(
        EditorFilePickerMode::OpenFile,
        fileDialogDesc);
    m_documentFileCommandService.PopulateSceneFilePickerRecentDirectories(
        dialog);
    dialog.onResult = [this](const EditorFilePickerDialogResult& result,
                             auto&) {
        m_documentFileCommandService.CompleteOpenSceneFilePickerResult(
            result,
            true);
    };

    if (!m_editorUIBackend->OpenFilePickerDialog(std::move(dialog)))
    {
        PublishWarningNotification("Open Scene",
                                   "Native file picker unavailable; using fallback dialog");
        return m_documentFileCommandService.RunFallbackOpenSceneFileDialog(
            false);
    }
    RefreshDocumentCommandStates();
    return m_commandExecutionService.MakeWaitingForUser(
        "Open Scene",
        "Choose a scene file");
}

EditorCommandActionResult EditorApplication::OpenFallbackOpenSceneFileDialog()
{
    return m_documentFileCommandService.RunFallbackOpenSceneFileDialog(false);
}

EditorCommandActionResult EditorApplication::HandleFileSaveScene()
{
    if (!m_documentSession)
    {
        return m_commandExecutionService.MakeUnavailable(
            "Save Scene",
            "Save Scene failed: document session is unavailable");
    }

    const EditorOperationResult result = m_operationService.SaveScene();
    if (!result)
    {
        RVX_CORE_WARN("{}", result.message);
    }
    RefreshDocumentCommandStates();
    return m_commandExecutionService.BuildOperationResult("Save Scene", result);
}

EditorCommandActionResult EditorApplication::HandleFileSaveSceneAs()
{
    return ExecuteFileSaveSceneAs();
}

EditorCommandActionResult EditorApplication::ExecuteFileSaveSceneAs()
{
    if (!m_documentSession)
    {
        return m_commandExecutionService.MakeUnavailable(
            "Save Scene As",
            "Save Scene As failed: document session is unavailable");
    }

    if (m_editorUIBackend)
    {
        return OpenNativeSaveSceneAsPicker({});
    }

    return OpenFallbackSaveSceneAsDialog();
}

EditorCommandActionResult EditorApplication::OpenNativeSaveSceneAsPicker(
    std::function<void(bool)> onSaved)
{
    if (!m_editorUIBackend || !m_documentSession)
    {
        if (onSaved)
        {
            onSaved(false);
        }
        return m_commandExecutionService.MakeUnavailable(
            "Save Scene As",
            "Native file picker is unavailable");
    }

    EditorFileDialogDesc fileDialogDesc =
        EditorFileDialogService::MakeSceneFileDialogDesc(
            m_documentFileCommandService.BuildDefaultSceneSavePath());
    EditorFilePickerDialogDesc dialog = MakeSceneFilePickerDialogDesc(
        EditorFilePickerMode::SaveFile,
        fileDialogDesc);
    m_documentFileCommandService.PopulateSceneFilePickerRecentDirectories(
        dialog);
    dialog.onResult = [this, onSaved = std::move(onSaved)](
                          const EditorFilePickerDialogResult& result,
                          auto&) mutable {
        m_documentFileCommandService.CompleteSaveSceneAsFilePickerResult(
            result,
            std::move(onSaved),
            [this](std::filesystem::path path,
                   std::function<void(bool)> completion) {
                OpenNativeSaveOverwriteConfirmation(
                    std::move(path),
                    std::move(completion));
            },
            true);
    };

    if (!m_editorUIBackend->OpenFilePickerDialog(std::move(dialog)))
    {
        if (onSaved)
        {
            onSaved(false);
        }
        RefreshDocumentCommandStates();
        return m_commandExecutionService.MakeRejected(
            "Save Scene As",
            "Native file picker failed to open");
    }
    RefreshDocumentCommandStates();
    return m_commandExecutionService.MakeWaitingForUser(
        "Save Scene As",
        "Choose a scene file path");
}

EditorCommandActionResult EditorApplication::OpenFallbackSaveSceneAsDialog()
{
    return m_documentFileCommandService.RunFallbackSaveSceneAsDialog(false);
}

void EditorApplication::OpenNativeSaveSceneAsPickerForPendingAction()
{
    if (!m_documentActionService.HasPendingAction())
    {
        RefreshDocumentCommandStates();
        return;
    }

    m_commandExecutionService.ExecutePlan(
        {"Save Scene As",
         [this]() {
             return OpenNativeSaveSceneAsPicker([this](bool saved) {
                 const EditorDocumentActionResult result =
                     m_documentActionService.CompletePendingActionAfterSave(saved);
                 if (!result.message.empty() &&
                     result.status != EditorDocumentActionStatus::Completed)
                 {
                     PublishWarningNotification("Pending Action", result.message);
                     RVX_CORE_WARN("{}", result.message);
                 }
                 if (result.openUnsavedChangesModal)
                 {
                     OpenUnsavedChangesModalForPendingAction();
                 }
                 RefreshDocumentCommandStates();
             });
         }});
}

void EditorApplication::OpenNativeSaveOverwriteConfirmation(
    std::filesystem::path path,
    std::function<void(bool)> onSaved)
{
    if (!m_editorUIBackend)
    {
        PublishErrorNotification("Save Scene As",
                                 "Overwrite confirmation is unavailable");
        CompleteNativeSaveSceneAs(false, std::move(onSaved));
        return;
    }

    auto completion =
        std::make_shared<std::function<void(bool)>>(std::move(onSaved));
    const EditorSaveOverwriteConfirmationDesc confirmation =
        m_documentActionService.BuildSaveOverwriteConfirmationDesc(path);

    EditorModalDialogDesc dialog;
    dialog.id = RVX_EDITOR_OVERWRITE_SCENE_MODAL_ID;
    dialog.title = confirmation.title;
    dialog.message = confirmation.message;
    dialog.severity = EditorModalDialogSeverity::Warning;
    dialog.defaultButtonId = "overwrite";
    dialog.cancelButtonId = "cancel";
    dialog.closeOnOutsideClick = false;
    dialog.buttons.push_back(EditorModalDialogButton::Action(
        "overwrite",
        confirmation.confirmButtonText,
        [this, path = std::move(path), completion](auto&) mutable {
            std::function<void(bool)> onSaved = std::move(*completion);
            *completion = {};
            SaveNativeSceneAsConfirmedPath(std::move(path), std::move(onSaved));
        }));
    dialog.buttons.push_back(EditorModalDialogButton::Action(
        "cancel",
        confirmation.cancelButtonText,
        [this, completion](auto&) {
            std::function<void(bool)> onSaved = std::move(*completion);
            *completion = {};
            PublishInfoNotification("Save Scene As", "Overwrite cancelled");
            CompleteNativeSaveSceneAs(false, std::move(onSaved));
        }));

    if (!m_editorUIBackend->OpenModalDialog(std::move(dialog)))
    {
        PublishErrorNotification("Save Scene As",
                                 "Overwrite confirmation failed to open");
        CompleteNativeSaveSceneAs(false, std::move(*completion));
        *completion = {};
        return;
    }
    RefreshDocumentCommandStates();
}

void EditorApplication::SaveNativeSceneAsConfirmedPath(
    std::filesystem::path path,
    std::function<void(bool)> onSaved)
{
    m_documentFileCommandService.SaveSceneAsConfirmedPath(
        std::move(path),
        std::move(onSaved),
        true);
}

void EditorApplication::CompleteNativeSaveSceneAs(
    bool saved,
    std::function<void(bool)> onSaved)
{
    if (onSaved)
    {
        onSaved(saved);
    }
    RefreshDocumentCommandStates();
}

EditorCommandActionResult EditorApplication::ExecuteFileExit()
{
    m_running = false;
    return m_commandExecutionService.MakeSucceeded("Exit", "Exit requested");
}

void EditorApplication::RefreshDocumentCommandStates()
{
    if (!m_editorUIBackend || !m_documentSession)
    {
        return;
    }

    EditorCommandRegistry* registry = m_editorUIBackend->GetCommandRegistry();
    if (!registry)
    {
        return;
    }

    const bool pendingAction = m_documentActionService.HasPendingAction();
    const EditorCommandPrecondition noPendingDocumentAction =
        EditorCommandPrecondition::Requires(
            !pendingAction,
            "Another document action is in progress.");
    const EditorCommandPrecondition hasOpenFileService =
        EditorCommandPrecondition::Requires(
            m_editorUIBackend ||
                (m_fileDialogService && m_fileDialogService->CanOpenFile()),
            "No open-file service is available.");
    const EditorCommandPrecondition hasSaveFileService =
        EditorCommandPrecondition::Requires(
            m_editorUIBackend ||
                (m_fileDialogService && m_fileDialogService->CanSaveFile()),
            "No save-file service is available.");
    const EditorCommandPrecondition canSaveLoadedScene =
        EditorCommandPrecondition::Requires(
            m_documentSession->CanSaveScene(),
            "No loaded scene can be saved.");
    const EditorCommandPrecondition canSaveSceneAs =
        EditorCommandPrecondition::Requires(
            m_documentSession->CanSaveSceneAs(),
            "No loaded scene can be saved.");

    registry->SetCommandPrecondition(EditorCommandIds::FileNewScene,
                                     noPendingDocumentAction);
    registry->SetCommandPrecondition(
        EditorCommandIds::FileOpenScene,
        EditorCommandPrecondition::All({noPendingDocumentAction,
                                        hasOpenFileService}));
    registry->SetCommandPrecondition(
        EditorCommandIds::FileSaveScene,
        canSaveLoadedScene);
    registry->SetCommandPrecondition(
        EditorCommandIds::FileSaveSceneAs,
        EditorCommandPrecondition::All({canSaveSceneAs, hasSaveFileService}));
    registry->SetCommandPrecondition(EditorCommandIds::FileExit,
                                     noPendingDocumentAction);
    registry->SetCommandPrecondition(
        EditorCommandIds::EditUndo,
        EditorCommandPrecondition::Requires(m_operationService.CanUndo(),
                                            "Nothing to undo."));
    registry->SetCommandPrecondition(
        EditorCommandIds::EditRedo,
        EditorCommandPrecondition::Requires(m_operationService.CanRedo(),
                                            "Nothing to redo."));
    registry->SetCommandPrecondition(
        EditorCommandIds::EditDelete,
        EditorCommandPrecondition::Requires(
            m_operationService.CanDeleteSelection(),
            "No selected object can be deleted."));
    registry->SetCommandPrecondition(
        EditorCommandIds::GameObjectCreateEmpty,
        EditorCommandPrecondition::Requires(
            m_operationService.CanCreateEntity(),
            "No editable scene is loaded."));
}

void EditorApplication::RefreshNativeViewCommandStates()
{
    m_nativePanelRegistrationService.RefreshViewCommandStates(m_panels);
    RefreshDocumentCommandStates();
}

void EditorApplication::HandleEditorContextChanged(const EditorContextChangeEvent& event)
{
    if (!m_editorUIBackend || event.reasonMask == 0u)
    {
        return;
    }

    m_nativePanelRegistrationService.RequestNativePanelRebuilds(
        EditorUIPanelRebuildReason::Data);
    RefreshDocumentCommandStates();
}

void EditorApplication::BindRenderDeviceToViewportPanels(IRHIDevice* device)
{
#if RVX_EDITOR_ENABLE_LEGACY_IMGUI
    for (auto& panel : m_panels)
    {
        if (auto* viewport = dynamic_cast<ViewportPanel*>(panel.get()))
        {
            viewport->SetRenderDevice(device);
        }
    }
#endif

    if (NativeViewportPanel* nativeViewport =
            FindNativeViewportPanel(m_editorUIBackend.get()))
    {
        nativeViewport->SetRenderDevice(device);
    }
}

IEditorPanel* EditorApplication::GetPanel(const std::string& name)
{
    for (auto& panel : m_panels)
    {
        if (panel->GetName() == name)
        {
            return panel.get();
        }
    }
    return nullptr;
}

} // namespace RVX::Editor
