/**
 * @file UICanvas.cpp
 * @brief UICanvas implementation
 */

#include "UI/UICanvas.h"

namespace RVX::UI
{

void UICanvas::Initialize(float width, float height)
{
    m_width = width;
    m_height = height;

    // Create default root widget
    m_root = std::make_shared<Widget>();
    m_root->SetName("Root");
    m_root->SetSize(width, height);
}

void UICanvas::Shutdown()
{
    m_root.reset();
    m_focusedWidget = nullptr;
    m_hoveredWidget = nullptr;
    m_pressedWidget = nullptr;
}

void UICanvas::SetSize(float width, float height)
{
    m_width = width;
    m_height = height;
    
    if (m_root)
    {
        m_root->SetSize(width, height);
        m_root->MarkLayoutDirty();
    }
}

void UICanvas::AddWidget(Widget::Ptr widget)
{
    if (m_root && widget)
    {
        m_root->AddChild(std::move(widget));
    }
}

void UICanvas::RemoveWidget(Widget::Ptr widget)
{
    if (m_root && widget)
    {
        m_root->RemoveChild(std::move(widget));
    }
}

Widget::Ptr UICanvas::FindWidget(const std::string& name) const
{
    if (!m_root) return nullptr;
    return m_root->FindChildRecursive(name);
}

void UICanvas::SetFocusedWidget(Widget* widget)
{
    if (m_focusedWidget == widget) return;

    if (m_focusedWidget)
    {
        m_focusedWidget->Blur();
    }

    m_focusedWidget = widget;

    if (m_focusedWidget)
    {
        m_focusedWidget->Focus();
    }
}

void UICanvas::ClearFocus()
{
    SetFocusedWidget(nullptr);
}

void UICanvas::Update(float deltaTime)
{
    if (!m_enabled || !m_root) return;

    UpdateLayout();
}

void UICanvas::UpdateLayout()
{
    if (m_root && m_root->IsLayoutDirty())
    {
        m_root->Layout();
    }
}

void UICanvas::Render(UIRenderer& renderer)
{
    if (!m_enabled || !m_root) return;

    m_root->Render(renderer);
}

bool UICanvas::HandleEvent(const UIEvent& event)
{
    if (!m_enabled || !m_root) return false;

    // Transform position by scale factor
    UIEvent scaledEvent = event;
    scaledEvent.position /= m_scaleFactor;

    switch (event.type)
    {
        case UIEventType::MouseMove:
        {
            Widget* newHovered = HitTest(scaledEvent.position);
            
            if (newHovered != m_hoveredWidget)
            {
                if (m_hoveredWidget)
                {
                    UIEvent leaveEvent;
                    leaveEvent.type = UIEventType::MouseLeave;
                    m_hoveredWidget->HandleEvent(leaveEvent);
                }
                
                m_hoveredWidget = newHovered;
                
                if (m_hoveredWidget)
                {
                    UIEvent enterEvent;
                    enterEvent.type = UIEventType::MouseEnter;
                    m_hoveredWidget->HandleEvent(enterEvent);
                }
            }
            
            return m_hoveredWidget != nullptr;
        }

        case UIEventType::MouseDown:
        {
            Widget* clicked = HitTest(scaledEvent.position);
            m_pressedWidget = clicked;
            
            if (clicked)
            {
                SetFocusedWidget(clicked);
                return clicked->HandleEvent(scaledEvent);
            }
            else
            {
                ClearFocus();
            }
            return false;
        }

        case UIEventType::MouseUp:
        {
            if (m_pressedWidget)
            {
                m_pressedWidget->HandleEvent(scaledEvent);
                m_pressedWidget = nullptr;
                return true;
            }
            return false;
        }

        case UIEventType::KeyDown:
        case UIEventType::KeyUp:
        case UIEventType::TextInput:
        {
            if (m_focusedWidget)
            {
                return m_focusedWidget->HandleEvent(scaledEvent);
            }
            return false;
        }

        default:
            return m_root->HandleEvent(scaledEvent);
    }
}

Widget* UICanvas::HitTest(const Vec2& position) const
{
    if (!m_root) return nullptr;
    return m_root->HitTest(position);
}

} // namespace RVX::UI
