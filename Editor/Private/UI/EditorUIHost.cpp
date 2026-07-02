/**
 * @file EditorUIHost.cpp
 * @brief Native editor UI host implementation
 */

#include "Editor/UI/EditorUIHost.h"

#include "Editor/EditorTheme.h"
#include "Editor/Panels/NativeViewport.h"
#include "Editor/UI/EditorCommandCatalog.h"
#include "Editor/UI/EditorLayoutSerializer.h"
#include "Editor/UI/EditorWorkspaceLayoutPolicy.h"

#include "UI/UIRenderer.h"
#include "UI/Widget.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <utility>
#include <vector>

namespace RVX::Editor
{
namespace
{
    constexpr const char* RVX_EDITOR_PANEL_WIDGET_PREFIX =
        "Editor.PanelSurface.Panel.";
    constexpr const char* RVX_EDITOR_COMMAND_STATUS_ITEM_ID = "editor.command";
    constexpr size_t RVX_EDITOR_COMMAND_HISTORY_CAPACITY = 64;

    bool IsAnyMouseButtonDown(const UI::UIInputSnapshot& input)
    {
        for (bool down : input.mouseButtonsDown)
        {
            if (down)
            {
                return true;
            }
        }
        return false;
    }

    bool IsAnyMouseButtonPressed(const UI::UIInputState& input)
    {
        for (bool pressed : input.mouseButtonsPressed)
        {
            if (pressed)
            {
                return true;
            }
        }
        return false;
    }

    bool IsViewportShortcutPanelId(const std::string& panelId)
    {
        return panelId == NativeViewportPanel::PanelId();
    }

    bool ShouldHideTransientToolPanelOnStartup(const std::string& panelId)
    {
        return panelId == "native.preferences" ||
               panelId == "native.panelCatalog" ||
               panelId == "native.commandHistory";
    }

    uint32 CursorRequestPriority(const EditorUICursorRequest& request)
    {
        if (request.cursorMode == EditorUICursorMode::Locked ||
            request.requestsCursorLock)
        {
            return 3u;
        }
        if (request.cursorMode == EditorUICursorMode::Hidden ||
            request.requestsCursorHidden)
        {
            return 2u;
        }
        if (request.requestsMouseCapture)
        {
            return 1u;
        }
        return 0u;
    }

    void AccumulateCursorRequest(EditorUICursorRequest& aggregate,
                                 EditorUICursorRequest request,
                                 const std::string& panelId)
    {
        if (!request.IsActive())
        {
            return;
        }

        if (request.ownerPanelId.empty())
        {
            request.ownerPanelId = panelId;
        }

        if (CursorRequestPriority(request) >= CursorRequestPriority(aggregate))
        {
            aggregate = std::move(request);
        }
    }

    constexpr uint32 RVX_EDITOR_PANEL_REBUILD_ALL_REASONS =
        ToEditorUIPanelRebuildReasonMask(EditorUIPanelRebuildReason::Initial) |
        ToEditorUIPanelRebuildReasonMask(EditorUIPanelRebuildReason::Explicit) |
        ToEditorUIPanelRebuildReasonMask(EditorUIPanelRebuildReason::Visibility) |
        ToEditorUIPanelRebuildReasonMask(EditorUIPanelRebuildReason::Layout) |
        ToEditorUIPanelRebuildReasonMask(EditorUIPanelRebuildReason::Docking) |
        ToEditorUIPanelRebuildReasonMask(EditorUIPanelRebuildReason::Data);

    uint32 SanitizePanelRebuildReasonMask(uint32 reasonMask)
    {
        return reasonMask & RVX_EDITOR_PANEL_REBUILD_ALL_REASONS;
    }

    bool RectNearlyEqual(const UI::Rect& lhs, const UI::Rect& rhs)
    {
        constexpr float RVX_EDITOR_PANEL_BOUNDS_EPSILON = 0.01f;
        return std::fabs(lhs.x - rhs.x) <= RVX_EDITOR_PANEL_BOUNDS_EPSILON &&
               std::fabs(lhs.y - rhs.y) <= RVX_EDITOR_PANEL_BOUNDS_EPSILON &&
               std::fabs(lhs.width - rhs.width) <= RVX_EDITOR_PANEL_BOUNDS_EPSILON &&
               std::fabs(lhs.height - rhs.height) <= RVX_EDITOR_PANEL_BOUNDS_EPSILON;
    }

    std::string CommandExecutionStatusText(EditorCommandExecutionStatus status)
    {
        switch (status)
        {
        case EditorCommandExecutionStatus::Succeeded:
            return "OK";
        case EditorCommandExecutionStatus::NotFound:
            return "Not found";
        case EditorCommandExecutionStatus::Disabled:
            return "Disabled";
        case EditorCommandExecutionStatus::MissingCallback:
            return "Missing callback";
        case EditorCommandExecutionStatus::ShortcutNotMatched:
            return "No shortcut";
        case EditorCommandExecutionStatus::BlockedByKeyboardCapture:
            return "Keyboard captured";
        }

        return "Unknown";
    }

    std::string ResolveCommandDiagnosticName(
        const EditorCommandRegistry& registry,
        const std::string& commandId)
    {
        const EditorCommand* command = registry.FindCommand(commandId);
        if (command && !command->desc.displayName.empty())
        {
            return command->desc.displayName;
        }
        return commandId.empty() ? "Command" : commandId;
    }

    std::string BuildCommandDiagnosticsText(
        const EditorCommandRegistry& registry,
        const EditorUICommandDiagnostics& diagnostics)
    {
        const std::string conflictSuffix =
            diagnostics.shortcutConflictCount == 0u
                ? std::string()
                : (" (" + std::to_string(diagnostics.shortcutConflictCount) +
                   " shortcut conflict" +
                   (diagnostics.shortcutConflictCount == 1u ? ")" : "s)"));

        if (!diagnostics.hasResult)
        {
            return "Ready" + conflictSuffix;
        }

        const std::string commandName = ResolveCommandDiagnosticName(
            registry,
            diagnostics.lastResult.commandId);
        const std::string statusText =
            CommandExecutionStatusText(diagnostics.lastResult.status);
        if (diagnostics.lastResult.Succeeded())
        {
            return statusText + " " + commandName + conflictSuffix;
        }

        return "Failed " + commandName + " (" + statusText + ")" +
               conflictSuffix;
    }

    std::string CommandHistoryStatusText(
        const EditorCommandRegistry& registry,
        const EditorCommandExecutionResult& result)
    {
        const std::string commandName = ResolveCommandDiagnosticName(
            registry,
            result.commandId);
        const std::string statusText = CommandExecutionStatusText(result.status);
        if (result.Succeeded())
        {
            return statusText + " " + commandName;
        }

        return "Failed " + commandName + " (" + statusText + ")";
    }

    std::string BuildAutomationDiagnosticText(
        const std::string& scenarioName,
        const std::string& target,
        bool succeeded,
        const std::string& message)
    {
        std::string text =
            succeeded ? "Automation OK" : "Automation failed";
        if (!scenarioName.empty())
        {
            text += " " + scenarioName;
        }
        if (!target.empty())
        {
            text += ": " + target;
        }
        if (!message.empty())
        {
            text += " - " + message;
        }
        return text;
    }
}

bool EditorUIHost::Initialize(const EditorUIHostDesc& desc)
{
    if (m_initialized)
    {
        Shutdown();
    }

    m_renderDevice = desc.renderDevice;

    UI::UIContextDesc contextDesc;
    contextDesc.width = desc.surfaceWidth;
    contextDesc.height = desc.surfaceHeight;
    contextDesc.scaleFactor = desc.scaleFactor;
    contextDesc.inputScaleFactor = desc.inputScaleFactor;
    contextDesc.debugName = desc.debugName;
    contextDesc.theme = EditorTheme::CreateNativeDarkUITheme();
    if (!m_uiContext.Initialize(contextDesc))
    {
        return false;
    }

    RegisterDefaultEditorPropertyDrawers(m_propertyDrawerRegistry);
    m_dockingModel.SetAreaSizing(
        EditorWorkspaceLayoutPolicy{}.GetDefaultPreset().areaSizing);
    m_commandDiagnostics = {};
    m_nextCommandHistorySequence = 1;

    m_initialized = true;
    return true;
}

void EditorUIHost::Shutdown()
{
    for (PanelEntry& entry : m_panels)
    {
        if (entry.panel)
        {
            entry.panel->OnDetach();
        }
    }
    m_panels.clear();
    m_dockingModel.Clear();
    m_commandRegistry = {};
    m_commandSurfaceModel.Clear();
    m_commandSurfaceRenderer.Clear(m_uiContext);
    m_commandPalette.Clear();
    m_filePickerDialog.Clear();
    m_contextMenu.Clear();
    m_modalDialog.Clear();
    m_panelSurfaceRenderer.Clear(m_uiContext);
    m_popupLayer.Clear(m_uiContext);
    m_tooltipLayer.Clear(m_uiContext);
    m_nativeRenderStats = {};
    m_commandDiagnostics = {};
    m_nextCommandHistorySequence = 1;
    m_propertyDrawerRegistry = {};
    m_contextMenuRestoreFocusWidgetName.clear();
    m_commandPaletteRestoreFocusWidgetName.clear();
    m_filePickerRestoreFocusWidgetName.clear();
    m_modalDialogRestoreFocusWidgetName.clear();
    m_hoveredPanelId.clear();
    m_activePanelId.clear();
    m_inputRoutingState = {};
    m_uiContext.Shutdown();
    m_renderDevice = nullptr;
    m_currentDeltaTime = 0.0f;
    m_initialized = false;
}

bool EditorUIHost::BeginFrame(const EditorUIFrameDesc& desc)
{
    if (!m_initialized)
    {
        return false;
    }

    m_currentDeltaTime = desc.deltaTime;

    UI::UIFrameDesc frameDesc;
    frameDesc.width = desc.surfaceWidth;
    frameDesc.height = desc.surfaceHeight;
    frameDesc.deltaTime = desc.deltaTime;
    frameDesc.scaleFactor = desc.scaleFactor;
    frameDesc.inputScaleFactor = desc.inputScaleFactor;
    frameDesc.input = desc.input;
    if (!m_uiContext.BeginFrame(frameDesc))
    {
        return false;
    }

    if (m_uiContext.GetInput().WasKeyPressed(UI::RVX_UI_KEY_ESCAPE))
    {
        m_commandSurfaceRenderer.CloseOpenMenu();
        if (!m_modalDialog.IsOpen() && !m_filePickerDialog.IsOpen())
        {
            m_commandPalette.Close();
            m_contextMenu.Close();
        }
    }
    TryHandleCommandSurfaceKeyboardEntry();

    EditorCommandContext commandContext;
    commandContext.host = this;
    commandContext.deltaTime = m_currentDeltaTime;
    EditorCommandShortcutRouteDesc routeDesc;
    routeDesc.registry = &m_commandRegistry;
    routeDesc.input = &m_uiContext.GetInput();
    routeDesc.context = &commandContext;
    routeDesc.policy = BuildShortcutRoutingPolicy();
    const EditorCommandShortcutRouteResult& routeResult =
        m_commandRouter.RouteShortcut(routeDesc);
    RecordCommandExecutionResult(routeResult.commandResult,
                                 EditorUICommandExecutionSource::Shortcut);
    return true;
}

bool EditorUIHost::TryHandleCommandSurfaceKeyboardEntry()
{
    const UI::UIInputState& input = m_uiContext.GetInput();
    const uint32 altMask = UI::ToMask(UI::UIInputModifier::Alt);
    const bool altPressed =
        (input.current.modifiers & altMask) != 0u &&
        (input.previous.modifiers & altMask) == 0u &&
        (input.current.modifiers & ~altMask) == 0u;
    const bool f10Pressed = input.WasKeyPressed(UI::RVX_UI_KEY_F10);
    if (!altPressed && !f10Pressed)
    {
        return false;
    }

    if (m_modalDialog.IsOpen() || m_filePickerDialog.IsOpen() ||
        m_commandPalette.IsOpen() || m_contextMenu.IsOpen())
    {
        return false;
    }

    if (!m_commandSurfaceRenderer.GetOpenMenuId().empty())
    {
        m_commandSurfaceRenderer.CloseOpenMenu();
        return true;
    }

    const std::vector<EditorMenuSurface>& menus = m_commandSurfaceModel.GetMenus();
    if (menus.empty())
    {
        return false;
    }

    return m_commandSurfaceRenderer.OpenMenu(menus.front().id,
                                             m_commandRegistry,
                                             m_commandSurfaceModel);
}

void EditorUIHost::Update(float deltaTime)
{
    if (!m_initialized)
    {
        return;
    }

    for (PanelEntry& entry : m_panels)
    {
        if (entry.visible && entry.panel)
        {
            entry.panel->OnUpdate(deltaTime);
        }
    }
}

void EditorUIHost::BuildPanels()
{
    if (!m_initialized || !m_uiContext.IsFrameActive())
    {
        return;
    }

    UI::UICanvas& canvas = m_uiContext.GetCanvas();
    std::string focusedWidgetName;
    if (UI::Widget* focusedWidget = canvas.GetFocusedWidget())
    {
        focusedWidgetName = focusedWidget->GetName();
    }
    const std::string preBuildKeyboardFocusPanelId =
        ResolvePanelIdForWidget(canvas.GetFocusedWidget());

    EditorUIPanelFrameContext frameContext;
    frameContext.ui = &m_uiContext;
    frameContext.canvas = &canvas;
    frameContext.host = this;
    frameContext.input = &m_uiContext.GetInput();
    frameContext.deltaTime = m_currentDeltaTime;

    m_popupLayer.Begin(m_uiContext);
    m_popupLayer.SetOutsideClickCallback([this]() {
        m_commandSurfaceRenderer.CloseOpenMenu();
        m_commandPalette.Close();
        m_contextMenu.Close();
    });

    EditorCommandSurfaceRenderDesc surfaceDesc;
    surfaceDesc.ui = &m_uiContext;
    surfaceDesc.host = this;
    surfaceDesc.commandRegistry = &m_commandRegistry;
    surfaceDesc.surfaceModel = &m_commandSurfaceModel;
    surfaceDesc.popupLayer = &m_popupLayer;
    RefreshCommandDiagnosticsStatus();
    m_commandSurfaceRenderer.Build(surfaceDesc);

    EditorPanelSurfaceRenderDesc panelSurfaceDesc;
    panelSurfaceDesc.ui = &m_uiContext;
    panelSurfaceDesc.host = this;
    panelSurfaceDesc.dockingModel = &m_dockingModel;
    const EditorCommandSurfaceRenderStats& commandSurfaceStats =
        m_commandSurfaceRenderer.GetLastBuildStats();
    panelSurfaceDesc.topReservedHeight = commandSurfaceStats.topReservedHeight;
    panelSurfaceDesc.bottomReservedHeight = commandSurfaceStats.bottomReservedHeight;
    std::unordered_map<std::string, std::string> panelTitles;
    panelTitles.reserve(m_panels.size());
    for (const PanelEntry& entry : m_panels)
    {
        if (entry.panel)
        {
            const EditorUIPanelDesc& panelDesc = entry.panel->GetPanelDesc();
            panelTitles[panelDesc.id] = panelDesc.title;
        }
    }
    panelSurfaceDesc.panelTitles = &panelTitles;
    m_panelSurfaceRenderer.Begin(panelSurfaceDesc);

    struct BuiltPanelFrame
    {
        PanelEntry* entry = nullptr;
        EditorPanelSurfaceFrame frame;
        bool hovered = false;
    };

    std::vector<PanelEntry*> panelBuildOrder;
    panelBuildOrder.reserve(m_panels.size());
    for (PanelEntry& entry : m_panels)
    {
        panelBuildOrder.push_back(&entry);
    }
    std::stable_sort(panelBuildOrder.begin(),
                     panelBuildOrder.end(),
                     [this](const PanelEntry* lhs, const PanelEntry* rhs) {
                         const EditorDockPanelPlacement* lhsPlacement =
                             lhs && lhs->panel
                                 ? m_dockingModel.FindPlacement(lhs->panel->GetPanelDesc().id)
                                 : nullptr;
                         const EditorDockPanelPlacement* rhsPlacement =
                             rhs && rhs->panel
                                 ? m_dockingModel.FindPlacement(rhs->panel->GetPanelDesc().id)
                                 : nullptr;
                         const bool lhsFloating =
                             lhsPlacement &&
                             lhsPlacement->area == EditorUIPanelDockArea::Floating;
                         const bool rhsFloating =
                             rhsPlacement &&
                             rhsPlacement->area == EditorUIPanelDockArea::Floating;
                         if (lhsFloating != rhsFloating)
                         {
                             return !lhsFloating;
                         }
                         if (!lhsFloating)
                         {
                             return false;
                         }
                         if (lhsPlacement->floatingZOrder != rhsPlacement->floatingZOrder)
                         {
                             return lhsPlacement->floatingZOrder <
                                    rhsPlacement->floatingZOrder;
                         }
                         if (lhsPlacement->order != rhsPlacement->order)
                         {
                             return lhsPlacement->order < rhsPlacement->order;
                         }
                         return lhsPlacement->panelId < rhsPlacement->panelId;
                     });

    std::vector<BuiltPanelFrame> builtPanelFrames;
    for (PanelEntry& entry : m_panels)
    {
        entry.builtThisFrame = false;
        entry.lastRebuildReasonMask = 0;
    }

    for (PanelEntry* entry : panelBuildOrder)
    {
        if (entry && entry->visible && entry->panel)
        {
            const EditorPanelSurfaceFrame panelFrame =
                m_panelSurfaceRenderer.BuildPanelFrame(entry->panel->GetPanelDesc());
            if (!panelFrame.built)
            {
                continue;
            }

            BuiltPanelFrame builtFrame;
            builtFrame.entry = entry;
            builtFrame.frame = panelFrame;
            builtPanelFrames.push_back(builtFrame);
        }
    }

    const UI::UIInputState& input = m_uiContext.GetInput();
    const bool anyMousePressed = IsAnyMouseButtonPressed(input);
    bool pressedInsidePanel = false;
    std::string pressedPanelId;
    m_hoveredPanelId.clear();
    if (!m_activePanelId.empty() && !IsPanelVisible(m_activePanelId))
    {
        m_activePanelId.clear();
    }

    for (BuiltPanelFrame& builtFrame : builtPanelFrames)
    {
        const std::string& panelId = builtFrame.entry->panel->GetPanelDesc().id;
        builtFrame.hovered =
            builtFrame.frame.panelBounds.Contains(input.current.mousePosition);
        if (builtFrame.hovered)
        {
            m_hoveredPanelId = panelId;
            if (anyMousePressed)
            {
                pressedInsidePanel = true;
                pressedPanelId = panelId;
            }
        }
    }

    if (anyMousePressed)
    {
        m_activePanelId = pressedInsidePanel ? pressedPanelId : std::string();
    }
    const std::string mouseCapturePanelId =
        IsAnyMouseButtonDown(input.current) ? m_activePanelId : std::string();

    EditorUICursorRequest cursorRequest;
    for (BuiltPanelFrame& builtFrame : builtPanelFrames)
    {
        PanelEntry* entry = builtFrame.entry;
        if (!entry || !entry->panel)
        {
            continue;
        }

        frameContext.panelContainer = builtFrame.frame.panelContainer;
        frameContext.contentContainer = builtFrame.frame.contentContainer;
        frameContext.panelBounds = builtFrame.frame.panelBounds;
        frameContext.contentBounds = builtFrame.frame.contentBounds;
        frameContext.panelId = entry->panel->GetPanelDesc().id;
        frameContext.isHovered = builtFrame.hovered;
        frameContext.isActive = frameContext.panelId == m_activePanelId;
        frameContext.hasKeyboardFocus =
            frameContext.panelId == preBuildKeyboardFocusPanelId;
        frameContext.hasMouseCapture =
            frameContext.panelId == mouseCapturePanelId;

        uint32 rebuildReasonMask =
            SanitizePanelRebuildReasonMask(entry->pendingRebuildReasonMask);
        if (!entry->builtLastFrame)
        {
            rebuildReasonMask |=
                ToEditorUIPanelRebuildReasonMask(EditorUIPanelRebuildReason::Visibility);
        }
        if (!entry->hasLastBounds)
        {
            rebuildReasonMask |=
                ToEditorUIPanelRebuildReasonMask(EditorUIPanelRebuildReason::Initial);
        }
        else if (!RectNearlyEqual(entry->lastPanelBounds,
                                  builtFrame.frame.panelBounds) ||
                 !RectNearlyEqual(entry->lastContentBounds,
                                  builtFrame.frame.contentBounds))
        {
            rebuildReasonMask |=
                ToEditorUIPanelRebuildReasonMask(EditorUIPanelRebuildReason::Layout);
        }

        frameContext.shouldRebuildContent = rebuildReasonMask != 0u;
        if (frameContext.shouldRebuildContent)
        {
            ++entry->rebuildRevision;
        }
        frameContext.rebuildRevision = entry->rebuildRevision;
        frameContext.rebuildReasonMask = rebuildReasonMask;

        entry->panel->BuildUI(frameContext);
        AccumulateCursorRequest(cursorRequest,
                                entry->panel->GetCursorRequest(),
                                frameContext.panelId);
        entry->pendingRebuildReasonMask = 0;
        entry->lastRebuildReasonMask = rebuildReasonMask;
        entry->lastPanelBounds = builtFrame.frame.panelBounds;
        entry->lastContentBounds = builtFrame.frame.contentBounds;
        entry->hasLastBounds = true;
        entry->builtThisFrame = true;
        frameContext.panelContainer = nullptr;
        frameContext.contentContainer = nullptr;
        frameContext.panelBounds = {};
        frameContext.contentBounds = {};
        frameContext.panelId.clear();
        frameContext.isHovered = false;
        frameContext.isActive = false;
        frameContext.hasKeyboardFocus = false;
        frameContext.hasMouseCapture = false;
        frameContext.shouldRebuildContent = false;
        frameContext.rebuildRevision = 0;
        frameContext.rebuildReasonMask = 0;
    }

    for (PanelEntry& entry : m_panels)
    {
        entry.builtLastFrame = entry.builtThisFrame;
    }

    m_panelSurfaceRenderer.End();
    m_contextMenu.Build(m_uiContext, m_popupLayer, *this);
    m_commandPalette.Build(m_uiContext, m_popupLayer, *this);
    m_filePickerDialog.Build(m_uiContext, m_popupLayer, *this);
    m_modalDialog.Build(m_uiContext, m_popupLayer, *this);
    m_popupLayer.End();
    m_tooltipLayer.Build(m_uiContext);

    canvas.ClearFocusScopeRoot();
    auto setFocusScopeRoot =
        [&canvas](const std::string& rootWidgetName) -> UI::Widget::Ptr {
        if (rootWidgetName.empty())
        {
            return nullptr;
        }

        UI::Widget::Ptr root = canvas.FindWidget(rootWidgetName);
        if (root)
        {
            canvas.SetFocusScopeRoot(root.get());
        }
        return root;
    };

    bool focusedPopup = false;
    if (m_modalDialog.IsOpen())
    {
        UI::Widget::Ptr modalRoot =
            setFocusScopeRoot(m_modalDialog.GetRootWidgetName());
        if (modalRoot && m_modalDialog.WantsKeyboardFocus())
        {
            canvas.SetFocusedWidget(modalRoot.get());
            focusedPopup = true;
        }
    }

    if (!canvas.GetFocusScopeRoot() && m_commandPalette.IsOpen())
    {
        setFocusScopeRoot(m_commandPalette.GetRootWidgetName());
    }
    if (!focusedPopup && m_commandPalette.WantsKeyboardFocus())
    {
        if (UI::Widget::Ptr paletteFocus =
                canvas.FindWidget(m_commandPalette.GetFocusWidgetName()))
        {
            canvas.SetFocusedWidget(paletteFocus.get());
            focusedPopup = true;
        }
    }

    if (!canvas.GetFocusScopeRoot() && m_filePickerDialog.IsOpen())
    {
        setFocusScopeRoot(m_filePickerDialog.GetRootWidgetName());
    }
    if (!focusedPopup && m_filePickerDialog.WantsKeyboardFocus())
    {
        if (UI::Widget::Ptr filePickerFocus =
                canvas.FindWidget(m_filePickerDialog.GetFocusWidgetName()))
        {
            canvas.SetFocusedWidget(filePickerFocus.get());
            focusedPopup = true;
        }
    }

    if (!canvas.GetFocusScopeRoot() && m_contextMenu.IsOpen())
    {
        setFocusScopeRoot(m_contextMenu.GetRootWidgetName());
    }
    if (!focusedPopup && m_contextMenu.WantsKeyboardFocus())
    {
        if (UI::Widget::Ptr contextMenuRoot =
                canvas.FindWidget(m_contextMenu.GetRootWidgetName()))
        {
            canvas.SetFocusedWidget(contextMenuRoot.get());
            focusedPopup = true;
        }
    }

    if (!canvas.GetFocusScopeRoot())
    {
        if (UI::Widget::Ptr commandMenuRoot =
                setFocusScopeRoot(m_commandSurfaceRenderer.GetOpenMenuRootWidgetName()))
        {
            canvas.SetFocusedWidget(commandMenuRoot.get());
            focusedPopup = true;
        }
    }
    if (!canvas.GetFocusScopeRoot())
    {
        if (UI::Widget::Ptr toolbarOverflowRoot = setFocusScopeRoot(
                m_commandSurfaceRenderer.GetToolbarOverflowRootWidgetName()))
        {
            canvas.SetFocusedWidget(toolbarOverflowRoot.get());
            focusedPopup = true;
        }
    }

    if (!focusedPopup && !m_postBuildFocusWidgetName.empty())
    {
        if (UI::Widget::Ptr focusWidget =
                canvas.FindWidget(m_postBuildFocusWidgetName))
        {
            canvas.SetFocusedWidget(focusWidget.get());
        }
        m_postBuildFocusWidgetName.clear();
    }
    else if (!focusedPopup && !m_modalDialogRestoreFocusWidgetName.empty())
    {
        if (UI::Widget::Ptr restoreWidget =
                canvas.FindWidget(m_modalDialogRestoreFocusWidgetName))
        {
            canvas.SetFocusedWidget(restoreWidget.get());
        }
        m_modalDialogRestoreFocusWidgetName.clear();
    }
    else if (!focusedPopup && !m_commandPaletteRestoreFocusWidgetName.empty())
    {
        if (UI::Widget::Ptr restoreWidget =
                canvas.FindWidget(m_commandPaletteRestoreFocusWidgetName))
        {
            canvas.SetFocusedWidget(restoreWidget.get());
        }
        m_commandPaletteRestoreFocusWidgetName.clear();
    }
    else if (!focusedPopup && !m_filePickerRestoreFocusWidgetName.empty())
    {
        if (UI::Widget::Ptr restoreWidget =
                canvas.FindWidget(m_filePickerRestoreFocusWidgetName))
        {
            canvas.SetFocusedWidget(restoreWidget.get());
        }
        m_filePickerRestoreFocusWidgetName.clear();
    }
    else if (!focusedPopup && !m_contextMenuRestoreFocusWidgetName.empty())
    {
        if (UI::Widget::Ptr restoreWidget =
                canvas.FindWidget(m_contextMenuRestoreFocusWidgetName))
        {
            canvas.SetFocusedWidget(restoreWidget.get());
        }
        m_contextMenuRestoreFocusWidgetName.clear();
    }
    else if (!focusedPopup && !focusedWidgetName.empty())
    {
        if (UI::Widget::Ptr focusedWidget = canvas.FindWidget(focusedWidgetName))
        {
            canvas.SetFocusedWidget(focusedWidget.get());
        }
    }

    UpdateInputRoutingState(ResolvePanelIdForWidget(canvas.GetFocusedWidget()),
                            mouseCapturePanelId,
                            canvas.GetFocusedWidget() != nullptr,
                            cursorRequest);
}

bool EditorUIHost::RenderNativeUI(UI::UIRenderer& renderer)
{
    m_nativeRenderStats = {};
    m_nativeRenderStats.rendererInitialized = renderer.IsInitialized();
    if (!m_initialized || !m_uiContext.IsFrameActive())
    {
        return false;
    }

    renderer.BeginRecordOnlyFrame(m_uiContext.GetWidth(), m_uiContext.GetHeight());
    m_uiContext.GetCanvas().Render(renderer);
    renderer.EndFrame();

    const UI::UIRenderStats& renderStats = renderer.GetStats();
    m_nativeRenderStats.frameRecorded = renderStats.commandCount > 0;
    m_nativeRenderStats.commandCount = renderStats.commandCount;
    m_nativeRenderStats.rectCount = renderStats.rectCount;
    m_nativeRenderStats.textCount = renderStats.textCount;
    m_nativeRenderStats.imageCount = renderStats.imageCount;
    m_nativeRenderStats.vertexCount = renderStats.vertexCount;
    m_nativeRenderStats.indexCount = renderStats.indexCount;

    const EditorCommandSurfaceRenderStats& surfaceStats =
        m_commandSurfaceRenderer.GetLastBuildStats();
    m_nativeRenderStats.menuCount = surfaceStats.menuCount;
    m_nativeRenderStats.toolbarCount = surfaceStats.toolbarCount;
    m_nativeRenderStats.statusItemCount = surfaceStats.statusItemCount;
    m_nativeRenderStats.executableItemCount = surfaceStats.executableItemCount;
    m_nativeRenderStats.menuBarHeight = surfaceStats.menuBarHeight;
    m_nativeRenderStats.toolbarHeight = surfaceStats.toolbarHeight;
    m_nativeRenderStats.statusBarHeight = surfaceStats.statusBarHeight;
    m_nativeRenderStats.topReservedHeight = surfaceStats.topReservedHeight;
    m_nativeRenderStats.bottomReservedHeight = surfaceStats.bottomReservedHeight;
    m_nativeRenderStats.panelFrameCount =
        m_panelSurfaceRenderer.GetLastBuildStats().panelFrameCount;
    m_nativeRenderStats.popupCount =
        m_popupLayer.GetLastBuildStats().popupCount;
    m_nativeRenderStats.tooltipCount =
        m_tooltipLayer.GetLastBuildStats().tooltipCount;
    m_nativeRenderStats.tooltipVisible =
        m_tooltipLayer.GetLastBuildStats().visible;
    m_nativeRenderStats.contextMenuOpen =
        m_contextMenu.GetLastBuildStats().open;
    m_nativeRenderStats.contextMenuItemCount =
        m_contextMenu.GetLastBuildStats().itemCount;
    m_nativeRenderStats.commandPaletteOpen =
        m_commandPalette.GetLastBuildStats().open;
    m_nativeRenderStats.commandPaletteItemCount =
        m_commandPalette.GetLastBuildStats().resultCount;
    m_nativeRenderStats.commandPaletteExecutableItemCount =
        m_commandPalette.GetLastBuildStats().executableItemCount;
    m_nativeRenderStats.filePickerOpen =
        m_filePickerDialog.GetLastBuildStats().open;
    m_nativeRenderStats.filePickerEntryCount =
        m_filePickerDialog.GetLastBuildStats().visibleEntryCount;
    m_nativeRenderStats.filePickerFilteredEntryCount =
        m_filePickerDialog.GetLastBuildStats().filteredEntryCount;
    m_nativeRenderStats.modalDialogOpen =
        m_modalDialog.GetLastBuildStats().open;
    m_nativeRenderStats.modalDialogButtonCount =
        m_modalDialog.GetLastBuildStats().buttonCount;
    m_nativeRenderStats.modalDialogEnabledButtonCount =
        m_modalDialog.GetLastBuildStats().enabledButtonCount;
    return m_nativeRenderStats.frameRecorded;
}

void EditorUIHost::EndFrame()
{
    if (m_initialized)
    {
        m_uiContext.EndFrame();
    }
}

void EditorUIHost::RegisterPanel(std::shared_ptr<IEditorUIPanel> panel)
{
    if (!panel)
    {
        return;
    }

    const std::string& id = panel->GetPanelDesc().id;
    if (id.empty() || FindPanelEntry(id))
    {
        return;
    }

    PanelEntry entry;
    entry.visible = panel->GetPanelDesc().visibleByDefault;
    entry.panel = std::move(panel);
    RequestPanelRebuild(entry,
                        ToEditorUIPanelRebuildReasonMask(
                            EditorUIPanelRebuildReason::Initial));
    entry.panel->OnAttach(*this);

    m_panels.push_back(std::move(entry));
    EnsureDockPlacement(m_panels.back());
}

bool EditorUIHost::UnregisterPanel(const std::string& id)
{
    const auto it = std::find_if(m_panels.begin(),
                                 m_panels.end(),
                                 [&id](const PanelEntry& entry) {
                                     return entry.panel && entry.panel->GetPanelDesc().id == id;
                                 });
    if (it == m_panels.end())
    {
        return false;
    }

    if (it->panel)
    {
        it->panel->OnDetach();
    }
    m_dockingModel.UndockPanel(id);
    m_panels.erase(it);
    if (m_hoveredPanelId == id)
    {
        m_hoveredPanelId.clear();
    }
    if (m_activePanelId == id)
    {
        m_activePanelId.clear();
    }
    ClearInputRoutingForPanel(id);
    return true;
}

IEditorUIPanel* EditorUIHost::GetPanel(const std::string& id) const
{
    const PanelEntry* entry = FindPanelEntry(id);
    return entry ? entry->panel.get() : nullptr;
}

uint32 EditorUIHost::GetVisiblePanelCount() const
{
    return static_cast<uint32>(std::count_if(m_panels.begin(),
                                             m_panels.end(),
                                             [](const PanelEntry& entry) {
                                                 return entry.visible && entry.panel;
                                             }));
}

std::vector<EditorUIPanelSnapshot> EditorUIHost::GetPanelSnapshots() const
{
    std::vector<EditorUIPanelSnapshot> snapshots;
    snapshots.reserve(m_panels.size());
    for (const PanelEntry& entry : m_panels)
    {
        if (entry.panel)
        {
            snapshots.push_back(BuildPanelSnapshot(entry));
        }
    }
    return snapshots;
}

bool EditorUIHost::TryGetPanelSnapshot(const std::string& id,
                                       EditorUIPanelSnapshot& outSnapshot) const
{
    const PanelEntry* entry = FindPanelEntry(id);
    if (!entry || !entry->panel)
    {
        outSnapshot = {};
        return false;
    }

    outSnapshot = BuildPanelSnapshot(*entry);
    return true;
}

void EditorUIHost::SetPanelVisible(const std::string& id, bool visible)
{
    if (PanelEntry* entry = FindPanelEntry(id))
    {
        const bool changed = entry->visible != visible;
        entry->visible = visible;
        m_dockingModel.SetPanelVisible(id, visible);
        if (changed)
        {
            RequestPanelRebuild(
                *entry,
                ToEditorUIPanelRebuildReasonMask(
                    EditorUIPanelRebuildReason::Visibility));
        }
        if (!visible)
        {
            if (m_hoveredPanelId == id)
            {
                m_hoveredPanelId.clear();
            }
            if (m_activePanelId == id)
            {
                m_activePanelId.clear();
            }
            ClearInputRoutingForPanel(id);
        }
    }
}

bool EditorUIHost::IsPanelVisible(const std::string& id) const
{
    const PanelEntry* entry = FindPanelEntry(id);
    return entry && entry->visible;
}

bool EditorUIHost::IsPanelHovered(const std::string& id) const
{
    return !id.empty() && m_hoveredPanelId == id;
}

bool EditorUIHost::IsPanelActive(const std::string& id) const
{
    return !id.empty() && m_activePanelId == id;
}

bool EditorUIHost::IsPanelKeyboardFocused(const std::string& id) const
{
    return !id.empty() && m_inputRoutingState.keyboardFocusPanelId == id;
}

bool EditorUIHost::IsPanelMouseCaptured(const std::string& id) const
{
    return !id.empty() && m_inputRoutingState.mouseCapturePanelId == id;
}

bool EditorUIHost::RequestPanelRebuild(const std::string& id,
                                       EditorUIPanelRebuildReason reason)
{
    PanelEntry* entry = FindPanelEntry(id);
    if (!entry)
    {
        return false;
    }

    const uint32 reasonMask =
        SanitizePanelRebuildReasonMask(ToEditorUIPanelRebuildReasonMask(reason));
    if (reasonMask == 0u)
    {
        return false;
    }

    RequestPanelRebuild(*entry, reasonMask);
    return true;
}

EditorUIPanelRebuildState EditorUIHost::GetPanelRebuildState(
    const std::string& id) const
{
    EditorUIPanelRebuildState state;
    const PanelEntry* entry = FindPanelEntry(id);
    if (!entry)
    {
        return state;
    }

    state.revision = entry->rebuildRevision;
    state.pendingReasonMask =
        SanitizePanelRebuildReasonMask(entry->pendingRebuildReasonMask);
    state.lastReasonMask =
        SanitizePanelRebuildReasonMask(entry->lastRebuildReasonMask);
    state.pending = state.pendingReasonMask != 0u;
    state.builtThisFrame = entry->builtThisFrame;
    return state;
}

bool EditorUIHost::ExecuteCommand(const std::string& id)
{
    EditorCommandContext context;
    context.host = this;
    context.input = &m_uiContext.GetInput();
    context.deltaTime = m_currentDeltaTime;
    const EditorCommandExecutionResult result =
        m_commandRegistry.ExecuteCommandDetailed(id, context);
    RecordCommandExecutionResult(result, EditorUICommandExecutionSource::Direct);
    return result.Succeeded();
}

void EditorUIHost::ClearCommandHistory()
{
    m_commandDiagnostics.history.clear();
    m_commandDiagnostics.historyDroppedCount = 0;
    m_nextCommandHistorySequence = 1;
}

void EditorUIHost::RecordCommandExecutionResult(
    const EditorCommandExecutionResult& result,
    EditorUICommandExecutionSource source)
{
    if (result.status == EditorCommandExecutionStatus::ShortcutNotMatched &&
        result.commandId.empty())
    {
        return;
    }

    m_commandDiagnostics.lastResult = result;
    m_commandDiagnostics.hasResult = true;
    m_commandDiagnostics.hasExternalDiagnostic = false;
    ++m_commandDiagnostics.attemptedCount;
    if (result.Succeeded())
    {
        ++m_commandDiagnostics.succeededCount;
    }
    else
    {
        ++m_commandDiagnostics.failedCount;
    }

    m_commandDiagnostics.shortcutConflictCount =
        static_cast<uint32>(m_commandRegistry.GetShortcutConflicts().size());
    m_commandDiagnostics.lastStatusText =
        BuildCommandDiagnosticsText(m_commandRegistry, m_commandDiagnostics);

    EditorUICommandHistoryEntry entry;
    entry.sequence = m_nextCommandHistorySequence++;
    entry.frameIndex = m_uiContext.GetFrameIndex();
    entry.deltaTime = m_currentDeltaTime;
    entry.source = source;
    entry.result = result;
    entry.displayName = ResolveCommandDiagnosticName(
        m_commandRegistry,
        result.commandId);
    entry.statusText = CommandHistoryStatusText(m_commandRegistry, result);
    entry.shortcutConflictCount =
        m_commandDiagnostics.shortcutConflictCount;

    if (m_commandDiagnostics.history.size() >=
        RVX_EDITOR_COMMAND_HISTORY_CAPACITY)
    {
        m_commandDiagnostics.history.erase(
            m_commandDiagnostics.history.begin());
        ++m_commandDiagnostics.historyDroppedCount;
    }
    m_commandDiagnostics.history.push_back(std::move(entry));
}

void EditorUIHost::RecordAutomationDiagnostic(
    const std::string& scenarioName,
    const std::string& target,
    bool succeeded,
    const std::string& message)
{
    m_commandDiagnostics.hasExternalDiagnostic = true;
    ++m_commandDiagnostics.attemptedCount;
    if (succeeded)
    {
        ++m_commandDiagnostics.succeededCount;
    }
    else
    {
        ++m_commandDiagnostics.failedCount;
    }

    m_commandDiagnostics.shortcutConflictCount =
        static_cast<uint32>(m_commandRegistry.GetShortcutConflicts().size());
    m_commandDiagnostics.lastStatusText =
        BuildAutomationDiagnosticText(scenarioName,
                                      target,
                                      succeeded,
                                      message);

    EditorUICommandHistoryEntry entry;
    entry.sequence = m_nextCommandHistorySequence++;
    entry.frameIndex = m_uiContext.GetFrameIndex();
    entry.deltaTime = m_currentDeltaTime;
    entry.source = EditorUICommandExecutionSource::Automation;
    entry.diagnostic = true;
    entry.diagnosticSucceeded = succeeded;
    entry.diagnosticSeverity =
        succeeded ? EditorUIDiagnosticSeverity::Info
                  : EditorUIDiagnosticSeverity::Error;
    entry.diagnosticMessage = message;
    entry.displayName = scenarioName.empty() ? "Editor automation"
                                             : scenarioName;
    entry.statusText = m_commandDiagnostics.lastStatusText;
    entry.shortcutConflictCount =
        m_commandDiagnostics.shortcutConflictCount;

    if (m_commandDiagnostics.history.size() >=
        RVX_EDITOR_COMMAND_HISTORY_CAPACITY)
    {
        m_commandDiagnostics.history.erase(
            m_commandDiagnostics.history.begin());
        ++m_commandDiagnostics.historyDroppedCount;
    }
    m_commandDiagnostics.history.push_back(std::move(entry));
}

void EditorUIHost::RefreshCommandDiagnosticsStatus()
{
    m_commandDiagnostics.shortcutConflictCount =
        static_cast<uint32>(m_commandRegistry.GetShortcutConflicts().size());
    if (!m_commandDiagnostics.hasExternalDiagnostic)
    {
        m_commandDiagnostics.lastStatusText =
            BuildCommandDiagnosticsText(m_commandRegistry, m_commandDiagnostics);
    }

    if (!m_commandDiagnostics.hasResult &&
        !m_commandDiagnostics.hasExternalDiagnostic &&
        m_commandSurfaceModel.GetStatusItems().empty())
    {
        return;
    }

    EditorStatusSurfaceItem item;
    item.id = RVX_EDITOR_COMMAND_STATUS_ITEM_ID;
    item.label = "Command";
    item.value = m_commandDiagnostics.lastStatusText;
    item.priority = 5;
    item.visible = true;
    m_commandSurfaceModel.SetStatusItem(std::move(item));
}

void EditorUIHost::OpenCommandPalette(std::string initialFilter)
{
    m_commandSurfaceRenderer.CloseOpenMenu();
    m_contextMenu.Close();
    m_filePickerDialog.Close();
    m_contextMenuRestoreFocusWidgetName.clear();
    m_filePickerRestoreFocusWidgetName.clear();
    m_commandPaletteRestoreFocusWidgetName.clear();
    if (UI::Widget* focusedWidget = m_uiContext.GetCanvas().GetFocusedWidget())
    {
        if (!focusedWidget->GetName().empty())
        {
            m_commandPaletteRestoreFocusWidgetName = focusedWidget->GetName();
        }
    }
    m_commandPalette.Open(std::move(initialFilter));
}

void EditorUIHost::CloseCommandPalette()
{
    m_commandPalette.Close();
}

void EditorUIHost::OpenFilePickerDialog(EditorFilePickerDialogDesc desc)
{
    m_commandSurfaceRenderer.CloseOpenMenu();
    m_commandPalette.Close();
    m_contextMenu.Close();
    m_commandPaletteRestoreFocusWidgetName.clear();
    m_contextMenuRestoreFocusWidgetName.clear();
    m_filePickerRestoreFocusWidgetName.clear();
    if (UI::Widget* focusedWidget = m_uiContext.GetCanvas().GetFocusedWidget())
    {
        if (!focusedWidget->GetName().empty())
        {
            m_filePickerRestoreFocusWidgetName = focusedWidget->GetName();
        }
    }
    m_filePickerDialog.Open(std::move(desc));
}

void EditorUIHost::CloseFilePickerDialog()
{
    m_filePickerDialog.Close();
}

void EditorUIHost::OpenContextMenu(EditorContextMenuDesc desc)
{
    m_commandSurfaceRenderer.CloseOpenMenu();
    m_commandPalette.Close();
    m_filePickerDialog.Close();
    m_commandPaletteRestoreFocusWidgetName.clear();
    m_filePickerRestoreFocusWidgetName.clear();
    m_contextMenuRestoreFocusWidgetName.clear();
    if (UI::Widget* focusedWidget = m_uiContext.GetCanvas().GetFocusedWidget())
    {
        if (!focusedWidget->GetName().empty())
        {
            m_contextMenuRestoreFocusWidgetName = focusedWidget->GetName();
        }
    }
    m_contextMenu.Open(std::move(desc));
}

void EditorUIHost::CloseContextMenu()
{
    m_contextMenu.Close();
}

void EditorUIHost::OpenModalDialog(EditorModalDialogDesc desc)
{
    m_commandSurfaceRenderer.CloseOpenMenu();
    m_commandPalette.Close();
    m_filePickerDialog.Close();
    m_contextMenu.Close();
    m_commandPaletteRestoreFocusWidgetName.clear();
    m_filePickerRestoreFocusWidgetName.clear();
    m_contextMenuRestoreFocusWidgetName.clear();
    m_modalDialogRestoreFocusWidgetName.clear();
    if (UI::Widget* focusedWidget = m_uiContext.GetCanvas().GetFocusedWidget())
    {
        if (!focusedWidget->GetName().empty())
        {
            m_modalDialogRestoreFocusWidgetName = focusedWidget->GetName();
        }
    }
    m_modalDialog.Open(std::move(desc));
}

void EditorUIHost::CloseModalDialog()
{
    m_modalDialog.Close();
}

void EditorUIHost::RequestFocusAfterBuild(std::string widgetName)
{
    m_postBuildFocusWidgetName = std::move(widgetName);
}

void EditorUIHost::ResetLayout()
{
    m_dockingModel.Clear();
    const EditorWorkspaceLayoutPolicy workspacePolicy;
    m_dockingModel.SetAreaSizing(workspacePolicy.GetDefaultPreset().areaSizing);
    RefreshLayoutCommandStates();
    for (PanelEntry& entry : m_panels)
    {
        if (!entry.panel)
        {
            continue;
        }

        entry.visible = entry.panel->GetPanelDesc().visibleByDefault;
        RequestPanelRebuild(
            entry,
            ToEditorUIPanelRebuildReasonMask(EditorUIPanelRebuildReason::Docking) |
                ToEditorUIPanelRebuildReasonMask(
                    EditorUIPanelRebuildReason::Visibility));
        m_dockingModel.DockPanel(workspacePolicy.BuildDefaultPlacement(
            entry.panel->GetPanelDesc(),
            entry.visible,
            static_cast<uint32>(m_dockingModel.GetPlacementCount())));
    }
    if (!m_hoveredPanelId.empty() && !IsPanelVisible(m_hoveredPanelId))
    {
        m_hoveredPanelId.clear();
    }
    if (!m_activePanelId.empty() && !IsPanelVisible(m_activePanelId))
    {
        m_activePanelId.clear();
    }
    m_inputRoutingState = {};
}

void EditorUIHost::RefreshLayoutCommandStates()
{
    m_commandRegistry.SetCommandChecked(EditorCommandIds::ViewToggleBottomDrawer,
                                        IsBottomDrawerCollapsed());
}

bool EditorUIHost::IsBottomDrawerCollapsed() const
{
    return m_dockingModel.IsDockAreaCollapsed(EditorUIPanelDockArea::Bottom);
}

void EditorUIHost::SetBottomDrawerCollapsed(bool collapsed)
{
    const bool wasCollapsed = IsBottomDrawerCollapsed();
    if (!m_dockingModel.SetDockAreaCollapsed(EditorUIPanelDockArea::Bottom,
                                             collapsed))
    {
        return;
    }

    RefreshLayoutCommandStates();
    if (wasCollapsed == collapsed)
    {
        return;
    }

    for (PanelEntry& entry : m_panels)
    {
        if (!entry.panel)
        {
            continue;
        }

        const EditorDockPanelPlacement* placement =
            m_dockingModel.FindPlacement(entry.panel->GetPanelDesc().id);
        if (!placement || placement->area != EditorUIPanelDockArea::Bottom)
        {
            continue;
        }

        RequestPanelRebuild(
            entry,
            ToEditorUIPanelRebuildReasonMask(EditorUIPanelRebuildReason::Layout) |
                ToEditorUIPanelRebuildReasonMask(
                    EditorUIPanelRebuildReason::Docking));
    }
}

void EditorUIHost::ToggleBottomDrawerCollapsed()
{
    SetBottomDrawerCollapsed(!IsBottomDrawerCollapsed());
}

bool EditorUIHost::SaveLayout(const std::filesystem::path& path, std::string* error) const
{
    return EditorLayoutSerializer::SaveToFile(m_dockingModel, path, error);
}

bool EditorUIHost::LoadLayout(const std::filesystem::path& path, std::string* error)
{
    EditorDockingModel loadedModel;
    if (!EditorLayoutSerializer::LoadFromFile(path, loadedModel, error))
    {
        return false;
    }

    m_dockingModel = std::move(loadedModel);
    RefreshLayoutCommandStates();
    SyncPanelVisibilityFromDocking();
    for (PanelEntry& entry : m_panels)
    {
        EnsureDockPlacement(entry);
        uint32 rebuildReasonMask =
            ToEditorUIPanelRebuildReasonMask(EditorUIPanelRebuildReason::Docking);
        if (entry.panel &&
            ShouldHideTransientToolPanelOnStartup(entry.panel->GetPanelDesc().id))
        {
            entry.visible = false;
            m_dockingModel.SetPanelVisible(entry.panel->GetPanelDesc().id, false);
            rebuildReasonMask |=
                ToEditorUIPanelRebuildReasonMask(
                    EditorUIPanelRebuildReason::Visibility);
        }
        RequestPanelRebuild(entry, rebuildReasonMask);
    }

    return true;
}

EditorUIHost::PanelEntry* EditorUIHost::FindPanelEntry(const std::string& id)
{
    const auto it = std::find_if(m_panels.begin(),
                                 m_panels.end(),
                                 [&id](const PanelEntry& entry) {
                                     return entry.panel && entry.panel->GetPanelDesc().id == id;
                                 });
    return it == m_panels.end() ? nullptr : &(*it);
}

const EditorUIHost::PanelEntry* EditorUIHost::FindPanelEntry(const std::string& id) const
{
    const auto it = std::find_if(m_panels.begin(),
                                 m_panels.end(),
                                 [&id](const PanelEntry& entry) {
                                     return entry.panel && entry.panel->GetPanelDesc().id == id;
                                 });
    return it == m_panels.end() ? nullptr : &(*it);
}

EditorUIPanelSnapshot EditorUIHost::BuildPanelSnapshot(
    const PanelEntry& entry) const
{
    EditorUIPanelSnapshot snapshot;
    if (!entry.panel)
    {
        return snapshot;
    }

    const EditorUIPanelDesc& desc = entry.panel->GetPanelDesc();
    snapshot.id = desc.id;
    snapshot.title = desc.title;
    snapshot.defaultDockArea = desc.defaultDockArea;
    snapshot.dockArea = desc.defaultDockArea;
    snapshot.registered = true;
    snapshot.visibleByDefault = desc.visibleByDefault;
    snapshot.visible = entry.visible;
    snapshot.closable = desc.closable;
    snapshot.hasLastBounds = entry.hasLastBounds;
    snapshot.lastPanelBounds = entry.lastPanelBounds;
    snapshot.lastContentBounds = entry.lastContentBounds;
    snapshot.rebuildState.revision = entry.rebuildRevision;
    snapshot.rebuildState.pendingReasonMask =
        SanitizePanelRebuildReasonMask(entry.pendingRebuildReasonMask);
    snapshot.rebuildState.lastReasonMask =
        SanitizePanelRebuildReasonMask(entry.lastRebuildReasonMask);
    snapshot.rebuildState.pending =
        snapshot.rebuildState.pendingReasonMask != 0u;
    snapshot.rebuildState.builtThisFrame = entry.builtThisFrame;

    if (const EditorDockPanelPlacement* placement =
            m_dockingModel.FindPlacement(desc.id))
    {
        snapshot.hasDockPlacement = true;
        snapshot.dockArea = placement->area;
        snapshot.dockTabStackId = placement->tabStackId;
        snapshot.dockNormalizedSize = placement->normalizedSize;
        snapshot.dockOrder = placement->order;
        snapshot.floatingZOrder = placement->floatingZOrder;
    }

    return snapshot;
}

void EditorUIHost::EnsureDockPlacement(PanelEntry& entry)
{
    if (!entry.panel)
    {
        return;
    }

    const std::string& id = entry.panel->GetPanelDesc().id;
    if (EditorDockPanelPlacement* existing = m_dockingModel.FindPlacement(id))
    {
        if (ShouldHideTransientToolPanelOnStartup(id))
        {
            const uint32 existingOrder = existing->order;
            m_dockingModel.DockPanel(
                EditorWorkspaceLayoutPolicy{}.BuildDefaultPlacement(
                    entry.panel->GetPanelDesc(),
                    false,
                    existingOrder));
            existing = m_dockingModel.FindPlacement(id);
        }
        if (!existing)
        {
            return;
        }
        if (entry.visible != existing->visible)
        {
            RequestPanelRebuild(
                entry,
                ToEditorUIPanelRebuildReasonMask(
                    EditorUIPanelRebuildReason::Visibility));
        }
        entry.visible = existing->visible;
        return;
    }

    m_dockingModel.DockPanel(EditorWorkspaceLayoutPolicy{}.BuildDefaultPlacement(
        entry.panel->GetPanelDesc(),
        entry.visible,
        static_cast<uint32>(m_dockingModel.GetPlacementCount())));
}

void EditorUIHost::SyncPanelVisibilityFromDocking()
{
    for (PanelEntry& entry : m_panels)
    {
        if (!entry.panel)
        {
            continue;
        }

        if (const EditorDockPanelPlacement* placement =
                m_dockingModel.FindPlacement(entry.panel->GetPanelDesc().id))
        {
            if (entry.visible != placement->visible)
            {
                RequestPanelRebuild(
                    entry,
                    ToEditorUIPanelRebuildReasonMask(
                        EditorUIPanelRebuildReason::Visibility));
            }
            entry.visible = placement->visible;
        }
    }

    if (!m_hoveredPanelId.empty() && !IsPanelVisible(m_hoveredPanelId))
    {
        m_hoveredPanelId.clear();
    }
    if (!m_activePanelId.empty() && !IsPanelVisible(m_activePanelId))
    {
        m_activePanelId.clear();
    }
    if (!m_inputRoutingState.hoveredPanelId.empty() &&
        !IsPanelVisible(m_inputRoutingState.hoveredPanelId))
    {
        m_inputRoutingState.hoveredPanelId.clear();
    }
    if (!m_inputRoutingState.activePanelId.empty() &&
        !IsPanelVisible(m_inputRoutingState.activePanelId))
    {
        m_inputRoutingState.activePanelId.clear();
    }
    if (!m_inputRoutingState.keyboardFocusPanelId.empty() &&
        !IsPanelVisible(m_inputRoutingState.keyboardFocusPanelId))
    {
        m_inputRoutingState.keyboardFocusPanelId.clear();
    }
    if (!m_inputRoutingState.mouseCapturePanelId.empty() &&
        !IsPanelVisible(m_inputRoutingState.mouseCapturePanelId))
    {
        m_inputRoutingState.mouseCapturePanelId.clear();
    }
    if (!m_inputRoutingState.cursorRequest.ownerPanelId.empty() &&
        !IsPanelVisible(m_inputRoutingState.cursorRequest.ownerPanelId))
    {
        m_inputRoutingState.cursorRequest = {};
    }
    const EditorCommandShortcutRoutingPolicy policy =
        BuildShortcutRoutingPolicy();
    m_inputRoutingState.commandSurfaceMenuOpen =
        policy.commandSurfaceMenuOpen;
    m_inputRoutingState.contextMenuOpen = policy.contextMenuOpen;
    m_inputRoutingState.commandPaletteOpen = policy.commandPaletteOpen;
    m_inputRoutingState.filePickerOpen = policy.filePickerOpen;
    m_inputRoutingState.modalDialogOpen = policy.modalDialogOpen;
    m_inputRoutingState.popupCapturesInput = policy.HasPopupCapture();
    m_inputRoutingState.wantsMouseCapture =
        m_inputRoutingState.popupCapturesInput ||
        m_inputRoutingState.cursorRequest.requestsMouseCapture ||
        !m_inputRoutingState.hoveredPanelId.empty() ||
        !m_inputRoutingState.mouseCapturePanelId.empty();
    m_inputRoutingState.wantsKeyboardCapture =
        m_inputRoutingState.popupCapturesInput ||
        !m_inputRoutingState.keyboardFocusPanelId.empty();
}

void EditorUIHost::ClearInputRoutingForPanel(const std::string& id)
{
    if (id.empty())
    {
        return;
    }

    if (m_inputRoutingState.hoveredPanelId == id)
    {
        m_inputRoutingState.hoveredPanelId.clear();
    }
    if (m_inputRoutingState.activePanelId == id)
    {
        m_inputRoutingState.activePanelId.clear();
    }
    if (m_inputRoutingState.keyboardFocusPanelId == id)
    {
        m_inputRoutingState.keyboardFocusPanelId.clear();
    }
    if (m_inputRoutingState.mouseCapturePanelId == id)
    {
        m_inputRoutingState.mouseCapturePanelId.clear();
    }
    if (m_inputRoutingState.cursorRequest.ownerPanelId == id)
    {
        m_inputRoutingState.cursorRequest = {};
    }

    const EditorCommandShortcutRoutingPolicy policy =
        BuildShortcutRoutingPolicy();
    m_inputRoutingState.commandSurfaceMenuOpen =
        policy.commandSurfaceMenuOpen;
    m_inputRoutingState.contextMenuOpen = policy.contextMenuOpen;
    m_inputRoutingState.commandPaletteOpen = policy.commandPaletteOpen;
    m_inputRoutingState.filePickerOpen = policy.filePickerOpen;
    m_inputRoutingState.modalDialogOpen = policy.modalDialogOpen;
    m_inputRoutingState.popupCapturesInput = policy.HasPopupCapture();
    m_inputRoutingState.wantsMouseCapture =
        m_inputRoutingState.popupCapturesInput ||
        m_inputRoutingState.cursorRequest.requestsMouseCapture ||
        !m_inputRoutingState.hoveredPanelId.empty() ||
        !m_inputRoutingState.mouseCapturePanelId.empty();
    m_inputRoutingState.wantsKeyboardCapture =
        m_inputRoutingState.popupCapturesInput ||
        !m_inputRoutingState.keyboardFocusPanelId.empty();
}

void EditorUIHost::RequestPanelRebuild(PanelEntry& entry, uint32 reasonMask)
{
    entry.pendingRebuildReasonMask |= SanitizePanelRebuildReasonMask(reasonMask);
}

std::string EditorUIHost::ResolvePanelIdForWidget(const UI::Widget* widget) const
{
    for (const UI::Widget* current = widget; current; current = current->GetParent())
    {
        const std::string& widgetName = current->GetName();
        if (widgetName.empty())
        {
            continue;
        }

        for (const PanelEntry& entry : m_panels)
        {
            if (!entry.panel)
            {
                continue;
            }

            const std::string& panelId = entry.panel->GetPanelDesc().id;
            if (panelId.empty())
            {
                continue;
            }

            const std::string expectedPrefix =
                std::string(RVX_EDITOR_PANEL_WIDGET_PREFIX) + panelId + ".";
            if (widgetName.rfind(expectedPrefix, 0) == 0)
            {
                return panelId;
            }
        }
    }

    return {};
}

EditorCommandShortcutRoutingPolicy
EditorUIHost::BuildShortcutRoutingPolicy() const
{
    EditorCommandShortcutRoutingPolicy policy;
    policy.commandSurfaceMenuOpen =
        !m_commandSurfaceRenderer.GetOpenMenuId().empty();
    policy.contextMenuOpen = m_contextMenu.IsOpen();
    policy.commandPaletteOpen = m_commandPalette.IsOpen();
    policy.filePickerOpen = m_filePickerDialog.IsOpen();
    policy.modalDialogOpen = m_modalDialog.IsOpen();
    policy.activeShortcutScopeMask = RVX_EDITOR_COMMAND_SCOPE_GLOBAL_MASK;
    if (!m_inputRoutingState.keyboardFocusPanelId.empty())
    {
        policy.activeShortcutScopeMask |=
            RVX_EDITOR_COMMAND_SCOPE_FOCUSED_PANEL_MASK;
    }
    if (IsViewportShortcutPanelId(m_inputRoutingState.keyboardFocusPanelId) ||
        IsViewportShortcutPanelId(m_inputRoutingState.mouseCapturePanelId))
    {
        policy.activeShortcutScopeMask |=
            RVX_EDITOR_COMMAND_SCOPE_VIEWPORT_MASK;
    }
    return policy;
}

void EditorUIHost::UpdateInputRoutingState(
    const std::string& keyboardFocusPanelId,
    const std::string& mouseCapturePanelId,
    bool hasFocusedWidget,
    const EditorUICursorRequest& cursorRequest)
{
    m_inputRoutingState = {};
    m_inputRoutingState.hoveredPanelId = m_hoveredPanelId;
    m_inputRoutingState.activePanelId = m_activePanelId;
    m_inputRoutingState.keyboardFocusPanelId = keyboardFocusPanelId;
    m_inputRoutingState.mouseCapturePanelId = mouseCapturePanelId;
    m_inputRoutingState.cursorRequest = cursorRequest;
    const EditorCommandShortcutRoutingPolicy policy =
        BuildShortcutRoutingPolicy();
    m_inputRoutingState.commandSurfaceMenuOpen =
        policy.commandSurfaceMenuOpen;
    m_inputRoutingState.contextMenuOpen = policy.contextMenuOpen;
    m_inputRoutingState.commandPaletteOpen = policy.commandPaletteOpen;
    m_inputRoutingState.filePickerOpen = policy.filePickerOpen;
    m_inputRoutingState.modalDialogOpen = policy.modalDialogOpen;
    m_inputRoutingState.popupCapturesInput = policy.HasPopupCapture();
    if (m_inputRoutingState.popupCapturesInput)
    {
        m_inputRoutingState.cursorRequest = {};
    }
    m_inputRoutingState.wantsMouseCapture =
        m_inputRoutingState.popupCapturesInput ||
        m_inputRoutingState.cursorRequest.requestsMouseCapture ||
        !m_inputRoutingState.hoveredPanelId.empty() ||
        !m_inputRoutingState.mouseCapturePanelId.empty() ||
        hasFocusedWidget;
    m_inputRoutingState.wantsKeyboardCapture =
        m_inputRoutingState.popupCapturesInput ||
        hasFocusedWidget;
}

} // namespace RVX::Editor
