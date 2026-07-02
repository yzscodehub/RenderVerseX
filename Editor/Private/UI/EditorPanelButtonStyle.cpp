/**
 * @file EditorPanelButtonStyle.cpp
 * @brief Shared native editor panel button styling implementation
 */

#include "Editor/UI/EditorPanelButtonStyle.h"

namespace RVX::Editor
{

void EditorPanelButtonStyle::Apply(UI::Button& button,
                                   const UI::UITheme& theme,
                                   const EditorPanelButtonStyleDesc& desc)
{
    EditorTypography::ApplyToButton(button, theme, desc.typographyRole);
    button.GetStyle().textColor = theme.colors.text;
    button.SetNormalColor(desc.active ? theme.colors.surfaceActive
                                      : theme.colors.surface);
    button.SetHoverColor(theme.colors.surfaceHover);
    button.SetPressedColor(theme.colors.accent.WithAlpha(desc.pressedAlpha));
    button.SetDisabledColor(theme.colors.surface.WithAlpha(0.35f));
    button.SetOverflowMode(desc.overflowMode);

    if (desc.setTooltipFromText && !button.GetText().empty())
    {
        button.SetTooltipText(button.GetText());
    }
}

} // namespace RVX::Editor
