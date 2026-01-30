/**
 * @file Panel.h
 * @brief Container panel widget
 */

#pragma once

#include "UI/Widget.h"

namespace RVX::UI
{

/**
 * @brief Panel container widget
 */
class Panel : public Widget
{
public:
    using Ptr = std::shared_ptr<Panel>;

    Panel() = default;

    const char* GetTypeName() const override { return "Panel"; }

    // =========================================================================
    // Background
    // =========================================================================

    const UIColor& GetBackgroundColor() const { return m_style.backgroundColor; }
    void SetBackgroundColor(const UIColor& color) { m_style.backgroundColor = color; }

    // =========================================================================
    // Border
    // =========================================================================

    float GetBorderWidth() const { return m_style.borderWidth; }
    void SetBorderWidth(float width) { m_style.borderWidth = width; }

    const UIColor& GetBorderColor() const { return m_style.borderColor; }
    void SetBorderColor(const UIColor& color) { m_style.borderColor = color; }

    float GetBorderRadius() const { return m_style.borderRadius; }
    void SetBorderRadius(float radius) { m_style.borderRadius = radius; }

    // =========================================================================
    // Clipping
    // =========================================================================

    bool GetClipChildren() const { return m_clipChildren; }
    void SetClipChildren(bool clip) { m_clipChildren = clip; }

    // =========================================================================
    // Factory
    // =========================================================================

    static Ptr Create()
    {
        return std::make_shared<Panel>();
    }

protected:
    void OnRender(UIRenderer& renderer) override;

private:
    bool m_clipChildren = false;
};

} // namespace RVX::UI
