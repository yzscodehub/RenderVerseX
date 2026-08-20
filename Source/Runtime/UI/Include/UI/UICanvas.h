/**
 * @file UICanvas.h
 * @brief UI canvas for managing and rendering UI trees
 */

#pragma once

#include "UI/Widget.h"

#include <memory>
#include <string>

namespace RVX::UI
{

class UIRenderer;

/**
 * @brief UI Canvas manages a tree of UI widgets
 * 
 * The canvas handles:
 * - Widget hierarchy
 * - Layout calculation
 * - Input dispatch
 * - Rendering coordination
 */
class UICanvas
{
public:
    using Ptr = std::shared_ptr<UICanvas>;

    UICanvas() = default;
    ~UICanvas() = default;

    // =========================================================================
    // Lifecycle
    // =========================================================================

    void Initialize(float width, float height);
    void Shutdown();

    // =========================================================================
    // Size
    // =========================================================================

    float GetWidth() const { return m_width; }
    float GetHeight() const { return m_height; }
    Vec2 GetSize() const { return Vec2(m_width, m_height); }

    void SetSize(float width, float height);
    void Resize(float width, float height) { SetSize(width, height); }

    // =========================================================================
    // Root Widget
    // =========================================================================

    Widget::Ptr GetRoot() const { return m_root; }

    void SetRoot(Widget::Ptr root);

    /**
     * @brief Add a widget to the canvas root
     */
    void AddWidget(Widget::Ptr widget);

    /**
     * @brief Remove a widget from the canvas
     */
    void RemoveWidget(Widget::Ptr widget);

    /**
     * @brief Find a widget by name
     */
    Widget::Ptr FindWidget(const std::string& name) const;

    // =========================================================================
    // Focus Management
    // =========================================================================

    Widget* GetFocusedWidget() const { return m_focusedWidget; }
    void SetFocusedWidget(Widget* widget);
    void ClearFocus();
    Widget* GetFocusScopeRoot() const { return m_focusScopeRoot; }
    void SetFocusScopeRoot(Widget* widget);
    void ClearFocusScopeRoot();
    bool FocusNextWidget(bool reverse = false);

    // =========================================================================
    // Update & Render
    // =========================================================================

    /**
     * @brief Update layout and animations
     */
    void Update(float deltaTime);

    /**
     * @brief Render the UI
     */
    void Render(UIRenderer& renderer);

    // =========================================================================
    // Input
    // =========================================================================

    /**
     * @brief Handle input event
     * @return True if event was handled by UI
     */
    bool HandleEvent(const UIEvent& event);

    /**
     * @brief Perform hit test at screen position
     */
    Widget* HitTest(const Vec2& position) const;

    // =========================================================================
    // Options
    // =========================================================================

    void SetEnabled(bool enabled) { m_enabled = enabled; }
    bool IsEnabled() const { return m_enabled; }

    /**
     * @brief Set scale factor for HiDPI displays
     */
    void SetScaleFactor(float scale) { m_scaleFactor = scale; }
    float GetScaleFactor() const { return m_scaleFactor; }

private:
    void UpdateLayout();
    Widget* ResolveFocusTraversalRoot();
    bool TryDispatchReleaseToRebuiltPressedWidget(const UIEvent& event);

    float m_width = 0.0f;
    float m_height = 0.0f;
    float m_scaleFactor = 1.0f;
    bool m_enabled = true;

    Widget::Ptr m_root;
    Widget* m_focusScopeRoot = nullptr;
    Widget* m_focusedWidget = nullptr;
    Widget* m_hoveredWidget = nullptr;
    Widget* m_pressedWidget = nullptr;
    std::string m_rebuiltPressedWidgetName;
};

} // namespace RVX::UI
