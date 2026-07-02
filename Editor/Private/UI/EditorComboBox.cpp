/**
 * @file EditorComboBox.cpp
 * @brief Native editor combo box widget implementation
 */

#include "Editor/UI/EditorComboBox.h"

#include "Editor/UI/EditorTypography.h"
#include "UI/UIRenderer.h"

#include <algorithm>
#include <utility>

namespace RVX::Editor
{
EditorComboBox::EditorComboBox()
{
    SetInteractive(true);
    SetTabStop(true);
}

void EditorComboBox::SetEnabled(bool enabled)
{
    m_enabled = enabled;
    SetTabStop(enabled);
}

void EditorComboBox::SetOptions(std::vector<std::string> options)
{
    m_options = std::move(options);
}

void EditorComboBox::ApplyTheme(const UI::UITheme& theme)
{
    m_backgroundColor = theme.colors.windowBackground;
    m_hoverColor = theme.colors.surfaceHover;
    m_focusColor = theme.colors.surface;
    m_borderColor = theme.colors.border;
    m_focusBorderColor = theme.colors.accent;
    m_textColor = theme.colors.text;
    m_placeholderColor = theme.colors.textMuted;
    m_disabledColor = theme.colors.surface.WithAlpha(0.45f);
    m_disabledTextColor = theme.colors.textMuted.WithAlpha(0.65f);
    m_fontSize = EditorTypography::GetFontSize(theme,
                                               EditorTypographyRole::Control);
}

void EditorComboBox::OnRender(UI::UIRenderer& renderer)
{
    const UI::Rect bounds = GetGlobalRect();
    UI::UIColor background = m_backgroundColor;
    if (!m_enabled)
    {
        background = m_disabledColor;
    }
    else if (IsFocused())
    {
        background = m_focusColor;
    }
    else if (IsHovered())
    {
        background = m_hoverColor;
    }

    renderer.DrawRect(bounds, background);
    renderer.DrawBorder(bounds,
                        IsFocused() ? m_focusBorderColor : m_borderColor,
                        IsFocused() ? 2.0f : 1.0f);

    const std::string text = GetSelectedText();
    const bool hasSelection = !text.empty();
    const float arrowWidth = std::min(28.0f, std::max(18.0f, bounds.height));
    const float horizontalPadding = 7.0f;
    const UI::Rect textBounds(bounds.x + horizontalPadding,
                              bounds.y,
                              std::max(0.0f,
                                       bounds.width - arrowWidth - horizontalPadding * 2.0f),
                              bounds.height);
    renderer.DrawText(hasSelection ? text : m_placeholder,
                      textBounds,
                      m_fontSize,
                      m_enabled
                          ? (hasSelection ? m_textColor : m_placeholderColor)
                          : m_disabledTextColor,
                      UI::TextAlign::Left,
                      UI::VerticalAlign::Middle);

    const UI::Rect arrowBounds(bounds.Right() - arrowWidth,
                               bounds.y,
                               arrowWidth,
                               bounds.height);
    renderer.DrawText("v",
                      arrowBounds,
                      m_fontSize,
                      m_enabled ? m_textColor : m_disabledTextColor,
                      UI::TextAlign::Center,
                      UI::VerticalAlign::Middle);
}

bool EditorComboBox::HandleEvent(const UI::UIEvent& event)
{
    if (!m_enabled)
    {
        return false;
    }

    if (event.type == UI::UIEventType::KeyDown && IsFocused())
    {
        if (event.keyCode == static_cast<int>(UI::RVX_UI_KEY_SPACE) ||
            event.keyCode == static_cast<int>(UI::RVX_UI_KEY_ENTER) ||
            event.keyCode == static_cast<int>(UI::RVX_UI_KEY_DOWN))
        {
            RequestOpen();
            return true;
        }
    }

    if (event.type == UI::UIEventType::MouseUp &&
        event.button == static_cast<int>(UI::UIMouseButton::Left) &&
        IsPressed())
    {
        m_pressed = false;
        if (GetGlobalRect().Contains(event.position))
        {
            RequestOpen();
        }
        return true;
    }

    return UI::Widget::HandleEvent(event);
}

void EditorComboBox::RequestOpen()
{
    if (m_onOpenRequested)
    {
        m_onOpenRequested(GetGlobalRect());
    }
}

std::string EditorComboBox::GetSelectedText() const
{
    if (m_selectedIndex < 0 ||
        static_cast<size_t>(m_selectedIndex) >= m_options.size())
    {
        return {};
    }

    return m_options[static_cast<size_t>(m_selectedIndex)];
}

} // namespace RVX::Editor
