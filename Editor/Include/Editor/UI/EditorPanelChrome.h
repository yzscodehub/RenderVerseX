/**
 * @file EditorPanelChrome.h
 * @brief Native editor panel chrome visual policy
 */

#pragma once

#include "UI/UIContext.h"

namespace RVX::Editor
{

struct EditorPanelChromeStyle
{
    UI::UIColor frameBackground;
    UI::UIColor contentBackground;
    UI::UIColor titleBackground;
    UI::UIColor floatingTitleBackground;
    UI::UIColor tabActiveBackground;
    UI::UIColor tabInactiveBackground;
    UI::UIColor tabActiveBorder;
    UI::UIColor tabInactiveBorder;
    UI::UIColor buttonBackground;
    UI::UIColor buttonHoverBackground;
    UI::UIColor buttonPressedBackground;
    UI::UIColor buttonDisabledBackground;
    UI::UIColor borderSubtle;
    UI::UIColor borderStrong;
    UI::UIColor splitter;
    UI::UIColor accent;
    UI::UIColor textPrimary;
    UI::UIColor textSecondary;

    float frameBorderWidth = 1.0f;
    float titleBarHeight = 30.0f;
    float tabHeight = 26.0f;
    float tabMinWidth = 92.0f;
    float tabMaxWidth = 180.0f;
    float tabGap = 2.0f;
    float tabAccentHeight = 2.0f;
    float panelPadding = 6.0f;
    float buttonSize = 22.0f;
};

EditorPanelChromeStyle BuildEditorPanelChromeStyle(const UI::UITheme& theme);

} // namespace RVX::Editor
