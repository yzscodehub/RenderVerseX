/**
 * @file EditorUIHost.h
 * @brief Host for native editor UI contexts and panels
 */

#pragma once

#include "Editor/UI/EditorCommandPalette.h"
#include "Editor/UI/EditorCommandRegistry.h"
#include "Editor/UI/EditorCommandRouter.h"
#include "Editor/UI/EditorCommandSurfaceModel.h"
#include "Editor/UI/EditorCommandSurfaceRenderer.h"
#include "Editor/UI/EditorContextMenu.h"
#include "Editor/UI/EditorDockingModel.h"
#include "Editor/UI/EditorFilePickerDialog.h"
#include "Editor/UI/EditorModalDialog.h"
#include "Editor/UI/EditorPanelSurfaceRenderer.h"
#include "Editor/UI/EditorPopupLayer.h"
#include "Editor/UI/EditorTooltipLayer.h"
#include "Editor/UI/EditorUIPanel.h"
#include "Editor/UI/EditorPropertyDrawerRegistry.h"
#include "Editor/UI/EditorUIBackendTypes.h"
#include "UI/UIContext.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace RVX
{
    class IRHIDevice;

    namespace UI
    {
        class UIRenderer;
        class Widget;
    }
}

namespace RVX::Editor
{

struct EditorPanelInputRoutingState
{
    std::string hoveredPanelId;
    std::string activePanelId;
    std::string keyboardFocusPanelId;
    std::string mouseCapturePanelId;
    bool wantsMouseCapture = false;
    bool wantsKeyboardCapture = false;
    bool popupCapturesInput = false;
    bool commandSurfaceMenuOpen = false;
    bool contextMenuOpen = false;
    bool commandPaletteOpen = false;
    bool filePickerOpen = false;
    bool modalDialogOpen = false;
    EditorUICursorRequest cursorRequest;
};

enum class EditorUICommandExecutionSource : uint8
{
    Direct = 0,
    Shortcut,
    Automation
};

enum class EditorUIDiagnosticSeverity : uint8
{
    Info = 0,
    Warning,
    Error
};

struct EditorUICommandHistoryEntry
{
    uint64 sequence = 0;
    uint64 frameIndex = 0;
    float deltaTime = 0.0f;
    EditorUICommandExecutionSource source =
        EditorUICommandExecutionSource::Direct;
    EditorCommandExecutionResult result;
    bool diagnostic = false;
    bool diagnosticSucceeded = false;
    EditorUIDiagnosticSeverity diagnosticSeverity =
        EditorUIDiagnosticSeverity::Info;
    std::string diagnosticMessage;
    std::string displayName;
    std::string statusText;
    uint32 shortcutConflictCount = 0;

    bool Succeeded() const
    {
        return diagnostic ? diagnosticSucceeded : result.Succeeded();
    }
};

struct EditorUICommandDiagnostics
{
    EditorCommandExecutionResult lastResult;
    std::string lastStatusText = "Ready";
    bool hasExternalDiagnostic = false;
    uint64 attemptedCount = 0;
    uint64 succeededCount = 0;
    uint64 failedCount = 0;
    uint64 historyDroppedCount = 0;
    uint32 shortcutConflictCount = 0;
    bool hasResult = false;
    std::vector<EditorUICommandHistoryEntry> history;
};

struct EditorUIPanelRebuildState
{
    uint64 revision = 0;
    uint32 pendingReasonMask = 0;
    uint32 lastReasonMask = 0;
    bool pending = false;
    bool builtThisFrame = false;
};

struct EditorUIPanelSnapshot
{
    std::string id;
    std::string title;
    EditorUIPanelDockArea defaultDockArea = EditorUIPanelDockArea::Floating;
    EditorUIPanelDockArea dockArea = EditorUIPanelDockArea::Floating;
    std::string dockTabStackId;
    float dockNormalizedSize = 1.0f;
    uint32 dockOrder = 0;
    uint32 floatingZOrder = 0;
    bool registered = false;
    bool visibleByDefault = false;
    bool visible = false;
    bool closable = false;
    bool hasDockPlacement = false;
    bool hasLastBounds = false;
    UI::Rect lastPanelBounds;
    UI::Rect lastContentBounds;
    EditorUIPanelRebuildState rebuildState;
};

class EditorUIHost
{
public:
    bool Initialize(const EditorUIHostDesc& desc);
    void Shutdown();

    bool BeginFrame(const EditorUIFrameDesc& desc);
    void Update(float deltaTime);
    void BuildPanels();
    bool RenderNativeUI(UI::UIRenderer& renderer);
    void EndFrame();

    bool IsInitialized() const { return m_initialized; }
    uint64 GetFrameIndex() const { return m_uiContext.GetFrameIndex(); }

    void SetRenderDevice(IRHIDevice* device) { m_renderDevice = device; }
    IRHIDevice* GetRenderDevice() const { return m_renderDevice; }

    UI::UIContext& GetUIContext() { return m_uiContext; }
    const UI::UIContext& GetUIContext() const { return m_uiContext; }

    EditorCommandRegistry& GetCommandRegistry() { return m_commandRegistry; }
    const EditorCommandRegistry& GetCommandRegistry() const { return m_commandRegistry; }
    EditorCommandSurfaceModel& GetCommandSurfaceModel() { return m_commandSurfaceModel; }
    const EditorCommandSurfaceModel& GetCommandSurfaceModel() const { return m_commandSurfaceModel; }
    const EditorCommandSurfaceRenderer& GetCommandSurfaceRenderer() const
    {
        return m_commandSurfaceRenderer;
    }
    const EditorPanelSurfaceRenderer& GetPanelSurfaceRenderer() const
    {
        return m_panelSurfaceRenderer;
    }
    const EditorPopupLayer& GetPopupLayer() const
    {
        return m_popupLayer;
    }
    const EditorTooltipLayer& GetTooltipLayer() const
    {
        return m_tooltipLayer;
    }
    EditorContextMenu& GetContextMenu()
    {
        return m_contextMenu;
    }
    const EditorContextMenu& GetContextMenu() const
    {
        return m_contextMenu;
    }
    EditorCommandPalette& GetCommandPalette()
    {
        return m_commandPalette;
    }
    const EditorCommandPalette& GetCommandPalette() const
    {
        return m_commandPalette;
    }
    EditorFilePickerDialog& GetFilePickerDialog()
    {
        return m_filePickerDialog;
    }
    const EditorFilePickerDialog& GetFilePickerDialog() const
    {
        return m_filePickerDialog;
    }
    EditorModalDialog& GetModalDialog()
    {
        return m_modalDialog;
    }
    const EditorModalDialog& GetModalDialog() const
    {
        return m_modalDialog;
    }
    const EditorUINativeRenderStats& GetNativeRenderStats() const
    {
        return m_nativeRenderStats;
    }
    EditorDockingModel& GetDockingModel() { return m_dockingModel; }
    const EditorDockingModel& GetDockingModel() const { return m_dockingModel; }
    EditorPropertyDrawerRegistry& GetPropertyDrawerRegistry() { return m_propertyDrawerRegistry; }
    const EditorPropertyDrawerRegistry& GetPropertyDrawerRegistry() const
    {
        return m_propertyDrawerRegistry;
    }

    void RegisterPanel(std::shared_ptr<IEditorUIPanel> panel);
    bool UnregisterPanel(const std::string& id);
    IEditorUIPanel* GetPanel(const std::string& id) const;

    size_t GetPanelCount() const { return m_panels.size(); }
    uint32 GetVisiblePanelCount() const;
    std::vector<EditorUIPanelSnapshot> GetPanelSnapshots() const;
    bool TryGetPanelSnapshot(const std::string& id,
                             EditorUIPanelSnapshot& outSnapshot) const;
    void SetPanelVisible(const std::string& id, bool visible);
    bool IsPanelVisible(const std::string& id) const;
    const std::string& GetHoveredPanelId() const { return m_hoveredPanelId; }
    const std::string& GetActivePanelId() const { return m_activePanelId; }
    const EditorPanelInputRoutingState& GetInputRoutingState() const
    {
        return m_inputRoutingState;
    }
    const EditorUICommandDiagnostics& GetCommandDiagnostics() const
    {
        return m_commandDiagnostics;
    }
    const std::vector<EditorUICommandHistoryEntry>& GetCommandHistory() const
    {
        return m_commandDiagnostics.history;
    }
    const EditorCommandShortcutRouteResult& GetLastShortcutRouteResult() const
    {
        return m_commandRouter.GetLastShortcutRouteResult();
    }
    void ClearCommandHistory();
    bool WantsMouseCapture() const { return m_inputRoutingState.wantsMouseCapture; }
    bool WantsKeyboardCapture() const { return m_inputRoutingState.wantsKeyboardCapture; }
    const EditorUICursorRequest& GetCursorRequest() const
    {
        return m_inputRoutingState.cursorRequest;
    }
    bool IsPanelHovered(const std::string& id) const;
    bool IsPanelActive(const std::string& id) const;
    bool IsPanelKeyboardFocused(const std::string& id) const;
    bool IsPanelMouseCaptured(const std::string& id) const;
    bool RequestPanelRebuild(
        const std::string& id,
        EditorUIPanelRebuildReason reason = EditorUIPanelRebuildReason::Explicit);
    EditorUIPanelRebuildState GetPanelRebuildState(const std::string& id) const;
    bool ExecuteCommand(const std::string& id);
    void RecordAutomationDiagnostic(const std::string& scenarioName,
                                    const std::string& target,
                                    bool succeeded,
                                    const std::string& message);
    void OpenCommandPalette(std::string initialFilter = {});
    void CloseCommandPalette();
    void OpenFilePickerDialog(EditorFilePickerDialogDesc desc);
    void CloseFilePickerDialog();
    void OpenContextMenu(EditorContextMenuDesc desc);
    void CloseContextMenu();
    void OpenModalDialog(EditorModalDialogDesc desc);
    void CloseModalDialog();
    void RequestFocusAfterBuild(std::string widgetName);
    void ResetLayout();
    void RefreshLayoutCommandStates();
    bool IsBottomDrawerCollapsed() const;
    void SetBottomDrawerCollapsed(bool collapsed);
    void ToggleBottomDrawerCollapsed();
    bool SaveLayout(const std::filesystem::path& path, std::string* error = nullptr) const;
    bool LoadLayout(const std::filesystem::path& path, std::string* error = nullptr);

private:
    struct PanelEntry
    {
        std::shared_ptr<IEditorUIPanel> panel;
        bool visible = true;
        uint64 rebuildRevision = 0;
        uint32 pendingRebuildReasonMask = 0;
        uint32 lastRebuildReasonMask = 0;
        bool builtThisFrame = false;
        bool builtLastFrame = false;
        bool hasLastBounds = false;
        UI::Rect lastPanelBounds;
        UI::Rect lastContentBounds;
    };

    PanelEntry* FindPanelEntry(const std::string& id);
    const PanelEntry* FindPanelEntry(const std::string& id) const;
    EditorUIPanelSnapshot BuildPanelSnapshot(const PanelEntry& entry) const;
    void EnsureDockPlacement(PanelEntry& entry);
    void SyncPanelVisibilityFromDocking();
    void ClearInputRoutingForPanel(const std::string& id);
    std::string ResolvePanelIdForWidget(const UI::Widget* widget) const;
    void RequestPanelRebuild(PanelEntry& entry, uint32 reasonMask);
    void RecordCommandExecutionResult(
        const EditorCommandExecutionResult& result,
        EditorUICommandExecutionSource source);
    void RefreshCommandDiagnosticsStatus();
    bool TryHandleCommandSurfaceKeyboardEntry();
    EditorCommandShortcutRoutingPolicy BuildShortcutRoutingPolicy() const;
    void UpdateInputRoutingState(const std::string& keyboardFocusPanelId,
                                 const std::string& mouseCapturePanelId,
                                 bool hasFocusedWidget,
                                 const EditorUICursorRequest& cursorRequest = {});

    bool m_initialized = false;
    IRHIDevice* m_renderDevice = nullptr;
    UI::UIContext m_uiContext;
    EditorCommandRegistry m_commandRegistry;
    EditorCommandRouter m_commandRouter;
    EditorCommandPalette m_commandPalette;
    EditorFilePickerDialog m_filePickerDialog;
    EditorCommandSurfaceModel m_commandSurfaceModel;
    EditorCommandSurfaceRenderer m_commandSurfaceRenderer;
    EditorContextMenu m_contextMenu;
    EditorModalDialog m_modalDialog;
    EditorPanelSurfaceRenderer m_panelSurfaceRenderer;
    EditorPopupLayer m_popupLayer;
    EditorTooltipLayer m_tooltipLayer;
    EditorUINativeRenderStats m_nativeRenderStats;
    EditorUICommandDiagnostics m_commandDiagnostics;
    EditorDockingModel m_dockingModel;
    EditorPropertyDrawerRegistry m_propertyDrawerRegistry;
    std::vector<PanelEntry> m_panels;
    std::string m_contextMenuRestoreFocusWidgetName;
    std::string m_commandPaletteRestoreFocusWidgetName;
    std::string m_filePickerRestoreFocusWidgetName;
    std::string m_modalDialogRestoreFocusWidgetName;
    std::string m_postBuildFocusWidgetName;
    std::string m_hoveredPanelId;
    std::string m_activePanelId;
    EditorPanelInputRoutingState m_inputRoutingState;
    uint64 m_nextCommandHistorySequence = 1;
    float m_currentDeltaTime = 0.0f;
};

} // namespace RVX::Editor
