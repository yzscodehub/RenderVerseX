/**
 * @file ScrollView.h
 * @brief Clipped scrollable container widget
 */

#pragma once

#include "UI/Widgets/Panel.h"

#include <algorithm>
#include <functional>
#include <string>
#include <utility>

namespace RVX::UI
{

/**
 * @brief Scrollable viewport with clipped content and lightweight scrollbars.
 */
class ScrollView : public Panel
{
public:
    using Ptr = std::shared_ptr<ScrollView>;
    using ScrollChangedCallback = std::function<void(const Vec2&)>;

    ScrollView();

    const char* GetTypeName() const override { return "ScrollView"; }

    // =========================================================================
    // Content
    // =========================================================================

    Panel::Ptr GetContentPanel() const { return m_contentPanel; }
    void AddContentChild(Widget::Ptr child);
    void ClearContentChildren();

    const Vec2& GetContentSize() const { return m_contentSize; }
    void SetContentSize(const Vec2& size);
    void SetContentSize(float width, float height) { SetContentSize(Vec2(width, height)); }

    // =========================================================================
    // Scrolling
    // =========================================================================

    const Vec2& GetScrollOffset() const { return m_scrollOffset; }
    Vec2 GetMaxScrollOffset() const;
    void SetScrollOffset(const Vec2& offset);
    void SetScrollOffset(float x, float y) { SetScrollOffset(Vec2(x, y)); }
    void ScrollBy(const Vec2& delta);

    float GetWheelStep() const { return m_wheelStep; }
    void SetWheelStep(float step) { m_wheelStep = std::max(1.0f, step); }

    void SetOnScrollChanged(ScrollChangedCallback callback)
    {
        m_onScrollChanged = std::move(callback);
    }

    // =========================================================================
    // Scrollbars
    // =========================================================================

    bool GetShowScrollbars() const { return m_showScrollbars; }
    void SetShowScrollbars(bool show);

    bool IsVerticalScrollbarVisible() const { return m_verticalScrollbarVisible; }
    bool IsHorizontalScrollbarVisible() const { return m_horizontalScrollbarVisible; }

    void SetScrollbarWidth(float width);
    float GetScrollbarWidth() const { return m_scrollbarWidth; }

    void SetScrollbarNamePrefix(const std::string& prefix);

    // =========================================================================
    // Rendering and Input
    // =========================================================================

    void Render(UIRenderer& renderer) override;
    bool HandleEvent(const UIEvent& event) override;
    Widget* HitTest(const Vec2& point) override;

    // =========================================================================
    // Factory
    // =========================================================================

    static Ptr Create()
    {
        return std::make_shared<ScrollView>();
    }

private:
    Rect GetViewportRectLocal() const;
    Rect GetViewportRectGlobal() const;
    Vec2 ClampScrollOffset(const Vec2& offset) const;
    void UpdateInternalWidgets();
    void UpdateScrollbarWidgets(const Rect& viewportLocal);
    void SetScrollbarWidgetsVisible(bool verticalVisible, bool horizontalVisible);

    Panel::Ptr m_contentPanel;
    Panel::Ptr m_verticalTrack;
    Panel::Ptr m_verticalThumb;
    Panel::Ptr m_horizontalTrack;
    Panel::Ptr m_horizontalThumb;

    Vec2 m_contentSize{0.0f};
    Vec2 m_scrollOffset{0.0f};
    float m_wheelStep = 48.0f;
    float m_scrollbarWidth = 8.0f;
    bool m_showScrollbars = true;
    bool m_verticalScrollbarVisible = false;
    bool m_horizontalScrollbarVisible = false;

    UIColor m_scrollbarTrackColor = UIColor(0.10f, 0.11f, 0.12f, 0.78f);
    UIColor m_scrollbarThumbColor = UIColor(0.30f, 0.48f, 0.72f, 0.72f);
    ScrollChangedCallback m_onScrollChanged;
};

} // namespace RVX::UI
