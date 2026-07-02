/**
 * @file EditorAutomation.h
 * @brief Reusable editor smoke automation scenario runner.
 */

#pragma once

#include "Core/Types.h"
#include "Editor/UI/EditorUIAutomation.h"

#include <functional>
#include <string>
#include <string_view>

namespace RVX::Editor
{

class EditorDocumentSession;
class EditorSettingsService;
class EditorShortcutProfileService;
class IEditorUIBackend;

enum class EditorAutomationScenarioType : uint8
{
    None = 0,
    OpenNativePanel,
    CommandPaletteOpen,
    CommandSurfaceClick,
    CommandMenuOpen,
    OpenSceneFilePicker,
    SaveSceneAsFilePicker,
    UIScaleAdjust,
    ShortcutAutosaveWrite
};

const char* GetEditorAutomationScenarioName(
    EditorAutomationScenarioType type);
bool TryParseEditorAutomationScenario(
    std::string_view value,
    EditorAutomationScenarioType& type);
bool EditorAutomationScenarioRequiresSettingsPath(
    EditorAutomationScenarioType type);

struct EditorAutomationScenarioContext
{
    IEditorUIBackend* uiBackend = nullptr;
    EditorDocumentSession* documentSession = nullptr;
    EditorSettingsService* settingsService = nullptr;
    EditorShortcutProfileService* shortcutProfileService = nullptr;
    std::function<void()> refreshDocumentCommands;
};

/**
 * @brief Thin scenario-level wrapper around native UI automation primitives.
 */
class EditorAutomationInteractionDriver
{
public:
    explicit EditorAutomationInteractionDriver(IEditorUIBackend& backend);

    bool IsFilePickerDialogOpen(std::string_view id = {}) const;
    bool IsModalDialogOpen(std::string_view id = {}) const;
    EditorUIAutomationResult RequireWidgetVisible(
        std::string_view widgetName,
        std::string_view actionDescription);
    EditorUIAutomationResult RequireWidgetEnabled(
        std::string_view widgetName,
        std::string_view actionDescription);
    EditorUIAutomationResult RequireWidgetTextContains(
        std::string_view widgetName,
        std::string_view expectedText,
        std::string_view actionDescription);
    EditorUIAutomationResult RequireCommandRegistered(
        std::string_view commandId,
        std::string_view actionDescription);
    EditorUIAutomationResult RequireCommandEnabled(
        std::string_view commandId,
        std::string_view actionDescription);
    EditorUIAutomationResult RequireMenuCommandVisible(
        std::string_view menuId,
        std::string_view commandId,
        std::string_view actionDescription);
    EditorUIAutomationResult RequireToolbarCommandVisible(
        std::string_view toolbarId,
        std::string_view commandId,
        std::string_view actionDescription);
    EditorUIAutomationResult ClickWidget(
        std::string_view widgetName,
        std::string_view actionDescription);
    EditorUIAutomationResult ReplaceTextInput(
        std::string_view widgetName,
        std::string_view text,
        std::string_view actionDescription);

private:
    IEditorUIBackend* m_backend = nullptr;
};

class EditorAutomationScenarioRunner
{
public:
    void Reset(EditorAutomationScenarioType scenario,
               std::string target = std::string(),
               bool confirmOverwrite = false);

    EditorAutomationScenarioType GetScenario() const { return m_scenario; }
    const std::string& GetTarget() const { return m_target; }
    bool ShouldConfirmOverwrite() const { return m_confirmOverwrite; }
    bool HasScenario() const
    {
        return m_scenario != EditorAutomationScenarioType::None;
    }
    bool IsComplete() const { return m_complete; }
    bool HasFailed() const { return m_failed; }
    const std::string& GetError() const { return m_error; }

    bool PrepareSettings(EditorSettingsService& settingsService);
    void Advance(const EditorAutomationScenarioContext& context);
    void Fail(std::string reason);

private:
    enum class ShortcutAutosaveWriteStep : uint8
    {
        WaitingForPreferences = 0,
        EnableAutosave,
        RebindShortcut,
        WaitingForAutosave
    };

    enum class UIScaleAdjustStep : uint8
    {
        WaitingForPreferences = 0,
        IncreaseScale,
        ValidateIncreaseApplied,
        ResetScale,
        ValidateReset
    };

    enum class FilePickerInteractionStep : uint8
    {
        WaitingForPicker = 0,
        FillTargetPath,
        AcceptTargetPath,
        WaitingForOverwriteModal,
        ConfirmOverwrite,
        WaitingForResult
    };

    enum class CommandSurfaceClickStep : uint8
    {
        ClickMenuTitle = 0,
        ClickMenuItem
    };

    bool PrepareShortcutAutosaveWriteSettings(
        EditorSettingsService& settingsService);
    bool PrepareUIScaleAdjustSettings(
        EditorSettingsService& settingsService);
    void AdvanceOpenNativePanel(
        const EditorAutomationScenarioContext& context);
    void AdvanceCommandPaletteOpen(
        const EditorAutomationScenarioContext& context);
    void AdvanceCommandSurfaceClick(
        const EditorAutomationScenarioContext& context);
    void AdvanceCommandMenuOpen(
        const EditorAutomationScenarioContext& context);
    void AdvanceFilePicker(
        const EditorAutomationScenarioContext& context);
    void ExecuteFilePickerCommand(
        const EditorAutomationScenarioContext& context);
    void AdvanceFilePickerInteraction(
        const EditorAutomationScenarioContext& context);
    std::string GetFilePickerWidgetPrefix() const;
    bool IsFilePickerResultReady(
        const EditorAutomationScenarioContext& context) const;
    bool RequireNativePanelFrame(
        const EditorAutomationScenarioContext& context);
    bool RequireCommandPaletteWidgets(
        const EditorAutomationScenarioContext& context,
        EditorAutomationInteractionDriver& interaction);
    bool RequireFilePickerWidgets(
        const EditorAutomationScenarioContext& context,
        EditorAutomationInteractionDriver& interaction);
    bool RequireShortcutAutosaveWidgets(
        const EditorAutomationScenarioContext& context,
        EditorAutomationInteractionDriver& interaction);
    bool RequireUIScaleAdjustWidgets(
        const EditorAutomationScenarioContext& context,
        EditorAutomationInteractionDriver& interaction);
    bool AdvanceWaitCounter(
        const EditorAutomationScenarioContext& context,
        uint32& counter,
        std::string reason);
    void PublishAutomationDiagnostic(
        const EditorAutomationScenarioContext& context,
        bool succeeded,
        const std::string& message) const;
    void Fail(const EditorAutomationScenarioContext& context,
              std::string reason);
    void AdvanceShortcutAutosaveWrite(
        const EditorAutomationScenarioContext& context);
    void AdvanceUIScaleAdjust(
        const EditorAutomationScenarioContext& context);
    bool IsShortcutAutosaveWriteComplete(
        const EditorAutomationScenarioContext& context) const;
    bool IsUIScaleAdjustComplete(
        const EditorAutomationScenarioContext& context) const;
    bool ParseCommandSurfaceClickTarget(std::string& outMenuId,
                                        std::string& outCommandId) const;
    bool ValidateCommandSurfaceClickHistory(
        const EditorAutomationScenarioContext& context,
        const std::string& commandId,
        uint64 minimumSequence) const;

    EditorAutomationScenarioType m_scenario =
        EditorAutomationScenarioType::None;
    std::string m_target;
    bool m_confirmOverwrite = false;
    bool m_complete = false;
    bool m_failed = false;
    std::string m_error;
    bool m_filePickerCommandExecuted = false;
    uint32 m_nativePanelWaitFrames = 0;
    uint32 m_commandPaletteWaitFrames = 0;
    uint32 m_filePickerWaitFrames = 0;
    uint32 m_filePickerModalWaitFrames = 0;
    uint32 m_filePickerResultWaitFrames = 0;
    uint32 m_commandSurfaceClickWaitFrames = 0;
    uint32 m_uiScaleAdjustWaitFrames = 0;
    float m_uiScaleAdjustBaselineRuntimeScale = 0.0f;
    uint64 m_commandSurfaceClickMinimumHistorySequence = 0;
    CommandSurfaceClickStep m_commandSurfaceClickStep =
        CommandSurfaceClickStep::ClickMenuTitle;
    FilePickerInteractionStep m_filePickerInteractionStep =
        FilePickerInteractionStep::WaitingForPicker;
    ShortcutAutosaveWriteStep m_shortcutAutosaveWriteStep =
        ShortcutAutosaveWriteStep::WaitingForPreferences;
    UIScaleAdjustStep m_uiScaleAdjustStep =
        UIScaleAdjustStep::WaitingForPreferences;
};

} // namespace RVX::Editor
