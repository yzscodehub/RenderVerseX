/**
 * @file NativeConsole.cpp
 * @brief Native UI console panel implementation
 */

#include "Editor/Panels/NativeConsole.h"

#include "Editor/UI/EditorPanelButtonStyle.h"
#include "Editor/UI/EditorShellMetrics.h"
#include "Editor/UI/EditorTypography.h"
#include "UI/Widgets/Button.h"
#include "UI/Widgets/Label.h"
#include "UI/Widgets/Panel.h"

#include <algorithm>
#include <ctime>
#include <string>

namespace RVX::Editor
{
namespace
{
    UI::UIColor LevelColor(ConsoleLogLevel level, const UI::UITheme& theme)
    {
        switch (level)
        {
            case ConsoleLogLevel::Trace:
                return theme.colors.textMuted;
            case ConsoleLogLevel::Debug:
                return UI::UIColor(0.45f, 0.76f, 0.88f, 1.0f);
            case ConsoleLogLevel::Info:
                return theme.colors.text;
            case ConsoleLogLevel::Warning:
                return theme.colors.warning;
            case ConsoleLogLevel::Error:
            case ConsoleLogLevel::Critical:
                return theme.colors.error;
        }
        return theme.colors.text;
    }

    const char* LevelTag(ConsoleLogLevel level)
    {
        switch (level)
        {
            case ConsoleLogLevel::Trace:
                return "T";
            case ConsoleLogLevel::Debug:
                return "D";
            case ConsoleLogLevel::Info:
                return "I";
            case ConsoleLogLevel::Warning:
                return "W";
            case ConsoleLogLevel::Error:
                return "E";
            case ConsoleLogLevel::Critical:
                return "!";
        }
        return " ";
    }

    std::string FormatTimestamp(uint64 timestamp)
    {
        std::time_t time = static_cast<std::time_t>(timestamp);
        std::tm localTime = {};
#if defined(_WIN32)
        localtime_s(&localTime, &time);
#else
        localtime_r(&time, &localTime);
#endif
        char buffer[16] = {};
        std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &localTime);
        return buffer;
    }

    UI::Button::Ptr CreateConsoleButton(const std::string& name,
                                        const std::string& text,
                                        const UI::UITheme& theme,
                                        bool active)
    {
        UI::Button::Ptr button = UI::Button::Create(text);
        button->SetName(name);
        EditorPanelButtonStyleDesc styleDesc;
        styleDesc.active = active;
        EditorPanelButtonStyle::Apply(*button, theme, styleDesc);
        return button;
    }

    float EstimateConsoleButtonWidth(const std::string& text,
                                     const UI::UITheme& theme,
                                     float minWidth,
                                     float maxWidth)
    {
        return EditorTypography::EstimatePaddedTextWidth(text,
                                                         theme,
                                                         EditorTypographyRole::Control,
                                                         minWidth,
                                                         maxWidth);
    }

    UI::Label::Ptr CreateConsoleLabel(const std::string& name,
                                      const std::string& text,
                                      const UI::UITheme& theme,
                                      const UI::UIColor& textColor)
    {
        UI::Label::Ptr label = UI::Label::Create(text);
        label->SetName(name);
        label->SetFontSize(EditorTypography::GetFontSize(
            theme,
            EditorTypographyRole::ListItem));
        label->SetTextColor(textColor);
        label->SetVerticalAlign(UI::VerticalAlign::Middle);
        label->SetOverflowMode(UI::TextOverflowMode::EndEllipsis);
        label->SetTooltipText(text);
        label->SetInteractive(false);
        return label;
    }
}

NativeConsolePanel::NativeConsolePanel()
{
    m_desc.id = "native.console";
    m_desc.title = "Console";
    m_desc.defaultDockArea = EditorUIPanelDockArea::Bottom;
    m_desc.visibleByDefault = true;
    m_desc.closable = true;
}

void NativeConsolePanel::BuildUI(EditorUIPanelFrameContext& context)
{
    if (!context.ui || !context.contentContainer)
    {
        return;
    }

    const uint32 infoCount = ConsolePanel::GetMessageCount(ConsoleLogLevel::Info);
    const uint32 warningCount = ConsolePanel::GetMessageCount(ConsoleLogLevel::Warning);
    const uint32 errorCount = ConsolePanel::GetMessageCount(ConsoleLogLevel::Error);
    const std::vector<ConsoleMessage> messages = ConsolePanel::GetMessagesSnapshot();

    AddToolbar(context, infoCount, warningCount, errorCount);
    AddMessageRows(context, messages);
}

bool NativeConsolePanel::ShouldShow(ConsoleLogLevel level) const
{
    switch (level)
    {
        case ConsoleLogLevel::Trace:
            return m_showTrace;
        case ConsoleLogLevel::Debug:
            return m_showDebug;
        case ConsoleLogLevel::Info:
            return m_showInfo;
        case ConsoleLogLevel::Warning:
            return m_showWarnings;
        case ConsoleLogLevel::Error:
        case ConsoleLogLevel::Critical:
            return m_showErrors;
    }
    return false;
}

void NativeConsolePanel::AddToolbar(EditorUIPanelFrameContext& context,
                                    uint32 infoCount,
                                    uint32 warningCount,
                                    uint32 errorCount)
{
    const UI::UITheme& theme = context.ui->GetTheme();
    const EditorShellMetrics shellMetrics = EditorShellMetricPolicy::Resolve(theme);
    const float padding = shellMetrics.contentPadding;
    const float gap = shellMetrics.compactGap;
    const float buttonHeight = shellMetrics.panelControlHeight;
    const float top = padding;
    float x = padding;

    UI::Button::Ptr clear = CreateConsoleButton("NativeConsole.Clear",
                                                "Clear",
                                                theme,
                                                false);
    clear->SetPosition(x, top);
    clear->SetSize(EstimateConsoleButtonWidth("Clear", theme, 58.0f, 110.0f),
                   buttonHeight);
    clear->SetOnClick([](const UI::UIEvent& event) {
        (void)event;
        ConsolePanel::Clear();
    });
    context.contentContainer->AddChild(clear);
    x += clear->GetWidth() + gap;

    const std::string infoText = "Info (" + std::to_string(infoCount) + ")";
    UI::Button::Ptr info = CreateConsoleButton("NativeConsole.Filter.Info",
                                               infoText,
                                               theme,
                                               m_showInfo);
    info->SetPosition(x, top);
    info->SetSize(EstimateConsoleButtonWidth(infoText, theme, 84.0f, 150.0f),
                  buttonHeight);
    info->SetOnClick([this](const UI::UIEvent& event) {
        (void)event;
        m_showInfo = !m_showInfo;
    });
    context.contentContainer->AddChild(info);
    x += info->GetWidth() + gap;

    const std::string warningText =
        "Warn (" + std::to_string(warningCount) + ")";
    UI::Button::Ptr warn = CreateConsoleButton("NativeConsole.Filter.Warning",
                                               warningText,
                                               theme,
                                               m_showWarnings);
    warn->SetPosition(x, top);
    warn->SetSize(EstimateConsoleButtonWidth(warningText, theme, 92.0f, 160.0f),
                  buttonHeight);
    warn->SetOnClick([this](const UI::UIEvent& event) {
        (void)event;
        m_showWarnings = !m_showWarnings;
    });
    context.contentContainer->AddChild(warn);
    x += warn->GetWidth() + gap;

    const std::string errorText = "Error (" + std::to_string(errorCount) + ")";
    UI::Button::Ptr error = CreateConsoleButton("NativeConsole.Filter.Error",
                                                errorText,
                                                theme,
                                                m_showErrors);
    error->SetPosition(x, top);
    error->SetSize(EstimateConsoleButtonWidth(errorText, theme, 92.0f, 160.0f),
                   buttonHeight);
    error->SetOnClick([this](const UI::UIEvent& event) {
        (void)event;
        m_showErrors = !m_showErrors;
    });
    context.contentContainer->AddChild(error);
}

void NativeConsolePanel::AddMessageRows(EditorUIPanelFrameContext& context,
                                        const std::vector<ConsoleMessage>& messages)
{
    const UI::UITheme& theme = context.ui->GetTheme();
    const EditorShellMetrics shellMetrics = EditorShellMetricPolicy::Resolve(theme);
    const float padding = shellMetrics.contentPadding;
    const float rowHeight = shellMetrics.compactPanelListRowHeight;
    const float startY = shellMetrics.singleRowPanelToolbarHeight;
    const float availableHeight =
        std::max(0.0f, context.contentContainer->GetHeight() - startY - padding);
    const uint32 maxRows = std::max(1u, static_cast<uint32>(availableHeight / rowHeight));

    std::vector<const ConsoleMessage*> visibleMessages;
    visibleMessages.reserve(messages.size());
    for (const ConsoleMessage& message : messages)
    {
        if (ShouldShow(message.level))
        {
            visibleMessages.push_back(&message);
        }
    }

    if (visibleMessages.empty())
    {
        UI::Label::Ptr empty = CreateConsoleLabel("NativeConsole.Empty",
                                                  "No console messages.",
                                                  theme,
                                                  theme.colors.textMuted);
        empty->SetPosition(padding, startY);
        empty->SetSize(std::max(0.0f, context.contentContainer->GetWidth() - padding * 2.0f),
                       rowHeight);
        context.contentContainer->AddChild(empty);
        return;
    }

    const size_t firstIndex =
        visibleMessages.size() > maxRows ? visibleMessages.size() - maxRows : 0u;
    float y = startY;
    uint32 rowIndex = 0;
    for (size_t index = firstIndex; index < visibleMessages.size(); ++index)
    {
        const ConsoleMessage& message = *visibleMessages[index];
        const UI::UIColor textColor = LevelColor(message.level, theme);
        const float rowWidth =
            std::max(0.0f, context.contentContainer->GetWidth() - padding * 2.0f);

        UI::Panel::Ptr row = UI::Panel::Create();
        row->SetName("NativeConsole.Row." + std::to_string(rowIndex));
        row->SetPosition(padding, y);
        row->SetSize(rowWidth, rowHeight);
        row->SetBackgroundColor(rowIndex % 2u == 0u
                                    ? theme.colors.panelBackground
                                    : theme.colors.windowBackground.WithAlpha(0.45f));
        row->SetBorderWidth(0.0f);
        row->SetInteractive(false);

        const std::string text = std::string("[") + LevelTag(message.level) + "] [" +
                                 FormatTimestamp(message.timestamp) + "] " +
                                 message.message +
                                 (message.count > 1u
                                      ? " (" + std::to_string(message.count) + ")"
                                      : "");
        UI::Label::Ptr label = CreateConsoleLabel("NativeConsole.Row." +
                                                      std::to_string(rowIndex) + ".Text",
                                                  text,
                                                  theme,
                                                  textColor);
        label->SetPosition(6.0f, 0.0f);
        label->SetSize(std::max(0.0f, rowWidth - 12.0f), rowHeight);
        row->AddChild(label);
        context.contentContainer->AddChild(row);

        y += rowHeight;
        ++rowIndex;
    }
}

} // namespace RVX::Editor
