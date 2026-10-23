/**
 * @file Panel.cpp
 * @brief Panel widget implementation
 */

#include "UI/Widgets/Panel.h"

#include "UI/UIRenderer.h"

namespace RVX::UI
{

void Panel::Render(UIRenderer& renderer)
{
    if (m_visibility != Visibility::Visible)
    {
        return;
    }

    OnRender(renderer);

    if (m_clipChildren)
    {
        renderer.PushClipRect(GetGlobalRect());
    }

    for (auto& child : m_children)
    {
        child->Render(renderer);
    }

    if (m_clipChildren)
    {
        renderer.PopClipRect();
    }
}

void Panel::OnRender(UIRenderer& renderer)
{
    const Rect bounds = GetGlobalRect();
    renderer.DrawRect(bounds, m_style.backgroundColor);
    renderer.DrawBorder(bounds, m_style.borderColor, m_style.borderWidth);
}

} // namespace RVX::UI
