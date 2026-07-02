/**
 * @file EditorNativePanelRegistrationService.cpp
 * @brief Editor native panel registration service implementation.
 */

#include "Editor/EditorNativePanelRegistrationService.h"

#include "Editor/EditorSettings.h"
#include "Editor/EditorSelectionService.h"
#include "Editor/EditorViewportToolService.h"
#include "Editor/Panels/IEditorPanel.h"
#include "Editor/Panels/NativeAnimationEditor.h"
#include "Editor/Panels/NativeAssetBrowser.h"
#include "Editor/Panels/NativeCommandHistory.h"
#include "Editor/Panels/NativeConsole.h"
#include "Editor/Panels/NativeEditorPreferences.h"
#include "Editor/Panels/NativeInspector.h"
#include "Editor/Panels/NativeMaterialEditor.h"
#include "Editor/Panels/NativePanelCatalog.h"
#include "Editor/Panels/NativeSceneHierarchy.h"
#include "Editor/Panels/NativeViewport.h"
#include "Editor/UI/EditorCommandCatalog.h"
#include "Editor/UI/EditorCommandRegistry.h"
#include "Editor/UI/EditorCommandSurfaceModel.h"
#include "Editor/UI/EditorNativePanelCommands.h"
#include "Editor/UI/EditorShortcutProfileService.h"
#include "Editor/UI/EditorUIPanel.h"
#include "Editor/UI/IEditorUIBackend.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace RVX::Editor
{
namespace
{
    constexpr const char* RVX_EDITOR_VIEW_MENU_ID = "view";
#if RVX_EDITOR_ENABLE_LEGACY_IMGUI
    constexpr const char* RVX_EDITOR_VIEW_IMGUI_DEMO_COMMAND_ID =
        "view.debug.imguiDemo";
    constexpr const char* RVX_EDITOR_VIEW_IMGUI_METRICS_COMMAND_ID =
        "view.debug.imguiMetrics";
#endif
}

bool EditorNativePanelRegistrationService::RegisterStandardNativePanels()
{
    if (!m_uiBackend)
    {
        return false;
    }

    auto nativeViewport = std::make_shared<NativeViewportPanel>();
    nativeViewport->SetSelectionService(m_selection);
    nativeViewport->SetToolService(m_viewportTools);
    RegisterNativePanel(std::move(nativeViewport));
    RegisterNativePanel(std::make_shared<NativeInspectorPanel>());
    RegisterNativePanel(std::make_shared<NativeAnimationEditorPanel>());
    RegisterNativePanel(std::make_shared<NativeMaterialEditorPanel>());
    RegisterNativePanel(std::make_shared<NativeSceneHierarchyPanel>());
    RegisterNativePanel(std::make_shared<NativeAssetBrowserPanel>());
    RegisterNativePanel(std::make_shared<NativeConsolePanel>());
    RegisterNativePanel(std::make_shared<NativeCommandHistoryPanel>());
    RegisterNativePanel(std::make_shared<NativePanelCatalogPanel>());
    RegisterNativePanel(std::make_shared<NativeEditorPreferencesPanel>(
        m_settings,
        m_shortcutProfiles));

    m_stats.standardPanelsRegistered = true;
    return true;
}

void EditorNativePanelRegistrationService::RegisterLegacyPanelViewCommand(
    IEditorPanel& panel)
{
    if (!m_uiBackend)
    {
        return;
    }

    const std::string panelName = panel.GetName();
    const std::string commandId = MakeLegacyPanelViewCommandId(panelName);
    IEditorPanel* panelPtr = &panel;
    if (commandId.empty())
    {
        return;
    }

    EditorCommandRegistry* registry = m_uiBackend->GetCommandRegistry();
    EditorCommandSurfaceModel* surfaceModel =
        m_uiBackend->GetCommandSurfaceModel();
    if (!registry || !surfaceModel)
    {
        return;
    }

    if (!registry->FindCommand(commandId))
    {
        EditorCommandDesc command;
        command.id = commandId;
        command.displayName = panelName;
        command.category = "View";
        command.tooltip = "Show or hide the " + panelName + " panel.";
        command.checkable = true;
        command.checked = panel.IsVisible();
        command.callback = [this, panelPtr, commandId](
                               EditorCommandContext&) {
            panelPtr->Toggle();
            if (EditorCommandRegistry* commandRegistry =
                    m_uiBackend ? m_uiBackend->GetCommandRegistry() : nullptr)
            {
                commandRegistry->SetCommandChecked(commandId,
                                                   panelPtr->IsVisible());
            }
        };
        registry->RegisterCommand(std::move(command));
        ++m_stats.legacyPanelViewCommandCount;
    }

    AddViewPanelGroupSeparator();
    surfaceModel->AddMenuItem(RVX_EDITOR_VIEW_MENU_ID,
                              EditorCommandSurfaceItem::Command(commandId));
    registry->SetCommandChecked(commandId, panel.IsVisible());
}

void EditorNativePanelRegistrationService::RegisterNativePanel(
    std::shared_ptr<IEditorUIPanel> panel)
{
    if (!m_uiBackend || !panel)
    {
        return;
    }

    const std::string panelId = panel->GetPanelDesc().id;
    const std::string title = panel->GetPanelDesc().title;
    if (panelId.empty())
    {
        return;
    }

    m_uiBackend->RegisterPanel(std::move(panel));
    if (!m_uiBackend->GetPanel(panelId))
    {
        return;
    }

    if (std::find(m_nativePanelIds.begin(), m_nativePanelIds.end(), panelId) ==
        m_nativePanelIds.end())
    {
        m_nativePanelIds.push_back(panelId);
        m_stats.nativePanelCount = static_cast<uint32>(m_nativePanelIds.size());
    }
    RegisterNativePanelViewCommand(panelId, title);
}

void EditorNativePanelRegistrationService::RegisterDebugViewCommands()
{
#if RVX_EDITOR_ENABLE_LEGACY_IMGUI
    if (!m_exposeLegacyImGuiDebugCommands || !m_uiBackend ||
        !m_showDemoWindow || !m_showMetricsWindow)
    {
        return;
    }

    EditorCommandRegistry* registry = m_uiBackend->GetCommandRegistry();
    if (!registry || !m_uiBackend->GetCommandSurfaceModel())
    {
        return;
    }

    auto registerDebugCommand = [this, registry](const char* id,
                                                 const char* displayName,
                                                 const char* tooltip,
                                                 bool* value) {
        if (!value)
        {
            return;
        }
        if (registry->FindCommand(id))
        {
            registry->SetCommandChecked(id, *value);
            return;
        }

        EditorCommandDesc command;
        command.id = id;
        command.displayName = displayName;
        command.category = "View";
        command.tooltip = tooltip;
        command.checkable = true;
        command.checked = *value;
        command.callback = [this, id, value](EditorCommandContext&) {
            *value = !(*value);
            if (EditorCommandRegistry* commandRegistry =
                    m_uiBackend ? m_uiBackend->GetCommandRegistry() : nullptr)
            {
                commandRegistry->SetCommandChecked(id, *value);
            }
        };
        registry->RegisterCommand(std::move(command));
        ++m_stats.debugViewCommandCount;
    };

    registerDebugCommand(RVX_EDITOR_VIEW_IMGUI_DEMO_COMMAND_ID,
                         "ImGui Demo",
                         "Show the ImGui demo diagnostics window.",
                         m_showDemoWindow);
    registerDebugCommand(RVX_EDITOR_VIEW_IMGUI_METRICS_COMMAND_ID,
                         "ImGui Metrics",
                         "Show the ImGui metrics diagnostics window.",
                         m_showMetricsWindow);

    AddViewDebugGroupSeparator();
    EditorCommandSurfaceModel* surfaceModel =
        m_uiBackend->GetCommandSurfaceModel();
    surfaceModel->AddMenuItem(
        RVX_EDITOR_VIEW_MENU_ID,
        EditorCommandSurfaceItem::Command(RVX_EDITOR_VIEW_IMGUI_DEMO_COMMAND_ID));
    surfaceModel->AddMenuItem(
        RVX_EDITOR_VIEW_MENU_ID,
        EditorCommandSurfaceItem::Command(
            RVX_EDITOR_VIEW_IMGUI_METRICS_COMMAND_ID));
#endif
}

void EditorNativePanelRegistrationService::RefreshViewCommandStates(
    const std::vector<std::shared_ptr<IEditorPanel>>& legacyPanels)
{
    if (!m_uiBackend)
    {
        return;
    }

    EditorCommandRegistry* registry = m_uiBackend->GetCommandRegistry();
    if (!registry)
    {
        return;
    }

    registry->SetCommandEnabled(
        EditorCommandIds::EditPreferences,
        m_uiBackend->GetPanel(NativeEditorPreferencesPanel::PanelId()) != nullptr);

    for (const std::shared_ptr<IEditorPanel>& panel : legacyPanels)
    {
        if (panel)
        {
            registry->SetCommandChecked(
                MakeLegacyPanelViewCommandId(panel->GetName()),
                panel->IsVisible());
        }
    }
    for (const std::string& panelId : m_nativePanelIds)
    {
        registry->SetCommandChecked(MakeNativeUIPanelViewCommandId(panelId),
                                    m_uiBackend->IsPanelVisible(panelId));
    }

#if RVX_EDITOR_ENABLE_LEGACY_IMGUI
    if (m_showDemoWindow)
    {
        registry->SetCommandChecked(RVX_EDITOR_VIEW_IMGUI_DEMO_COMMAND_ID,
                                    *m_showDemoWindow);
    }
    if (m_showMetricsWindow)
    {
        registry->SetCommandChecked(RVX_EDITOR_VIEW_IMGUI_METRICS_COMMAND_ID,
                                    *m_showMetricsWindow);
    }
#endif
}

void EditorNativePanelRegistrationService::RequestNativePanelRebuilds(
    EditorUIPanelRebuildReason reason)
{
    if (!m_uiBackend)
    {
        return;
    }

    for (const std::string& panelId : m_nativePanelIds)
    {
        m_uiBackend->RequestPanelRebuild(panelId, reason);
    }
}

bool EditorNativePanelRegistrationService::OpenPreferencesPanel()
{
    if (!m_uiBackend)
    {
        return false;
    }

    constexpr const char* panelId = NativeEditorPreferencesPanel::PanelId();
    if (!m_uiBackend->GetPanel(panelId))
    {
        return false;
    }

    m_uiBackend->SetPanelVisible(panelId, true);
    m_uiBackend->RequestPanelRebuild(panelId,
                                     EditorUIPanelRebuildReason::Data);
    if (EditorCommandRegistry* registry = m_uiBackend->GetCommandRegistry())
    {
        registry->SetCommandChecked(MakeNativeUIPanelViewCommandId(panelId),
                                    true);
    }
    return true;
}

std::string EditorNativePanelRegistrationService::MakeLegacyPanelViewCommandId(
    std::string_view panelName)
{
    if (panelName.empty())
    {
        return {};
    }

    std::string id = "view.panel.";
    bool lastWasSeparator = false;
    for (unsigned char character : panelName)
    {
        if (std::isalnum(character) != 0)
        {
            id.push_back(static_cast<char>(std::tolower(character)));
            lastWasSeparator = false;
            continue;
        }

        if (!lastWasSeparator)
        {
            id.push_back('.');
            lastWasSeparator = true;
        }
    }

    while (!id.empty() && id.back() == '.')
    {
        id.pop_back();
    }
    return id == "view.panel" ? std::string{} : id;
}

void EditorNativePanelRegistrationService::AddViewPanelGroupSeparator()
{
    if (m_viewPanelGroupAdded || !m_uiBackend)
    {
        return;
    }

    if (EditorCommandSurfaceModel* surfaceModel =
            m_uiBackend->GetCommandSurfaceModel())
    {
        surfaceModel->AddMenuItem(RVX_EDITOR_VIEW_MENU_ID,
                                  EditorCommandSurfaceItem::Separator());
        m_viewPanelGroupAdded = true;
    }
}

void EditorNativePanelRegistrationService::AddViewDebugGroupSeparator()
{
    if (m_debugGroupAdded || !m_uiBackend)
    {
        return;
    }

    if (EditorCommandSurfaceModel* surfaceModel =
            m_uiBackend->GetCommandSurfaceModel())
    {
        surfaceModel->AddMenuItem(RVX_EDITOR_VIEW_MENU_ID,
                                  EditorCommandSurfaceItem::Separator());
        m_debugGroupAdded = true;
    }
}

void EditorNativePanelRegistrationService::RegisterNativePanelViewCommand(
    const std::string& panelId,
    const std::string& title)
{
    if (!m_uiBackend)
    {
        return;
    }

    const std::string commandId = MakeNativeUIPanelViewCommandId(panelId);
    if (commandId.empty())
    {
        return;
    }

    EditorCommandRegistry* registry = m_uiBackend->GetCommandRegistry();
    EditorCommandSurfaceModel* surfaceModel =
        m_uiBackend->GetCommandSurfaceModel();
    if (!registry || !surfaceModel)
    {
        return;
    }

    if (!registry->FindCommand(commandId))
    {
        EditorCommandDesc command;
        command.id = commandId;
        command.displayName = title.empty() ? panelId : title;
        command.category = "View";
        command.tooltip = "Show or hide the " + command.displayName + " panel.";
        command.checkable = true;
        command.checked = m_uiBackend->IsPanelVisible(panelId);
        command.callback = [this, panelId, commandId](EditorCommandContext&) {
            if (!m_uiBackend)
            {
                return;
            }
            const bool nextVisible = !m_uiBackend->IsPanelVisible(panelId);
            m_uiBackend->SetPanelVisible(panelId, nextVisible);
            if (EditorCommandRegistry* commandRegistry =
                    m_uiBackend->GetCommandRegistry())
            {
                commandRegistry->SetCommandChecked(commandId, nextVisible);
            }
        };
        registry->RegisterCommand(std::move(command));
    }

    AddViewPanelGroupSeparator();
    surfaceModel->AddMenuItem(RVX_EDITOR_VIEW_MENU_ID,
                              EditorCommandSurfaceItem::Command(commandId));
    registry->SetCommandChecked(commandId, m_uiBackend->IsPanelVisible(panelId));
}

} // namespace RVX::Editor
