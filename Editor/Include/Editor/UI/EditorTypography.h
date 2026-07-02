/**
 * @file EditorTypography.h
 * @brief Editor typography tokens and text layout helpers
 */

#pragma once

#include "Core/Types.h"
#include "UI/UIContext.h"

#include <string>

namespace RVX::UI
{
class Button;
class Label;
}

namespace RVX::Editor
{

enum class EditorTypographyRole : uint8
{
    Body = 0,
    Caption,
    Menu,
    Toolbar,
    Status,
    PanelTitle,
    PanelTab,
    Input,
    Control,
    ListItem,
    PropertyLabel,
    PropertyValue,
    ContextMenu,
    Tooltip,
    DialogTitle,
    DialogBody
};

struct EditorTypographyStyle
{
    EditorTypographyRole role = EditorTypographyRole::Body;
    float fontSize = 14.0f;
    float lineHeight = 18.0f;
    float layoutWidthScale = 1.0f;
};

class EditorTypography
{
public:
    static EditorTypographyStyle Resolve(const UI::UITheme& theme,
                                         EditorTypographyRole role);
    static float GetFontSize(const UI::UITheme& theme, EditorTypographyRole role);
    static float GetLineHeight(const UI::UITheme& theme, EditorTypographyRole role);
    static void ApplyToButton(UI::Button& button,
                              const UI::UITheme& theme,
                              EditorTypographyRole role);
    static void ApplyToLabel(UI::Label& label,
                             const UI::UITheme& theme,
                             EditorTypographyRole role);
    static float MeasureTextWidth(const std::string& text,
                                  const UI::UITheme& theme,
                                  EditorTypographyRole role);
    static float EstimatePaddedTextWidth(const std::string& text,
                                         const UI::UITheme& theme,
                                         EditorTypographyRole role,
                                         float minWidth,
                                         float maxWidth);
};

} // namespace RVX::Editor
