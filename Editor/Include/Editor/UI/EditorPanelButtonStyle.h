/**
 * @file EditorPanelButtonStyle.h
 * @brief Shared styling for native editor panel text buttons
 */

#pragma once

#include "Editor/UI/EditorTypography.h"
#include "UI/Widgets/Button.h"

namespace RVX::Editor
{

struct EditorPanelButtonStyleDesc
{
    bool active = false;
    EditorTypographyRole typographyRole = EditorTypographyRole::Control;
    UI::TextOverflowMode overflowMode = UI::TextOverflowMode::EndEllipsis;
    bool setTooltipFromText = true;
    float pressedAlpha = 0.75f;
};

class EditorPanelButtonStyle
{
public:
    static void Apply(UI::Button& button,
                      const UI::UITheme& theme,
                      const EditorPanelButtonStyleDesc& desc = {});
};

} // namespace RVX::Editor
