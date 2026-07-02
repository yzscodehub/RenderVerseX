/**
 * @file Label.cpp
 * @brief Label widget implementation
 */

#include "UI/Widgets/Label.h"

#include "UI/UIRenderer.h"
#include "UI/UITextOverflow.h"

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
    const UITextMetrics metrics =
        UIFontFallbackChain::Default().MeasureText(m_text, m_style.fontSize);
    return Vec2(metrics.width, metrics.height);
}

std::string Label::ResolveRenderText(UIRenderer& renderer,
                                     const Rect& bounds) const
{
    if (m_text.empty() ||
        m_wordWrap ||
        m_overflowMode == TextOverflowMode::Clip ||
        bounds.width <= 0.0f)
    {
        return m_text;
    }

    return ResolveSingleLineTextOverflow(renderer,
                                         m_text,
                                         bounds.width,
                                         m_style.fontSize,
                                         m_overflowMode);
}

void Label::OnRender(UIRenderer& renderer)
{
    const Rect bounds = GetGlobalRect();
    const std::string renderText = ResolveRenderText(renderer, bounds);
    renderer.PushClipRect(bounds);
    renderer.DrawText(renderText,
                      bounds,
                      m_style.fontSize,
                      m_style.textColor,
                      m_textAlign,
                      m_verticalAlign);
    renderer.PopClipRect();
}

} // namespace RVX::UI
