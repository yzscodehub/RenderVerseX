/**
 * @file EditorTooltipLayer.cpp
 * @brief Native editor non-interactive tooltip overlay layer implementation
 */

#include "Editor/UI/EditorTooltipLayer.h"

#include "Editor/UI/EditorTypography.h"
#include "UI/UICanvas.h"
#include "UI/UIContext.h"
#include "UI/UIRenderer.h"
#include "UI/Widget.h"
#include "UI/Widgets/Label.h"
#include "UI/Widgets/Panel.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <vector>

namespace RVX::Editor
{
namespace
{
    constexpr const char* RVX_EDITOR_TOOLTIP_LAYER = "Editor.TooltipLayer";
    constexpr const char* RVX_EDITOR_TOOLTIP_PANEL = "Editor.Tooltip";
    constexpr const char* RVX_EDITOR_TOOLTIP_LABEL = "Editor.Tooltip.Text";
    constexpr float RVX_EDITOR_TOOLTIP_TEXT_LAYOUT_WIDTH_SCALE = 1.2f;
    constexpr uint32 RVX_EDITOR_TOOLTIP_MAX_LINES = 4;

    struct WrappedTooltipText
    {
        std::string text;
        uint32 lineCount = 0;
        bool clipped = false;
        float width = 0.0f;
    };

    float MeasureTooltipTextWidth(const std::string& text, float fontSize)
    {
        return UI::UIFontFallbackChain::Default().MeasureText(text, fontSize).width *
               RVX_EDITOR_TOOLTIP_TEXT_LAYOUT_WIDTH_SCALE;
    }

    void PopLastUtf8Codepoint(std::string& text)
    {
        if (text.empty())
        {
            return;
        }

        size_t index = text.size() - 1u;
        while (index > 0u &&
               (static_cast<unsigned char>(text[index]) & 0xC0u) == 0x80u)
        {
            --index;
        }
        text.erase(index);
    }

    std::string EllipsizeLineToWidth(const std::string& text,
                                     float fontSize,
                                     float maxWidth,
                                     bool* clipped)
    {
        if (text.empty() || maxWidth <= 0.0f)
        {
            return {};
        }

        if (MeasureTooltipTextWidth(text, fontSize) <= maxWidth)
        {
            return text;
        }

        if (clipped)
        {
            *clipped = true;
        }

        constexpr const char* RVX_EDITOR_TOOLTIP_ELLIPSIS = "...";
        const float ellipsisWidth =
            MeasureTooltipTextWidth(RVX_EDITOR_TOOLTIP_ELLIPSIS, fontSize);
        if (ellipsisWidth > maxWidth)
        {
            return {};
        }

        std::string clippedText = text;
        while (!clippedText.empty() &&
               MeasureTooltipTextWidth(clippedText + RVX_EDITOR_TOOLTIP_ELLIPSIS,
                                       fontSize) > maxWidth)
        {
            PopLastUtf8Codepoint(clippedText);
        }

        return clippedText.empty() ? std::string(RVX_EDITOR_TOOLTIP_ELLIPSIS)
                                   : clippedText + RVX_EDITOR_TOOLTIP_ELLIPSIS;
    }

    std::string AppendEllipsisToWidth(const std::string& text,
                                      float fontSize,
                                      float maxWidth)
    {
        constexpr const char* RVX_EDITOR_TOOLTIP_ELLIPSIS = "...";
        if (text.size() >= 3u &&
            text.compare(text.size() - 3u, 3u, RVX_EDITOR_TOOLTIP_ELLIPSIS) == 0)
        {
            return text;
        }

        if (MeasureTooltipTextWidth(RVX_EDITOR_TOOLTIP_ELLIPSIS, fontSize) >
            maxWidth)
        {
            return {};
        }

        std::string clippedText = text;
        while (!clippedText.empty() &&
               MeasureTooltipTextWidth(clippedText + RVX_EDITOR_TOOLTIP_ELLIPSIS,
                                       fontSize) > maxWidth)
        {
            PopLastUtf8Codepoint(clippedText);
        }

        return clippedText.empty() ? std::string(RVX_EDITOR_TOOLTIP_ELLIPSIS)
                                   : clippedText + RVX_EDITOR_TOOLTIP_ELLIPSIS;
    }

    void PushWrappedLine(std::vector<std::string>& lines,
                         std::string& currentLine)
    {
        if (!currentLine.empty())
        {
            lines.push_back(currentLine);
            currentLine.clear();
        }
        else if (lines.empty())
        {
            lines.emplace_back();
        }
    }

    std::vector<std::string> TokenizeTooltipText(const std::string& text)
    {
        std::vector<std::string> tokens;
        std::string token;
        for (char ch : text)
        {
            const unsigned char c = static_cast<unsigned char>(ch);
            if (ch == '\r')
            {
                continue;
            }
            if (ch == '\n')
            {
                if (!token.empty())
                {
                    tokens.push_back(token);
                    token.clear();
                }
                tokens.emplace_back("\n");
                continue;
            }
            if (std::isspace(c) != 0)
            {
                if (!token.empty())
                {
                    tokens.push_back(token);
                    token.clear();
                }
                continue;
            }
            token.push_back(ch);
        }
        if (!token.empty())
        {
            tokens.push_back(token);
        }
        return tokens;
    }

    WrappedTooltipText WrapTooltipText(const std::string& text,
                                       float fontSize,
                                       float maxWidth,
                                       uint32 maxLines)
    {
        WrappedTooltipText wrapped;
        if (text.empty() || maxWidth <= 0.0f || maxLines == 0u)
        {
            return wrapped;
        }

        std::vector<std::string> lines;
        std::string currentLine;
        const std::vector<std::string> tokens = TokenizeTooltipText(text);
        for (const std::string& token : tokens)
        {
            if (token == "\n")
            {
                PushWrappedLine(lines, currentLine);
                continue;
            }

            const std::string candidate =
                currentLine.empty() ? token : (currentLine + " " + token);
            if (MeasureTooltipTextWidth(candidate, fontSize) <= maxWidth)
            {
                currentLine = candidate;
                continue;
            }

            if (!currentLine.empty())
            {
                lines.push_back(currentLine);
                currentLine.clear();
            }

            bool tokenClipped = false;
            currentLine = EllipsizeLineToWidth(token,
                                               fontSize,
                                               maxWidth,
                                               &tokenClipped);
            wrapped.clipped = wrapped.clipped || tokenClipped;
        }
        if (!currentLine.empty())
        {
            lines.push_back(currentLine);
        }

        if (lines.empty())
        {
            return wrapped;
        }

        if (lines.size() > static_cast<size_t>(maxLines))
        {
            lines.resize(static_cast<size_t>(maxLines));
            lines.back() = AppendEllipsisToWidth(lines.back(), fontSize, maxWidth);
            wrapped.clipped = true;
        }

        std::ostringstream textStream;
        for (size_t index = 0; index < lines.size(); ++index)
        {
            if (index > 0u)
            {
                textStream << '\n';
            }
            textStream << lines[index];
            wrapped.width =
                std::max(wrapped.width,
                         MeasureTooltipTextWidth(lines[index], fontSize));
        }

        wrapped.text = textStream.str();
        wrapped.lineCount = static_cast<uint32>(lines.size());
        return wrapped;
    }
}

void EditorTooltipLayer::Build(UI::UIContext& ui)
{
    UI::UICanvas& canvas = ui.GetCanvas();
    RemoveTooltipLayer(canvas);
    m_lastBuildStats = {};

    if (ui.GetWidth() == 0u || ui.GetHeight() == 0u)
    {
        return;
    }

    const float inputScale = std::max(0.01f, canvas.GetScaleFactor());
    const Vec2 logicalMouse = ui.GetInput().current.mousePosition / inputScale;
    const UI::Widget* tooltipSource = FindTooltipSource(canvas.HitTest(logicalMouse));
    if (!tooltipSource)
    {
        return;
    }

    const std::string tooltipText = ResolveTooltipText(*tooltipSource);
    if (tooltipText.empty())
    {
        return;
    }

    const UI::UITheme& theme = ui.GetTheme();
    const float paddingX = std::max(8.0f, theme.metrics.padding);
    const float paddingY = std::max(5.0f, theme.metrics.spacing * 0.75f);
    const float margin = std::max(4.0f, theme.metrics.spacing);
    const float offset = std::max(12.0f, theme.metrics.spacing * 2.0f);
    const float fontSize = EditorTypography::GetFontSize(
        theme,
        EditorTypographyRole::Tooltip);
    const float availableTextWidth =
        std::max(24.0f, static_cast<float>(ui.GetWidth()) - margin * 2.0f -
                            paddingX * 2.0f);
    const float maxTextWidth =
        std::min(std::max(420.0f, theme.metrics.controlHeight * 24.0f),
                 availableTextWidth);
    const WrappedTooltipText wrappedText =
        WrapTooltipText(tooltipText,
                        fontSize,
                        maxTextWidth,
                        RVX_EDITOR_TOOLTIP_MAX_LINES);
    if (wrappedText.text.empty())
    {
        return;
    }

    const UI::UITextMetrics textMetrics =
        UI::UIFontFallbackChain::Default().MeasureText(wrappedText.text, fontSize);
    const float tooltipWidth =
        std::min(maxTextWidth, wrappedText.width) + paddingX * 2.0f;
    const float tooltipHeight =
        std::max(theme.metrics.controlHeight,
                 std::max(fontSize, textMetrics.height) + paddingY * 2.0f);

    float x = logicalMouse.x + offset;
    float y = logicalMouse.y + offset;
    if (x + tooltipWidth + margin > static_cast<float>(ui.GetWidth()))
    {
        x = logicalMouse.x - tooltipWidth - offset;
    }
    if (y + tooltipHeight + margin > static_cast<float>(ui.GetHeight()))
    {
        y = logicalMouse.y - tooltipHeight - offset;
    }
    x = std::clamp(x,
                   margin,
                   std::max(margin,
                            static_cast<float>(ui.GetWidth()) - tooltipWidth -
                                margin));
    y = std::clamp(y,
                   margin,
                   std::max(margin,
                            static_cast<float>(ui.GetHeight()) - tooltipHeight -
                                margin));

    UI::Panel::Ptr root = UI::Panel::Create();
    root->SetName(RVX_EDITOR_TOOLTIP_LAYER);
    root->SetPosition(0.0f, 0.0f);
    root->SetSize(static_cast<float>(ui.GetWidth()),
                  static_cast<float>(ui.GetHeight()));
    root->SetBackgroundColor(UI::UIColor::Transparent());
    root->SetBorderWidth(0.0f);
    root->SetInteractive(false);

    UI::Panel::Ptr panel = UI::Panel::Create();
    panel->SetName(RVX_EDITOR_TOOLTIP_PANEL);
    panel->SetPosition(x, y);
    panel->SetSize(tooltipWidth, tooltipHeight);
    panel->SetBackgroundColor(theme.colors.panelBackground.WithAlpha(0.98f));
    panel->SetBorderColor(theme.colors.border);
    panel->SetBorderWidth(theme.metrics.borderWidth);
    panel->SetInteractive(false);

    UI::Label::Ptr label = UI::Label::Create(wrappedText.text);
    label->SetName(RVX_EDITOR_TOOLTIP_LABEL);
    label->SetPosition(paddingX, 0.0f);
    label->SetSize(std::max(0.0f, tooltipWidth - paddingX * 2.0f),
                   tooltipHeight);
    label->SetFontSize(fontSize);
    label->SetTextColor(theme.colors.text);
    label->SetTextAlign(UI::TextAlign::Left);
    label->SetVerticalAlign(UI::VerticalAlign::Middle);
    panel->AddChild(label);
    root->AddChild(std::move(panel));

    canvas.AddWidget(root);

    m_lastBuildStats.visible = true;
    m_lastBuildStats.tooltipCount = 1;
    m_lastBuildStats.lineCount = wrappedText.lineCount;
    m_lastBuildStats.clipped = wrappedText.clipped;
    m_lastBuildStats.sourceWidgetName = tooltipSource->GetName();
    m_lastBuildStats.text = wrappedText.text;
    m_lastBuildStats.bounds = UI::Rect(x, y, tooltipWidth, tooltipHeight);
}

void EditorTooltipLayer::Clear(UI::UIContext& ui)
{
    RemoveTooltipLayer(ui.GetCanvas());
    m_lastBuildStats = {};
}

const UI::Widget* EditorTooltipLayer::FindTooltipSource(const UI::Widget* widget)
{
    const UI::Widget* current = widget;
    while (current)
    {
        if (!ResolveTooltipText(*current).empty())
        {
            return current;
        }
        current = current->GetParent();
    }
    return nullptr;
}

std::string EditorTooltipLayer::ResolveTooltipText(const UI::Widget& widget)
{
    return widget.GetTooltipText();
}

void EditorTooltipLayer::RemoveTooltipLayer(UI::UICanvas& canvas)
{
    if (UI::Widget::Ptr layer = canvas.FindWidget(RVX_EDITOR_TOOLTIP_LAYER))
    {
        canvas.RemoveWidget(std::move(layer));
    }
}

} // namespace RVX::Editor
