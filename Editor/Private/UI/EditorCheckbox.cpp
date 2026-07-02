/**
 * @file EditorCheckbox.cpp
 * @brief Native editor checkbox widget implementation
 */

#include "Editor/UI/EditorCheckbox.h"

#include "Editor/UI/EditorTypography.h"
#include "UI/UIRenderer.h"

#include <algorithm>

namespace RVX::Editor
{
EditorCheckbox::EditorCheckbox()
{
    SetInteractive(true);
    SetTabStop(true);
}

void EditorCheckbox::SetEnabled(bool enabled)
{
    m_enabled = enabled;
    SetTabStop(enabled);
}

void EditorCheckbox::ApplyTheme(const UI::UITheme& theme)
{
    m_backgroundColor = theme.colors.windowBackground;
    m_hoverColor = theme.colors.surfaceHover;
    m_pressedColor = theme.colors.surfaceActive;
    m_borderColor = theme.colors.border;
    m_accentColor = theme.colors.accent;
    m_disabledColor = theme.colors.surface.WithAlpha(0.45f);
    m_textColor = theme.colors.text;
    m_disabledTextColor = theme.colors.textMuted.WithAlpha(0.65f);
    m_fontSize = EditorTypography::GetFontSize(theme,
                                               EditorTypographyRole::Control);
}

void EditorCheckbox::OnRender(UI::UIRenderer& renderer)
{
    const UI::Rect bounds = GetGlobalRect();
    const float boxSize = std::max(10.0f, std::min(16.0f, bounds.height - 4.0f));
    const UI::Rect box(bounds.x,
                       bounds.y + (bounds.height - boxSize) * 0.5f,
                       boxSize,
                       boxSize);

    UI::UIColor background = m_backgroundColor;
    if (!m_enabled)
    {
        background = m_disabledColor;
    }
    else if (IsPressed())
    {
        background = m_pressedColor;
    }
    else if (IsHovered() || IsFocused())
    {
        background = m_hoverColor;
    }

    renderer.DrawRect(box, background);
    renderer.DrawBorder(box,
                        IsFocused() ? m_accentColor : m_borderColor,
                        IsFocused() ? 2.0f : 1.0f);

    if (m_checked)
    {
        const float inset = std::max(3.0f, boxSize * 0.28f);
        renderer.DrawRect(UI::Rect(box.x + inset,
                                   box.y + inset,
                                   box.width - inset * 2.0f,
                                   box.height - inset * 2.0f),
                          m_enabled ? m_accentColor : m_disabledTextColor);
    }

    if (!m_label.empty())
    {
        const float textX = box.Right() + 7.0f;
        const UI::Rect textBounds(textX,
                                  bounds.y,
                                  std::max(0.0f, bounds.Right() - textX),
                                  bounds.height);
        renderer.DrawText(m_label,
                          textBounds,
                          m_fontSize,
                          m_enabled ? m_textColor : m_disabledTextColor,
                          UI::TextAlign::Left,
                          UI::VerticalAlign::Middle);
    }
}

bool EditorCheckbox::HandleEvent(const UI::UIEvent& event)
{
    if (!m_enabled)
    {
        return false;
    }

    if (event.type == UI::UIEventType::KeyDown && IsFocused())
    {
        if (event.keyCode == static_cast<int>(UI::RVX_UI_KEY_SPACE) ||
            event.keyCode == static_cast<int>(UI::RVX_UI_KEY_ENTER))
        {
            Toggle();
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
            Toggle();
        }
        return true;
    }

    return UI::Widget::HandleEvent(event);
}

void EditorCheckbox::Toggle()
{
    m_checked = !m_checked;
    if (m_onCheckedChanged)
    {
        m_onCheckedChanged(m_checked);
    }
}

} // namespace RVX::Editor
