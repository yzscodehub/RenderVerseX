/**
 * @file Widget.h
 * @brief Base widget class for UI elements
 */

#pragma once

#include "UI/UITypes.h"
#include <memory>
#include <vector>
#include <string>

namespace RVX::UI
{

class UICanvas;
class UIRenderer;

/**
 * @brief Base class for all UI widgets
 * 
 * Widgets form a tree hierarchy for layout and rendering.
 * Each widget has transform, style, and input handling.
 */
class Widget : public std::enable_shared_from_this<Widget>
{
public:
    using Ptr = std::shared_ptr<Widget>;
    using WeakPtr = std::weak_ptr<Widget>;

    Widget() = default;
    virtual ~Widget() = default;

    // =========================================================================
    // Identity
    // =========================================================================

    const std::string& GetName() const { return m_name; }
    void SetName(const std::string& name) { m_name = name; }

    uint64 GetId() const { return m_id; }

    virtual const char* GetTypeName() const { return "Widget"; }

    // =========================================================================
    // Hierarchy
    // =========================================================================

    Widget* GetParent() const { return m_parent; }

    void AddChild(Ptr child);
    void RemoveChild(Ptr child);
    void RemoveFromParent();
    void RemoveAllChildren();

    const std::vector<Ptr>& GetChildren() const { return m_children; }
    size_t GetChildCount() const { return m_children.size(); }

    Ptr FindChild(const std::string& name) const;
    Ptr FindChildRecursive(const std::string& name) const;

    // =========================================================================
    // Transform
    // =========================================================================

    const Vec2& GetPosition() const { return m_position; }
    void SetPosition(const Vec2& position) { m_position = position; MarkLayoutDirty(); }
    void SetPosition(float x, float y) { SetPosition(Vec2(x, y)); }

    const Vec2& GetSize() const { return m_size; }
    void SetSize(const Vec2& size) { m_size = size; MarkLayoutDirty(); }
    void SetSize(float width, float height) { SetSize(Vec2(width, height)); }

    float GetWidth() const { return m_size.x; }
    void SetWidth(float width) { m_size.x = width; MarkLayoutDirty(); }

    float GetHeight() const { return m_size.y; }
    void SetHeight(float height) { m_size.y = height; MarkLayoutDirty(); }

    Anchor GetAnchor() const { return m_anchor; }
    void SetAnchor(Anchor anchor) { m_anchor = anchor; MarkLayoutDirty(); }

    const Vec2& GetPivot() const { return m_pivot; }
    void SetPivot(const Vec2& pivot) { m_pivot = pivot; MarkLayoutDirty(); }

    Rect GetLocalRect() const { return Rect(0, 0, m_size.x, m_size.y); }
    Rect GetGlobalRect() const;

    // =========================================================================
    // Style
    // =========================================================================

    const Style& GetStyle() const { return m_style; }
    Style& GetStyle() { return m_style; }
    void SetStyle(const Style& style) { m_style = style; }

    // =========================================================================
    // Visibility
    // =========================================================================

    Visibility GetVisibility() const { return m_visibility; }
    void SetVisibility(Visibility visibility) { m_visibility = visibility; }

    bool IsVisible() const { return m_visibility == Visibility::Visible; }
    void SetVisible(bool visible) 
    { 
        m_visibility = visible ? Visibility::Visible : Visibility::Hidden; 
    }

    // =========================================================================
    // Interaction
    // =========================================================================

    bool IsInteractive() const { return m_interactive; }
    void SetInteractive(bool interactive) { m_interactive = interactive; }

    bool IsFocused() const { return m_focused; }
    void Focus();
    void Blur();

    bool IsHovered() const { return m_hovered; }
    bool IsPressed() const { return m_pressed; }

    // =========================================================================
    // Events
    // =========================================================================

    void SetOnClick(EventCallback callback) { m_onClick = std::move(callback); }
    void SetOnHover(EventCallback callback) { m_onHover = std::move(callback); }
    void SetOnFocus(EventCallback callback) { m_onFocus = std::move(callback); }

    // =========================================================================
    // Layout
    // =========================================================================

    virtual void Layout();
    virtual Vec2 MeasureContent() const { return m_size; }

    void MarkLayoutDirty() { m_layoutDirty = true; }
    bool IsLayoutDirty() const { return m_layoutDirty; }

    // =========================================================================
    // Rendering
    // =========================================================================

    virtual void Render(UIRenderer& renderer);

    // =========================================================================
    // Input
    // =========================================================================

    virtual bool HandleEvent(const UIEvent& event);
    Widget* HitTest(const Vec2& point);

protected:
    virtual void OnRender(UIRenderer& renderer);
    virtual void OnLayoutChildren();

    std::string m_name;
    uint64 m_id = 0;
    
    Widget* m_parent = nullptr;
    std::vector<Ptr> m_children;

    Vec2 m_position{0.0f};
    Vec2 m_size{100.0f, 100.0f};
    Vec2 m_pivot{0.0f};
    Anchor m_anchor = Anchor::TopLeft;

    Style m_style;
    Visibility m_visibility = Visibility::Visible;

    bool m_interactive = true;
    bool m_focused = false;
    bool m_hovered = false;
    bool m_pressed = false;
    bool m_layoutDirty = true;

    EventCallback m_onClick;
    EventCallback m_onHover;
    EventCallback m_onFocus;

    static uint64 s_nextId;
};

} // namespace RVX::UI
