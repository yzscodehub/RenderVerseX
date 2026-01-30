/**
 * @file Button.h
 * @brief Button widget
 */

#pragma once

#include "UI/Widget.h"

namespace RVX::UI
{

/**
 * @brief Button widget with text label
 */
class Button : public Widget
{
public:
    using Ptr = std::shared_ptr<Button>;

    Button();
    explicit Button(const std::string& text);

    const char* GetTypeName() const override { return "Button"; }

    // =========================================================================
    // Text
    // =========================================================================

    const std::string& GetText() const { return m_text; }
    void SetText(const std::string& text) { m_text = text; }

    // =========================================================================
    // State Colors
    // =========================================================================

    void SetNormalColor(const UIColor& color) { m_normalColor = color; }
    void SetHoverColor(const UIColor& color) { m_hoverColor = color; }
    void SetPressedColor(const UIColor& color) { m_pressedColor = color; }
    void SetDisabledColor(const UIColor& color) { m_disabledColor = color; }

    // =========================================================================
    // Enabled State
    // =========================================================================

    bool IsEnabled() const { return m_enabled; }
    void SetEnabled(bool enabled) { m_enabled = enabled; }

    // =========================================================================
    // Factory
    // =========================================================================

    static Ptr Create(const std::string& text = "Button")
    {
        return std::make_shared<Button>(text);
    }

protected:
    void OnRender(UIRenderer& renderer) override;
    bool HandleEvent(const UIEvent& event) override;

private:
    std::string m_text = "Button";
    bool m_enabled = true;

    UIColor m_normalColor = UIColor(0.2f, 0.2f, 0.25f, 1.0f);
    UIColor m_hoverColor = UIColor(0.3f, 0.3f, 0.35f, 1.0f);
    UIColor m_pressedColor = UIColor(0.15f, 0.15f, 0.2f, 1.0f);
    UIColor m_disabledColor = UIColor(0.3f, 0.3f, 0.3f, 0.5f);
};

} // namespace RVX::UI
