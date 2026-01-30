/**
 * @file Label.cpp
 * @brief Label widget implementation
 */

#include "UI/Widgets/Label.h"

namespace RVX::UI
{

Label::Label()
{
    SetInteractive(false);
}

Label::Label(const std::string& text)
    : m_text(text)
{
    SetInteractive(false);
}

void Label::SetText(const std::string& text)
{
    m_text = text;
    MarkLayoutDirty();
}

Vec2 Label::MeasureContent() const
{
    // TODO: Calculate text size based on font metrics
    return Vec2(m_text.length() * m_style.fontSize * 0.6f, m_style.fontSize);
}

void Label::OnRender(UIRenderer& renderer)
{
    // TODO: Render text using UIRenderer
}

} // namespace RVX::UI
