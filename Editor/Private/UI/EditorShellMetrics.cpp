/**
 * @file EditorShellMetrics.cpp
 * @brief Native editor shell density and layout metric implementation
 */

#include "Editor/UI/EditorShellMetrics.h"

#include "Editor/UI/EditorTypography.h"

#include <algorithm>
#include <cmath>

namespace RVX::Editor
{
namespace
{
    float Snap(float value)
    {
        if (!std::isfinite(value))
        {
            return 0.0f;
        }
        return std::round(value);
    }

    float AtLeast(float minimum, float value)
    {
        return Snap(std::max(minimum, value));
    }
}

EditorShellMetrics EditorShellMetricPolicy::Resolve(const UI::UITheme& theme)
{
    EditorShellMetrics metrics;

    const EditorTypographyStyle menuText =
        EditorTypography::Resolve(theme, EditorTypographyRole::Menu);
    const EditorTypographyStyle toolbarText =
        EditorTypography::Resolve(theme, EditorTypographyRole::Toolbar);
    const EditorTypographyStyle statusText =
        EditorTypography::Resolve(theme, EditorTypographyRole::Status);
    const EditorTypographyStyle panelTitleText =
        EditorTypography::Resolve(theme, EditorTypographyRole::PanelTitle);
    const EditorTypographyStyle panelTabText =
        EditorTypography::Resolve(theme, EditorTypographyRole::PanelTab);
    const EditorTypographyStyle controlText =
        EditorTypography::Resolve(theme, EditorTypographyRole::Control);
    const EditorTypographyStyle listText =
        EditorTypography::Resolve(theme, EditorTypographyRole::ListItem);
    const EditorTypographyStyle propertyText =
        EditorTypography::Resolve(theme, EditorTypographyRole::PropertyLabel);
    const EditorTypographyStyle dialogTitleText =
        EditorTypography::Resolve(theme, EditorTypographyRole::DialogTitle);
    const EditorTypographyStyle dialogBodyText =
        EditorTypography::Resolve(theme, EditorTypographyRole::DialogBody);

    metrics.menuBarHeight =
        AtLeast(24.0f,
                std::max(theme.metrics.controlHeight * 0.82f,
                         menuText.lineHeight + 8.0f));
    metrics.commandToolbarHeight =
        AtLeast(32.0f,
                std::max(theme.metrics.toolbarHeight, toolbarText.lineHeight + 13.0f));
    metrics.statusBarHeight =
        AtLeast(24.0f,
                std::max(theme.metrics.statusBarHeight, statusText.lineHeight + 8.0f));
    metrics.topReservedHeight = metrics.menuBarHeight + metrics.commandToolbarHeight;
    metrics.bottomReservedHeight = metrics.statusBarHeight;

    metrics.contentPadding = AtLeast(5.0f, theme.metrics.padding * 0.72f);
    metrics.compactGap = AtLeast(5.0f, metrics.contentPadding * 0.72f);
    metrics.itemGap = AtLeast(8.0f, theme.metrics.spacing);
    metrics.statusItemGap = AtLeast(14.0f, theme.metrics.spacing * 1.8f);

    metrics.panelControlHeight =
        AtLeast(24.0f,
                std::max(theme.metrics.controlHeight * 0.82f,
                         controlText.lineHeight + 7.0f));
    metrics.singleRowPanelToolbarHeight =
        Snap(metrics.contentPadding + metrics.panelControlHeight + metrics.contentPadding);
    metrics.twoRowPanelToolbarHeight =
        Snap(metrics.contentPadding + metrics.panelControlHeight +
             metrics.compactGap + metrics.panelControlHeight + metrics.contentPadding);
    metrics.panelListRowHeight =
        AtLeast(26.0f, listText.lineHeight + 7.0f);
    metrics.compactPanelListRowHeight =
        AtLeast(23.0f, listText.lineHeight + 4.0f);

    metrics.formSpacing = metrics.itemGap;
    metrics.formLineHeight =
        AtLeast(23.0f, propertyText.lineHeight + 5.0f);
    metrics.formRowHeight = metrics.panelControlHeight;
    metrics.pickerRowHeight =
        AtLeast(26.0f,
                std::max(theme.metrics.controlHeight, listText.lineHeight + 8.0f));
    metrics.pickerCategoryHeight =
        AtLeast(24.0f, listText.lineHeight + 6.0f);

    metrics.dialogPadding = AtLeast(12.0f, theme.metrics.padding * 1.5f);
    metrics.dialogSpacing = AtLeast(8.0f, theme.metrics.spacing);
    metrics.dialogTitleHeight =
        AtLeast(30.0f, dialogTitleText.lineHeight + 6.0f);
    metrics.dialogBodyLineHeight = AtLeast(20.0f, dialogBodyText.lineHeight);
    metrics.dialogControlHeight =
        AtLeast(30.0f,
                std::max(theme.metrics.controlHeight, controlText.lineHeight + 8.0f));

    metrics.panelTitleBarHeight =
        AtLeast(28.0f,
                std::max(theme.metrics.controlHeight * 0.84f,
                         panelTitleText.lineHeight + 8.0f));
    metrics.panelTabHeight =
        AtLeast(23.0f,
                std::min(metrics.panelTitleBarHeight - 4.0f,
                         std::max(panelTabText.lineHeight + 6.0f,
                                  theme.metrics.controlHeight * 0.72f)));
    metrics.panelChromeButtonSize =
        AtLeast(20.0f, metrics.panelTitleBarHeight - 8.0f);

    return metrics;
}

} // namespace RVX::Editor
