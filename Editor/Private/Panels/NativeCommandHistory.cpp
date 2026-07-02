/**
 * @file NativeCommandHistory.cpp
 * @brief Native UI command history diagnostics panel implementation
 */

#include "Editor/Panels/NativeCommandHistory.h"

#include "Editor/UI/EditorListLayout.h"
#include "Editor/UI/EditorPanelButtonStyle.h"
#include "Editor/UI/EditorShellMetrics.h"
#include "Editor/UI/EditorTypography.h"
#include "Editor/UI/EditorUIHost.h"
#include "UI/Widgets/Button.h"
#include "UI/Widgets/Label.h"
#include "UI/Widgets/Panel.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace RVX::Editor
{
namespace
{
    const char* CommandStatusLabel(const EditorUICommandHistoryEntry& entry)
    {
        if (entry.diagnostic)
        {
            if (entry.diagnosticSucceeded)
            {
                return "OK";
            }
            return entry.diagnosticSeverity == EditorUIDiagnosticSeverity::Warning
                       ? "Warning"
                       : "Error";
        }

        switch (entry.result.status)
        {
            case EditorCommandExecutionStatus::Succeeded:
                return "OK";
            case EditorCommandExecutionStatus::NotFound:
                return "Not found";
            case EditorCommandExecutionStatus::Disabled:
                return "Disabled";
            case EditorCommandExecutionStatus::MissingCallback:
                return "No callback";
            case EditorCommandExecutionStatus::ShortcutNotMatched:
                return "No shortcut";
            case EditorCommandExecutionStatus::BlockedByKeyboardCapture:
                return "Input captured";
        }

        return "Unknown";
    }

    UI::UIColor CommandStatusColor(const EditorUICommandHistoryEntry& entry,
                                   const UI::UITheme& theme)
    {
        if (entry.diagnostic)
        {
            if (entry.diagnosticSucceeded)
            {
                return theme.colors.accent;
            }
            return entry.diagnosticSeverity == EditorUIDiagnosticSeverity::Warning
                       ? theme.colors.warning
                       : theme.colors.error;
        }

        switch (entry.result.status)
        {
            case EditorCommandExecutionStatus::Succeeded:
                return theme.colors.accent;
            case EditorCommandExecutionStatus::Disabled:
            case EditorCommandExecutionStatus::BlockedByKeyboardCapture:
                return theme.colors.warning;
            case EditorCommandExecutionStatus::NotFound:
            case EditorCommandExecutionStatus::MissingCallback:
                return theme.colors.error;
            case EditorCommandExecutionStatus::ShortcutNotMatched:
                return theme.colors.textMuted;
        }

        return theme.colors.text;
    }

    const char* CommandSourceLabel(EditorUICommandExecutionSource source)
    {
        switch (source)
        {
            case EditorUICommandExecutionSource::Direct:
                return "Direct";
            case EditorUICommandExecutionSource::Shortcut:
                return "Shortcut";
            case EditorUICommandExecutionSource::Automation:
                return "Automation";
        }

        return "Unknown";
    }

    std::string CommandDisplayText(const EditorUICommandHistoryEntry& entry)
    {
        if (entry.diagnostic)
        {
            if (entry.diagnosticMessage.empty())
            {
                return entry.displayName.empty() ? "Editor automation"
                                                 : entry.displayName;
            }

            return (entry.displayName.empty() ? "Editor automation"
                                              : entry.displayName) +
                   " - " + entry.diagnosticMessage;
        }

        if (entry.displayName.empty())
        {
            return entry.result.commandId.empty() ? "(unknown command)"
                                                 : entry.result.commandId;
        }

        if (entry.result.commandId.empty() ||
            entry.displayName == entry.result.commandId)
        {
            return entry.displayName;
        }

        return entry.displayName + " (" + entry.result.commandId + ")";
    }

    UI::Button::Ptr CreateHistoryButton(const std::string& name,
                                        const std::string& text,
                                        const UI::UITheme& theme)
    {
        UI::Button::Ptr button = UI::Button::Create(text);
        button->SetName(name);
        EditorPanelButtonStyle::Apply(*button, theme);
        return button;
    }

    UI::Label::Ptr CreateHistoryLabel(const std::string& name,
                                      const std::string& text,
                                      const UI::UITheme& theme,
                                      const UI::UIColor& color)
    {
        UI::Label::Ptr label = UI::Label::Create(text);
        label->SetName(name);
        label->SetFontSize(EditorTypography::GetFontSize(
            theme,
            EditorTypographyRole::ListItem));
        label->SetTextColor(color);
        label->SetVerticalAlign(UI::VerticalAlign::Middle);
        label->SetOverflowMode(UI::TextOverflowMode::EndEllipsis);
        label->SetTooltipText(text);
        label->SetInteractive(false);
        return label;
    }
}

NativeCommandHistoryPanel::NativeCommandHistoryPanel()
{
    m_desc.id = PanelId();
    m_desc.title = "Command History";
    m_desc.defaultDockArea = EditorUIPanelDockArea::Bottom;
    m_desc.visibleByDefault = false;
    m_desc.closable = true;
}

void NativeCommandHistoryPanel::BuildUI(EditorUIPanelFrameContext& context)
{
    m_lastBuildStats = {};
    m_lastBuildStats.built = true;
    m_lastBuildStats.hasDiagnostics = context.host != nullptr;

    if (!context.ui || !context.contentContainer || !context.host)
    {
        return;
    }

    const EditorUICommandDiagnostics& diagnostics =
        context.host->GetCommandDiagnostics();
    const std::vector<EditorUICommandHistoryEntry>& history =
        diagnostics.history;

    m_lastBuildStats.totalEntryCount = static_cast<uint32>(history.size());
    m_lastBuildStats.visibleEntryCount = static_cast<uint32>(history.size());
    m_lastBuildStats.historyDroppedCount = diagnostics.historyDroppedCount;
    if (!history.empty())
    {
        m_lastBuildStats.lastSequence = history.back().sequence;
    }

    for (const EditorUICommandHistoryEntry& entry : history)
    {
        if (entry.Succeeded())
        {
            ++m_lastBuildStats.succeededEntryCount;
        }
        else
        {
            ++m_lastBuildStats.failedEntryCount;
        }

        switch (entry.source)
        {
            case EditorUICommandExecutionSource::Direct:
                ++m_lastBuildStats.directEntryCount;
                break;
            case EditorUICommandExecutionSource::Shortcut:
                ++m_lastBuildStats.shortcutEntryCount;
                break;
            case EditorUICommandExecutionSource::Automation:
                ++m_lastBuildStats.automationEntryCount;
                break;
        }
    }

    AddToolbar(context);
    AddHistoryRows(context);
}

void NativeCommandHistoryPanel::AddToolbar(EditorUIPanelFrameContext& context)
{
    const UI::UITheme& theme = context.ui->GetTheme();
    const EditorShellMetrics shellMetrics = EditorShellMetricPolicy::Resolve(theme);
    const float padding = shellMetrics.contentPadding;
    const float buttonHeight = shellMetrics.panelControlHeight;
    const float rowWidth =
        std::max(0.0f, context.contentContainer->GetWidth() - padding * 2.0f);
    const float clearWidth = 68.0f;
    const float summaryWidth = std::max(80.0f, rowWidth - clearWidth - padding);

    const std::string summary =
        std::to_string(m_lastBuildStats.totalEntryCount) + " entries | " +
        std::to_string(m_lastBuildStats.succeededEntryCount) + " ok | " +
        std::to_string(m_lastBuildStats.failedEntryCount) + " failed | " +
        std::to_string(m_lastBuildStats.historyDroppedCount) + " dropped";

    UI::Label::Ptr summaryLabel = CreateHistoryLabel("NativeCommandHistory.Summary",
                                                     summary,
                                                     theme,
                                                     theme.colors.textMuted);
    summaryLabel->SetPosition(padding, padding);
    summaryLabel->SetSize(summaryWidth, buttonHeight);
    context.contentContainer->AddChild(summaryLabel);

    UI::Button::Ptr clear = CreateHistoryButton("NativeCommandHistory.Clear",
                                                "Clear",
                                                theme);
    clear->SetPosition(padding + summaryWidth + padding, padding);
    clear->SetSize(clearWidth, buttonHeight);
    clear->SetEnabled(m_lastBuildStats.totalEntryCount > 0u);
    clear->SetOnClick([host = context.host](const UI::UIEvent& event) {
        (void)event;
        if (!host)
        {
            return;
        }

        host->ClearCommandHistory();
        host->RequestPanelRebuild(NativeCommandHistoryPanel::PanelId(),
                                  EditorUIPanelRebuildReason::Data);
    });
    context.contentContainer->AddChild(clear);
}

void NativeCommandHistoryPanel::AddHistoryRows(EditorUIPanelFrameContext& context)
{
    const UI::UITheme& theme = context.ui->GetTheme();
    const EditorShellMetrics shellMetrics = EditorShellMetricPolicy::Resolve(theme);
    const float padding = shellMetrics.contentPadding;
    const float rowHeight = shellMetrics.panelListRowHeight;
    const float startY = shellMetrics.singleRowPanelToolbarHeight;
    const float rowWidth =
        std::max(0.0f, context.contentContainer->GetWidth() - padding * 2.0f);
    const float listHeight =
        std::max(0.0f, context.contentContainer->GetHeight() - startY - padding);

    const std::vector<EditorUICommandHistoryEntry>& history =
        context.host->GetCommandHistory();
    if (history.empty())
    {
        UI::Label::Ptr empty = CreateHistoryLabel("NativeCommandHistory.Empty",
                                                  "No command history.",
                                                  theme,
                                                  theme.colors.textMuted);
        empty->SetPosition(padding, startY);
        empty->SetSize(rowWidth, rowHeight);
        context.contentContainer->AddChild(empty);
        return;
    }

    EditorListLayoutDesc listDesc;
    listDesc.context = &context;
    listDesc.namePrefix = "NativeCommandHistory";
    listDesc.bounds = UI::Rect(padding, startY, rowWidth, listHeight);
    listDesc.rowHeight = rowHeight;
    listDesc.contentHeight = static_cast<float>(history.size()) * rowHeight;
    listDesc.scrollOffsetY = m_scrollOffsetY;
    listDesc.onScrollChanged = [this](const Vec2& offset) {
        m_scrollOffsetY = offset.y;
    };

    EditorListLayoutFrame list =
        EditorListLayout::BeginScrollableList(std::move(listDesc));
    if (!list)
    {
        return;
    }

    m_lastBuildStats.hasListViewport = true;
    m_lastBuildStats.listViewportHeight = list.viewportHeight;
    m_lastBuildStats.listContentHeight = list.contentHeight;
    m_lastBuildStats.listScrollOffsetY = m_scrollOffsetY;

    const float seqWidth = 58.0f;
    const float statusWidth = 96.0f;
    const float sourceWidth = 72.0f;
    const float frameWidth = 74.0f;
    const float commandWidth =
        std::max(120.0f,
                 rowWidth - seqWidth - statusWidth - sourceWidth -
                     frameWidth - 40.0f);

    for (size_t index = 0; index < history.size(); ++index)
    {
        const EditorUICommandHistoryEntry& entry = history[index];
        const std::string rowName =
            "NativeCommandHistory.Row." + std::to_string(entry.sequence);

        UI::Panel::Ptr row = UI::Panel::Create();
        row->SetName(rowName);
        row->SetPosition(0.0f, static_cast<float>(index) * rowHeight);
        row->SetSize(rowWidth, rowHeight);
        row->SetBackgroundColor(index % 2u == 0u
                                    ? theme.colors.panelBackground
                                    : theme.colors.windowBackground.WithAlpha(0.35f));
        row->SetBorderWidth(0.0f);
        row->SetInteractive(false);

        float x = 8.0f;
        UI::Label::Ptr sequence = CreateHistoryLabel(rowName + ".Sequence",
                                                     "#" + std::to_string(entry.sequence),
                                                     theme,
                                                     theme.colors.textMuted);
        sequence->SetPosition(x, 0.0f);
        sequence->SetSize(seqWidth, rowHeight);
        row->AddChild(sequence);
        x += seqWidth;

        UI::Label::Ptr status = CreateHistoryLabel(
            rowName + ".Status",
            CommandStatusLabel(entry),
            theme,
            CommandStatusColor(entry, theme));
        status->SetPosition(x, 0.0f);
        status->SetSize(statusWidth, rowHeight);
        row->AddChild(status);
        x += statusWidth;

        UI::Label::Ptr command = CreateHistoryLabel(rowName + ".Command",
                                                    CommandDisplayText(entry),
                                                    theme,
                                                    theme.colors.text);
        command->SetPosition(x, 0.0f);
        command->SetSize(commandWidth, rowHeight);
        row->AddChild(command);
        x += commandWidth;

        UI::Label::Ptr source = CreateHistoryLabel(rowName + ".Source",
                                                   CommandSourceLabel(entry.source),
                                                   theme,
                                                   theme.colors.textMuted);
        source->SetPosition(x, 0.0f);
        source->SetSize(sourceWidth, rowHeight);
        row->AddChild(source);
        x += sourceWidth;

        UI::Label::Ptr frame = CreateHistoryLabel(rowName + ".Frame",
                                                  "F" +
                                                      std::to_string(entry.frameIndex),
                                                  theme,
                                                  theme.colors.textMuted);
        frame->SetPosition(x, 0.0f);
        frame->SetSize(frameWidth, rowHeight);
        row->AddChild(frame);

        list.contentPanel->AddChild(row);
    }

    EditorListLayout::EndScrollableList(list);
}

} // namespace RVX::Editor
