/**
 * @file Label.h
 * @brief Text label widget
 */

#pragma once

#include "UI/Widget.h"

namespace RVX::UI
{

/**
 * @brief Simple text label widget
 */
class Label : public Widget
{
public:
    using Ptr = std::shared_ptr<Label>;

    Label();
    explicit Label(const std::string& text);

    const char* GetTypeName() const override { return "Label"; }

    // =========================================================================
    // Text
    // =========================================================================

    const std::string& GetText() const { return m_text; }
    void SetText(const std::string& text);

    // =========================================================================
    // Text Formatting
    // =========================================================================

    float GetFontSize() const { return m_style.fontSize; }
    void SetFontSize(float size) { m_style.fontSize = size; }

    const UIColor& GetTextColor() const { return m_style.textColor; }
    void SetTextColor(const UIColor& color) { m_style.textColor = color; }

    TextAlign GetTextAlign() const { return m_textAlign; }
    void SetTextAlign(TextAlign align) { m_textAlign = align; }

    VerticalAlign GetVerticalAlign() const { return m_verticalAlign; }
    void SetVerticalAlign(VerticalAlign align) { m_verticalAlign = align; }

    // =========================================================================
    // Word Wrap
    // =========================================================================

    bool GetWordWrap() const { return m_wordWrap; }
    void SetWordWrap(bool wrap) { m_wordWrap = wrap; }

    // =========================================================================
    // Size
    // =========================================================================

    Vec2 MeasureContent() const override;

    // =========================================================================
    // Factory
    // =========================================================================

    static Ptr Create(const std::string& text = "")
    {
        return std::make_shared<Label>(text);
    }

protected:
    void OnRender(UIRenderer& renderer) override;

private:
    std::string m_text;
    TextAlign m_textAlign = TextAlign::Left;
    VerticalAlign m_verticalAlign = VerticalAlign::Top;
    bool m_wordWrap = false;
};

} // namespace RVX::UI
