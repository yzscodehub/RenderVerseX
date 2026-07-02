/**
 * @file NativeEditorPreferences.h
 * @brief Native UI editor preferences panel.
 */

#pragma once

#include "Editor/EditorSettings.h"
#include "Editor/UI/EditorUIBackendCatalog.h"
#include "Editor/UI/EditorUIPanel.h"
#include "Editor/UI/EditorShortcutBindingModel.h"
#include "Editor/UI/EditorShortcutProfileService.h"
#include "UI/UIRenderer.h"

#include <filesystem>
#include <string>
#include <utility>

namespace RVX::Editor
{

struct EditorFilePickerDialogResult;

struct NativeEditorPreferencesStats
{
    bool built = false;
    bool hasSettingsService = false;
    bool currentBackendFound = false;
    EditorUIBackendType currentBackendType = GetDefaultEditorUIBackendType();
    float currentUIScaleFactor = RVX_EDITOR_DEFAULT_UI_SCALE_FACTOR;
    uint32 uiScaleButtonCount = 0;
    bool uiScaleSaveAttempted = false;
    bool lastUIScaleSaveSucceeded = false;
    std::string uiScaleStatusText;
    uint32 fontPathCount = 0;
    bool appendDefaultSystemFonts = true;
    uint32 fontSettingsButtonCount = 0;
    uint32 fontPathRowCount = 0;
    bool fontSettingsSaveAttempted = false;
    bool lastFontSettingsSaveSucceeded = false;
    std::string fontStatusText;
    UI::UIFontFallbackChainDiagnostics fontDiagnostics;
    std::string fontDiagnosticsText;
    std::string fontDiagnosticsCoverageText;
    uint32 backendOptionCount = 0;
    uint32 selectableBackendCount = 0;
    uint32 activeBackendCount = 0;
    uint32 rowCount = 0;
    uint32 selectButtonCount = 0;
    bool saveAttempted = false;
    bool lastSaveSucceeded = false;
    bool hasCommandRegistry = false;
    uint32 shortcutCommandCount = 0;
    uint32 shortcutConflictCount = 0;
    uint32 shortcutExportButtonCount = 0;
    uint32 shortcutImportButtonCount = 0;
    bool shortcutExportAttempted = false;
    bool shortcutImportAttempted = false;
    bool lastShortcutExportSucceeded = false;
    bool lastShortcutImportSucceeded = false;
    uint32 lastShortcutApplyCount = 0;
    uint32 lastShortcutMissingCommandCount = 0;
    uint32 lastShortcutImportConflictCount = 0;
    std::filesystem::path shortcutProfilePath;
    bool shortcutProfileDirty = false;
    bool shortcutProfileAutosaveEnabled = false;
    float shortcutProfileAutosaveDebounceSeconds = 0.0f;
    uint32 shortcutProfileDirtyGeneration = 0;
    uint32 shortcutProfileSavedGeneration = 0;
    uint32 shortcutAutosaveToggleButtonCount = 0;
    bool shortcutAutosaveSettingsSaveAttempted = false;
    bool lastShortcutAutosaveSettingsSaveSucceeded = false;
    std::string shortcutAutosaveStatusText;
    std::string shortcutStatusText;
    uint32 shortcutBindingRowCount = 0;
    uint32 shortcutBindingButtonCount = 0;
    uint32 shortcutBindingClearButtonCount = 0;
    uint32 shortcutBindingConflictRowCount = 0;
    bool shortcutCaptureActive = false;
    bool shortcutCaptureAttempted = false;
    bool lastShortcutBindingSucceeded = false;
    bool lastShortcutBindingCleared = false;
    uint32 lastShortcutBindingConflictCount = 0;
    std::string shortcutCaptureCommandId;
    std::string shortcutCaptureSlotText;
    std::string shortcutBindingStatusText;
    bool hasScrollViewport = false;
    float scrollViewportHeight = 0.0f;
    float scrollContentHeight = 0.0f;
    float scrollOffsetY = 0.0f;
};

class NativeEditorPreferencesPanel final : public IEditorUIPanel
{
public:
    explicit NativeEditorPreferencesPanel(
        EditorSettingsService* settingsService = nullptr,
        EditorShortcutProfileService* shortcutProfileService = nullptr);

    static constexpr const char* PanelId() { return "native.preferences"; }

    const EditorUIPanelDesc& GetPanelDesc() const override { return m_desc; }
    void BuildUI(EditorUIPanelFrameContext& context) override;

    void SetSettingsService(EditorSettingsService* settingsService)
    {
        m_settingsService = settingsService;
        if (m_shortcutProfileService)
        {
            m_shortcutProfileService->SetSettingsService(settingsService);
        }
    }

    void SetShortcutProfilePath(std::filesystem::path path)
    {
        if (m_shortcutProfileService)
        {
            m_shortcutProfileService->SetProfilePathOverride(std::move(path));
        }
    }

    void SetShortcutProfileService(
        EditorShortcutProfileService* shortcutProfileService);

    const NativeEditorPreferencesStats& GetLastBuildStats() const
    {
        return m_lastBuildStats;
    }

private:
    float AddHeader(EditorUIPanelFrameContext& context);
    float AddUIScaleRows(EditorUIPanelFrameContext& context, float startY);
    float AddUIFontRows(EditorUIPanelFrameContext& context, float startY);
    float AddShortcutProfileRows(EditorUIPanelFrameContext& context,
                                 float startY);
    float AddBackendRows(EditorUIPanelFrameContext& context, float startY);
    void RefreshShortcutStats(EditorUIPanelFrameContext& context);
    void ProcessShortcutBindingCapture(EditorUIPanelFrameContext& context);
    bool SelectBackend(EditorUIBackendType type);
    bool AdjustUIScaleFactor(float delta);
    bool ResetUIScaleFactor();
    bool OpenUIFontPicker(EditorUIHost* host);
    void HandleUIFontPickerResult(const EditorFilePickerDialogResult& result,
                                  EditorUIHost& host);
    bool AddUIFontPath(std::filesystem::path path);
    bool RemoveUIFontPath(uint32 index);
    bool ToggleAppendDefaultSystemFonts(bool enabled);
    bool ResetUIFontSettings();
    bool ExportShortcutProfile(EditorUIHost* host);
    bool ImportShortcutProfile(EditorUIHost* host);
    void MarkShortcutProfileDirty();
    void ApplyShortcutAutosaveSettings();
    bool ToggleShortcutAutosaveEnabled(bool enabled);
    std::filesystem::path ResolveShortcutProfilePath() const;

    EditorUIPanelDesc m_desc;
    EditorSettingsService* m_settingsService = nullptr;
    NativeEditorPreferencesStats m_lastBuildStats;
    EditorShortcutBindingModel m_shortcutBindingModel;
    EditorShortcutProfileService m_ownedShortcutProfileService;
    EditorShortcutProfileService* m_shortcutProfileService = nullptr;
    std::string m_shortcutStatusText = "Shortcut profile ready.";
    std::string m_shortcutBindingStatusText =
        "Select a shortcut to rebind.";
    bool m_shortcutCaptureAttempted = false;
    bool m_lastShortcutBindingSucceeded = false;
    bool m_lastShortcutBindingCleared = false;
    uint32 m_lastShortcutBindingConflictCount = 0;
    bool m_shortcutExportAttempted = false;
    bool m_shortcutImportAttempted = false;
    bool m_lastShortcutExportSucceeded = false;
    bool m_lastShortcutImportSucceeded = false;
    bool m_shortcutAutosaveSettingsSaveAttempted = false;
    bool m_lastShortcutAutosaveSettingsSaveSucceeded = false;
    uint32 m_lastShortcutApplyCount = 0;
    uint32 m_lastShortcutMissingCommandCount = 0;
    uint32 m_lastShortcutImportConflictCount = 0;
    std::string m_shortcutAutosaveStatusText =
        "Shortcut autosave is disabled.";
    bool m_uiScaleSaveAttempted = false;
    bool m_lastUIScaleSaveSucceeded = false;
    std::string m_uiScaleStatusText = "UI scale ready.";
    bool m_fontSettingsSaveAttempted = false;
    bool m_lastFontSettingsSaveSucceeded = false;
    std::string m_fontStatusText =
        "Font settings apply on next editor launch.";
    float m_scrollOffsetY = 0.0f;
};

} // namespace RVX::Editor
