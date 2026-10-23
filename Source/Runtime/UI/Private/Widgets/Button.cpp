/**
 * @file Button.cpp
 * @brief Button widget implementation
 */

#include "UI/Widgets/Button.h"

#include "UI/UIRenderer.h"
#include "UI/UITextOverflow.h"

namespace RVX::UI
{

Button::Button()
{
    SetInteractive(true);
    SetTabStop(true);
}

Button::Button(const std::string& text)
    : m_text(text)
{
    SetInteractive(true);
    SetTabStop(true);
}

void Button::SetText(const std::string& text)
{
    m_text = text;
    MarkLayoutDirty();
}

void Button::SetEnabled(bool enabled)
{
    m_enabled = enabled;
    SetTabStop(enabled);
}

void Button::OnRender(UIRenderer& renderer)
{
    UIColor background = m_normalColor;
    if (!m_enabled)
    {
        background = m_disabledColor;
    }
    else if (IsPressed())
    {
        background = m_pressedColor;
    }
    else if (IsHovered())
    {
        background = m_hoverColor;
    }

    const Rect bounds = GetGlobalRect();
    renderer.DrawRect(bounds, background);
    if (m_enabled && IsFocused())
    {
        renderer.DrawBorder(bounds.Expand(1.0f), m_focusColor.WithAlpha(0.35f), 1.0f);
        renderer.DrawBorder(bounds, m_focusColor, m_focusWidth);
    }

    renderer.PushClipRect(bounds);
    const std::string renderText = ResolveSingleLineTextOverflow(
        renderer,
        m_text,
        bounds.width,
        m_style.fontSize,
        m_overflowMode);
    renderer.DrawText(renderText,
                      bounds,
                      m_style.fontSize,
                      m_style.textColor,
                      TextAlign::Center,
                      VerticalAlign::Middle);
    renderer.PopClipRect();
}

bool Button::HandleEvent(const UIEvent& event)
{
    if (!m_enabled) return false;
    return Widget::HandleEvent(event);
}

} // namespace RVX::UI
