/**
 * @file EditorAutomation.cpp
 * @brief Reusable editor smoke automation scenario runner.
 */

#include "Editor/EditorAutomation.h"
#include "Editor/EditorDocument.h"
#include "Editor/EditorSettings.h"
#include "Editor/Panels/NativeEditorPreferences.h"
#include "Editor/UI/EditorCommandCatalog.h"
#include "Editor/UI/EditorCommandRegistry.h"
#include "Editor/UI/EditorCommandSurfaceModel.h"
#include "Editor/UI/EditorNativePanelCommands.h"
#include "Editor/UI/EditorShortcutProfileService.h"
#include "Editor/UI/EditorUIHost.h"
#include "Editor/UI/IEditorUIBackend.h"
#include "Core/Log.h"
#include "UI/UIContext.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <system_error>
#include <utility>

namespace RVX::Editor
{
namespace
{
    constexpr const char* RVX_EDITOR_AUTOMATION_OPEN_SCENE_PICKER_ID =
        "OpenScene";
    constexpr const char* RVX_EDITOR_AUTOMATION_SAVE_SCENE_PICKER_ID =
        "SaveScene";
    constexpr const char* RVX_EDITOR_AUTOMATION_OVERWRITE_SCENE_MODAL_ID =
        "OverwriteScene";
    constexpr const char* RVX_EDITOR_AUTOMATION_FILE_MENU_ID = "file";
    constexpr const char* RVX_EDITOR_AUTOMATION_VIEW_MENU_ID = "view";
    constexpr const char* RVX_EDITOR_AUTOMATION_MAIN_TOOLBAR_ID = "main";
    constexpr uint32 RVX_EDITOR_AUTOMATION_WAIT_FRAME_LIMIT = 12;
    constexpr char RVX_EDITOR_AUTOMATION_TARGET_SEPARATOR = ':';
    constexpr float RVX_EDITOR_AUTOMATION_UI_SCALE_DELTA = 0.25f;
    constexpr float RVX_EDITOR_AUTOMATION_UI_SCALE_EPSILON = 0.001f;

    bool NearlyEqual(float lhs, float rhs)
    {
        return std::fabs(lhs - rhs) <= RVX_EDITOR_AUTOMATION_UI_SCALE_EPSILON;
    }

    std::string ToString(std::string_view value)
    {
        return std::string(value.data(), value.size());
    }

    std::filesystem::path ResolveAutomationPath(std::string_view path)
    {
        if (path.empty())
        {
            return {};
        }

        std::error_code ec;
        std::filesystem::path resolved =
            std::filesystem::absolute(
                std::filesystem::path(std::string(path)),
                ec);
        if (ec)
        {
            resolved = std::filesystem::path(std::string(path));
        }
        return resolved.lexically_normal();
    }

    bool IsFilePickerScenario(EditorAutomationScenarioType scenario)
    {
        return scenario == EditorAutomationScenarioType::OpenSceneFilePicker ||
               scenario == EditorAutomationScenarioType::SaveSceneAsFilePicker;
    }

    bool ContainsVisibleSurfaceCommand(
        const std::vector<EditorCommandSurfaceItem>& items,
        std::string_view commandId)
    {
        return std::find_if(
                   items.begin(),
                   items.end(),
                   [commandId](const EditorCommandSurfaceItem& item) {
                       return item.type ==
                                  EditorCommandSurfaceItemType::Command &&
                              item.visible &&
                              std::string_view(item.commandId) == commandId;
                   }) != items.end();
    }

    std::string MakeNativePanelFrameWidgetName(std::string_view panelId)
    {
        return std::string("Editor.PanelSurface.Panel.") +
               ToString(panelId) + ".Frame";
    }

    std::string MakeCommandSurfaceMenuTitleWidgetName(
        std::string_view menuId)
    {
        return std::string("Editor.CommandSurface.Menu.") +
               ToString(menuId) + ".Title";
    }

    std::string MakeCommandSurfaceMenuItemWidgetName(
        std::string_view menuId,
        std::string_view commandId)
    {
        return std::string("Editor.CommandSurface.Menu.") +
               ToString(menuId) + ".Item." + ToString(commandId);
    }

    std::string GetFilePickerExpectedTitle(
        EditorAutomationScenarioType scenario)
    {
        if (scenario == EditorAutomationScenarioType::OpenSceneFilePicker)
        {
            return "Open Scene";
        }
        if (scenario == EditorAutomationScenarioType::SaveSceneAsFilePicker)
        {
            return "Save Scene";
        }
        return {};
    }

    std::string GetFilePickerExpectedAcceptText(
        EditorAutomationScenarioType scenario)
    {
        if (scenario == EditorAutomationScenarioType::OpenSceneFilePicker)
        {
            return "Open";
        }
        if (scenario == EditorAutomationScenarioType::SaveSceneAsFilePicker)
        {
            return "Save";
        }
        return {};
    }
}

EditorAutomationInteractionDriver::EditorAutomationInteractionDriver(
    IEditorUIBackend& backend)
    : m_backend(&backend)
{
}

bool EditorAutomationInteractionDriver::IsFilePickerDialogOpen(
    std::string_view id) const
{
    return m_backend &&
           m_backend->IsFilePickerDialogOpen(ToString(id));
}

bool EditorAutomationInteractionDriver::IsModalDialogOpen(
    std::string_view id) const
{
    return m_backend && m_backend->IsModalDialogOpen(ToString(id));
}

EditorUIAutomationResult EditorAutomationInteractionDriver::RequireWidgetVisible(
    std::string_view widgetName,
    std::string_view actionDescription)
{
    if (!m_backend)
    {
        return EditorUIAutomationResult::Failure(
            ToString(actionDescription) + " failed: UI backend is unavailable");
    }
    EditorUIHost* host = m_backend->GetHost();
    if (!host)
    {
        return EditorUIAutomationResult::Failure(
            ToString(actionDescription) + " failed: UI host is unavailable");
    }

    const std::string widget = ToString(widgetName);
    EditorUIAutomationDriver automation(*host);
    EditorUIAutomationResult result = automation.RequireWidgetVisible(widget);
    if (!result)
    {
        return EditorUIAutomationResult::Failure(
            ToString(actionDescription) + " failed for widget '" + widget +
            "': " + result.error);
    }
    return EditorUIAutomationResult::Success();
}

EditorUIAutomationResult EditorAutomationInteractionDriver::RequireWidgetEnabled(
    std::string_view widgetName,
    std::string_view actionDescription)
{
    if (!m_backend)
    {
        return EditorUIAutomationResult::Failure(
            ToString(actionDescription) + " failed: UI backend is unavailable");
    }
    EditorUIHost* host = m_backend->GetHost();
    if (!host)
    {
        return EditorUIAutomationResult::Failure(
            ToString(actionDescription) + " failed: UI host is unavailable");
    }

    const std::string widget = ToString(widgetName);
    EditorUIAutomationDriver automation(*host);
    EditorUIAutomationResult result = automation.RequireWidgetEnabled(widget);
    if (!result)
    {
        return EditorUIAutomationResult::Failure(
            ToString(actionDescription) + " failed for widget '" + widget +
            "': " + result.error);
    }
    return EditorUIAutomationResult::Success();
}

EditorUIAutomationResult
EditorAutomationInteractionDriver::RequireWidgetTextContains(
    std::string_view widgetName,
    std::string_view expectedText,
    std::string_view actionDescription)
{
    if (!m_backend)
    {
        return EditorUIAutomationResult::Failure(
            ToString(actionDescription) + " failed: UI backend is unavailable");
    }
    EditorUIHost* host = m_backend->GetHost();
    if (!host)
    {
        return EditorUIAutomationResult::Failure(
            ToString(actionDescription) + " failed: UI host is unavailable");
    }

    const std::string widget = ToString(widgetName);
    EditorUIAutomationDriver automation(*host);
    EditorUIAutomationResult result =
        automation.RequireWidgetTextContains(widget, ToString(expectedText));
    if (!result)
    {
        return EditorUIAutomationResult::Failure(
            ToString(actionDescription) + " failed for widget '" + widget +
            "': " + result.error);
    }
    return EditorUIAutomationResult::Success();
}

EditorUIAutomationResult
EditorAutomationInteractionDriver::RequireCommandRegistered(
    std::string_view commandId,
    std::string_view actionDescription)
{
    if (!m_backend)
    {
        return EditorUIAutomationResult::Failure(
            ToString(actionDescription) + " failed: UI backend is unavailable");
    }

    const EditorCommandRegistry* registry = m_backend->GetCommandRegistry();
    if (!registry)
    {
        return EditorUIAutomationResult::Failure(
            ToString(actionDescription) +
            " failed: command registry is unavailable");
    }

    const std::string command = ToString(commandId);
    if (!registry->FindCommand(command))
    {
        return EditorUIAutomationResult::Failure(
            ToString(actionDescription) + " failed: command '" + command +
            "' is not registered");
    }

    return EditorUIAutomationResult::Success();
}

EditorUIAutomationResult
EditorAutomationInteractionDriver::RequireCommandEnabled(
    std::string_view commandId,
    std::string_view actionDescription)
{
    EditorUIAutomationResult registered =
        RequireCommandRegistered(commandId, actionDescription);
    if (!registered)
    {
        return registered;
    }

    const EditorCommandRegistry* registry = m_backend->GetCommandRegistry();
    const std::string command = ToString(commandId);
    const EditorCommand* found = registry ? registry->FindCommand(command)
                                          : nullptr;
    if (!found || !found->desc.enabled)
    {
        return EditorUIAutomationResult::Failure(
            ToString(actionDescription) + " failed: command '" + command +
            "' is disabled");
    }

    return EditorUIAutomationResult::Success();
}

EditorUIAutomationResult
EditorAutomationInteractionDriver::RequireMenuCommandVisible(
    std::string_view menuId,
    std::string_view commandId,
    std::string_view actionDescription)
{
    EditorUIAutomationResult registered =
        RequireCommandRegistered(commandId, actionDescription);
    if (!registered)
    {
        return registered;
    }

    const EditorCommandSurfaceModel* surfaceModel =
        m_backend ? m_backend->GetCommandSurfaceModel() : nullptr;
    if (!surfaceModel)
    {
        return EditorUIAutomationResult::Failure(
            ToString(actionDescription) +
            " failed: command surface model is unavailable");
    }

    const std::string menu = ToString(menuId);
    const std::string command = ToString(commandId);
    const EditorMenuSurface* surface = surfaceModel->FindMenu(menu);
    if (!surface)
    {
        return EditorUIAutomationResult::Failure(
            ToString(actionDescription) + " failed: menu '" + menu +
            "' is not registered");
    }

    if (!ContainsVisibleSurfaceCommand(surface->items, commandId))
    {
        return EditorUIAutomationResult::Failure(
            ToString(actionDescription) + " failed: command '" + command +
            "' is not visible in menu '" + menu + "'");
    }

    return EditorUIAutomationResult::Success();
}

EditorUIAutomationResult
EditorAutomationInteractionDriver::RequireToolbarCommandVisible(
    std::string_view toolbarId,
    std::string_view commandId,
    std::string_view actionDescription)
{
    EditorUIAutomationResult registered =
        RequireCommandRegistered(commandId, actionDescription);
    if (!registered)
    {
        return registered;
    }

    const EditorCommandSurfaceModel* surfaceModel =
        m_backend ? m_backend->GetCommandSurfaceModel() : nullptr;
    if (!surfaceModel)
    {
        return EditorUIAutomationResult::Failure(
            ToString(actionDescription) +
            " failed: command surface model is unavailable");
    }

    const std::string toolbar = ToString(toolbarId);
    const std::string command = ToString(commandId);
    const EditorToolbarSurface* surface =
        surfaceModel->FindToolbar(toolbar);
    if (!surface)
    {
        return EditorUIAutomationResult::Failure(
            ToString(actionDescription) + " failed: toolbar '" + toolbar +
            "' is not registered");
    }

    if (!ContainsVisibleSurfaceCommand(surface->items, commandId))
    {
        return EditorUIAutomationResult::Failure(
            ToString(actionDescription) + " failed: command '" + command +
            "' is not visible in toolbar '" + toolbar + "'");
    }

    return EditorUIAutomationResult::Success();
}

EditorUIAutomationResult EditorAutomationInteractionDriver::ClickWidget(
    std::string_view widgetName,
    std::string_view actionDescription)
{
    if (!m_backend)
    {
        return EditorUIAutomationResult::Failure(
            ToString(actionDescription) + " failed: UI backend is unavailable");
    }

    const std::string widget = ToString(widgetName);
    EditorUIAutomationResult result = m_backend->ClickWidget(widget);
    if (!result)
    {
        return EditorUIAutomationResult::Failure(
            ToString(actionDescription) + " failed for widget '" + widget +
            "': " + result.error);
    }

    return EditorUIAutomationResult::Success();
}

EditorUIAutomationResult EditorAutomationInteractionDriver::ReplaceTextInput(
    std::string_view widgetName,
    std::string_view text,
    std::string_view actionDescription)
{
    if (!m_backend)
    {
        return EditorUIAutomationResult::Failure(
            ToString(actionDescription) + " failed: UI backend is unavailable");
    }

    const std::string widget = ToString(widgetName);
    EditorUIAutomationResult result =
        m_backend->ReplaceTextInput(widget, ToString(text));
    if (!result)
    {
        return EditorUIAutomationResult::Failure(
            ToString(actionDescription) + " failed for widget '" + widget +
            "': " + result.error);
    }

    return EditorUIAutomationResult::Success();
}

const char* GetEditorAutomationScenarioName(
    EditorAutomationScenarioType type)
{
    switch (type)
    {
        case EditorAutomationScenarioType::None:
            return "none";
        case EditorAutomationScenarioType::OpenNativePanel:
            return "open-native-panel";
        case EditorAutomationScenarioType::CommandPaletteOpen:
            return "command-palette-open";
        case EditorAutomationScenarioType::CommandSurfaceClick:
            return "command-surface-click";
        case EditorAutomationScenarioType::CommandMenuOpen:
            return "command-menu-open";
        case EditorAutomationScenarioType::OpenSceneFilePicker:
            return "open-scene-file-picker";
        case EditorAutomationScenarioType::SaveSceneAsFilePicker:
            return "save-scene-as-file-picker";
        case EditorAutomationScenarioType::UIScaleAdjust:
            return "ui-scale-adjust";
        case EditorAutomationScenarioType::ShortcutAutosaveWrite:
            return "shortcut-autosave-write";
    }
    return "unknown";
}

bool TryParseEditorAutomationScenario(
    std::string_view value,
    EditorAutomationScenarioType& type)
{
    if (value == "none")
    {
        type = EditorAutomationScenarioType::None;
        return true;
    }
    if (value == "shortcut-autosave-write")
    {
        type = EditorAutomationScenarioType::ShortcutAutosaveWrite;
        return true;
    }
    if (value == "open-native-panel")
    {
        type = EditorAutomationScenarioType::OpenNativePanel;
        return true;
    }
    if (value == "command-palette-open")
    {
        type = EditorAutomationScenarioType::CommandPaletteOpen;
        return true;
    }
    if (value == "command-surface-click")
    {
        type = EditorAutomationScenarioType::CommandSurfaceClick;
        return true;
    }
    if (value == "command-menu-open")
    {
        type = EditorAutomationScenarioType::CommandMenuOpen;
        return true;
    }
    if (value == "open-scene-file-picker")
    {
        type = EditorAutomationScenarioType::OpenSceneFilePicker;
        return true;
    }
    if (value == "save-scene-as-file-picker")
    {
        type = EditorAutomationScenarioType::SaveSceneAsFilePicker;
        return true;
    }
    if (value == "ui-scale-adjust")
    {
        type = EditorAutomationScenarioType::UIScaleAdjust;
        return true;
    }
    return false;
}

bool EditorAutomationScenarioRequiresSettingsPath(
    EditorAutomationScenarioType type)
{
    switch (type)
    {
        case EditorAutomationScenarioType::None:
        case EditorAutomationScenarioType::OpenNativePanel:
        case EditorAutomationScenarioType::CommandPaletteOpen:
        case EditorAutomationScenarioType::CommandSurfaceClick:
        case EditorAutomationScenarioType::CommandMenuOpen:
        case EditorAutomationScenarioType::OpenSceneFilePicker:
        case EditorAutomationScenarioType::SaveSceneAsFilePicker:
            return false;
        case EditorAutomationScenarioType::UIScaleAdjust:
        case EditorAutomationScenarioType::ShortcutAutosaveWrite:
            return true;
    }
    return false;
}

void EditorAutomationScenarioRunner::Reset(
    EditorAutomationScenarioType scenario,
    std::string target,
    bool confirmOverwrite)
{
    m_scenario = scenario;
    m_target = std::move(target);
    m_confirmOverwrite = confirmOverwrite;
    m_complete = scenario == EditorAutomationScenarioType::None;
    m_failed = false;
    m_error.clear();
    m_filePickerCommandExecuted = false;
    m_nativePanelWaitFrames = 0;
    m_commandPaletteWaitFrames = 0;
    m_filePickerWaitFrames = 0;
    m_filePickerModalWaitFrames = 0;
    m_filePickerResultWaitFrames = 0;
    m_commandSurfaceClickWaitFrames = 0;
    m_uiScaleAdjustWaitFrames = 0;
    m_uiScaleAdjustBaselineRuntimeScale = 0.0f;
    m_commandSurfaceClickMinimumHistorySequence = 0;
    m_commandSurfaceClickStep = CommandSurfaceClickStep::ClickMenuTitle;
    m_filePickerInteractionStep = FilePickerInteractionStep::WaitingForPicker;
    m_shortcutAutosaveWriteStep =
        ShortcutAutosaveWriteStep::WaitingForPreferences;
    m_uiScaleAdjustStep = UIScaleAdjustStep::WaitingForPreferences;
}

bool EditorAutomationScenarioRunner::PrepareSettings(
    EditorSettingsService& settingsService)
{
    if (m_failed || !HasScenario())
    {
        return !m_failed;
    }

    switch (m_scenario)
    {
        case EditorAutomationScenarioType::None:
            return true;
        case EditorAutomationScenarioType::OpenNativePanel:
        case EditorAutomationScenarioType::CommandPaletteOpen:
        case EditorAutomationScenarioType::CommandSurfaceClick:
        case EditorAutomationScenarioType::CommandMenuOpen:
        case EditorAutomationScenarioType::OpenSceneFilePicker:
        case EditorAutomationScenarioType::SaveSceneAsFilePicker:
            return true;
        case EditorAutomationScenarioType::UIScaleAdjust:
            return PrepareUIScaleAdjustSettings(settingsService);
        case EditorAutomationScenarioType::ShortcutAutosaveWrite:
            return PrepareShortcutAutosaveWriteSettings(settingsService);
    }

    Fail("Unsupported editor automation scenario");
    return false;
}

void EditorAutomationScenarioRunner::Advance(
    const EditorAutomationScenarioContext& context)
{
    if (m_failed || m_complete || !HasScenario())
    {
        return;
    }

    switch (m_scenario)
    {
        case EditorAutomationScenarioType::None:
            m_complete = true;
            return;
        case EditorAutomationScenarioType::OpenNativePanel:
            AdvanceOpenNativePanel(context);
            return;
        case EditorAutomationScenarioType::CommandPaletteOpen:
            AdvanceCommandPaletteOpen(context);
            return;
        case EditorAutomationScenarioType::CommandSurfaceClick:
            AdvanceCommandSurfaceClick(context);
            return;
        case EditorAutomationScenarioType::CommandMenuOpen:
            AdvanceCommandMenuOpen(context);
            return;
        case EditorAutomationScenarioType::OpenSceneFilePicker:
        case EditorAutomationScenarioType::SaveSceneAsFilePicker:
            AdvanceFilePicker(context);
            return;
        case EditorAutomationScenarioType::UIScaleAdjust:
            AdvanceUIScaleAdjust(context);
            return;
        case EditorAutomationScenarioType::ShortcutAutosaveWrite:
            AdvanceShortcutAutosaveWrite(context);
            return;
    }

    Fail(context, "Unsupported editor automation scenario");
}

void EditorAutomationScenarioRunner::Fail(std::string reason)
{
    if (m_failed)
    {
        return;
    }

    m_error = std::move(reason);
    m_failed = true;
    m_complete = false;
    RVX_CORE_ERROR("{}", m_error);
}

void EditorAutomationScenarioRunner::Fail(
    const EditorAutomationScenarioContext& context,
    std::string reason)
{
    PublishAutomationDiagnostic(context, false, reason);
    Fail(std::move(reason));
}

bool EditorAutomationScenarioRunner::PrepareShortcutAutosaveWriteSettings(
    EditorSettingsService& settingsService)
{
    settingsService.SetShortcutProfileAutosaveEnabled(false);
    settingsService.SetShortcutProfileAutosaveDebounceSeconds(0.0f);
    if (!settingsService.Save())
    {
        Fail("Automated shortcut autosave settings save failed: " +
             settingsService.GetLastError());
        return false;
    }
    return true;
}

bool EditorAutomationScenarioRunner::PrepareUIScaleAdjustSettings(
    EditorSettingsService& settingsService)
{
    settingsService.SetUIScaleFactor(RVX_EDITOR_DEFAULT_UI_SCALE_FACTOR);
    if (!settingsService.Save())
    {
        Fail("Automated UI scale settings save failed: " +
             settingsService.GetLastError());
        return false;
    }
    return true;
}

void EditorAutomationScenarioRunner::AdvanceOpenNativePanel(
    const EditorAutomationScenarioContext& context)
{
    if (!context.uiBackend)
    {
        return;
    }

    if (m_target.empty())
    {
        Fail(context,
             "Automated native panel open requires an automation target");
        return;
    }

    if (!context.uiBackend->GetPanel(m_target))
    {
        Fail(context,
             "Automated native panel open failed: unknown panel '" +
             m_target + "'");
        return;
    }

    if (context.uiBackend->IsPanelVisible(m_target))
    {
        if (EditorUIHost* host = context.uiBackend->GetHost())
        {
            host->GetDockingModel().SetActiveDockTab(m_target);
        }

        if (!RequireNativePanelFrame(context))
        {
            return;
        }

        m_complete = true;
        PublishAutomationDiagnostic(context,
                                    true,
                                    "Opened and validated native panel: " +
                                        m_target);
        RVX_CORE_INFO("Automated editor scenario '{}' completed: {}",
                      GetEditorAutomationScenarioName(m_scenario),
                      m_target);
        return;
    }

    const std::string commandId =
        MakeNativeUIPanelViewCommandId(m_target);
    if (commandId.empty())
    {
        Fail(context,
             "Automated native panel open failed: missing view command for '" +
             m_target + "'");
        return;
    }

    EditorAutomationInteractionDriver interaction(*context.uiBackend);
    EditorUIAutomationResult commandResult =
        interaction.RequireCommandEnabled(
            commandId,
            "Automated native panel view command validation");
    if (!commandResult)
    {
        Fail(context, commandResult.error);
        return;
    }
    commandResult = interaction.RequireMenuCommandVisible(
        RVX_EDITOR_AUTOMATION_VIEW_MENU_ID,
        commandId,
        "Automated native panel view menu validation");
    if (!commandResult)
    {
        Fail(context, commandResult.error);
        return;
    }

    if (!context.uiBackend->ExecuteCommand(commandId) ||
        !context.uiBackend->IsPanelVisible(m_target))
    {
        Fail(context,
             "Automated native panel open failed: command '" + commandId +
             "' did not show '" + m_target + "'");
        return;
    }

    m_nativePanelWaitFrames = 0;
}

void EditorAutomationScenarioRunner::AdvanceCommandPaletteOpen(
    const EditorAutomationScenarioContext& context)
{
    if (!context.uiBackend)
    {
        return;
    }

    EditorAutomationInteractionDriver interaction(*context.uiBackend);
    EditorUIAutomationResult commandResult =
        interaction.RequireCommandEnabled(
            EditorCommandIds::ViewCommandPalette,
            "Automated command palette command validation");
    if (!commandResult)
    {
        Fail(context, commandResult.error);
        return;
    }

    EditorUIHost* host = context.uiBackend->GetHost();
    if (!host)
    {
        Fail(context, "Automated command palette open requires native UI host");
        return;
    }

    if (!host->GetCommandPalette().IsOpen())
    {
        context.uiBackend->OpenCommandPalette(m_target);
    }

    if (!host->GetCommandPalette().IsOpen())
    {
        AdvanceWaitCounter(context,
                           m_commandPaletteWaitFrames,
                           "Automated command palette did not open");
        return;
    }

    if (!RequireCommandPaletteWidgets(context, interaction))
    {
        return;
    }

    m_complete = true;
    PublishAutomationDiagnostic(context,
                                true,
                                "Opened command palette");
    RVX_CORE_INFO("Automated editor scenario '{}' completed",
                  GetEditorAutomationScenarioName(m_scenario));
}

void EditorAutomationScenarioRunner::AdvanceCommandSurfaceClick(
    const EditorAutomationScenarioContext& context)
{
    if (!context.uiBackend)
    {
        return;
    }

    std::string menuId;
    std::string commandId;
    if (!ParseCommandSurfaceClickTarget(menuId, commandId))
    {
        Fail(context,
             "Automated command surface click requires target "
             "'<menu-id>:<command-id>'");
        return;
    }

    EditorAutomationInteractionDriver interaction(*context.uiBackend);
    EditorUIAutomationResult commandResult =
        interaction.RequireCommandEnabled(
            commandId,
            "Automated command surface command validation");
    if (!commandResult)
    {
        Fail(context, commandResult.error);
        return;
    }
    commandResult = interaction.RequireMenuCommandVisible(
        menuId,
        commandId,
        "Automated command surface menu validation");
    if (!commandResult)
    {
        Fail(context, commandResult.error);
        return;
    }

    switch (m_commandSurfaceClickStep)
    {
        case CommandSurfaceClickStep::ClickMenuTitle:
        {
            const EditorUIAutomationResult clickResult =
                interaction.ClickWidget(
                    MakeCommandSurfaceMenuTitleWidgetName(menuId),
                    "Automated command surface menu title click");
            if (!clickResult)
            {
                AdvanceWaitCounter(context,
                                   m_commandSurfaceClickWaitFrames,
                                   clickResult.error);
                return;
            }

            EditorUIHost* host = context.uiBackend->GetHost();
            if (!host ||
                !host->GetCommandSurfaceRenderer().IsMenuOpen(menuId))
            {
                AdvanceWaitCounter(
                    context,
                    m_commandSurfaceClickWaitFrames,
                    "Automated command surface menu title click did not open "
                    "menu: " +
                        menuId);
                return;
            }

            const std::vector<EditorUICommandHistoryEntry>& history =
                host->GetCommandHistory();
            m_commandSurfaceClickMinimumHistorySequence =
                history.empty() ? 1u : history.back().sequence + 1u;
            m_commandSurfaceClickStep =
                CommandSurfaceClickStep::ClickMenuItem;
            m_commandSurfaceClickWaitFrames = 0;
            return;
        }

        case CommandSurfaceClickStep::ClickMenuItem:
            break;
    }

    const std::string itemWidget =
        MakeCommandSurfaceMenuItemWidgetName(menuId, commandId);
    EditorUIAutomationResult itemVisible =
        interaction.RequireWidgetEnabled(
            itemWidget,
            "Automated command surface menu item validation");
    if (!itemVisible)
    {
        AdvanceWaitCounter(context,
                           m_commandSurfaceClickWaitFrames,
                           itemVisible.error);
        return;
    }

    const EditorUIAutomationResult clickResult =
        interaction.ClickWidget(
            itemWidget,
            "Automated command surface menu item click");
    if (!clickResult)
    {
        AdvanceWaitCounter(context,
                           m_commandSurfaceClickWaitFrames,
                           clickResult.error);
        return;
    }

    if (!ValidateCommandSurfaceClickHistory(
            context,
            commandId,
            m_commandSurfaceClickMinimumHistorySequence))
    {
        AdvanceWaitCounter(
            context,
            m_commandSurfaceClickWaitFrames,
            "Automated command surface click did not record successful "
            "command execution: " +
                commandId);
        return;
    }

    m_complete = true;
    PublishAutomationDiagnostic(
        context,
        true,
        "Clicked command surface menu item: " + menuId + ":" + commandId);
    RVX_CORE_INFO("Automated editor scenario '{}' completed: {}",
                  GetEditorAutomationScenarioName(m_scenario),
                  m_target);
}

void EditorAutomationScenarioRunner::AdvanceCommandMenuOpen(
    const EditorAutomationScenarioContext& context)
{
    if (!context.uiBackend)
    {
        return;
    }

    if (m_target.empty())
    {
        Fail(context,
             "Automated command menu open requires an automation target");
        return;
    }

    EditorCommandSurfaceModel* surfaceModel =
        context.uiBackend->GetCommandSurfaceModel();
    if (!surfaceModel || !surfaceModel->FindMenu(m_target))
    {
        Fail(context,
             "Automated command menu open failed: unknown menu '" +
             m_target + "'");
        return;
    }

    EditorUIHost* host = context.uiBackend->GetHost();
    if (!host)
    {
        Fail(context, "Automated command menu open requires native UI host");
        return;
    }

    EditorAutomationInteractionDriver interaction(*context.uiBackend);
    if (!host->GetCommandSurfaceRenderer().IsMenuOpen(m_target))
    {
        const EditorUIAutomationResult clickResult =
            interaction.ClickWidget(
                MakeCommandSurfaceMenuTitleWidgetName(m_target),
                "Automated command menu title click");
        if (!clickResult)
        {
            AdvanceWaitCounter(context,
                               m_commandSurfaceClickWaitFrames,
                               clickResult.error);
            return;
        }
    }

    if (!host->GetCommandSurfaceRenderer().IsMenuOpen(m_target))
    {
        AdvanceWaitCounter(
            context,
            m_commandSurfaceClickWaitFrames,
            "Automated command menu title click did not open menu: " +
                m_target);
        return;
    }

    const EditorUIAutomationResult dropdownVisible =
        interaction.RequireWidgetVisible(
            host->GetCommandSurfaceRenderer().GetOpenMenuRootWidgetName(),
            "Automated command menu dropdown validation");
    if (!dropdownVisible)
    {
        AdvanceWaitCounter(context,
                           m_commandSurfaceClickWaitFrames,
                           dropdownVisible.error);
        return;
    }

    m_complete = true;
    PublishAutomationDiagnostic(
        context,
        true,
        "Opened command menu: " + m_target);
    RVX_CORE_INFO("Automated editor scenario '{}' completed: {}",
                  GetEditorAutomationScenarioName(m_scenario),
                  m_target);
}

void EditorAutomationScenarioRunner::AdvanceFilePicker(
    const EditorAutomationScenarioContext& context)
{
    if (!context.uiBackend)
    {
        return;
    }

    if (!m_filePickerCommandExecuted)
    {
        ExecuteFilePickerCommand(context);
        return;
    }

    AdvanceFilePickerInteraction(context);
}

void EditorAutomationScenarioRunner::ExecuteFilePickerCommand(
    const EditorAutomationScenarioContext& context)
{
    if (!context.uiBackend || !IsFilePickerScenario(m_scenario))
    {
        return;
    }

    if (m_scenario == EditorAutomationScenarioType::SaveSceneAsFilePicker)
    {
        if (!context.documentSession)
        {
            Fail(context,
                 "Automated Save Scene As picker requires a document session");
            return;
        }

        if (!context.documentSession->HasActiveScene())
        {
            if (!context.documentSession->NewScene())
            {
                Fail(context,
                     "Automated Save Scene As picker setup failed: " +
                     context.documentSession->GetLastError());
                return;
            }

            if (context.refreshDocumentCommands)
            {
                context.refreshDocumentCommands();
            }
        }

        EditorAutomationInteractionDriver interaction(*context.uiBackend);
        EditorUIAutomationResult commandResult =
            interaction.RequireCommandEnabled(
                EditorCommandIds::FileSaveSceneAs,
                "Automated Save Scene As command validation");
        if (!commandResult)
        {
            Fail(context, commandResult.error);
            return;
        }
        commandResult = interaction.RequireMenuCommandVisible(
            RVX_EDITOR_AUTOMATION_FILE_MENU_ID,
            EditorCommandIds::FileSaveSceneAs,
            "Automated Save Scene As menu validation");
        if (!commandResult)
        {
            Fail(context, commandResult.error);
            return;
        }

        if (!context.uiBackend->ExecuteCommand(EditorCommandIds::FileSaveSceneAs))
        {
            Fail(context, "Automated Save Scene As picker command failed");
            return;
        }
        if (!context.uiBackend->IsFilePickerDialogOpen(
                RVX_EDITOR_AUTOMATION_SAVE_SCENE_PICKER_ID))
        {
            Fail(context,
                 "Automated Save Scene As command did not open the native "
                 "file picker");
            return;
        }
    }
    else if (m_scenario == EditorAutomationScenarioType::OpenSceneFilePicker)
    {
        EditorAutomationInteractionDriver interaction(*context.uiBackend);
        EditorUIAutomationResult commandResult =
            interaction.RequireCommandEnabled(
                EditorCommandIds::FileOpenScene,
                "Automated Open Scene command validation");
        if (!commandResult)
        {
            Fail(context, commandResult.error);
            return;
        }
        commandResult = interaction.RequireMenuCommandVisible(
            RVX_EDITOR_AUTOMATION_FILE_MENU_ID,
            EditorCommandIds::FileOpenScene,
            "Automated Open Scene menu validation");
        if (!commandResult)
        {
            Fail(context, commandResult.error);
            return;
        }

        if (!context.uiBackend->ExecuteCommand(EditorCommandIds::FileOpenScene))
        {
            Fail(context, "Automated Open Scene picker command failed");
            return;
        }
        if (!context.uiBackend->IsFilePickerDialogOpen(
                RVX_EDITOR_AUTOMATION_OPEN_SCENE_PICKER_ID))
        {
            Fail(context,
                 "Automated Open Scene command did not open the native file "
                 "picker");
            return;
        }
    }

    m_filePickerCommandExecuted = true;
    if (m_target.empty())
    {
        m_complete = true;
        PublishAutomationDiagnostic(context,
                                    true,
                                    "Opened native file picker");
        RVX_CORE_INFO("Automated editor scenario '{}' completed",
                      GetEditorAutomationScenarioName(m_scenario));
    }
}

void EditorAutomationScenarioRunner::AdvanceFilePickerInteraction(
    const EditorAutomationScenarioContext& context)
{
    if (!context.uiBackend || m_target.empty())
    {
        return;
    }

    EditorAutomationInteractionDriver interaction(*context.uiBackend);

    switch (m_filePickerInteractionStep)
    {
        case FilePickerInteractionStep::WaitingForPicker:
        {
            if (!interaction.IsFilePickerDialogOpen())
            {
                AdvanceWaitCounter(
                    context,
                    m_filePickerWaitFrames,
                    "Automated file picker did not open before the wait "
                    "limit");
                return;
            }
            if (!RequireFilePickerWidgets(context, interaction))
            {
                return;
            }
            m_filePickerInteractionStep =
                FilePickerInteractionStep::FillTargetPath;
            break;
        }

        case FilePickerInteractionStep::FillTargetPath:
            break;

        case FilePickerInteractionStep::AcceptTargetPath:
            break;

        case FilePickerInteractionStep::WaitingForOverwriteModal:
        {
            if (!interaction.IsModalDialogOpen(
                    RVX_EDITOR_AUTOMATION_OVERWRITE_SCENE_MODAL_ID))
            {
                AdvanceWaitCounter(
                    context,
                    m_filePickerModalWaitFrames,
                    "Automated file picker overwrite confirmation did not "
                    "open before the wait limit");
                return;
            }
            m_filePickerModalWaitFrames = 0;
            m_filePickerInteractionStep =
                FilePickerInteractionStep::ConfirmOverwrite;
            break;
        }

        case FilePickerInteractionStep::WaitingForResult:
        {
            if (IsFilePickerResultReady(context))
            {
                m_complete = true;
                PublishAutomationDiagnostic(
                    context,
                    true,
                    "Completed file picker target: " +
                    ResolveAutomationPath(m_target).string());
                RVX_CORE_INFO(
                    "Automated editor scenario '{}' completed: {}",
                    GetEditorAutomationScenarioName(m_scenario),
                    ResolveAutomationPath(m_target).string());
            }
            else
            {
                AdvanceWaitCounter(
                    context,
                    m_filePickerResultWaitFrames,
                    "Automated file picker did not produce the expected "
                    "scene result before the wait limit: " +
                    ResolveAutomationPath(m_target).string());
            }
            return;
        }

        case FilePickerInteractionStep::ConfirmOverwrite:
            break;
    }

    if (m_filePickerInteractionStep == FilePickerInteractionStep::FillTargetPath)
    {
        const std::string widgetPrefix = GetFilePickerWidgetPrefix();
        if (widgetPrefix.empty())
        {
            Fail(context,
                 "Automated file picker target requires an open-scene or "
                 "save-scene-as scenario");
            return;
        }

        const std::filesystem::path targetPath =
            ResolveAutomationPath(m_target);
        const EditorUIAutomationResult replaceResult =
            interaction.ReplaceTextInput(widgetPrefix + ".FileName",
                                         targetPath.string(),
                                         "Automated file picker file name fill");
        if (!replaceResult)
        {
            Fail(context, replaceResult.error);
            return;
        }

        m_filePickerInteractionStep =
            FilePickerInteractionStep::AcceptTargetPath;
        m_filePickerWaitFrames = 0;
        return;
    }

    if (m_filePickerInteractionStep == FilePickerInteractionStep::AcceptTargetPath)
    {
        const std::string widgetPrefix = GetFilePickerWidgetPrefix();
        if (widgetPrefix.empty())
        {
            Fail(context,
                 "Automated file picker accept requires an open-scene or "
                 "save-scene-as scenario");
            return;
        }

        const std::filesystem::path targetPath =
            ResolveAutomationPath(m_target);
        const EditorUIAutomationResult enabledResult =
            interaction.RequireWidgetEnabled(
                widgetPrefix + ".Button.accept",
                "Automated file picker accept enabled validation");
        if (!enabledResult)
        {
            AdvanceWaitCounter(context,
                               m_filePickerWaitFrames,
                               enabledResult.error);
            return;
        }
        const EditorUIAutomationResult acceptResult =
            interaction.ClickWidget(widgetPrefix + ".Button.accept",
                                    "Automated file picker accept");
        if (!acceptResult)
        {
            AdvanceWaitCounter(context,
                               m_filePickerWaitFrames,
                               acceptResult.error);
            return;
        }

        m_filePickerInteractionStep =
            m_confirmOverwrite
                ? FilePickerInteractionStep::WaitingForOverwriteModal
                : FilePickerInteractionStep::WaitingForResult;
        m_filePickerModalWaitFrames = 0;
        m_filePickerResultWaitFrames = 0;
        if (IsFilePickerResultReady(context))
        {
            m_complete = true;
            PublishAutomationDiagnostic(
                context,
                true,
                "Completed file picker target: " + targetPath.string());
            RVX_CORE_INFO("Automated editor scenario '{}' completed: {}",
                          GetEditorAutomationScenarioName(m_scenario),
                          targetPath.string());
        }
        return;
    }

    if (m_filePickerInteractionStep ==
        FilePickerInteractionStep::ConfirmOverwrite)
    {
        const EditorUIAutomationResult overwriteResult =
            interaction.ClickWidget(
                "Editor.ModalDialog.OverwriteScene.Button.overwrite",
                "Automated file picker overwrite confirmation");
        if (!overwriteResult)
        {
            Fail(context, overwriteResult.error);
            return;
        }

        m_filePickerInteractionStep =
            FilePickerInteractionStep::WaitingForResult;
        m_filePickerResultWaitFrames = 0;
        if (IsFilePickerResultReady(context))
        {
            m_complete = true;
            PublishAutomationDiagnostic(
                context,
                true,
                "Completed file picker target: " +
                ResolveAutomationPath(m_target).string());
            RVX_CORE_INFO(
                "Automated editor scenario '{}' completed: {}",
                GetEditorAutomationScenarioName(m_scenario),
                ResolveAutomationPath(m_target).string());
        }
    }
}

std::string EditorAutomationScenarioRunner::GetFilePickerWidgetPrefix() const
{
    if (m_scenario == EditorAutomationScenarioType::SaveSceneAsFilePicker)
    {
        return "Editor.FilePicker.SaveScene";
    }
    if (m_scenario == EditorAutomationScenarioType::OpenSceneFilePicker)
    {
        return "Editor.FilePicker.OpenScene";
    }
    return {};
}

bool EditorAutomationScenarioRunner::IsFilePickerResultReady(
    const EditorAutomationScenarioContext& context) const
{
    if (m_target.empty())
    {
        return true;
    }
    if (!context.documentSession)
    {
        return false;
    }

    const std::filesystem::path expectedPath = ResolveAutomationPath(m_target);
    std::filesystem::path actualPath =
        context.documentSession->GetScenePath().lexically_normal();
    if (!actualPath.empty())
    {
        std::error_code ec;
        actualPath = std::filesystem::absolute(actualPath, ec);
        if (!ec)
        {
            actualPath = actualPath.lexically_normal();
        }
    }

    if (actualPath != expectedPath)
    {
        return false;
    }

    if (m_scenario == EditorAutomationScenarioType::SaveSceneAsFilePicker)
    {
        return !context.documentSession->IsDirty();
    }

    return m_scenario == EditorAutomationScenarioType::OpenSceneFilePicker;
}

bool EditorAutomationScenarioRunner::RequireNativePanelFrame(
    const EditorAutomationScenarioContext& context)
{
    if (!context.uiBackend)
    {
        return false;
    }

    EditorAutomationInteractionDriver interaction(*context.uiBackend);
    const EditorUIAutomationResult result =
        interaction.RequireWidgetVisible(
            MakeNativePanelFrameWidgetName(m_target),
            "Automated native panel surface validation");
    if (!result)
    {
        AdvanceWaitCounter(context,
                           m_nativePanelWaitFrames,
                           result.error);
        return false;
    }
    m_nativePanelWaitFrames = 0;
    return true;
}

bool EditorAutomationScenarioRunner::RequireCommandPaletteWidgets(
    const EditorAutomationScenarioContext& context,
    EditorAutomationInteractionDriver& interaction)
{
    EditorUIAutomationResult result =
        interaction.RequireWidgetVisible(
            "Editor.CommandPalette",
            "Automated command palette root validation");
    if (!result)
    {
        AdvanceWaitCounter(context, m_commandPaletteWaitFrames, result.error);
        return false;
    }

    result = interaction.RequireWidgetVisible(
        "Editor.CommandPalette.Search",
        "Automated command palette search validation");
    if (!result)
    {
        AdvanceWaitCounter(context, m_commandPaletteWaitFrames, result.error);
        return false;
    }

    result = interaction.RequireWidgetVisible(
        "Editor.CommandPalette.Item.view.commandPalette",
        "Automated command palette item validation");
    if (!result)
    {
        AdvanceWaitCounter(context, m_commandPaletteWaitFrames, result.error);
        return false;
    }

    result = interaction.RequireWidgetTextContains(
        "Editor.CommandPalette.Item.view.commandPalette.Metadata",
        "Ctrl+Shift+P",
        "Automated command palette shortcut metadata validation");
    if (!result)
    {
        AdvanceWaitCounter(context, m_commandPaletteWaitFrames, result.error);
        return false;
    }

    m_commandPaletteWaitFrames = 0;
    return true;
}

bool EditorAutomationScenarioRunner::RequireFilePickerWidgets(
    const EditorAutomationScenarioContext& context,
    EditorAutomationInteractionDriver& interaction)
{
    const std::string widgetPrefix = GetFilePickerWidgetPrefix();
    if (widgetPrefix.empty())
    {
        Fail(context,
             "Automated file picker validation requires an open-scene or "
             "save-scene-as scenario");
        return false;
    }

    EditorUIAutomationResult result =
        interaction.RequireWidgetVisible(
            widgetPrefix + ".Panel",
            "Automated file picker panel validation");
    if (!result)
    {
        AdvanceWaitCounter(context, m_filePickerWaitFrames, result.error);
        return false;
    }

    result = interaction.RequireWidgetTextContains(
        widgetPrefix + ".Title",
        GetFilePickerExpectedTitle(m_scenario),
        "Automated file picker title validation");
    if (!result)
    {
        AdvanceWaitCounter(context, m_filePickerWaitFrames, result.error);
        return false;
    }

    result = interaction.RequireWidgetTextContains(
        widgetPrefix + ".Button.accept",
        GetFilePickerExpectedAcceptText(m_scenario),
        "Automated file picker accept button label validation");
    if (!result)
    {
        AdvanceWaitCounter(context, m_filePickerWaitFrames, result.error);
        return false;
    }

    result = interaction.RequireWidgetVisible(
        widgetPrefix + ".FileName",
        "Automated file picker file name input validation");
    if (!result)
    {
        AdvanceWaitCounter(context, m_filePickerWaitFrames, result.error);
        return false;
    }

    m_filePickerWaitFrames = 0;
    return true;
}

bool EditorAutomationScenarioRunner::RequireShortcutAutosaveWidgets(
    const EditorAutomationScenarioContext& context,
    EditorAutomationInteractionDriver& interaction)
{
    EditorUIAutomationResult result =
        interaction.RequireWidgetVisible(
            MakeNativePanelFrameWidgetName(
                NativeEditorPreferencesPanel::PanelId()),
            "Automated shortcut autosave preferences panel validation");
    if (!result)
    {
        AdvanceWaitCounter(context, m_nativePanelWaitFrames, result.error);
        return false;
    }

    result = interaction.RequireWidgetVisible(
        "NativePreferences.Shortcuts.Autosave.Status",
        "Automated shortcut autosave status validation");
    if (!result)
    {
        AdvanceWaitCounter(context, m_nativePanelWaitFrames, result.error);
        return false;
    }

    result = interaction.RequireWidgetEnabled(
        "NativePreferences.Shortcuts.Autosave.Toggle",
        "Automated shortcut autosave toggle validation");
    if (!result)
    {
        AdvanceWaitCounter(context, m_nativePanelWaitFrames, result.error);
        return false;
    }

    m_nativePanelWaitFrames = 0;
    return true;
}

bool EditorAutomationScenarioRunner::RequireUIScaleAdjustWidgets(
    const EditorAutomationScenarioContext& context,
    EditorAutomationInteractionDriver& interaction)
{
    EditorUIAutomationResult result =
        interaction.RequireWidgetVisible(
            MakeNativePanelFrameWidgetName(
                NativeEditorPreferencesPanel::PanelId()),
            "Automated UI scale preferences panel validation");
    if (!result)
    {
        AdvanceWaitCounter(context, m_uiScaleAdjustWaitFrames, result.error);
        return false;
    }

    result = interaction.RequireWidgetVisible(
        "NativePreferences.UIScale.Value",
        "Automated UI scale value validation");
    if (!result)
    {
        AdvanceWaitCounter(context, m_uiScaleAdjustWaitFrames, result.error);
        return false;
    }

    result = interaction.RequireWidgetEnabled(
        "NativePreferences.UIScale.Increase",
        "Automated UI scale increase validation");
    if (!result)
    {
        AdvanceWaitCounter(context, m_uiScaleAdjustWaitFrames, result.error);
        return false;
    }

    result = interaction.RequireWidgetEnabled(
        "NativePreferences.UIScale.Reset",
        "Automated UI scale reset validation");
    if (!result)
    {
        AdvanceWaitCounter(context, m_uiScaleAdjustWaitFrames, result.error);
        return false;
    }

    m_uiScaleAdjustWaitFrames = 0;
    return true;
}

bool EditorAutomationScenarioRunner::AdvanceWaitCounter(
    const EditorAutomationScenarioContext& context,
    uint32& counter,
    std::string reason)
{
    ++counter;
    if (counter < RVX_EDITOR_AUTOMATION_WAIT_FRAME_LIMIT)
    {
        return true;
    }

    Fail(context, std::move(reason));
    return false;
}

void EditorAutomationScenarioRunner::PublishAutomationDiagnostic(
    const EditorAutomationScenarioContext& context,
    bool succeeded,
    const std::string& message) const
{
    if (!context.uiBackend)
    {
        return;
    }

    EditorUIHost* host = context.uiBackend->GetHost();
    if (!host)
    {
        return;
    }

    host->RecordAutomationDiagnostic(
        GetEditorAutomationScenarioName(m_scenario),
        m_target,
        succeeded,
        message);
}

void EditorAutomationScenarioRunner::AdvanceShortcutAutosaveWrite(
    const EditorAutomationScenarioContext& context)
{
    if (!context.uiBackend)
    {
        return;
    }

    EditorAutomationInteractionDriver interaction(*context.uiBackend);

    switch (m_shortcutAutosaveWriteStep)
    {
        case ShortcutAutosaveWriteStep::WaitingForPreferences:
        {
            if (!context.uiBackend->IsPanelVisible(
                    NativeEditorPreferencesPanel::PanelId()))
            {
                context.uiBackend->SetPanelVisible(
                    NativeEditorPreferencesPanel::PanelId(),
                    true);
                context.uiBackend->RequestPanelRebuild(
                    NativeEditorPreferencesPanel::PanelId(),
                    EditorUIPanelRebuildReason::Visibility);
                return;
            }
            if (EditorUIHost* host = context.uiBackend->GetHost())
            {
                host->GetDockingModel().SetActiveDockTab(
                    NativeEditorPreferencesPanel::PanelId());
            }
            if (!RequireShortcutAutosaveWidgets(context, interaction))
            {
                return;
            }
            m_shortcutAutosaveWriteStep =
                ShortcutAutosaveWriteStep::EnableAutosave;
            return;
        }

        case ShortcutAutosaveWriteStep::EnableAutosave:
        {
            if (!RequireShortcutAutosaveWidgets(context, interaction))
            {
                return;
            }
            const EditorUIAutomationResult result =
                interaction.ClickWidget(
                    "NativePreferences.Shortcuts.Autosave.Toggle",
                    "Automated shortcut autosave toggle");
            if (!result)
            {
                Fail(context, result.error);
                return;
            }
            m_shortcutAutosaveWriteStep =
                ShortcutAutosaveWriteStep::RebindShortcut;
            return;
        }

        case ShortcutAutosaveWriteStep::RebindShortcut:
        {
            EditorUIAutomationResult commandResult =
                interaction.RequireCommandRegistered(
                    EditorCommandIds::FileSaveScene,
                    "Automated shortcut autosave command validation");
            if (!commandResult)
            {
                Fail(context, commandResult.error);
                return;
            }
            commandResult = interaction.RequireMenuCommandVisible(
                RVX_EDITOR_AUTOMATION_FILE_MENU_ID,
                EditorCommandIds::FileSaveScene,
                "Automated shortcut autosave file menu validation");
            if (!commandResult)
            {
                Fail(context, commandResult.error);
                return;
            }
            commandResult = interaction.RequireToolbarCommandVisible(
                RVX_EDITOR_AUTOMATION_MAIN_TOOLBAR_ID,
                EditorCommandIds::FileSaveScene,
                "Automated shortcut autosave toolbar validation");
            if (!commandResult)
            {
                Fail(context, commandResult.error);
                return;
            }

            EditorCommandRegistry* registry =
                context.uiBackend->GetCommandRegistry();
            if (!registry || !context.shortcutProfileService)
            {
                Fail(context,
                     "Automated shortcut autosave registry is unavailable");
                return;
            }

            if (!registry->SetCommandShortcuts(
                    EditorCommandIds::FileSaveScene,
                    EditorShortcut::Key(
                        static_cast<uint32>('M'),
                        UI::ToMask(UI::UIInputModifier::Ctrl)),
                    {},
                    RVX_EDITOR_COMMAND_SCOPE_GLOBAL_MASK))
            {
                Fail(context,
                     "Automated shortcut autosave could not rebind "
                     "file.saveScene");
                return;
            }
            if (!registry->GetShortcutConflicts().empty())
            {
                Fail(context,
                     "Automated shortcut autosave introduced shortcut "
                     "conflicts");
                return;
            }

            context.shortcutProfileService->MarkProfileDirty();
            context.uiBackend->RequestPanelRebuild(
                NativeEditorPreferencesPanel::PanelId(),
                EditorUIPanelRebuildReason::Data);
            m_shortcutAutosaveWriteStep =
                ShortcutAutosaveWriteStep::WaitingForAutosave;
            return;
        }

        case ShortcutAutosaveWriteStep::WaitingForAutosave:
            if (IsShortcutAutosaveWriteComplete(context))
            {
                m_complete = true;
                PublishAutomationDiagnostic(
                    context,
                    true,
                    "Shortcut autosave wrote profile");
                RVX_CORE_INFO(
                    "Automated editor scenario '{}' completed: {}",
                    GetEditorAutomationScenarioName(m_scenario),
                    context.shortcutProfileService
                        ? context.shortcutProfileService
                              ->ResolveProfilePath()
                              .string()
                        : std::string{});
            }
            return;
    }
}

void EditorAutomationScenarioRunner::AdvanceUIScaleAdjust(
    const EditorAutomationScenarioContext& context)
{
    if (!context.uiBackend)
    {
        return;
    }
    if (!context.settingsService)
    {
        Fail(context, "Automated UI scale adjust requires settings service");
        return;
    }

    EditorAutomationInteractionDriver interaction(*context.uiBackend);

    switch (m_uiScaleAdjustStep)
    {
        case UIScaleAdjustStep::WaitingForPreferences:
        {
            if (!context.uiBackend->IsPanelVisible(
                    NativeEditorPreferencesPanel::PanelId()))
            {
                context.uiBackend->SetPanelVisible(
                    NativeEditorPreferencesPanel::PanelId(),
                    true);
                context.uiBackend->RequestPanelRebuild(
                    NativeEditorPreferencesPanel::PanelId(),
                    EditorUIPanelRebuildReason::Visibility);
                return;
            }
            if (EditorUIHost* host = context.uiBackend->GetHost())
            {
                host->GetDockingModel().SetActiveDockTab(
                    NativeEditorPreferencesPanel::PanelId());
            }
            if (!RequireUIScaleAdjustWidgets(context, interaction))
            {
                return;
            }
            const EditorUIHost* host = context.uiBackend->GetHost();
            if (!host)
            {
                Fail(context,
                     "Automated UI scale adjust requires native UI host");
                return;
            }
            m_uiScaleAdjustBaselineRuntimeScale =
                host->GetUIContext().GetScaleFactor();
            if (m_uiScaleAdjustBaselineRuntimeScale <= 0.0f)
            {
                Fail(context,
                     "Automated UI scale adjust captured invalid baseline scale");
                return;
            }
            m_uiScaleAdjustStep = UIScaleAdjustStep::IncreaseScale;
            return;
        }

        case UIScaleAdjustStep::IncreaseScale:
        {
            if (!RequireUIScaleAdjustWidgets(context, interaction))
            {
                return;
            }
            const EditorUIAutomationResult result =
                interaction.ClickWidget(
                    "NativePreferences.UIScale.Increase",
                    "Automated UI scale increase");
            if (!result)
            {
                Fail(context, result.error);
                return;
            }
            if (!NearlyEqual(context.settingsService->GetUIScaleFactor(),
                             RVX_EDITOR_DEFAULT_UI_SCALE_FACTOR +
                                 RVX_EDITOR_AUTOMATION_UI_SCALE_DELTA))
            {
                Fail(context,
                     "Automated UI scale increase did not update settings");
                return;
            }
            m_uiScaleAdjustStep = UIScaleAdjustStep::ValidateIncreaseApplied;
            m_uiScaleAdjustWaitFrames = 0;
            return;
        }

        case UIScaleAdjustStep::ValidateIncreaseApplied:
        {
            if (!RequireUIScaleAdjustWidgets(context, interaction))
            {
                return;
            }
            const EditorUIHost* host = context.uiBackend->GetHost();
            if (!host)
            {
                Fail(context,
                     "Automated UI scale runtime validation requires native UI host");
                return;
            }

            const float expectedSettingsScale =
                RVX_EDITOR_DEFAULT_UI_SCALE_FACTOR +
                RVX_EDITOR_AUTOMATION_UI_SCALE_DELTA;
            const float expectedRuntimeScale =
                m_uiScaleAdjustBaselineRuntimeScale *
                (expectedSettingsScale / RVX_EDITOR_DEFAULT_UI_SCALE_FACTOR);
            const float runtimeScale = host->GetUIContext().GetScaleFactor();
            const float tolerance =
                std::max(0.01f, expectedRuntimeScale * 0.01f);
            if (std::fabs(runtimeScale - expectedRuntimeScale) <= tolerance)
            {
                m_uiScaleAdjustStep = UIScaleAdjustStep::ResetScale;
                m_uiScaleAdjustWaitFrames = 0;
                return;
            }

            AdvanceWaitCounter(
                context,
                m_uiScaleAdjustWaitFrames,
                "Automated UI scale increase did not apply to runtime UI");
            return;
        }

        case UIScaleAdjustStep::ResetScale:
        {
            if (!RequireUIScaleAdjustWidgets(context, interaction))
            {
                return;
            }
            const EditorUIAutomationResult result =
                interaction.ClickWidget(
                    "NativePreferences.UIScale.Reset",
                    "Automated UI scale reset");
            if (!result)
            {
                Fail(context, result.error);
                return;
            }
            m_uiScaleAdjustStep = UIScaleAdjustStep::ValidateReset;
            return;
        }

        case UIScaleAdjustStep::ValidateReset:
        {
            if (IsUIScaleAdjustComplete(context))
            {
                m_complete = true;
                PublishAutomationDiagnostic(
                    context,
                    true,
                    "UI scale adjusted and reset");
                RVX_CORE_INFO(
                    "Automated editor scenario '{}' completed: {}",
                    GetEditorAutomationScenarioName(m_scenario),
                    context.settingsService->GetSettingsPath().string());
                return;
            }

            AdvanceWaitCounter(context,
                               m_uiScaleAdjustWaitFrames,
                               "Automated UI scale reset did not persist");
            return;
        }
    }
}

bool EditorAutomationScenarioRunner::IsShortcutAutosaveWriteComplete(
    const EditorAutomationScenarioContext& context) const
{
    if (!context.shortcutProfileService)
    {
        return false;
    }

    const EditorShortcutProfileAutosaveState& autosaveState =
        context.shortcutProfileService->GetAutosaveState();
    if (!autosaveState.enabled || autosaveState.dirty ||
        autosaveState.lastResult.status !=
            EditorShortcutProfileOperationStatus::Exported)
    {
        return false;
    }

    const std::filesystem::path profilePath =
        context.shortcutProfileService->ResolveProfilePath();
    if (profilePath.empty())
    {
        return false;
    }

    EditorShortcutProfile profile;
    if (!EditorShortcutProfileSerializer::LoadFromFile(profilePath, profile))
    {
        return false;
    }

    const EditorShortcutProfileEntry* saveEntry =
        profile.FindEntry(EditorCommandIds::FileSaveScene);
    return saveEntry && saveEntry->shortcut.valid &&
           saveEntry->shortcut.keyCode == static_cast<uint32>('M') &&
           saveEntry->shortcut.modifiers ==
               UI::ToMask(UI::UIInputModifier::Ctrl);
}

bool EditorAutomationScenarioRunner::IsUIScaleAdjustComplete(
    const EditorAutomationScenarioContext& context) const
{
    if (!context.settingsService ||
        !NearlyEqual(context.settingsService->GetUIScaleFactor(),
                     RVX_EDITOR_DEFAULT_UI_SCALE_FACTOR))
    {
        return false;
    }
    if (!context.uiBackend || !context.uiBackend->GetHost() ||
        m_uiScaleAdjustBaselineRuntimeScale <= 0.0f)
    {
        return false;
    }

    const float runtimeScale =
        context.uiBackend->GetHost()->GetUIContext().GetScaleFactor();
    const float tolerance =
        std::max(0.01f, m_uiScaleAdjustBaselineRuntimeScale * 0.01f);
    if (std::fabs(runtimeScale - m_uiScaleAdjustBaselineRuntimeScale) >
        tolerance)
    {
        return false;
    }

    EditorSettingsService loaded(context.settingsService->GetSettingsPath());
    if (!loaded.Load())
    {
        return false;
    }

    return NearlyEqual(loaded.GetUIScaleFactor(),
                       RVX_EDITOR_DEFAULT_UI_SCALE_FACTOR);
}

bool EditorAutomationScenarioRunner::ParseCommandSurfaceClickTarget(
    std::string& outMenuId,
    std::string& outCommandId) const
{
    outMenuId.clear();
    outCommandId.clear();

    const size_t separator =
        m_target.find(RVX_EDITOR_AUTOMATION_TARGET_SEPARATOR);
    if (separator == std::string::npos || separator == 0u ||
        separator + 1u >= m_target.size())
    {
        return false;
    }

    outMenuId = m_target.substr(0u, separator);
    outCommandId = m_target.substr(separator + 1u);
    return !outMenuId.empty() && !outCommandId.empty();
}

bool EditorAutomationScenarioRunner::ValidateCommandSurfaceClickHistory(
    const EditorAutomationScenarioContext& context,
    const std::string& commandId,
    uint64 minimumSequence) const
{
    if (!context.uiBackend)
    {
        return false;
    }

    const EditorUIHost* host = context.uiBackend->GetHost();
    if (!host)
    {
        return false;
    }

    const std::vector<EditorUICommandHistoryEntry>& history =
        host->GetCommandHistory();
    return std::find_if(
               history.begin(),
               history.end(),
               [&commandId, minimumSequence](
                   const EditorUICommandHistoryEntry& entry) {
                   return !entry.diagnostic &&
                          entry.sequence >= minimumSequence &&
                          entry.source ==
                              EditorUICommandExecutionSource::Direct &&
                          entry.result.commandId == commandId &&
                          entry.result.Succeeded();
               }) != history.end();
}

} // namespace RVX::Editor
