/**
 * @file EditorIconButton.cpp
 * @brief Native editor icon button widget implementation
 */

#include "Editor/UI/EditorIconButton.h"

#include "Editor/UI/EditorTypography.h"
#include "UI/UIContext.h"
#include "UI/UIRenderer.h"

#include <algorithm>

namespace RVX::Editor
{

EditorIconButton::EditorIconButton()
    : UI::Button("")
{
}

EditorIconButton::EditorIconButton(const std::string& text)
    : UI::Button(text)
{
}

void EditorIconButton::SetSurfaceColors(const UI::UIColor& normal,
                                        const UI::UIColor& hover,
                                        const UI::UIColor& pressed,
                                        const UI::UIColor& disabled)
{
    m_normalColor = normal;
    m_hoverColor = hover;
    m_pressedColor = pressed;
    m_disabledColor = disabled;
}

void EditorIconButton::SetIconColors(const UI::UIColor& normal,
                                     const UI::UIColor& hover,
                                     const UI::UIColor& pressed,
                                     const UI::UIColor& disabled)
{
    m_iconColor = normal;
    m_iconHoverColor = hover;
    m_iconPressedColor = pressed;
    m_iconDisabledColor = disabled;
}

void EditorIconButton::SetFocusIndicator(const UI::UIColor& color, float width)
{
    m_focusColor = color;
    m_focusWidth = width;
}

void EditorIconButton::ApplyActionStyle(
    const UI::UITheme& theme,
    const EditorIconButtonActionStyleDesc& desc)
{
    SetShowText(false);
    SetIconSize(std::max(14.0f, theme.metrics.controlHeight * desc.iconScale));
    GetStyle().fontSize = EditorTypography::GetFontSize(
        theme,
        EditorTypographyRole::Control);
    GetStyle().textColor = desc.active ? theme.colors.text : theme.colors.textMuted;

    const UI::UIColor normalSurface =
        desc.usePanelBackground ? theme.colors.panelBackground
                                : theme.colors.surface;
    SetSurfaceColors(desc.active ? theme.colors.surfaceActive : normalSurface,
                     theme.colors.surfaceHover,
                     theme.colors.accent.WithAlpha(desc.pressedAlpha),
                     normalSurface.WithAlpha(0.35f));
    SetIconColors(desc.active ? theme.colors.text : theme.colors.textMuted,
                  theme.colors.text,
                  theme.colors.text,
                  theme.colors.textMuted.WithAlpha(0.55f));
    SetFocusIndicator(theme.colors.accent,
                      std::max(1.0f, theme.metrics.borderWidth));
}

void EditorIconButton::OnRender(UI::UIRenderer& renderer)
{
    const UI::Rect bounds = GetGlobalRect();
    renderer.DrawRect(bounds, ResolveSurfaceColor());
    if (IsEnabled() && IsFocused())
    {
        renderer.DrawBorder(bounds.Expand(1.0f), m_focusColor.WithAlpha(0.35f), 1.0f);
        renderer.DrawBorder(bounds, m_focusColor, m_focusWidth);
    }

    renderer.PushClipRect(bounds);

    const float availableIconSize =
        std::max(8.0f, std::min(bounds.width, bounds.height) - 8.0f);
    const float iconSize = std::clamp(m_iconSize, 8.0f, availableIconSize);
    const float iconY = bounds.y + (bounds.height - iconSize) * 0.5f;
    UI::Rect iconRect(bounds.x + (bounds.width - iconSize) * 0.5f,
                      iconY,
                      iconSize,
                      iconSize);

    if (m_showText && !GetText().empty())
    {
        const float padding = std::max(4.0f, GetStyle().fontSize * 0.45f);
        iconRect.x = bounds.x + padding;
        const float textX = iconRect.Right() + padding;
        const UI::Rect textRect(textX,
                                bounds.y,
                                std::max(0.0f, bounds.Right() - textX - padding),
                                bounds.height);
        renderer.DrawText(GetText(),
                          textRect,
                          GetStyle().fontSize,
                          IsEnabled() ? GetStyle().textColor : m_iconDisabledColor,
                          UI::TextAlign::Left,
                          UI::VerticalAlign::Middle);
    }

    EditorVectorIconDrawDesc iconDesc;
    iconDesc.name = m_iconName;
    iconDesc.bounds = iconRect;
    iconDesc.color = ResolveIconColor();
    iconDesc.fallbackLabel = GetText();
    iconDesc.fallbackFontSize = GetStyle().fontSize;
    m_lastIconDrawStats = EditorVectorIconLibrary::Draw(renderer, iconDesc);
    renderer.PopClipRect();
}

UI::UIColor EditorIconButton::ResolveSurfaceColor() const
{
    if (!IsEnabled())
    {
        return m_disabledColor;
    }
    if (IsPressed())
    {
        return m_pressedColor;
    }
    if (IsHovered())
    {
        return m_hoverColor;
    }
    return m_normalColor;
}

UI::UIColor EditorIconButton::ResolveIconColor() const
{
    if (!IsEnabled())
    {
        return m_iconDisabledColor;
    }
    if (IsPressed())
    {
        return m_iconPressedColor;
    }
    if (IsHovered())
    {
        return m_iconHoverColor;
    }
    return m_iconColor;
}

} // namespace RVX::Editor
