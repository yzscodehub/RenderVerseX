/**
 * @file EditorShellMetrics.h
 * @brief Native editor shell density and layout metrics
 */

#pragma once

#include "Core/Types.h"
#include "UI/UIContext.h"

namespace RVX::Editor
{

struct EditorShellMetrics
{
    float menuBarHeight = 0.0f;
    float commandToolbarHeight = 0.0f;
    float statusBarHeight = 0.0f;
    float topReservedHeight = 0.0f;
    float bottomReservedHeight = 0.0f;

    float contentPadding = 0.0f;
    float compactGap = 0.0f;
    float itemGap = 0.0f;
    float statusItemGap = 0.0f;

    float panelControlHeight = 0.0f;
    float singleRowPanelToolbarHeight = 0.0f;
    float twoRowPanelToolbarHeight = 0.0f;
    float panelListRowHeight = 0.0f;
    float compactPanelListRowHeight = 0.0f;

    float formSpacing = 0.0f;
    float formLineHeight = 0.0f;
    float formRowHeight = 0.0f;
    float pickerRowHeight = 0.0f;
    float pickerCategoryHeight = 0.0f;

    float dialogPadding = 0.0f;
    float dialogSpacing = 0.0f;
    float dialogTitleHeight = 0.0f;
    float dialogBodyLineHeight = 0.0f;
    float dialogControlHeight = 0.0f;

    float panelTitleBarHeight = 0.0f;
    float panelTabHeight = 0.0f;
    float panelChromeButtonSize = 0.0f;
};

class EditorShellMetricPolicy
{
public:
    static EditorShellMetrics Resolve(const UI::UITheme& theme);
};

} // namespace RVX::Editor
