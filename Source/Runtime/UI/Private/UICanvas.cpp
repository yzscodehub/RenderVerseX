/**
 * @file UICanvas.cpp
 * @brief UICanvas implementation
 */

#include "UI/UICanvas.h"

#include "UI/UIContext.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace RVX::UI
{
namespace
{
    bool ContainsWidget(const Widget* root, const Widget* widget)
    {
        if (!root || !widget)
        {
            return false;
        }
        if (root == widget)
        {
            return true;
        }

        for (const Widget::Ptr& child : root->GetChildren())
        {
            if (ContainsWidget(child.get(), widget))
            {
                return true;
            }
        }
        return false;
    }

    void CollectTabStops(const Widget* root, Widget* widget, std::vector<Widget*>& order)
    {
        if (!widget || widget->GetVisibility() != Visibility::Visible)
        {
            return;
        }

        const bool isRoot = widget == root;
        if (!isRoot && !widget->IsInteractive())
        {
            return;
        }

        if (!isRoot && widget->IsTabStop() && widget->GetWidth() > 0.0f &&
            widget->GetHeight() > 0.0f)
        {
            order.push_back(widget);
        }

        for (const Widget::Ptr& child : widget->GetChildren())
        {
            CollectTabStops(root, child.get(), order);
        }
    }

    Widget* FindSelfOrAncestorByName(Widget* widget, const std::string& name)
    {
        for (Widget* current = widget; current; current = current->GetParent())
        {
            if (current->GetName() == name)
            {
                return current;
            }
        }
        return nullptr;
    }
}

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
    m_focusScopeRoot = nullptr;
    m_focusedWidget = nullptr;
    m_hoveredWidget = nullptr;
    m_pressedWidget = nullptr;
    m_rebuiltPressedWidgetName.clear();
}

void UICanvas::SetRoot(Widget::Ptr root)
{
    m_root = std::move(root);
    m_focusScopeRoot = nullptr;
    m_focusedWidget = nullptr;
    m_hoveredWidget = nullptr;
    m_pressedWidget = nullptr;
    m_rebuiltPressedWidgetName.clear();
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
        Widget* removedWidget = widget.get();
        if (ContainsWidget(removedWidget, m_focusedWidget))
        {
            ClearFocus();
        }
        if (ContainsWidget(removedWidget, m_focusScopeRoot))
        {
            ClearFocusScopeRoot();
        }
        if (ContainsWidget(removedWidget, m_hoveredWidget))
        {
            m_hoveredWidget = nullptr;
        }
        if (ContainsWidget(removedWidget, m_pressedWidget))
        {
            m_rebuiltPressedWidgetName =
                m_pressedWidget ? m_pressedWidget->GetName() : std::string{};
            m_pressedWidget = nullptr;
        }
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
    Widget* focusScopeRoot = ResolveFocusTraversalRoot();
    if (widget && focusScopeRoot && focusScopeRoot != m_root.get() &&
        !ContainsWidget(focusScopeRoot, widget))
    {
        widget = nullptr;
    }

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

void UICanvas::SetFocusScopeRoot(Widget* widget)
{
    if (widget && (!m_root || !ContainsWidget(m_root.get(), widget)))
    {
        widget = nullptr;
    }

    m_focusScopeRoot = widget;
    if (m_focusScopeRoot && m_focusedWidget &&
        !ContainsWidget(m_focusScopeRoot, m_focusedWidget))
    {
        ClearFocus();
    }
}

void UICanvas::ClearFocusScopeRoot()
{
    m_focusScopeRoot = nullptr;
}

Widget* UICanvas::ResolveFocusTraversalRoot()
{
    if (!m_root)
    {
        m_focusScopeRoot = nullptr;
        return nullptr;
    }

    if (!m_focusScopeRoot)
    {
        return m_root.get();
    }

    if (!ContainsWidget(m_root.get(), m_focusScopeRoot) ||
        m_focusScopeRoot->GetVisibility() != Visibility::Visible)
    {
        m_focusScopeRoot = nullptr;
        return m_root.get();
    }

    return m_focusScopeRoot;
}

bool UICanvas::FocusNextWidget(bool reverse)
{
    Widget* traversalRoot = ResolveFocusTraversalRoot();
    if (!traversalRoot)
    {
        return false;
    }

    std::vector<Widget*> tabStops;
    CollectTabStops(traversalRoot, traversalRoot, tabStops);
    if (tabStops.empty())
    {
        return false;
    }

    auto focusedIt = std::find(tabStops.begin(), tabStops.end(), m_focusedWidget);
    if (focusedIt == tabStops.end())
    {
        SetFocusedWidget(reverse ? tabStops.back() : tabStops.front());
        return true;
    }

    const size_t focusedIndex =
        static_cast<size_t>(std::distance(tabStops.begin(), focusedIt));
    const size_t nextIndex =
        reverse
            ? (focusedIndex == 0u ? tabStops.size() - 1u : focusedIndex - 1u)
            : ((focusedIndex + 1u) % tabStops.size());
    SetFocusedWidget(tabStops[nextIndex]);
    return true;
}

void UICanvas::Update(float deltaTime)
{
    (void)deltaTime;
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
            const bool handledByPressedWidget = m_pressedWidget != nullptr;
            if (m_pressedWidget)
            {
                m_pressedWidget->HandleEvent(scaledEvent);
            }

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
            
            return handledByPressedWidget || m_hoveredWidget != nullptr;
        }

        case UIEventType::MouseDown:
        {
            m_rebuiltPressedWidgetName.clear();
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
                m_rebuiltPressedWidgetName.clear();
                return true;
            }
            return TryDispatchReleaseToRebuiltPressedWidget(scaledEvent);
        }

        case UIEventType::KeyDown:
        case UIEventType::KeyUp:
        case UIEventType::TextInput:
        {
            if (scaledEvent.type == UIEventType::KeyDown &&
                scaledEvent.keyCode == static_cast<int>(RVX_UI_KEY_TAB))
            {
                const bool reverse =
                    (scaledEvent.modifiers & ToMask(UIInputModifier::Shift)) != 0u;
                return FocusNextWidget(reverse);
            }

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

bool UICanvas::TryDispatchReleaseToRebuiltPressedWidget(const UIEvent& event)
{
    if (m_rebuiltPressedWidgetName.empty())
    {
        return false;
    }

    Widget* released = FindSelfOrAncestorByName(HitTest(event.position),
                                                m_rebuiltPressedWidgetName);
    m_rebuiltPressedWidgetName.clear();

    if (!released)
    {
        return false;
    }

    SetFocusedWidget(released);

    UIEvent replayedPress = event;
    replayedPress.type = UIEventType::MouseDown;
    const bool pressHandled = released->HandleEvent(replayedPress);
    const bool releaseHandled = released->HandleEvent(event);
    return pressHandled || releaseHandled;
}

} // namespace RVX::UI
