/**
 * @file EditorApplication.h
 * @brief Main editor application class
 */

#pragma once

#include "Core/Types.h"
#include "Editor/EditorAutomationScenarioService.h"
#include "Editor/EditorBeginFrameService.h"
#include "Editor/EditorCommandBindingService.h"
#include "Editor/EditorCommandExecutionService.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorDocumentActionService.h"
#include "Editor/EditorDocumentCommandService.h"
#include "Editor/EditorDocumentFileCommandService.h"
#include "Editor/EditorEndFrameService.h"
#include "Editor/EditorFrameCoordinator.h"
#include "Editor/EditorFrameLifecycleService.h"
#include "Editor/EditorFrameSubmissionService.h"
#include "Editor/EditorInputBridgeService.h"
#include "Editor/EditorLegacyDebugUIService.h"
#include "Editor/EditorLayoutPersistenceService.h"
#include "Editor/EditorMainFramePresentationService.h"
#include "Editor/EditorMainFramebufferService.h"
#include "Editor/EditorMainSwapChainService.h"
#include "Editor/EditorNativeUIFrameService.h"
#include "Editor/EditorNativeUIRenderStatsService.h"
#include "Editor/EditorNativePanelRegistrationService.h"
#include "Editor/EditorNativeUISubmissionService.h"
#include "Editor/EditorNotificationService.h"
#include "Editor/EditorOperationService.h"
#include "Editor/EditorRenderBootstrapService.h"
#include "Editor/EditorRenderFrameService.h"
#include "Editor/EditorRenderRuntimeAdapter.h"
#include "Editor/EditorRenderShutdownService.h"
#include "Editor/EditorRunLoopService.h"
#include "Editor/EditorScreenshotRequestService.h"
#include "Editor/EditorScreenshotService.h"
#include "Editor/EditorSelectionService.h"
#include "Editor/EditorServiceRegistrationService.h"
#include "Editor/EditorServiceRegistry.h"
#include "Editor/EditorShortcutProfileLifecycleService.h"
#include "Editor/EditorUpdateService.h"
#include "Editor/EditorUIBootstrapService.h"
#include "Editor/EditorUIShutdownService.h"
#include "Editor/EditorViewportRenderService.h"
#include "Editor/EditorViewportToolService.h"
#include "Editor/EditorWindow.h"
#include "Editor/EditorWindowFrameService.h"
#include "Editor/EditorWindowLifecycleService.h"
#include "Editor/UI/EditorShellPolicy.h"
#include "Editor/UI/EditorShortcutProfileService.h"
#include "Editor/UI/EditorUIBackendCatalog.h"
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>
#include <string>

#ifndef RVX_EDITOR_ENABLE_LEGACY_IMGUI
#define RVX_EDITOR_ENABLE_LEGACY_IMGUI 1
#endif

struct GLFWwindow;

namespace RVX
{
    class IRHIDevice;
    class RenderContext;
    class SceneRenderer;

    namespace UI
    {
        struct UIInputState;
        class UIRenderer;
    }
}

namespace RVX::Editor
{

#if RVX_EDITOR_ENABLE_LEGACY_IMGUI
class DebugImGuiLayer;
#endif
class EditorDocumentSession;
class EditorFileDialogService;
class EditorInputBridge;
class EditorPendingActionQueue;
class EditorSettingsService;
class EditorShortcutProfileService;
class EditorUnsavedChangesGuard;
class IEditorPanel;
class IEditorUIBackend;
enum class EditorUnsavedChangesChoice : uint8;

struct EditorApplicationRunConfig
{
    uint32 maxFrames = 0;
    std::string screenshotPath;
    std::filesystem::path settingsPathOverride;
    EditorAutomationScenarioType automationScenario =
        EditorAutomationScenarioType::None;
    std::string automationTarget;
    bool automationConfirmOverwrite = false;
    EditorUIBackendType uiBackendType = GetDefaultEditorUIBackendType();
    bool overrideUIBackendType = false;
    std::filesystem::path autoShortcutProfilePath;
    bool exposeLegacyImGuiDebugCommands = false;
    bool startMaximized = true;
};

/**
 * @brief Main editor application
 *
 * Manages the editor window, native editor UI host, debug ImGui bridge,
 * and all editor panels.
 */
class EditorApplication
{
public:
    EditorApplication();
    ~EditorApplication();

    // =========================================================================
    // Lifecycle
    // =========================================================================

    /**
     * @brief Initialize the editor
     * @return true if initialization succeeded
     */
    bool Initialize();

    /**
     * @brief Run the main editor loop
     * @return Exit code
     */
    int Run();

    /**
     * @brief Configure automated run behavior before Run().
     */
    void SetRunConfig(const EditorApplicationRunConfig& config) { m_runConfig = config; }

    /**
     * @brief Shutdown the editor
     */
    void Shutdown();

    // =========================================================================
    // Window
    // =========================================================================

    /**
     * @brief Get the main window
     */
    GLFWwindow* GetWindow() const { return m_window; }

    /**
     * @brief Get window dimensions
     */
    void GetWindowSize(int& width, int& height) const;

    // =========================================================================
    // Panels
    // =========================================================================

    /**
     * @brief Register a panel
     */
    void RegisterPanel(std::shared_ptr<IEditorPanel> panel);

    /**
     * @brief Dispatch native input to visible legacy panels.
     */
    void DispatchNativeInputToPanels(const UI::UIInputState& input);

    /**
     * @brief Get a panel by name
     */
    IEditorPanel* GetPanel(const std::string& name);

    /**
     * @brief Bind an RHI device to viewport panels
     */
    void BindRenderDeviceToViewportPanels(IRHIDevice* device);

    /**
     * @brief Get the editor RHI device when available
     */
    IRHIDevice* GetRenderDevice() const;

    /**
     * @brief Get viewport scene-render bridge diagnostics.
     */
    const EditorViewportSceneRenderStats& GetViewportSceneRenderStats() const
    {
        return m_viewportSceneRenderStats;
    }

    /**
     * @brief Get native editor UI render bridge diagnostics.
     */
    const EditorNativeUIRenderStats& GetNativeUIRenderStats() const
    {
        return m_nativeUIRenderStats;
    }

    /** @brief Get the value-only dedicated render runtime diagnostics. */
    [[nodiscard]] RenderDiagnosticsSnapshot GetRenderDiagnostics() const
    {
        return m_renderRuntimeAdapter.GetDiagnostics();
    }

    /** @brief Query whether an Editor render feature is adapted in M1. */
    [[nodiscard]] EditorRenderFeatureStatus GetRenderFeatureStatus(
        EditorRenderFeature feature) const noexcept
    {
        return m_renderRuntimeAdapter.GetFeatureStatus(feature);
    }

    /**
     * @brief Get the typed editor service registry.
     */
    EditorServiceRegistry& GetServices() { return m_services; }
    const EditorServiceRegistry& GetServices() const { return m_services; }

    /**
     * @brief Get startup shortcut profile load diagnostics.
     */
    bool WasStartupShortcutProfileLoadAttempted() const
    {
        return m_shortcutProfileLifecycleService.WasStartupLoadAttempted();
    }
    const EditorShortcutProfileOperationResult&
    GetStartupShortcutProfileLoadResult() const
    {
        return m_shortcutProfileLifecycleService.GetStartupLoadResult();
    }

    // =========================================================================
    // Frame
    // =========================================================================

    /**
     * @brief Get delta time for current frame
     */
    float GetDeltaTime() const { return m_deltaTime; }

    /**
     * @brief Get total elapsed time
     */
    float GetTime() const { return m_totalTime; }

private:
    bool InitializeWindow();
    bool InitializeRHI();
    bool InitializeEditorUI();
    bool InitializeImGui();
    bool InitializePanels();
    void RegisterCoreEditorServices();

    void BeginFrame();
    void Update(float deltaTime);
    void Render();
    void EndFrame();
    EditorShellPolicyState BuildShellPolicyState() const;
    EditorShellPolicyDecision EvaluateShellPolicy() const;
    uint32 CountVisibleLegacyPanels() const;
    bool ShouldUseNativeMainShell() const;
    bool HasVisibleLegacyPanels() const;
    bool ShouldDrawLegacyDockSpace() const;
    bool ShouldRunDebugImGuiFrame() const;

    void DrawMainMenuBar();
    void DrawDockSpace();
    void DrawStatusBar();
    void DrawPanels();

    EditorCommandActionResult HandleFileNewScene();
    EditorCommandActionResult HandleFileOpenScene();
    EditorCommandActionResult HandleFileSaveScene();
    EditorCommandActionResult HandleFileSaveSceneAs();
    EditorCommandActionResult HandleFileExit();
    EditorCommandActionResult RequestGuardedDocumentAction(
        std::string actionId,
        std::string label,
        std::function<EditorCommandActionResult()> continuation);
    void OpenUnsavedChangesModalForPendingAction();
    void ResolvePendingUnsavedChanges(EditorUnsavedChangesChoice choice);
    EditorCommandActionResult ExecuteFileNewScene();
    EditorCommandActionResult ExecuteFileOpenScene();
    EditorCommandActionResult ExecuteFileSaveSceneAs();
    void PublishInfoNotification(const std::string& title,
                                 const std::string& message);
    void PublishWarningNotification(const std::string& title,
                                    const std::string& message);
    void PublishErrorNotification(const std::string& title,
                                  const std::string& message);
    EditorCommandActionResult OpenNativeOpenScenePicker();
    EditorCommandActionResult OpenNativeSaveSceneAsPicker(
        std::function<void(bool)> onSaved);
    EditorCommandActionResult OpenFallbackOpenSceneFileDialog();
    EditorCommandActionResult OpenFallbackSaveSceneAsDialog();
    void OpenNativeSaveSceneAsPickerForPendingAction();
    void OpenNativeSaveOverwriteConfirmation(
        std::filesystem::path path,
        std::function<void(bool)> onSaved);
    void SaveNativeSceneAsConfirmedPath(std::filesystem::path path,
                                        std::function<void(bool)> onSaved);
    void CompleteNativeSaveSceneAs(bool saved,
                                   std::function<void(bool)> onSaved);
    EditorCommandActionResult ExecuteFileExit();
    void RefreshDocumentCommandStates();
    void RefreshNativeViewCommandStates();
    void HandleEditorContextChanged(const EditorContextChangeEvent& event);

    // Window
    GLFWwindow* m_window = nullptr;
    int m_windowWidth = 1920;
    int m_windowHeight = 1080;
    std::string m_windowTitle = "RenderVerseX Editor";

    // RHI
    EditorRenderRuntimeAdapter m_renderRuntimeAdapter;
    std::unique_ptr<RenderContext> m_renderContext;
    std::unique_ptr<SceneRenderer> m_sceneRenderer;
    std::unique_ptr<UI::UIRenderer> m_runtimeUIRenderer;
    std::unique_ptr<UI::UIRenderer> m_editorUIRenderer;
    EditorFrameCoordinator m_frameCoordinator;
    EditorFrameSubmissionService m_frameSubmissionService;
    EditorMainFramePresentationService m_mainFramePresentationService;
    EditorMainFramebufferService m_mainFramebufferService;
    EditorMainSwapChainService m_mainSwapChainService;
    EditorNativeUIFrameService m_nativeUIFrameService;
    EditorNativeUIRenderStatsService m_nativeUIRenderStatsService;
    EditorNativePanelRegistrationService m_nativePanelRegistrationService;
    EditorNativeUISubmissionService m_nativeUISubmissionService;
    EditorBeginFrameService m_beginFrameService;
    EditorCommandBindingService m_commandBindingService;
    EditorCommandExecutionService m_commandExecutionService;
    EditorDocumentActionService m_documentActionService;
    EditorDocumentCommandService m_documentCommandService;
    EditorDocumentFileCommandService m_documentFileCommandService;
    EditorEndFrameService m_endFrameService;
    EditorFrameLifecycleService m_frameLifecycleService;
    EditorInputBridgeService m_inputBridgeService;
    EditorLegacyDebugUIService m_legacyDebugUIService;
    EditorLayoutPersistenceService m_layoutPersistenceService;
    EditorNotificationService m_notificationService;
    EditorOperationService m_operationService;
    EditorRenderBootstrapService m_renderBootstrapService;
    EditorRenderFrameService m_renderFrameService;
    EditorRenderShutdownService m_renderShutdownService;
    EditorRunLoopService m_runLoopService;
    EditorScreenshotRequestService m_screenshotRequestService;
    EditorScreenshotService m_screenshotService;
    EditorSelectionService m_selectionService;
    EditorServiceRegistrationService m_serviceRegistrationService;
    EditorAutomationScenarioService m_automationScenarioService;
    EditorShortcutProfileLifecycleService m_shortcutProfileLifecycleService;
    EditorUpdateService m_updateService;
    EditorUIBootstrapService m_uiBootstrapService;
    EditorUIShutdownService m_uiShutdownService;
    EditorViewportRenderService m_viewportRenderService;
    EditorViewportToolService m_viewportToolService;
    EditorWindowFrameService m_windowFrameService;
    EditorWindowLifecycleService m_windowLifecycleService;
    EditorViewportSceneRenderStats m_viewportSceneRenderStats;
    EditorNativeUIRenderStats m_nativeUIRenderStats;
    EditorShellPolicy m_shellPolicy;
    EditorServiceRegistry m_services;

    // Native editor UI infrastructure
    std::unique_ptr<IEditorUIBackend> m_editorUIBackend;
    std::unique_ptr<EditorInputBridge> m_editorInputBridge;
    std::unique_ptr<EditorDocumentSession> m_documentSession;
    std::unique_ptr<EditorFileDialogService> m_fileDialogService;
    std::unique_ptr<EditorSettingsService> m_settingsService;
    std::unique_ptr<EditorShortcutProfileService> m_shortcutProfileService;
    std::unique_ptr<EditorUnsavedChangesGuard> m_unsavedChangesGuard;
    std::unique_ptr<EditorPendingActionQueue> m_pendingActions;
    uint64 m_editorContextChangeCallbackId = 0;

#if RVX_EDITOR_ENABLE_LEGACY_IMGUI
    // Legacy/debug UI bridge
    std::unique_ptr<DebugImGuiLayer> m_debugImGuiLayer;
#endif

    // Panels
    std::vector<std::shared_ptr<IEditorPanel>> m_panels;

    // Timing
    float m_deltaTime = 0.0f;
    float m_totalTime = 0.0f;
    double m_lastFrameTime = 0.0;

    // State
    bool m_running = false;
    bool m_showDemoWindow = false;
    bool m_showMetricsWindow = false;
    bool m_debugImGuiFrameActive = false;
    EditorApplicationRunConfig m_runConfig;
};

} // namespace RVX::Editor
