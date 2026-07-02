/**
 * @file NativeEditorPreferences.cpp
 * @brief Native UI editor preferences panel implementation.
 */

#include "Editor/Panels/NativeEditorPreferences.h"

#include "Editor/EditorSettings.h"
#include "Editor/UI/EditorFormLayout.h"
#include "Editor/UI/EditorFilePickerDialog.h"
#include "Editor/UI/EditorPanelButtonStyle.h"
#include "Editor/UI/EditorShellMetrics.h"
#include "Editor/UI/EditorTypography.h"
#include "Editor/UI/EditorUIHost.h"
#include "UI/Widgets/Button.h"
#include "UI/Widgets/Label.h"
#include "UI/Widgets/Panel.h"
#include "UI/Widgets/ScrollView.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <span>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace RVX::Editor
{
namespace
{
    constexpr float RVX_NATIVE_PREFERENCES_UI_SCALE_STEP = 0.25f;

    UI::Label::Ptr CreatePreferencesLabel(const std::string& name,
                                          const std::string& text,
                                          const UI::UITheme& theme,
                                          const UI::UIColor& color)
    {
        UI::Label::Ptr label = UI::Label::Create(text);
        label->SetName(name);
        label->SetFontSize(EditorTypography::GetFontSize(
            theme,
            EditorTypographyRole::PropertyLabel));
        label->SetTextColor(color);
        label->SetVerticalAlign(UI::VerticalAlign::Middle);
        label->SetInteractive(false);
        return label;
    }

    UI::Button::Ptr CreatePreferencesButton(const std::string& name,
                                            const std::string& text,
                                            const UI::UITheme& theme)
    {
        UI::Button::Ptr button = UI::Button::Create(text);
        button->SetName(name);
        EditorPanelButtonStyleDesc styleDesc;
        styleDesc.overflowMode = UI::TextOverflowMode::Clip;
        styleDesc.setTooltipFromText = false;
        EditorPanelButtonStyle::Apply(*button, theme, styleDesc);
        return button;
    }

    std::string BackendStatusLabel(const EditorUIBackendDescriptor& descriptor,
                                   EditorUIBackendType currentBackend)
    {
        if (descriptor.type == currentBackend)
        {
            return "Active";
        }
        return descriptor.canCreate ? "Available" : "Unavailable";
    }

    UI::UIColor BackendStatusColor(const EditorUIBackendDescriptor& descriptor,
                                   EditorUIBackendType currentBackend,
                                   const UI::UITheme& theme)
    {
        if (descriptor.type == currentBackend)
        {
            return theme.colors.accent;
        }
        return descriptor.canCreate ? theme.colors.textMuted : theme.colors.warning;
    }

    std::string FitTextToWidth(const std::string& text,
                               float width,
                               float fontSize)
    {
        const float characterWidth = std::max(1.0f, fontSize * 0.58f);
        const size_t maxCharacters =
            static_cast<size_t>(std::max(1.0f, width / characterWidth));
        if (text.size() <= maxCharacters)
        {
            return text;
        }
        if (maxCharacters <= 3u)
        {
            return text.substr(0, maxCharacters);
        }
        return text.substr(0, maxCharacters - 3u) + "...";
    }

    std::string ShortcutSlotButtonText(const char* slotPrefix,
                                       const std::string& shortcutText,
                                       bool captureTarget,
                                       float width,
                                       float fontSize)
    {
        const std::string text =
            captureTarget ? "Press key..." :
                            (std::string(slotPrefix) + ": " + shortcutText);
        return FitTextToWidth(text, width, fontSize);
    }

    std::string FormatShortcutAutosaveSeconds(float seconds)
    {
        std::ostringstream stream;
        stream << std::fixed << std::setprecision(2) << seconds;
        std::string text = stream.str();
        while (text.size() > 1u && text.back() == '0')
        {
            text.pop_back();
        }
        if (!text.empty() && text.back() == '.')
        {
            text.pop_back();
        }
        return text + "s";
    }

    std::string FormatUIScalePercent(float scaleFactor)
    {
        const int percent =
            static_cast<int>(std::round(scaleFactor * 100.0f));
        return std::to_string(percent) + "%";
    }

    std::string ToLowerAscii(std::string value)
    {
        std::transform(value.begin(),
                       value.end(),
                       value.begin(),
                       [](unsigned char ch) {
                           return static_cast<char>(std::tolower(ch));
                       });
        return value;
    }

    bool IsSupportedUIFontPath(const std::filesystem::path& path)
    {
        const std::string extension = ToLowerAscii(path.extension().string());
        return extension == ".ttf" || extension == ".ttc" ||
               extension == ".otf";
    }

    std::filesystem::path NormalizeUIFontPath(std::filesystem::path path)
    {
        return path.lexically_normal();
    }

    std::string FormatUIFontPathLabel(uint32 index,
                                      const std::filesystem::path& path,
                                      float width,
                                      float fontSize)
    {
        const std::string prefix =
            "Font " + std::to_string(index + 1u) + ": ";
        (void)width;
        (void)fontSize;
        return prefix + path.string();
    }

    std::string FormatUIFontSummary(uint32 fontPathCount,
                                    bool appendDefaultSystemFonts)
    {
        std::string summary = "Custom font paths: ";
        summary += std::to_string(fontPathCount);
        summary += " | System fallback: ";
        summary += appendDefaultSystemFonts ? "On" : "Off";
        return summary;
    }

    std::string FormatUIFontDiagnosticsStatus(
        const UI::UIFontFallbackChainDiagnostics& diagnostics)
    {
        std::string text = "Core ";
        text += diagnostics.missingCoreCodepointCount == 0u
                    ? "OK"
                    : ("missing " +
                       std::to_string(diagnostics.missingCoreCodepointCount));
        text += " | Fonts: ";
        text += std::to_string(diagnostics.fontCount);
        return text;
    }

    std::string FormatUIFontDiagnosticsCoverage(
        const UI::UIFontFallbackChainDiagnostics& diagnostics)
    {
        std::string text = "Coverage: Latin ";
        text += diagnostics.hasLatinText ? "OK" : "missing";
        text += " | Symbols ";
        text += diagnostics.hasEditorSymbols ? "OK" : "missing";
        text += " | CJK ";
        text += diagnostics.hasCjkSample ? "OK" : "missing";
        return text;
    }

    std::string FormatShortcutAutosaveStatus(
        const NativeEditorPreferencesStats& stats)
    {
        std::string status = "Autosave: ";
        status += stats.shortcutProfileAutosaveEnabled ? "On" : "Off";
        status += stats.shortcutProfileDirty ? " | Unsaved changes"
                                             : " | Clean";
        status += " | Delay ";
        status += FormatShortcutAutosaveSeconds(
            stats.shortcutProfileAutosaveDebounceSeconds);
        return status;
    }

    UI::UIColor ShortcutStatusColor(const NativeEditorPreferencesStats& stats,
                                    const UI::UITheme& theme)
    {
        if ((stats.shortcutImportAttempted &&
             !stats.lastShortcutImportSucceeded) ||
            (stats.shortcutExportAttempted &&
             !stats.lastShortcutExportSucceeded))
        {
            return theme.colors.warning;
        }

        return stats.shortcutConflictCount > 0u ? theme.colors.warning
                                                : theme.colors.textMuted;
    }
} // namespace

NativeEditorPreferencesPanel::NativeEditorPreferencesPanel(
    EditorSettingsService* settingsService,
    EditorShortcutProfileService* shortcutProfileService)
    : m_settingsService(settingsService),
      m_ownedShortcutProfileService(settingsService),
      m_shortcutProfileService(shortcutProfileService
                                   ? shortcutProfileService
                                   : &m_ownedShortcutProfileService)
{
    m_desc.id = PanelId();
    m_desc.title = "Preferences";
    m_desc.defaultDockArea = EditorUIPanelDockArea::Floating;
    m_desc.visibleByDefault = false;
    m_desc.closable = true;
    if (m_shortcutProfileService)
    {
        m_shortcutProfileService->SetSettingsService(settingsService);
    }
}

void NativeEditorPreferencesPanel::SetShortcutProfileService(
    EditorShortcutProfileService* shortcutProfileService)
{
    m_shortcutProfileService = shortcutProfileService
                                   ? shortcutProfileService
                                   : &m_ownedShortcutProfileService;
    if (m_shortcutProfileService)
    {
        m_shortcutProfileService->SetSettingsService(m_settingsService);
    }
}

void NativeEditorPreferencesPanel::BuildUI(EditorUIPanelFrameContext& context)
{
    m_lastBuildStats = {};
    m_lastBuildStats.built = true;
    m_lastBuildStats.hasSettingsService = m_settingsService != nullptr;
    m_lastBuildStats.currentBackendType =
        m_settingsService ? m_settingsService->GetUIBackendType()
                          : GetDefaultEditorUIBackendType();
    m_lastBuildStats.currentUIScaleFactor =
        m_settingsService ? m_settingsService->GetUIScaleFactor()
                          : RVX_EDITOR_DEFAULT_UI_SCALE_FACTOR;
    m_lastBuildStats.uiScaleSaveAttempted = m_uiScaleSaveAttempted;
    m_lastBuildStats.lastUIScaleSaveSucceeded =
        m_lastUIScaleSaveSucceeded;
    m_lastBuildStats.uiScaleStatusText = m_uiScaleStatusText;
    m_lastBuildStats.fontPathCount =
        m_settingsService
            ? static_cast<uint32>(m_settingsService->GetUIFontPaths().size())
            : 0u;
    m_lastBuildStats.appendDefaultSystemFonts =
        !m_settingsService || m_settingsService->ShouldAppendDefaultSystemFonts();
    m_lastBuildStats.fontSettingsSaveAttempted =
        m_fontSettingsSaveAttempted;
    m_lastBuildStats.lastFontSettingsSaveSucceeded =
        m_lastFontSettingsSaveSucceeded;
    m_lastBuildStats.fontStatusText = m_fontStatusText;
    m_lastBuildStats.fontDiagnostics =
        UI::UIFontFallbackChain::GetDefaultDiagnostics();
    m_lastBuildStats.fontDiagnosticsText =
        FormatUIFontDiagnosticsStatus(m_lastBuildStats.fontDiagnostics);
    m_lastBuildStats.fontDiagnosticsCoverageText =
        FormatUIFontDiagnosticsCoverage(m_lastBuildStats.fontDiagnostics);

    const std::span<const EditorUIBackendDescriptor> backends =
        GetEditorUIBackendCatalog();
    m_lastBuildStats.backendOptionCount =
        static_cast<uint32>(backends.size());
    for (const EditorUIBackendDescriptor& backend : backends)
    {
        if (backend.canCreate)
        {
            ++m_lastBuildStats.selectableBackendCount;
        }
        if (backend.type == m_lastBuildStats.currentBackendType)
        {
            ++m_lastBuildStats.activeBackendCount;
        }
    }
    m_lastBuildStats.currentBackendFound =
        FindEditorUIBackendDescriptor(m_lastBuildStats.currentBackendType) !=
        nullptr;
    ApplyShortcutAutosaveSettings();
    ProcessShortcutBindingCapture(context);
    RefreshShortcutStats(context);

    if (!context.ui || !context.contentContainer)
    {
        return;
    }

    EditorFormLayoutDesc formDesc;
    formDesc.context = &context;
    formDesc.namePrefix = "NativePreferences";
    formDesc.scrollOffsetY = m_scrollOffsetY;
    formDesc.onScrollChanged = [this](const Vec2& offset) {
        m_scrollOffsetY = offset.y;
    };

    EditorFormLayoutFrame form =
        EditorFormLayout::BeginScrollableFrame(std::move(formDesc));
    if (!form)
    {
        return;
    }

    m_lastBuildStats.hasScrollViewport = true;
    m_lastBuildStats.scrollViewportHeight =
        form.metrics.viewportHeight;
    m_lastBuildStats.scrollOffsetY = m_scrollOffsetY;

    EditorUIPanelFrameContext contentContext = form.contentContext;
    const float uiScaleStartY = AddHeader(contentContext);
    const float fontStartY = AddUIScaleRows(contentContext,
                                            uiScaleStartY);
    const float shortcutStartY = AddUIFontRows(contentContext,
                                               fontStartY);
    const float backendStartY = AddShortcutProfileRows(contentContext,
                                                       shortcutStartY);
    const float contentHeight = AddBackendRows(contentContext,
                                               backendStartY);
    m_lastBuildStats.scrollContentHeight = contentHeight;
    EditorFormLayout::EndScrollableFrame(form, contentHeight);
}

float NativeEditorPreferencesPanel::AddHeader(
    EditorUIPanelFrameContext& context)
{
    const UI::UITheme& theme = context.ui->GetTheme();
    const EditorShellMetrics shellMetrics = EditorShellMetricPolicy::Resolve(theme);
    const float padding = shellMetrics.contentPadding;
    const float lineHeight = shellMetrics.formLineHeight;
    const float rowWidth =
        std::max(0.0f, context.contentContainer->GetWidth() - padding * 2.0f);

    UI::Label::Ptr title = CreatePreferencesLabel("NativePreferences.Header",
                                                  "Editor Preferences",
                                                  theme,
                                                  theme.colors.text);
    title->SetPosition(padding, padding);
    title->SetSize(rowWidth, lineHeight);
    context.contentContainer->AddChild(title);

    return padding + lineHeight + padding * 0.5f;
}

float NativeEditorPreferencesPanel::AddUIScaleRows(
    EditorUIPanelFrameContext& context,
    float startY)
{
    const UI::UITheme& theme = context.ui->GetTheme();
    const EditorShellMetrics shellMetrics = EditorShellMetricPolicy::Resolve(theme);
    const float padding = shellMetrics.contentPadding;
    const float lineHeight = shellMetrics.formLineHeight;
    const float rowHeight = shellMetrics.formRowHeight;
    const float rowWidth =
        std::max(0.0f, context.contentContainer->GetWidth() - padding * 2.0f);

    UI::Label::Ptr header = CreatePreferencesLabel(
        "NativePreferences.UIScale.Header",
        "UI Scale",
        theme,
        theme.colors.text);
    header->SetPosition(padding, startY);
    header->SetSize(rowWidth, lineHeight);
    context.contentContainer->AddChild(header);

    UI::Label::Ptr value = CreatePreferencesLabel(
        "NativePreferences.UIScale.Value",
        "Current: " +
            FormatUIScalePercent(m_lastBuildStats.currentUIScaleFactor),
        theme,
        theme.colors.accent);
    value->SetPosition(padding, startY + lineHeight);
    value->SetSize(rowWidth, lineHeight);
    context.contentContainer->AddChild(value);

    UI::Label::Ptr status = CreatePreferencesLabel(
        "NativePreferences.UIScale.Status",
        m_lastBuildStats.uiScaleStatusText,
        theme,
        m_lastBuildStats.uiScaleSaveAttempted &&
                !m_lastBuildStats.lastUIScaleSaveSucceeded
            ? theme.colors.warning
            : theme.colors.textMuted);
    status->SetPosition(padding, startY + lineHeight * 2.0f);
    status->SetSize(rowWidth, lineHeight);
    context.contentContainer->AddChild(status);

    const float buttonY = startY + lineHeight * 3.0f;
    const float buttonGap = shellMetrics.compactGap;
    const float minimumCompactButtonWidth = 36.0f;
    const float preferredResetButtonWidth = 72.0f;
    float compactButtonWidth = std::max(
        minimumCompactButtonWidth,
        (rowWidth - preferredResetButtonWidth - buttonGap * 2.0f) * 0.5f);
    float resetButtonWidth = preferredResetButtonWidth;
    const float preferredButtonRowWidth =
        compactButtonWidth * 2.0f + resetButtonWidth + buttonGap * 2.0f;
    if (preferredButtonRowWidth > rowWidth)
    {
        compactButtonWidth =
            std::max(minimumCompactButtonWidth,
                     (rowWidth - buttonGap * 2.0f) / 3.0f);
        resetButtonWidth = compactButtonWidth;
    }
    const bool canSaveScale = m_settingsService != nullptr;
    auto addButton = [&](const std::string& name,
                         const std::string& text,
                         float x,
                         float width,
                         auto&& onClick) {
        UI::Button::Ptr button = CreatePreferencesButton(name, text, theme);
        button->SetPosition(x, buttonY);
        button->SetSize(width, rowHeight);
        button->SetEnabled(canSaveScale);
        button->SetOnClick(std::forward<decltype(onClick)>(onClick));
        context.contentContainer->AddChild(button);
        ++m_lastBuildStats.uiScaleButtonCount;
    };

    addButton("NativePreferences.UIScale.Decrease",
              "-",
              padding,
              compactButtonWidth,
              [this, host = context.host](const UI::UIEvent& event) {
                  (void)event;
                  AdjustUIScaleFactor(-RVX_NATIVE_PREFERENCES_UI_SCALE_STEP);
                  if (host)
                  {
                      host->RequestPanelRebuild(
                          NativeEditorPreferencesPanel::PanelId(),
                          EditorUIPanelRebuildReason::Data);
                  }
              });

    addButton("NativePreferences.UIScale.Increase",
              "+",
              padding + compactButtonWidth + buttonGap,
              compactButtonWidth,
              [this, host = context.host](const UI::UIEvent& event) {
                  (void)event;
                  AdjustUIScaleFactor(RVX_NATIVE_PREFERENCES_UI_SCALE_STEP);
                  if (host)
                  {
                      host->RequestPanelRebuild(
                          NativeEditorPreferencesPanel::PanelId(),
                          EditorUIPanelRebuildReason::Data);
                  }
              });

    addButton("NativePreferences.UIScale.Reset",
              "Reset",
              padding + compactButtonWidth * 2.0f + buttonGap * 2.0f,
              resetButtonWidth,
              [this, host = context.host](const UI::UIEvent& event) {
                  (void)event;
                  ResetUIScaleFactor();
                  if (host)
                  {
                      host->RequestPanelRebuild(
                          NativeEditorPreferencesPanel::PanelId(),
                          EditorUIPanelRebuildReason::Data);
                  }
              });

    return buttonY + rowHeight + padding * 2.0f;
}

float NativeEditorPreferencesPanel::AddUIFontRows(
    EditorUIPanelFrameContext& context,
    float startY)
{
    const UI::UITheme& theme = context.ui->GetTheme();
    const EditorShellMetrics shellMetrics = EditorShellMetricPolicy::Resolve(theme);
    const float padding = shellMetrics.contentPadding;
    const float lineHeight = shellMetrics.formLineHeight;
    const float rowHeight = shellMetrics.formRowHeight;
    const float rowWidth =
        std::max(0.0f, context.contentContainer->GetWidth() - padding * 2.0f);
    const uint32 fontPathCount = m_lastBuildStats.fontPathCount;
    const bool appendDefaultSystemFonts =
        m_lastBuildStats.appendDefaultSystemFonts;
    const float fontSize = EditorTypography::GetFontSize(
        theme,
        EditorTypographyRole::PropertyLabel);

    UI::Label::Ptr header = CreatePreferencesLabel(
        "NativePreferences.Font.Header",
        "Editor Font",
        theme,
        theme.colors.text);
    header->SetPosition(padding, startY);
    header->SetSize(rowWidth, lineHeight);
    context.contentContainer->AddChild(header);

    UI::Label::Ptr summary = CreatePreferencesLabel(
        "NativePreferences.Font.Summary",
        FormatUIFontSummary(fontPathCount, appendDefaultSystemFonts),
        theme,
        appendDefaultSystemFonts || fontPathCount > 0u ? theme.colors.textMuted
                                                       : theme.colors.warning);
    summary->SetPosition(padding, startY + lineHeight);
    summary->SetSize(rowWidth, lineHeight);
    context.contentContainer->AddChild(summary);

    const std::filesystem::path primaryFontPath =
        (m_settingsService && !m_settingsService->GetUIFontPaths().empty())
            ? m_settingsService->GetUIFontPaths().front()
            : std::filesystem::path();
    UI::Label::Ptr primary = CreatePreferencesLabel(
        "NativePreferences.Font.Primary",
        primaryFontPath.empty()
            ? std::string("Primary: system default")
            : ("Primary: " + primaryFontPath.string()),
        theme,
        theme.colors.textMuted);
    primary->SetOverflowMode(UI::TextOverflowMode::MiddleEllipsis);
    if (!primaryFontPath.empty())
    {
        primary->SetTooltipText(primaryFontPath.string());
    }
    primary->SetPosition(padding, startY + lineHeight * 2.0f);
    primary->SetSize(rowWidth, lineHeight);
    context.contentContainer->AddChild(primary);

    UI::Label::Ptr status = CreatePreferencesLabel(
        "NativePreferences.Font.Status",
        m_lastBuildStats.fontStatusText,
        theme,
        m_lastBuildStats.fontSettingsSaveAttempted &&
                !m_lastBuildStats.lastFontSettingsSaveSucceeded
            ? theme.colors.warning
            : theme.colors.textMuted);
    status->SetPosition(padding, startY + lineHeight * 3.0f);
    status->SetSize(rowWidth, lineHeight);
    context.contentContainer->AddChild(status);

    UI::Label::Ptr diagnostics = CreatePreferencesLabel(
        "NativePreferences.Font.Diagnostics",
        m_lastBuildStats.fontDiagnosticsText,
        theme,
        m_lastBuildStats.fontDiagnostics.hasPrimaryFont &&
                m_lastBuildStats.fontDiagnostics.missingCoreCodepointCount == 0u
            ? theme.colors.textMuted
            : theme.colors.warning);
    diagnostics->SetOverflowMode(UI::TextOverflowMode::EndEllipsis);
    diagnostics->SetTooltipText(m_lastBuildStats.fontDiagnosticsText);
    diagnostics->SetPosition(padding, startY + lineHeight * 4.0f);
    diagnostics->SetSize(rowWidth, lineHeight);
    context.contentContainer->AddChild(diagnostics);

    UI::Label::Ptr coverage = CreatePreferencesLabel(
        "NativePreferences.Font.Diagnostics.Coverage",
        m_lastBuildStats.fontDiagnosticsCoverageText,
        theme,
        m_lastBuildStats.fontDiagnostics.hasLatinText &&
                m_lastBuildStats.fontDiagnostics.hasEditorSymbols &&
                m_lastBuildStats.fontDiagnostics.hasCjkSample
            ? theme.colors.textMuted
            : theme.colors.warning);
    coverage->SetOverflowMode(UI::TextOverflowMode::EndEllipsis);
    coverage->SetTooltipText(m_lastBuildStats.fontDiagnosticsCoverageText);
    coverage->SetPosition(padding, startY + lineHeight * 5.0f);
    coverage->SetSize(rowWidth, lineHeight);
    context.contentContainer->AddChild(coverage);

    const float buttonGap = shellMetrics.compactGap;
    const bool canSaveFontSettings = m_settingsService != nullptr;
    const bool canToggleFallback = canSaveFontSettings && fontPathCount > 0u;
    float nextY = startY + lineHeight * 6.0f;

    if (m_settingsService && !m_settingsService->GetUIFontPaths().empty())
    {
        const std::vector<std::filesystem::path>& fontPaths =
            m_settingsService->GetUIFontPaths();
        const uint32 visiblePathCount =
            std::min<uint32>(static_cast<uint32>(fontPaths.size()), 3u);
        const float removeWidth = std::min(74.0f, std::max(56.0f,
                                                           rowWidth * 0.24f));
        const float labelWidth =
            std::max(0.0f, rowWidth - removeWidth - buttonGap);
        for (uint32 index = 0; index < visiblePathCount; ++index)
        {
            UI::Label::Ptr pathLabel = CreatePreferencesLabel(
                "NativePreferences.Font.Path." + std::to_string(index),
                FormatUIFontPathLabel(index,
                                      fontPaths[index],
                                      labelWidth,
                                      fontSize),
                theme,
                theme.colors.textMuted);
            pathLabel->SetOverflowMode(UI::TextOverflowMode::MiddleEllipsis);
            pathLabel->SetTooltipText(fontPaths[index].string());
            pathLabel->SetPosition(padding, nextY);
            pathLabel->SetSize(labelWidth, lineHeight);
            context.contentContainer->AddChild(pathLabel);

            UI::Button::Ptr removeButton = CreatePreferencesButton(
                "NativePreferences.Font.Remove." + std::to_string(index),
                "Remove",
                theme);
            removeButton->SetPosition(padding + labelWidth + buttonGap,
                                      nextY);
            removeButton->SetSize(removeWidth, rowHeight);
            removeButton->SetEnabled(canSaveFontSettings);
            removeButton->SetOnClick([this,
                                      host = context.host,
                                      index](const UI::UIEvent& event) {
                (void)event;
                RemoveUIFontPath(index);
                if (host)
                {
                    host->RequestPanelRebuild(
                        NativeEditorPreferencesPanel::PanelId(),
                        EditorUIPanelRebuildReason::Data);
                }
            });
            context.contentContainer->AddChild(removeButton);
            ++m_lastBuildStats.fontSettingsButtonCount;
            ++m_lastBuildStats.fontPathRowCount;
            nextY += rowHeight + buttonGap;
        }

        if (visiblePathCount < static_cast<uint32>(fontPaths.size()))
        {
            UI::Label::Ptr moreLabel = CreatePreferencesLabel(
                "NativePreferences.Font.Path.More",
                "+" +
                    std::to_string(static_cast<uint32>(fontPaths.size()) -
                                   visiblePathCount) +
                    " more font paths",
                theme,
                theme.colors.textMuted);
            moreLabel->SetPosition(padding, nextY);
            moreLabel->SetSize(rowWidth, lineHeight);
            context.contentContainer->AddChild(moreLabel);
            nextY += lineHeight;
        }
    }

    const float addWidth = std::max(92.0f, rowWidth * 0.48f);
    const float resetWidth =
        std::max(88.0f, rowWidth - addWidth - buttonGap);

    UI::Button::Ptr addFont = CreatePreferencesButton(
        "NativePreferences.Font.Add",
        "Add Font",
        theme);
    addFont->SetPosition(padding, nextY);
    addFont->SetSize(addWidth, rowHeight);
    addFont->SetEnabled(canSaveFontSettings && context.host != nullptr);
    addFont->SetOnClick([this, host = context.host](
                            const UI::UIEvent& event) {
        (void)event;
        OpenUIFontPicker(host);
        if (host)
        {
            host->RequestPanelRebuild(NativeEditorPreferencesPanel::PanelId(),
                                      EditorUIPanelRebuildReason::Data);
        }
    });
    context.contentContainer->AddChild(addFont);
    ++m_lastBuildStats.fontSettingsButtonCount;

    UI::Button::Ptr resetFonts = CreatePreferencesButton(
        "NativePreferences.Font.Reset",
        "Use System Fonts",
        theme);
    resetFonts->SetPosition(padding + addWidth + buttonGap, nextY);
    resetFonts->SetSize(resetWidth, rowHeight);
    resetFonts->SetEnabled(canSaveFontSettings);
    resetFonts->SetOnClick([this, host = context.host](
                               const UI::UIEvent& event) {
        (void)event;
        ResetUIFontSettings();
        if (host)
        {
            host->RequestPanelRebuild(NativeEditorPreferencesPanel::PanelId(),
                                      EditorUIPanelRebuildReason::Data);
        }
    });
    context.contentContainer->AddChild(resetFonts);
    ++m_lastBuildStats.fontSettingsButtonCount;

    nextY += rowHeight + buttonGap;

    UI::Button::Ptr toggleFallback = CreatePreferencesButton(
        "NativePreferences.Font.SystemFallback.Toggle",
        appendDefaultSystemFonts ? "Disable Fallback" : "Enable Fallback",
        theme);
    toggleFallback->SetPosition(padding, nextY);
    toggleFallback->SetSize(rowWidth, rowHeight);
    toggleFallback->SetEnabled(canToggleFallback);
    toggleFallback->SetOnClick([this,
                                host = context.host,
                                enabled = !appendDefaultSystemFonts](
                                   const UI::UIEvent& event) {
        (void)event;
        ToggleAppendDefaultSystemFonts(enabled);
        if (host)
        {
            host->RequestPanelRebuild(NativeEditorPreferencesPanel::PanelId(),
                                      EditorUIPanelRebuildReason::Data);
        }
    });
    context.contentContainer->AddChild(toggleFallback);
    ++m_lastBuildStats.fontSettingsButtonCount;

    return nextY + rowHeight + padding * 2.0f;
}

float NativeEditorPreferencesPanel::AddShortcutProfileRows(
    EditorUIPanelFrameContext& context,
    float startY)
{
    const UI::UITheme& theme = context.ui->GetTheme();
    const EditorShellMetrics shellMetrics = EditorShellMetricPolicy::Resolve(theme);
    const float padding = shellMetrics.contentPadding;
    const float lineHeight = shellMetrics.formLineHeight;
    const float rowHeight = shellMetrics.formRowHeight;
    const float rowWidth =
        std::max(0.0f, context.contentContainer->GetWidth() - padding * 2.0f);
    const bool canUseProfile =
        m_lastBuildStats.hasCommandRegistry &&
        !m_lastBuildStats.shortcutProfilePath.empty();

    UI::Label::Ptr header = CreatePreferencesLabel(
        "NativePreferences.Shortcuts.Header",
        "Shortcut Profile",
        theme,
        theme.colors.text);
    header->SetPosition(padding, startY);
    header->SetSize(rowWidth, lineHeight);
    context.contentContainer->AddChild(header);

    UI::Label::Ptr path = CreatePreferencesLabel(
        "NativePreferences.Shortcuts.Path",
        "Profile: " + m_lastBuildStats.shortcutProfilePath.string(),
        theme,
        theme.colors.textMuted);
    path->SetOverflowMode(UI::TextOverflowMode::MiddleEllipsis);
    path->SetTooltipText(m_lastBuildStats.shortcutProfilePath.string());
    path->SetPosition(padding, startY + lineHeight);
    path->SetSize(rowWidth, lineHeight);
    context.contentContainer->AddChild(path);

    UI::Label::Ptr status = CreatePreferencesLabel(
        "NativePreferences.Shortcuts.Status",
        m_lastBuildStats.shortcutStatusText,
        theme,
        ShortcutStatusColor(m_lastBuildStats, theme));
    status->SetPosition(padding, startY + lineHeight * 2.0f);
    status->SetSize(rowWidth, lineHeight);
    context.contentContainer->AddChild(status);

    UI::Label::Ptr conflicts = CreatePreferencesLabel(
        "NativePreferences.Shortcuts.Conflicts",
        "Conflicts: " + std::to_string(m_lastBuildStats.shortcutConflictCount),
        theme,
        m_lastBuildStats.shortcutConflictCount > 0u ? theme.colors.warning
                                                     : theme.colors.textMuted);
    conflicts->SetPosition(padding, startY + lineHeight * 3.0f);
    conflicts->SetSize(rowWidth, lineHeight);
    context.contentContainer->AddChild(conflicts);

    UI::Label::Ptr autosaveStatus = CreatePreferencesLabel(
        "NativePreferences.Shortcuts.Autosave.Status",
        m_lastBuildStats.shortcutAutosaveStatusText,
        theme,
        m_lastBuildStats.shortcutProfileAutosaveEnabled
            ? theme.colors.accent
            : (m_lastBuildStats.shortcutProfileDirty ? theme.colors.warning
                                                     : theme.colors.textMuted));
    autosaveStatus->SetPosition(padding, startY + lineHeight * 4.0f);
    autosaveStatus->SetSize(rowWidth, lineHeight);
    context.contentContainer->AddChild(autosaveStatus);

    const float buttonY = startY + lineHeight * 5.0f;
    const float buttonGap = shellMetrics.compactGap;
    const float buttonWidth = std::max(
        68.0f,
        std::max(EditorTypography::EstimatePaddedTextWidth(
                     "Export",
                     theme,
                     EditorTypographyRole::Control,
                     58.0f,
                     96.0f),
                 EditorTypography::EstimatePaddedTextWidth(
                     "Import",
                     theme,
                     EditorTypographyRole::Control,
                     58.0f,
                     96.0f)));
    UI::Button::Ptr exportButton = CreatePreferencesButton(
        "NativePreferences.Shortcuts.Export",
        "Export",
        theme);
    exportButton->SetPosition(padding, buttonY);
    exportButton->SetSize(buttonWidth, rowHeight);
    exportButton->SetEnabled(canUseProfile);
    exportButton->SetOnClick([this, host = context.host](
                                 const UI::UIEvent& event) {
        (void)event;
        ExportShortcutProfile(host);
        if (host)
        {
            host->RequestPanelRebuild(NativeEditorPreferencesPanel::PanelId(),
                                      EditorUIPanelRebuildReason::Data);
        }
    });
    context.contentContainer->AddChild(exportButton);
    ++m_lastBuildStats.shortcutExportButtonCount;

    UI::Button::Ptr importButton = CreatePreferencesButton(
        "NativePreferences.Shortcuts.Import",
        "Import",
        theme);
    importButton->SetPosition(padding + buttonWidth + buttonGap, buttonY);
    importButton->SetSize(buttonWidth, rowHeight);
    importButton->SetEnabled(canUseProfile);
    importButton->SetOnClick([this, host = context.host](
                                 const UI::UIEvent& event) {
        (void)event;
        ImportShortcutProfile(host);
        if (host)
        {
            host->RequestPanelRebuild(NativeEditorPreferencesPanel::PanelId(),
                                      EditorUIPanelRebuildReason::Data);
        }
    });
    context.contentContainer->AddChild(importButton);
    ++m_lastBuildStats.shortcutImportButtonCount;

    const bool autosaveToggleTarget =
        !m_lastBuildStats.shortcutProfileAutosaveEnabled;
    UI::Button::Ptr autosaveToggle = CreatePreferencesButton(
        "NativePreferences.Shortcuts.Autosave.Toggle",
        autosaveToggleTarget ? "Enable Autosave" : "Disable Autosave",
        theme);
    const float autosaveX = padding + (buttonWidth + buttonGap) * 2.0f;
    autosaveToggle->SetPosition(autosaveX, buttonY);
    autosaveToggle->SetSize(std::max(70.0f, rowWidth - (autosaveX - padding)),
                            rowHeight);
    autosaveToggle->SetEnabled(m_settingsService != nullptr &&
                               m_shortcutProfileService != nullptr);
    autosaveToggle->SetOnClick([this,
                                host = context.host,
                                enabled = autosaveToggleTarget](
                                   const UI::UIEvent& event) {
        (void)event;
        ToggleShortcutAutosaveEnabled(enabled);
        if (host)
        {
            host->RequestPanelRebuild(NativeEditorPreferencesPanel::PanelId(),
                                      EditorUIPanelRebuildReason::Data);
        }
    });
    context.contentContainer->AddChild(autosaveToggle);
    ++m_lastBuildStats.shortcutAutosaveToggleButtonCount;

    const float bindingHeaderY = buttonY + rowHeight + padding;
    UI::Label::Ptr bindingHeader = CreatePreferencesLabel(
        "NativePreferences.ShortcutBindings.Header",
        "Shortcut Bindings",
        theme,
        theme.colors.text);
    bindingHeader->SetPosition(padding, bindingHeaderY);
    bindingHeader->SetSize(rowWidth, lineHeight);
    context.contentContainer->AddChild(bindingHeader);

    UI::Label::Ptr bindingStatus = CreatePreferencesLabel(
        "NativePreferences.ShortcutBindings.Status",
        m_lastBuildStats.shortcutBindingStatusText,
        theme,
        (m_lastBuildStats.lastShortcutBindingSucceeded ||
         m_lastBuildStats.lastShortcutBindingCleared)
            ? theme.colors.accent
            : (m_lastBuildStats.lastShortcutBindingConflictCount > 0u
                   ? theme.colors.warning
                   : theme.colors.textMuted));
    bindingStatus->SetPosition(padding, bindingHeaderY + lineHeight);
    bindingStatus->SetSize(rowWidth, lineHeight);
    context.contentContainer->AddChild(bindingStatus);

    const float listY = bindingHeaderY + lineHeight * 2.0f;
    const float slotButtonHeight =
        std::max(22.0f, shellMetrics.formRowHeight - shellMetrics.compactGap);
    const float bindingRowHeight =
        std::max(98.0f, lineHeight + slotButtonHeight * 3.0f + 16.0f);
    const float listHeight = bindingRowHeight * 3.0f;
    UI::ScrollView::Ptr bindingViewport = UI::ScrollView::Create();
    bindingViewport->SetName("NativePreferences.ShortcutBindings.Viewport");
    bindingViewport->SetPosition(padding, listY);
    bindingViewport->SetSize(rowWidth, listHeight);
    bindingViewport->SetBackgroundColor(UI::UIColor::Transparent());
    bindingViewport->SetBorderWidth(0.0f);
    bindingViewport->SetScrollbarNamePrefix(
        "NativePreferences.ShortcutBindings.Scrollbar");
    bindingViewport->SetWheelStep(bindingRowHeight * 3.0f);

    const std::vector<EditorShortcutBindingRow>& bindingRows =
        m_shortcutBindingModel.GetRows();
    bindingViewport->SetContentSize(
        rowWidth,
        std::max(listHeight,
                 static_cast<float>(bindingRows.size()) * bindingRowHeight));

    if (bindingRows.empty())
    {
        UI::Label::Ptr empty = CreatePreferencesLabel(
            "NativePreferences.ShortcutBindings.Empty",
            "No commands registered.",
            theme,
            theme.colors.textMuted);
        empty->SetPosition(8.0f, 0.0f);
        empty->SetSize(rowWidth - 16.0f, bindingRowHeight);
        bindingViewport->AddContentChild(empty);
    }

    const float clearWidth = 44.0f;
    const float slotGap = 5.0f;
    const float slotButtonWidth =
        std::max(64.0f, rowWidth - clearWidth - slotGap - 20.0f);
    const float clearX = 8.0f + slotButtonWidth + slotGap;
    const float categoryWidth = 62.0f;
    const float conflictWidth = 58.0f;
    const float nameWidth =
        std::max(42.0f,
                 rowWidth - categoryWidth - conflictWidth - 24.0f);
    for (size_t index = 0; index < bindingRows.size(); ++index)
    {
        const EditorShortcutBindingRow& binding = bindingRows[index];
        const std::string token =
            EditorShortcutBindingModel::FormatCommandWidgetToken(
                binding.commandId);
        const std::string rowName =
            "NativePreferences.ShortcutBindings.Row." + token;

        UI::Panel::Ptr row = UI::Panel::Create();
        row->SetName(rowName);
        row->SetPosition(0.0f, static_cast<float>(index) * bindingRowHeight);
        row->SetSize(rowWidth, bindingRowHeight);
        row->SetBackgroundColor(index % 2u == 0u
                                    ? theme.colors.panelBackground
                                    : theme.colors.windowBackground.WithAlpha(0.35f));
        row->SetBorderWidth(0.0f);
        row->SetInteractive(true);

        float x = 8.0f;
        UI::Label::Ptr category = CreatePreferencesLabel(
            rowName + ".Category",
            binding.category.empty() ? "General" : binding.category,
            theme,
            theme.colors.textMuted);
        category->SetPosition(x, 0.0f);
        category->SetSize(categoryWidth, lineHeight);
        row->AddChild(category);
        x += categoryWidth;

        UI::Label::Ptr name = CreatePreferencesLabel(
            rowName + ".Name",
            FitTextToWidth(binding.displayName,
                           nameWidth,
                           EditorTypography::GetFontSize(
                               theme,
                               EditorTypographyRole::PropertyLabel)),
            theme,
            theme.colors.text);
        name->SetPosition(x, 0.0f);
        name->SetSize(nameWidth, lineHeight);
        row->AddChild(name);
        x += nameWidth;

        UI::Label::Ptr conflict = CreatePreferencesLabel(
            rowName + ".Conflict",
            binding.conflictCount > 0u
                ? ("Conflict " + std::to_string(binding.conflictCount))
                : "OK",
            theme,
            binding.conflictCount > 0u ? theme.colors.warning
                                       : theme.colors.textMuted);
        conflict->SetPosition(x, 0.0f);
        conflict->SetSize(conflictWidth, lineHeight);
        row->AddChild(conflict);

        auto addSlotControls = [&](const std::string& suffix,
                                   const char* prefix,
                                   EditorShortcutBindingSlot slot,
                                   const std::string& shortcutText,
                                   bool hasShortcut,
                                   bool captureTarget,
                                   float y) {
            UI::Button::Ptr capture = CreatePreferencesButton(
                "NativePreferences.ShortcutBindings." + suffix + "." + token,
                ShortcutSlotButtonText(prefix,
                                       shortcutText,
                                       captureTarget,
                                       slotButtonWidth,
                                       EditorTypography::GetFontSize(
                                           theme,
                                           EditorTypographyRole::Control)),
                theme);
            capture->SetPosition(8.0f, y);
            capture->SetSize(slotButtonWidth, slotButtonHeight);
            capture->SetEnabled(m_lastBuildStats.hasCommandRegistry);
            capture->SetOnClick([this,
                                 host = context.host,
                                 commandId = binding.commandId,
                                 slot](const UI::UIEvent& event) {
                (void)event;
                m_shortcutCaptureAttempted = true;
                if (m_shortcutBindingModel.BeginCapture(commandId, slot))
                {
                    m_lastShortcutBindingSucceeded = false;
                    m_lastShortcutBindingCleared = false;
                    m_lastShortcutBindingConflictCount = 0;
                    m_shortcutBindingStatusText =
                        "Press a key for " + commandId + " " +
                        EditorShortcutBindingModel::FormatSlotLabel(slot) +
                        ". Escape cancels.";
                }
                if (host)
                {
                    host->RequestPanelRebuild(
                        NativeEditorPreferencesPanel::PanelId(),
                        EditorUIPanelRebuildReason::Data);
                }
            });
            row->AddChild(capture);
            ++m_lastBuildStats.shortcutBindingButtonCount;

            UI::Button::Ptr clear = CreatePreferencesButton(
                "NativePreferences.ShortcutBindings.Clear" + suffix + "." +
                    token,
                "Clear",
                theme);
            clear->SetPosition(clearX, y);
            clear->SetSize(clearWidth, slotButtonHeight);
            clear->SetEnabled(m_lastBuildStats.hasCommandRegistry &&
                              hasShortcut);
            clear->SetOnClick([this,
                               host = context.host,
                               commandId = binding.commandId,
                               slot](const UI::UIEvent& event) {
                (void)event;
                if (!host)
                {
                    return;
                }

                EditorShortcutBindingApplyResult result =
                    m_shortcutBindingModel.ClearShortcut(
                        commandId,
                        slot,
                        host->GetCommandRegistry());
                m_lastShortcutBindingSucceeded = result.Succeeded();
                m_lastShortcutBindingCleared = result.Cleared();
                m_lastShortcutBindingConflictCount = result.conflictCount;
                m_shortcutBindingStatusText = result.statusText;
                if (result.Succeeded() || result.Cleared())
                {
                    MarkShortcutProfileDirty();
                }
                host->RequestPanelRebuild(
                    NativeEditorPreferencesPanel::PanelId(),
                    EditorUIPanelRebuildReason::Data);
            });
            row->AddChild(clear);
            ++m_lastBuildStats.shortcutBindingClearButtonCount;
        };

        const float slotY = lineHeight + 4.0f;
        addSlotControls("Primary",
                        "P",
                        EditorShortcutBindingSlot::Primary(),
                        binding.primaryShortcutText,
                        binding.hasPrimaryShortcut,
                        binding.isPrimaryCaptureTarget,
                        slotY);
        for (uint32 slotIndex = 0;
             slotIndex < RVX_EDITOR_SHORTCUT_BINDING_SECONDARY_SLOT_COUNT;
             ++slotIndex)
        {
            addSlotControls(
                "Secondary" + std::to_string(slotIndex),
                slotIndex == 0u ? "S1" : "S2",
                EditorShortcutBindingSlot::Secondary(slotIndex),
                binding.secondaryShortcutTexts[slotIndex],
                binding.hasSecondaryShortcuts[slotIndex],
                binding.isSecondaryCaptureTarget[slotIndex],
                slotY + (slotButtonHeight + 4.0f) *
                            static_cast<float>(slotIndex + 1u));
        }

        bindingViewport->AddContentChild(row);
    }

    context.contentContainer->AddChild(bindingViewport);
    return listY + listHeight + padding;
}

float NativeEditorPreferencesPanel::AddBackendRows(
    EditorUIPanelFrameContext& context,
    float startY)
{
    const UI::UITheme& theme = context.ui->GetTheme();
    const EditorShellMetrics shellMetrics = EditorShellMetricPolicy::Resolve(theme);
    const float padding = shellMetrics.contentPadding;
    const float lineHeight = shellMetrics.formLineHeight;
    const float rowHeight = shellMetrics.formRowHeight;
    const float rowWidth =
        std::max(0.0f, context.contentContainer->GetWidth() - padding * 2.0f);
    const char* currentBackendName = ToString(m_lastBuildStats.currentBackendType);
    UI::Label::Ptr current = CreatePreferencesLabel(
        "NativePreferences.Backend.Current.Value",
        std::string("UI backend: ") + currentBackendName,
        theme,
        m_lastBuildStats.currentBackendFound ? theme.colors.accent
                                             : theme.colors.warning);
    current->SetPosition(padding, startY);
    current->SetSize(rowWidth, lineHeight);
    context.contentContainer->AddChild(current);

    const std::string settingsPath =
        m_settingsService ? m_settingsService->GetSettingsPath().string()
                          : std::string("Unavailable");
    UI::Label::Ptr path = CreatePreferencesLabel(
        "NativePreferences.Settings.Path",
        "Settings: " + settingsPath,
        theme,
        theme.colors.textMuted);
    path->SetPosition(padding, startY + lineHeight);
    path->SetSize(rowWidth, lineHeight);
    context.contentContainer->AddChild(path);

    UI::Label::Ptr hint = CreatePreferencesLabel(
        "NativePreferences.Backend.RestartHint",
        "Backend changes are applied on next launch.",
        theme,
        theme.colors.textMuted);
    hint->SetPosition(padding, startY + lineHeight * 2.0f);
    hint->SetSize(rowWidth, lineHeight);
    context.contentContainer->AddChild(hint);

    const float listY = startY + lineHeight * 3.0f + padding;
    const std::span<const EditorUIBackendDescriptor> backends =
        GetEditorUIBackendCatalog();
    m_lastBuildStats.rowCount = static_cast<uint32>(backends.size());
    const float visibleRows =
        std::max(1.0f,
                 std::min(4.0f, static_cast<float>(backends.size())));
    const float listHeight = rowHeight * visibleRows;

    if (backends.empty())
    {
        UI::Label::Ptr empty = CreatePreferencesLabel(
            "NativePreferences.Backend.Empty",
            "No editor UI backends registered.",
            theme,
            theme.colors.textMuted);
        empty->SetPosition(padding, listY);
        empty->SetSize(rowWidth, rowHeight);
        context.contentContainer->AddChild(empty);
        return listY + rowHeight + padding;
    }

    UI::ScrollView::Ptr viewport = UI::ScrollView::Create();
    viewport->SetName("NativePreferences.Backend.Viewport");
    viewport->SetPosition(padding, listY);
    viewport->SetSize(rowWidth, listHeight);
    viewport->SetBackgroundColor(UI::UIColor::Transparent());
    viewport->SetBorderWidth(0.0f);
    viewport->SetScrollbarNamePrefix("NativePreferences.Backend.Scrollbar");
    viewport->SetWheelStep(rowHeight * 3.0f);
    viewport->SetContentSize(rowWidth,
                             static_cast<float>(backends.size()) * rowHeight);

    const float actionWidth = 72.0f;
    const float statusWidth = 86.0f;
    const float configWidth = 78.0f;
    const float nameWidth = 96.0f;
    const float descriptionWidth =
        std::max(120.0f,
                 rowWidth - actionWidth - statusWidth - configWidth -
                     nameWidth - 44.0f);

    for (size_t index = 0; index < backends.size(); ++index)
    {
        const EditorUIBackendDescriptor& backend = backends[index];
        const bool active = backend.type == m_lastBuildStats.currentBackendType;
        const std::string configName(backend.configName);
        const std::string rowName = "NativePreferences.Backend.Row." + configName;

        UI::Panel::Ptr row = UI::Panel::Create();
        row->SetName(rowName);
        row->SetPosition(0.0f, static_cast<float>(index) * rowHeight);
        row->SetSize(rowWidth, rowHeight);
        row->SetBackgroundColor(index % 2u == 0u
                                    ? theme.colors.panelBackground
                                    : theme.colors.windowBackground.WithAlpha(0.35f));
        row->SetBorderWidth(0.0f);
        row->SetInteractive(true);

        float x = shellMetrics.contentPadding;
        UI::Button::Ptr select = CreatePreferencesButton(
            "NativePreferences.Backend.Select." + configName,
            active ? "Active" : "Select",
            theme);
        select->SetPosition(x, shellMetrics.compactGap * 0.5f);
        select->SetSize(actionWidth - shellMetrics.contentPadding,
                        std::max(1.0f, rowHeight - shellMetrics.compactGap));
        select->SetEnabled(m_settingsService && backend.canCreate && !active);
        select->SetOnClick([this, host = context.host, type = backend.type](
                               const UI::UIEvent& event) {
            (void)event;
            if (SelectBackend(type) && host)
            {
                host->RequestPanelRebuild(NativeEditorPreferencesPanel::PanelId(),
                                          EditorUIPanelRebuildReason::Data);
            }
        });
        row->AddChild(select);
        ++m_lastBuildStats.selectButtonCount;
        x += actionWidth;

        UI::Label::Ptr status = CreatePreferencesLabel(
            rowName + ".Status",
            BackendStatusLabel(backend, m_lastBuildStats.currentBackendType),
            theme,
            BackendStatusColor(backend,
                               m_lastBuildStats.currentBackendType,
                               theme));
        status->SetPosition(x, 0.0f);
        status->SetSize(statusWidth, rowHeight);
        row->AddChild(status);
        x += statusWidth;

        UI::Label::Ptr name = CreatePreferencesLabel(rowName + ".Name",
                                                     std::string(backend.displayName),
                                                     theme,
                                                     theme.colors.text);
        name->SetPosition(x, 0.0f);
        name->SetSize(nameWidth, rowHeight);
        row->AddChild(name);
        x += nameWidth;

        UI::Label::Ptr config = CreatePreferencesLabel(rowName + ".Config",
                                                       configName,
                                                       theme,
                                                       theme.colors.textMuted);
        config->SetPosition(x, 0.0f);
        config->SetSize(configWidth, rowHeight);
        row->AddChild(config);
        x += configWidth;

        UI::Label::Ptr description = CreatePreferencesLabel(
            rowName + ".Description",
            std::string(backend.description),
            theme,
            theme.colors.textMuted);
        description->SetPosition(x, 0.0f);
        description->SetSize(descriptionWidth, rowHeight);
        row->AddChild(description);

        viewport->AddContentChild(row);
    }

    context.contentContainer->AddChild(viewport);
    return listY + listHeight + padding;
}

void NativeEditorPreferencesPanel::RefreshShortcutStats(
    EditorUIPanelFrameContext& context)
{
    m_lastBuildStats.hasCommandRegistry = context.host != nullptr;
    m_lastBuildStats.shortcutProfilePath = ResolveShortcutProfilePath();
    m_lastBuildStats.shortcutExportAttempted = m_shortcutExportAttempted;
    m_lastBuildStats.shortcutImportAttempted = m_shortcutImportAttempted;
    m_lastBuildStats.lastShortcutExportSucceeded =
        m_lastShortcutExportSucceeded;
    m_lastBuildStats.lastShortcutImportSucceeded =
        m_lastShortcutImportSucceeded;
    m_lastBuildStats.shortcutAutosaveSettingsSaveAttempted =
        m_shortcutAutosaveSettingsSaveAttempted;
    m_lastBuildStats.lastShortcutAutosaveSettingsSaveSucceeded =
        m_lastShortcutAutosaveSettingsSaveSucceeded;
    m_lastBuildStats.lastShortcutApplyCount = m_lastShortcutApplyCount;
    m_lastBuildStats.lastShortcutMissingCommandCount =
        m_lastShortcutMissingCommandCount;
    m_lastBuildStats.lastShortcutImportConflictCount =
        m_lastShortcutImportConflictCount;
    if (m_shortcutProfileService)
    {
        const EditorShortcutProfileAutosaveState& autosaveState =
            m_shortcutProfileService->GetAutosaveState();
        m_lastBuildStats.shortcutProfileDirty = autosaveState.dirty;
        m_lastBuildStats.shortcutProfileAutosaveEnabled =
            autosaveState.enabled;
        m_lastBuildStats.shortcutProfileAutosaveDebounceSeconds =
            autosaveState.debounceSeconds;
        m_lastBuildStats.shortcutProfileDirtyGeneration =
            autosaveState.dirtyGeneration;
        m_lastBuildStats.shortcutProfileSavedGeneration =
            autosaveState.savedGeneration;
    }
    m_lastBuildStats.shortcutAutosaveStatusText =
        m_shortcutAutosaveSettingsSaveAttempted &&
                !m_lastShortcutAutosaveSettingsSaveSucceeded
            ? m_shortcutAutosaveStatusText
            : FormatShortcutAutosaveStatus(m_lastBuildStats);
    m_lastBuildStats.shortcutStatusText = m_shortcutStatusText;

    if (!context.host)
    {
        m_lastBuildStats.shortcutBindingStatusText =
            m_shortcutBindingStatusText;
        return;
    }

    EditorCommandRegistry& registry = context.host->GetCommandRegistry();
    m_shortcutBindingModel.Rebuild(registry);

    m_lastBuildStats.shortcutCommandCount =
        static_cast<uint32>(registry.GetCommandCount());
    m_lastBuildStats.shortcutConflictCount =
        static_cast<uint32>(registry.GetShortcutConflicts().size());
    m_lastBuildStats.shortcutBindingRowCount =
        static_cast<uint32>(m_shortcutBindingModel.GetRows().size());
    m_lastBuildStats.shortcutBindingConflictRowCount =
        m_shortcutBindingModel.GetConflictRowCount();
    m_lastBuildStats.shortcutCaptureActive =
        m_shortcutBindingModel.IsCapturing();
    m_lastBuildStats.shortcutCaptureAttempted = m_shortcutCaptureAttempted;
    m_lastBuildStats.lastShortcutBindingSucceeded =
        m_lastShortcutBindingSucceeded;
    m_lastBuildStats.lastShortcutBindingCleared =
        m_lastShortcutBindingCleared;
    m_lastBuildStats.lastShortcutBindingConflictCount =
        m_lastShortcutBindingConflictCount;
    m_lastBuildStats.shortcutCaptureCommandId =
        m_shortcutBindingModel.GetCaptureCommandId();
    m_lastBuildStats.shortcutCaptureSlotText =
        EditorShortcutBindingModel::FormatSlotLabel(
            m_shortcutBindingModel.GetCaptureSlot());
    m_lastBuildStats.shortcutBindingStatusText =
        m_shortcutBindingStatusText;
}

void NativeEditorPreferencesPanel::ProcessShortcutBindingCapture(
    EditorUIPanelFrameContext& context)
{
    if (!context.host || !context.input ||
        !m_shortcutBindingModel.IsCapturing())
    {
        return;
    }

    EditorShortcutBindingApplyResult result =
        m_shortcutBindingModel.CaptureFromInput(*context.input,
                                                context.host->GetCommandRegistry());
    if (result.status == EditorShortcutBindingApplyStatus::None ||
        result.status == EditorShortcutBindingApplyStatus::WaitingForKey)
    {
        m_shortcutBindingStatusText = result.statusText.empty()
                                          ? m_shortcutBindingStatusText
                                          : result.statusText;
        return;
    }

    m_lastShortcutBindingSucceeded = result.Succeeded();
    m_lastShortcutBindingCleared = result.Cleared();
    m_lastShortcutBindingConflictCount = result.conflictCount;
    m_shortcutBindingStatusText = result.statusText;
    if (result.Succeeded() || result.Cleared())
    {
        MarkShortcutProfileDirty();
    }
}

bool NativeEditorPreferencesPanel::ExportShortcutProfile(EditorUIHost* host)
{
    m_shortcutExportAttempted = true;
    m_lastShortcutExportSucceeded = false;

    if (!host || !m_shortcutProfileService)
    {
        m_shortcutStatusText =
            "Shortcut export failed: command registry unavailable.";
        return false;
    }

    const EditorShortcutProfileOperationResult result =
        m_shortcutProfileService->SaveCurrentProfile(
            host->GetCommandRegistry());
    m_lastShortcutExportSucceeded = result.Succeeded();
    m_shortcutStatusText = result.statusText;
    return result.Succeeded();
}

bool NativeEditorPreferencesPanel::ImportShortcutProfile(EditorUIHost* host)
{
    m_shortcutImportAttempted = true;
    m_lastShortcutImportSucceeded = false;
    m_lastShortcutApplyCount = 0;
    m_lastShortcutMissingCommandCount = 0;
    m_lastShortcutImportConflictCount = 0;

    if (!host || !m_shortcutProfileService)
    {
        m_shortcutStatusText =
            "Shortcut import failed: command registry unavailable.";
        return false;
    }

    const EditorShortcutProfileOperationResult result =
        m_shortcutProfileService->LoadAndApplyProfile(
            host->GetCommandRegistry());
    m_lastShortcutApplyCount = result.appliedCount;
    m_lastShortcutMissingCommandCount = result.missingCommandCount;
    m_lastShortcutImportConflictCount =
        result.conflictCount;
    m_lastShortcutImportSucceeded = result.Succeeded();
    m_shortcutStatusText = result.statusText;
    return result.Succeeded();
}

void NativeEditorPreferencesPanel::MarkShortcutProfileDirty()
{
    if (!m_shortcutProfileService)
    {
        return;
    }

    m_shortcutProfileService->MarkProfileDirty();
    m_shortcutStatusText = "Shortcut profile has unsaved changes.";
}

void NativeEditorPreferencesPanel::ApplyShortcutAutosaveSettings()
{
    if (!m_settingsService || !m_shortcutProfileService)
    {
        return;
    }

    m_shortcutProfileService->SetAutosaveEnabled(
        m_settingsService->IsShortcutProfileAutosaveEnabled());
    m_shortcutProfileService->SetAutosaveDebounceSeconds(
        m_settingsService->GetShortcutProfileAutosaveDebounceSeconds());
}

bool NativeEditorPreferencesPanel::ToggleShortcutAutosaveEnabled(bool enabled)
{
    m_shortcutAutosaveSettingsSaveAttempted = true;
    m_lastShortcutAutosaveSettingsSaveSucceeded = false;

    if (!m_settingsService || !m_shortcutProfileService)
    {
        m_shortcutAutosaveStatusText =
            "Shortcut autosave setting failed: settings unavailable.";
        return false;
    }

    const bool previousEnabled =
        m_settingsService->IsShortcutProfileAutosaveEnabled();
    m_settingsService->SetShortcutProfileAutosaveEnabled(enabled);
    if (!m_settingsService->Save())
    {
        m_shortcutAutosaveStatusText =
            "Shortcut autosave setting failed: " +
            m_settingsService->GetLastError();
        m_settingsService->SetShortcutProfileAutosaveEnabled(previousEnabled);
        return false;
    }

    m_shortcutProfileService->SetAutosaveEnabled(enabled);
    m_lastShortcutAutosaveSettingsSaveSucceeded = true;
    m_shortcutAutosaveStatusText =
        enabled ? "Shortcut autosave enabled."
                : "Shortcut autosave disabled.";
    return true;
}

std::filesystem::path NativeEditorPreferencesPanel::ResolveShortcutProfilePath()
    const
{
    if (m_shortcutProfileService)
    {
        return m_shortcutProfileService->ResolveProfilePath();
    }

    const std::filesystem::path settingsPath =
        m_settingsService ? m_settingsService->GetSettingsPath()
                          : EditorSettingsService::ResolveDefaultSettingsPath();
    if (settingsPath.has_parent_path())
    {
        return settingsPath.parent_path() / "EditorShortcuts.shortcuts";
    }

    return std::filesystem::path("EditorShortcuts.shortcuts");
}

bool NativeEditorPreferencesPanel::SelectBackend(EditorUIBackendType type)
{
    m_lastBuildStats.saveAttempted = true;
    m_lastBuildStats.lastSaveSucceeded = false;

    if (!m_settingsService)
    {
        return false;
    }

    const EditorUIBackendDescriptor* descriptor =
        FindEditorUIBackendDescriptor(type);
    if (!descriptor || !descriptor->canCreate)
    {
        return false;
    }

    m_settingsService->SetUIBackendType(type);
    m_lastBuildStats.lastSaveSucceeded = m_settingsService->Save();
    return m_lastBuildStats.lastSaveSucceeded;
}

bool NativeEditorPreferencesPanel::AdjustUIScaleFactor(float delta)
{
    m_uiScaleSaveAttempted = true;
    m_lastUIScaleSaveSucceeded = false;

    if (!m_settingsService)
    {
        m_uiScaleStatusText = "UI scale setting failed: settings unavailable.";
        return false;
    }

    const float previousScale = m_settingsService->GetUIScaleFactor();
    m_settingsService->SetUIScaleFactor(previousScale + delta);
    const float savedScale = m_settingsService->GetUIScaleFactor();
    if (!m_settingsService->Save())
    {
        m_uiScaleStatusText =
            "UI scale setting failed: " + m_settingsService->GetLastError();
        m_settingsService->SetUIScaleFactor(previousScale);
        return false;
    }

    m_lastUIScaleSaveSucceeded = true;
    m_uiScaleStatusText =
        "UI scale saved: " + FormatUIScalePercent(savedScale);
    return true;
}

bool NativeEditorPreferencesPanel::ResetUIScaleFactor()
{
    m_uiScaleSaveAttempted = true;
    m_lastUIScaleSaveSucceeded = false;

    if (!m_settingsService)
    {
        m_uiScaleStatusText = "UI scale reset failed: settings unavailable.";
        return false;
    }

    const float previousScale = m_settingsService->GetUIScaleFactor();
    m_settingsService->SetUIScaleFactor(RVX_EDITOR_DEFAULT_UI_SCALE_FACTOR);
    const float savedScale = m_settingsService->GetUIScaleFactor();
    if (!m_settingsService->Save())
    {
        m_uiScaleStatusText =
            "UI scale reset failed: " + m_settingsService->GetLastError();
        m_settingsService->SetUIScaleFactor(previousScale);
        return false;
    }

    m_lastUIScaleSaveSucceeded = true;
    m_uiScaleStatusText =
        "UI scale reset: " + FormatUIScalePercent(savedScale);
    return true;
}

bool NativeEditorPreferencesPanel::OpenUIFontPicker(EditorUIHost* host)
{
    if (!host)
    {
        m_fontSettingsSaveAttempted = true;
        m_lastFontSettingsSaveSucceeded = false;
        m_fontStatusText = "Font picker failed: UI host unavailable.";
        return false;
    }

    if (!m_settingsService)
    {
        m_fontSettingsSaveAttempted = true;
        m_lastFontSettingsSaveSucceeded = false;
        m_fontStatusText = "Font picker failed: settings unavailable.";
        return false;
    }

    EditorFilePickerDialogDesc dialog;
    dialog.id = "EditorFont";
    dialog.title = "Add Editor Font";
    dialog.acceptButtonText = "Add";
    dialog.picker.mode = EditorFilePickerMode::OpenFile;
    dialog.picker.filters.push_back(
        {"Font Files (*.ttf; *.ttc; *.otf)", {"*.ttf", "*.ttc", "*.otf"}});
    dialog.picker.filters.push_back({"All Files (*.*)", {"*.*"}});

    auto addRecentDirectory = [&dialog](const std::filesystem::path& path) {
        if (path.empty())
        {
            return;
        }
        const std::filesystem::path normalized = path.lexically_normal();
        const auto existing = std::find(dialog.picker.recentDirectories.begin(),
                                        dialog.picker.recentDirectories.end(),
                                        normalized);
        if (existing == dialog.picker.recentDirectories.end())
        {
            dialog.picker.recentDirectories.push_back(normalized);
        }
    };

    const std::vector<std::filesystem::path>& fontPaths =
        m_settingsService->GetUIFontPaths();
    for (const std::filesystem::path& fontPath : fontPaths)
    {
        addRecentDirectory(fontPath.parent_path());
    }
    if (m_settingsService->GetSettingsPath().has_parent_path())
    {
        addRecentDirectory(m_settingsService->GetSettingsPath().parent_path());
    }

    if (!dialog.picker.recentDirectories.empty())
    {
        dialog.picker.initialDirectory = dialog.picker.recentDirectories.front();
    }

    dialog.onResult = [this](const EditorFilePickerDialogResult& result,
                             EditorUIHost& callbackHost) {
        HandleUIFontPickerResult(result, callbackHost);
    };

    m_fontStatusText = "Select a .ttf, .ttc, or .otf font file.";
    host->OpenFilePickerDialog(std::move(dialog));
    return true;
}

void NativeEditorPreferencesPanel::HandleUIFontPickerResult(
    const EditorFilePickerDialogResult& result,
    EditorUIHost& host)
{
    if (!result.accepted)
    {
        m_fontStatusText = result.error.empty() ? "Font selection cancelled."
                                                : result.error;
        host.RequestPanelRebuild(NativeEditorPreferencesPanel::PanelId(),
                                 EditorUIPanelRebuildReason::Data);
        return;
    }

    AddUIFontPath(result.path);
    host.RequestPanelRebuild(NativeEditorPreferencesPanel::PanelId(),
                             EditorUIPanelRebuildReason::Data);
}

bool NativeEditorPreferencesPanel::AddUIFontPath(std::filesystem::path path)
{
    m_fontSettingsSaveAttempted = true;
    m_lastFontSettingsSaveSucceeded = false;

    if (!m_settingsService)
    {
        m_fontStatusText = "Font add failed: settings unavailable.";
        return false;
    }

    if (path.empty())
    {
        m_fontStatusText = "Font add failed: path is empty.";
        return false;
    }

    const std::filesystem::path normalizedPath = NormalizeUIFontPath(path);
    if (!IsSupportedUIFontPath(normalizedPath))
    {
        m_fontStatusText =
            "Font add failed: choose a .ttf, .ttc, or .otf file.";
        return false;
    }

    std::error_code ec;
    if (!std::filesystem::is_regular_file(normalizedPath, ec) || ec)
    {
        m_fontStatusText =
            "Font add failed: file does not exist: " +
            normalizedPath.string();
        return false;
    }

    const std::vector<std::filesystem::path> previousFontPaths =
        m_settingsService->GetUIFontPaths();
    for (const std::filesystem::path& fontPath : previousFontPaths)
    {
        if (NormalizeUIFontPath(fontPath) == normalizedPath)
        {
            m_fontStatusText =
                "Font add skipped: path is already configured.";
            return false;
        }
    }

    std::vector<std::filesystem::path> nextFontPaths = previousFontPaths;
    nextFontPaths.push_back(normalizedPath);
    m_settingsService->SetUIFontPaths(nextFontPaths);
    if (!m_settingsService->Save())
    {
        m_fontStatusText =
            "Font add failed: " + m_settingsService->GetLastError();
        m_settingsService->SetUIFontPaths(previousFontPaths);
        return false;
    }

    m_lastFontSettingsSaveSucceeded = true;
    m_fontStatusText =
        "Font added: " + normalizedPath.filename().string() +
        ". Restart editor to apply.";
    return true;
}

bool NativeEditorPreferencesPanel::RemoveUIFontPath(uint32 index)
{
    m_fontSettingsSaveAttempted = true;
    m_lastFontSettingsSaveSucceeded = false;

    if (!m_settingsService)
    {
        m_fontStatusText = "Font remove failed: settings unavailable.";
        return false;
    }

    const std::vector<std::filesystem::path> previousFontPaths =
        m_settingsService->GetUIFontPaths();
    if (index >= static_cast<uint32>(previousFontPaths.size()))
    {
        m_fontStatusText = "Font remove failed: path index is invalid.";
        return false;
    }

    const bool previousAppendDefaultSystemFonts =
        m_settingsService->ShouldAppendDefaultSystemFonts();
    const std::filesystem::path removedPath = previousFontPaths[index];
    std::vector<std::filesystem::path> nextFontPaths = previousFontPaths;
    nextFontPaths.erase(nextFontPaths.begin() + static_cast<std::ptrdiff_t>(index));

    m_settingsService->SetUIFontPaths(nextFontPaths);
    if (nextFontPaths.empty())
    {
        m_settingsService->SetAppendDefaultSystemFonts(true);
    }

    if (!m_settingsService->Save())
    {
        m_fontStatusText =
            "Font remove failed: " + m_settingsService->GetLastError();
        m_settingsService->SetUIFontPaths(previousFontPaths);
        m_settingsService->SetAppendDefaultSystemFonts(
            previousAppendDefaultSystemFonts);
        return false;
    }

    m_lastFontSettingsSaveSucceeded = true;
    m_fontStatusText =
        nextFontPaths.empty()
            ? "Font removed: system fallback enabled. Restart editor to apply."
            : ("Font removed: " + removedPath.filename().string() +
               ". Restart editor to apply.");
    return true;
}

bool NativeEditorPreferencesPanel::ToggleAppendDefaultSystemFonts(bool enabled)
{
    m_fontSettingsSaveAttempted = true;
    m_lastFontSettingsSaveSucceeded = false;

    if (!m_settingsService)
    {
        m_fontStatusText =
            "Font setting failed: settings unavailable.";
        return false;
    }

    if (!enabled && m_settingsService->GetUIFontPaths().empty())
    {
        m_fontStatusText =
            "Font fallback cannot be disabled without a custom font path.";
        return false;
    }

    const bool previousAppendDefaultSystemFonts =
        m_settingsService->ShouldAppendDefaultSystemFonts();
    m_settingsService->SetAppendDefaultSystemFonts(enabled);
    if (!m_settingsService->Save())
    {
        m_fontStatusText =
            "Font setting failed: " + m_settingsService->GetLastError();
        m_settingsService->SetAppendDefaultSystemFonts(
            previousAppendDefaultSystemFonts);
        return false;
    }

    m_lastFontSettingsSaveSucceeded = true;
    m_fontStatusText =
        enabled ? "System font fallback enabled. Restart editor to apply."
                : "System font fallback disabled. Restart editor to apply.";
    return true;
}

bool NativeEditorPreferencesPanel::ResetUIFontSettings()
{
    m_fontSettingsSaveAttempted = true;
    m_lastFontSettingsSaveSucceeded = false;

    if (!m_settingsService)
    {
        m_fontStatusText =
            "Font reset failed: settings unavailable.";
        return false;
    }

    const std::vector<std::filesystem::path> previousFontPaths =
        m_settingsService->GetUIFontPaths();
    const bool previousAppendDefaultSystemFonts =
        m_settingsService->ShouldAppendDefaultSystemFonts();

    m_settingsService->SetUIFontPaths({});
    m_settingsService->SetAppendDefaultSystemFonts(true);
    if (!m_settingsService->Save())
    {
        m_fontStatusText =
            "Font reset failed: " + m_settingsService->GetLastError();
        m_settingsService->SetUIFontPaths(previousFontPaths);
        m_settingsService->SetAppendDefaultSystemFonts(
            previousAppendDefaultSystemFonts);
        return false;
    }

    m_lastFontSettingsSaveSucceeded = true;
    m_fontStatusText =
        "Font settings reset to system defaults. Restart editor to apply.";
    return true;
}

} // namespace RVX::Editor
