/**
 * @file Button.cpp
 * @brief Button widget implementation
 */

#include "UI/Widgets/Button.h"

namespace RVX::UI
{

Button::Button()
{
    SetInteractive(true);
}

Button::Button(const std::string& text)
    : m_text(text)
{
    SetInteractive(true);
}

void Button::OnRender(UIRenderer& renderer)
{
    // TODO: Render button background and text using UIRenderer
}

bool Button::HandleEvent(const UIEvent& event)
{
    if (!m_enabled) return false;
    return Widget::HandleEvent(event);
}

} // namespace RVX::UI
