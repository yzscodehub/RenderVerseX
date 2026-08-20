/**
 * @file ScrollView.cpp
 * @brief ScrollView widget implementation
 */

#include "UI/Widgets/ScrollView.h"

#include "UI/UIRenderer.h"

#include <algorithm>
#include <cmath>

namespace RVX::UI
{
namespace
{
    constexpr float kMinScrollbarThumbSize = 18.0f;

    bool SameOffset(const Vec2& lhs, const Vec2& rhs)
    {
        return std::abs(lhs.x - rhs.x) < 0.001f &&
               std::abs(lhs.y - rhs.y) < 0.001f;
    }
}

ScrollView::ScrollView()
{
    SetInteractive(true);
    SetClipChildren(false);
    SetBackgroundColor(UIColor::Transparent());
    SetBorderWidth(0.0f);

    m_contentPanel = Panel::Create();
    m_contentPanel->SetName("ScrollView.Content");
    m_contentPanel->SetBackgroundColor(UIColor::Transparent());
    m_contentPanel->SetBorderWidth(0.0f);
    m_contentPanel->SetClipChildren(false);
    AddChild(m_contentPanel);

    m_verticalTrack = Panel::Create();
    m_verticalTrack->SetName("ScrollView.Scrollbar.Track");
    m_verticalTrack->SetBackgroundColor(m_scrollbarTrackColor);
    m_verticalTrack->SetBorderWidth(0.0f);
    m_verticalTrack->SetInteractive(false);
    AddChild(m_verticalTrack);

    m_verticalThumb = Panel::Create();
    m_verticalThumb->SetName("ScrollView.Scrollbar.Thumb");
    m_verticalThumb->SetBackgroundColor(m_scrollbarThumbColor);
    m_verticalThumb->SetBorderWidth(0.0f);
    m_verticalThumb->SetInteractive(false);
    AddChild(m_verticalThumb);

    m_horizontalTrack = Panel::Create();
    m_horizontalTrack->SetName("ScrollView.HorizontalScrollbar.Track");
    m_horizontalTrack->SetBackgroundColor(m_scrollbarTrackColor);
    m_horizontalTrack->SetBorderWidth(0.0f);
    m_horizontalTrack->SetInteractive(false);
    AddChild(m_horizontalTrack);

    m_horizontalThumb = Panel::Create();
    m_horizontalThumb->SetName("ScrollView.HorizontalScrollbar.Thumb");
    m_horizontalThumb->SetBackgroundColor(m_scrollbarThumbColor);
    m_horizontalThumb->SetBorderWidth(0.0f);
    m_horizontalThumb->SetInteractive(false);
    AddChild(m_horizontalThumb);

    SetScrollbarWidgetsVisible(false, false);
}

void ScrollView::AddContentChild(Widget::Ptr child)
{
    if (m_contentPanel && child)
    {
        m_contentPanel->AddChild(std::move(child));
    }
}

void ScrollView::ClearContentChildren()
{
    if (m_contentPanel)
    {
        m_contentPanel->RemoveAllChildren();
    }
}

void ScrollView::SetContentSize(const Vec2& size)
{
    m_contentSize.x = std::max(0.0f, size.x);
    m_contentSize.y = std::max(0.0f, size.y);
    const Vec2 previous = m_scrollOffset;
    UpdateInternalWidgets();
    if (!SameOffset(previous, m_scrollOffset) && m_onScrollChanged)
    {
        m_onScrollChanged(m_scrollOffset);
    }
}

Vec2 ScrollView::GetMaxScrollOffset() const
{
    const Rect viewport = GetViewportRectLocal();
    return Vec2(std::max(0.0f, m_contentSize.x - viewport.width),
                std::max(0.0f, m_contentSize.y - viewport.height));
}

void ScrollView::SetScrollOffset(const Vec2& offset)
{
    const Vec2 previous = m_scrollOffset;
    m_scrollOffset = ClampScrollOffset(offset);
    UpdateInternalWidgets();
    if (!SameOffset(previous, m_scrollOffset) && m_onScrollChanged)
    {
        m_onScrollChanged(m_scrollOffset);
    }
}

void ScrollView::ScrollBy(const Vec2& delta)
{
    SetScrollOffset(Vec2(m_scrollOffset.x + delta.x,
                         m_scrollOffset.y + delta.y));
}

void ScrollView::SetShowScrollbars(bool show)
{
    const Vec2 previous = m_scrollOffset;
    m_showScrollbars = show;
    UpdateInternalWidgets();
    if (!SameOffset(previous, m_scrollOffset) && m_onScrollChanged)
    {
        m_onScrollChanged(m_scrollOffset);
    }
}

void ScrollView::SetScrollbarWidth(float width)
{
    const Vec2 previous = m_scrollOffset;
    m_scrollbarWidth = std::max(1.0f, width);
    UpdateInternalWidgets();
    if (!SameOffset(previous, m_scrollOffset) && m_onScrollChanged)
    {
        m_onScrollChanged(m_scrollOffset);
    }
}

void ScrollView::SetScrollbarNamePrefix(const std::string& prefix)
{
    if (prefix.empty())
    {
        return;
    }

    m_verticalTrack->SetName(prefix + ".Track");
    m_verticalThumb->SetName(prefix + ".Thumb");
    m_horizontalTrack->SetName(prefix + ".HorizontalTrack");
    m_horizontalThumb->SetName(prefix + ".HorizontalThumb");
}

void ScrollView::Render(UIRenderer& renderer)
{
    if (m_visibility != Visibility::Visible)
    {
        return;
    }

    UpdateInternalWidgets();
    Panel::OnRender(renderer);

    if (m_contentPanel)
    {
        renderer.PushClipRect(GetViewportRectGlobal());
        m_contentPanel->Render(renderer);
        renderer.PopClipRect();
    }

    if (m_verticalScrollbarVisible)
    {
        m_verticalTrack->Render(renderer);
        m_verticalThumb->Render(renderer);
    }
    if (m_horizontalScrollbarVisible)
    {
        m_horizontalTrack->Render(renderer);
        m_horizontalThumb->Render(renderer);
    }
}

bool ScrollView::HandleEvent(const UIEvent& event)
{
    if (!m_interactive || m_visibility != Visibility::Visible)
    {
        return false;
    }

    if (event.type == UIEventType::Scroll)
    {
        const Rect bounds = GetGlobalRect();
        if (!bounds.Contains(event.position))
        {
            return false;
        }

        if (m_contentPanel && m_contentPanel->HandleEvent(event))
        {
            return true;
        }

        const Vec2 previous = m_scrollOffset;
        ScrollBy(Vec2(-event.delta.x * m_wheelStep,
                      -event.delta.y * m_wheelStep));
        return !SameOffset(previous, m_scrollOffset) ||
               GetMaxScrollOffset().x > 0.0f ||
               GetMaxScrollOffset().y > 0.0f;
    }

    return Panel::HandleEvent(event);
}

Widget* ScrollView::HitTest(const Vec2& point)
{
    if (m_visibility != Visibility::Visible || !m_interactive)
    {
        return nullptr;
    }

    if (!GetGlobalRect().Contains(point))
    {
        return nullptr;
    }

    UpdateInternalWidgets();

    if (m_verticalScrollbarVisible)
    {
        if (Widget* hit = m_verticalThumb->HitTest(point))
        {
            return hit;
        }
        if (Widget* hit = m_verticalTrack->HitTest(point))
        {
            return hit;
        }
    }
    if (m_horizontalScrollbarVisible)
    {
        if (Widget* hit = m_horizontalThumb->HitTest(point))
        {
            return hit;
        }
        if (Widget* hit = m_horizontalTrack->HitTest(point))
        {
            return hit;
        }
    }

    if (GetViewportRectGlobal().Contains(point) && m_contentPanel)
    {
        Widget* contentHit = m_contentPanel->HitTest(point);
        if (contentHit && contentHit != m_contentPanel.get())
        {
            return contentHit;
        }
    }

    return this;
}

Rect ScrollView::GetViewportRectLocal() const
{
    float viewportWidth = std::max(0.0f, GetWidth());
    float viewportHeight = std::max(0.0f, GetHeight());

    if (m_showScrollbars && m_contentSize.y > viewportHeight)
    {
        viewportWidth = std::max(0.0f, viewportWidth - m_scrollbarWidth);
    }
    if (m_showScrollbars && m_contentSize.x > viewportWidth)
    {
        viewportHeight = std::max(0.0f, viewportHeight - m_scrollbarWidth);
        if (m_contentSize.y > viewportHeight && viewportWidth >= GetWidth())
        {
            viewportWidth = std::max(0.0f, viewportWidth - m_scrollbarWidth);
        }
    }

    return Rect(0.0f, 0.0f, viewportWidth, viewportHeight);
}

Rect ScrollView::GetViewportRectGlobal() const
{
    const Rect bounds = GetGlobalRect();
    const Rect viewport = GetViewportRectLocal();
    return Rect(bounds.x + viewport.x,
                bounds.y + viewport.y,
                viewport.width,
                viewport.height);
}

Vec2 ScrollView::ClampScrollOffset(const Vec2& offset) const
{
    const Vec2 maxOffset = GetMaxScrollOffset();
    return Vec2(std::clamp(offset.x, 0.0f, maxOffset.x),
                std::clamp(offset.y, 0.0f, maxOffset.y));
}

void ScrollView::UpdateInternalWidgets()
{
    const Rect viewport = GetViewportRectLocal();
    m_verticalScrollbarVisible = m_showScrollbars && m_contentSize.y > viewport.height;
    m_horizontalScrollbarVisible = m_showScrollbars && m_contentSize.x > viewport.width;
    m_scrollOffset = ClampScrollOffset(m_scrollOffset);

    if (m_contentPanel)
    {
        m_contentPanel->SetPosition(viewport.x - m_scrollOffset.x,
                                    viewport.y - m_scrollOffset.y);
        m_contentPanel->SetSize(std::max(viewport.width, m_contentSize.x),
                                std::max(viewport.height, m_contentSize.y));
    }

    UpdateScrollbarWidgets(viewport);
    SetScrollbarWidgetsVisible(m_verticalScrollbarVisible, m_horizontalScrollbarVisible);
}

void ScrollView::UpdateScrollbarWidgets(const Rect& viewportLocal)
{
    if (m_verticalScrollbarVisible)
    {
        const float trackX = viewportLocal.Right();
        const float trackHeight = viewportLocal.height;
        m_verticalTrack->SetPosition(trackX, 0.0f);
        m_verticalTrack->SetSize(m_scrollbarWidth, trackHeight);

        const float visibleRatio = m_contentSize.y > 0.0f
                                       ? std::min(1.0f, viewportLocal.height / m_contentSize.y)
                                       : 1.0f;
        const float thumbHeight =
            std::min(trackHeight, std::max(kMinScrollbarThumbSize, trackHeight * visibleRatio));
        const float maxOffsetY = std::max(1.0f, GetMaxScrollOffset().y);
        const float scrollRatio = m_scrollOffset.y / maxOffsetY;
        const float thumbY = (trackHeight - thumbHeight) * scrollRatio;

        m_verticalThumb->SetPosition(trackX, thumbY);
        m_verticalThumb->SetSize(m_scrollbarWidth, thumbHeight);
    }

    if (m_horizontalScrollbarVisible)
    {
        const float trackY = viewportLocal.Bottom();
        const float trackWidth = viewportLocal.width;
        m_horizontalTrack->SetPosition(0.0f, trackY);
        m_horizontalTrack->SetSize(trackWidth, m_scrollbarWidth);

        const float visibleRatio = m_contentSize.x > 0.0f
                                       ? std::min(1.0f, viewportLocal.width / m_contentSize.x)
                                       : 1.0f;
        const float thumbWidth =
            std::min(trackWidth, std::max(kMinScrollbarThumbSize, trackWidth * visibleRatio));
        const float maxOffsetX = std::max(1.0f, GetMaxScrollOffset().x);
        const float scrollRatio = m_scrollOffset.x / maxOffsetX;
        const float thumbX = (trackWidth - thumbWidth) * scrollRatio;

        m_horizontalThumb->SetPosition(thumbX, trackY);
        m_horizontalThumb->SetSize(thumbWidth, m_scrollbarWidth);
    }
}

void ScrollView::SetScrollbarWidgetsVisible(bool verticalVisible, bool horizontalVisible)
{
    m_verticalTrack->SetVisible(verticalVisible);
    m_verticalThumb->SetVisible(verticalVisible);
    m_horizontalTrack->SetVisible(horizontalVisible);
    m_horizontalThumb->SetVisible(horizontalVisible);
}

} // namespace RVX::UI
