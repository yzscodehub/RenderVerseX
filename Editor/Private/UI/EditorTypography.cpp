/**
 * @file EditorTypography.cpp
 * @brief Editor typography token implementation
 */

#include "Editor/UI/EditorTypography.h"

#include "UI/UIRenderer.h"
#include "UI/Widgets/Button.h"
#include "UI/Widgets/Label.h"

#include <algorithm>
#include <cmath>

namespace RVX::Editor
{
namespace
{
    constexpr float RVX_EDITOR_LAYOUT_TEXT_WIDTH_SCALE = 1.2f;

    float SnapFontSize(float value)
    {
        if (!std::isfinite(value))
        {
            return 14.0f;
        }
        return std::max(1.0f, std::round(value));
    }

    float ResolveFontSize(const UI::UITheme& theme, EditorTypographyRole role)
    {
        const UI::UIThemeMetrics& metrics = theme.metrics;
        switch (role)
        {
            case EditorTypographyRole::Caption:
            case EditorTypographyRole::Tooltip:
                return SnapFontSize(metrics.smallFontSize);
            case EditorTypographyRole::Status:
                return SnapFontSize(std::max(metrics.smallFontSize,
                                             metrics.fontSize * 0.84f));
            case EditorTypographyRole::Menu:
            case EditorTypographyRole::Toolbar:
            case EditorTypographyRole::PanelTab:
            case EditorTypographyRole::ContextMenu:
                return SnapFontSize(metrics.fontSize * 0.90f);
            case EditorTypographyRole::PanelTitle:
                return SnapFontSize(metrics.fontSize * 0.95f);
            case EditorTypographyRole::ListItem:
            case EditorTypographyRole::PropertyLabel:
            case EditorTypographyRole::PropertyValue:
                return SnapFontSize(metrics.fontSize * 0.92f);
            case EditorTypographyRole::Input:
            case EditorTypographyRole::Control:
                return SnapFontSize(metrics.fontSize * 0.96f);
            case EditorTypographyRole::DialogTitle:
                return SnapFontSize(metrics.largeFontSize);
            case EditorTypographyRole::Body:
            case EditorTypographyRole::DialogBody:
                return SnapFontSize(metrics.fontSize);
        }

        return SnapFontSize(metrics.fontSize);
    }

    float ResolveLayoutWidthScale(EditorTypographyRole role)
    {
        switch (role)
        {
            case EditorTypographyRole::Menu:
            case EditorTypographyRole::Toolbar:
            case EditorTypographyRole::Status:
            case EditorTypographyRole::PanelTitle:
            case EditorTypographyRole::PanelTab:
            case EditorTypographyRole::Input:
            case EditorTypographyRole::Control:
            case EditorTypographyRole::ListItem:
            case EditorTypographyRole::PropertyLabel:
            case EditorTypographyRole::PropertyValue:
            case EditorTypographyRole::ContextMenu:
            case EditorTypographyRole::Tooltip:
                return RVX_EDITOR_LAYOUT_TEXT_WIDTH_SCALE;
            case EditorTypographyRole::Body:
            case EditorTypographyRole::Caption:
            case EditorTypographyRole::DialogTitle:
            case EditorTypographyRole::DialogBody:
                return 1.0f;
        }

        return 1.0f;
    }
}

EditorTypographyStyle EditorTypography::Resolve(const UI::UITheme& theme,
                                                EditorTypographyRole role)
{
    EditorTypographyStyle style;
    style.role = role;
    style.fontSize = ResolveFontSize(theme, role);
    style.lineHeight = std::max(style.fontSize + 4.0f,
                                UI::UIFontFallbackChain::Default().GetLineHeight(
                                    style.fontSize));
    style.lineHeight = std::round(style.lineHeight);
    style.layoutWidthScale = ResolveLayoutWidthScale(role);
    return style;
}

float EditorTypography::GetFontSize(const UI::UITheme& theme,
                                    EditorTypographyRole role)
{
    return Resolve(theme, role).fontSize;
}

float EditorTypography::GetLineHeight(const UI::UITheme& theme,
                                      EditorTypographyRole role)
{
    return Resolve(theme, role).lineHeight;
}

void EditorTypography::ApplyToButton(UI::Button& button,
                                     const UI::UITheme& theme,
                                     EditorTypographyRole role)
{
    button.GetStyle().fontSize = GetFontSize(theme, role);
}

void EditorTypography::ApplyToLabel(UI::Label& label,
                                    const UI::UITheme& theme,
                                    EditorTypographyRole role)
{
    label.SetFontSize(GetFontSize(theme, role));
}

float EditorTypography::MeasureTextWidth(const std::string& text,
                                         const UI::UITheme& theme,
                                         EditorTypographyRole role)
{
    const EditorTypographyStyle style = Resolve(theme, role);
    return UI::UIFontFallbackChain::Default().MeasureText(text,
                                                          style.fontSize).width *
           style.layoutWidthScale;
}

float EditorTypography::EstimatePaddedTextWidth(const std::string& text,
                                                const UI::UITheme& theme,
                                                EditorTypographyRole role,
                                                float minWidth,
                                                float maxWidth)
{
    const EditorTypographyStyle style = Resolve(theme, role);
    const float horizontalPadding = std::max(18.0f, style.fontSize * 1.25f);
    const float estimatedWidth = MeasureTextWidth(text, theme, role) + horizontalPadding;
    return std::clamp(estimatedWidth, minWidth, maxWidth);
}

} // namespace RVX::Editor
