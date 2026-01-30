/**
 * @file Widget.cpp
 * @brief Widget implementation
 */

#include "UI/Widget.h"
#include <algorithm>

namespace RVX::UI
{

uint64 Widget::s_nextId = 1;

void Widget::AddChild(Ptr child)
{
    if (!child || child.get() == this) return;

    // Remove from previous parent
    if (child->m_parent)
    {
        child->RemoveFromParent();
    }

    child->m_parent = this;
    child->m_id = s_nextId++;
    m_children.push_back(std::move(child));
    MarkLayoutDirty();
}

void Widget::RemoveChild(Ptr child)
{
    if (!child) return;

    auto it = std::find(m_children.begin(), m_children.end(), child);
    if (it != m_children.end())
    {
        (*it)->m_parent = nullptr;
        m_children.erase(it);
        MarkLayoutDirty();
    }
}

void Widget::RemoveFromParent()
{
    if (m_parent)
    {
        auto& siblings = m_parent->m_children;
        siblings.erase(
            std::remove_if(siblings.begin(), siblings.end(),
                [this](const Ptr& p) { return p.get() == this; }),
            siblings.end()
        );
        m_parent->MarkLayoutDirty();
        m_parent = nullptr;
    }
}

void Widget::RemoveAllChildren()
{
    for (auto& child : m_children)
    {
        child->m_parent = nullptr;
    }
    m_children.clear();
    MarkLayoutDirty();
}

Widget::Ptr Widget::FindChild(const std::string& name) const
{
    for (const auto& child : m_children)
    {
        if (child->GetName() == name)
        {
            return child;
        }
    }
    return nullptr;
}

Widget::Ptr Widget::FindChildRecursive(const std::string& name) const
{
    for (const auto& child : m_children)
    {
        if (child->GetName() == name)
        {
            return child;
        }
        auto found = child->FindChildRecursive(name);
        if (found) return found;
    }
    return nullptr;
}

Rect Widget::GetGlobalRect() const
{
    Vec2 globalPos = m_position;
    const Widget* parent = m_parent;
    while (parent)
    {
        globalPos += parent->m_position;
        parent = parent->m_parent;
    }
    return Rect(globalPos.x, globalPos.y, m_size.x, m_size.y);
}

void Widget::Focus()
{
    m_focused = true;
    if (m_onFocus)
    {
        UIEvent event;
        event.type = UIEventType::Focus;
        m_onFocus(event);
    }
}

void Widget::Blur()
{
    m_focused = false;
    if (m_onFocus)
    {
        UIEvent event;
        event.type = UIEventType::Blur;
        m_onFocus(event);
    }
}

void Widget::Layout()
{
    if (!m_layoutDirty) return;

    OnLayoutChildren();

    for (auto& child : m_children)
    {
        child->Layout();
    }

    m_layoutDirty = false;
}

void Widget::Render(UIRenderer& renderer)
{
    if (m_visibility != Visibility::Visible) return;

    OnRender(renderer);

    for (auto& child : m_children)
    {
        child->Render(renderer);
    }
}

bool Widget::HandleEvent(const UIEvent& event)
{
    if (!m_interactive || m_visibility != Visibility::Visible)
    {
        return false;
    }

    // Check children first (reverse order for front-to-back)
    for (auto it = m_children.rbegin(); it != m_children.rend(); ++it)
    {
        if ((*it)->HandleEvent(event))
        {
            return true;
        }
    }

    // Handle events
    switch (event.type)
    {
        case UIEventType::MouseEnter:
            m_hovered = true;
            if (m_onHover) m_onHover(event);
            return true;

        case UIEventType::MouseLeave:
            m_hovered = false;
            m_pressed = false;
            return true;

        case UIEventType::MouseDown:
            m_pressed = true;
            return true;

        case UIEventType::MouseUp:
            if (m_pressed)
            {
                m_pressed = false;
                if (m_onClick)
                {
                    UIEvent clickEvent = event;
                    clickEvent.type = UIEventType::Click;
                    m_onClick(clickEvent);
                }
            }
            return true;

        default:
            break;
    }

    return false;
}

Widget* Widget::HitTest(const Vec2& point)
{
    if (m_visibility != Visibility::Visible || !m_interactive)
    {
        return nullptr;
    }

    Rect globalRect = GetGlobalRect();
    if (!globalRect.Contains(point))
    {
        return nullptr;
    }

    // Check children (reverse for front-to-back)
    for (auto it = m_children.rbegin(); it != m_children.rend(); ++it)
    {
        Widget* hit = (*it)->HitTest(point);
        if (hit) return hit;
    }

    return this;
}

void Widget::OnRender(UIRenderer& renderer)
{
    // Base widget renders background if set
    // Derived classes override this
}

void Widget::OnLayoutChildren()
{
    // Default: no automatic layout
}

} // namespace RVX::UI
