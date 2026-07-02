/**
 * @file EditorPanelChrome.cpp
 * @brief Native editor panel chrome visual policy implementation
 */

#include "Editor/UI/EditorPanelChrome.h"

#include "Editor/UI/EditorShellMetrics.h"
#include <algorithm>

namespace RVX::Editor
{

EditorPanelChromeStyle BuildEditorPanelChromeStyle(const UI::UITheme& theme)
{
    EditorPanelChromeStyle style;
    style.frameBackground = UI::UIColor{0.088f, 0.096f, 0.108f, 1.0f};
    style.contentBackground = theme.colors.panelBackground;
    style.titleBackground = UI::UIColor{0.070f, 0.078f, 0.088f, 1.0f};
    style.floatingTitleBackground = UI::UIColor{0.086f, 0.098f, 0.112f, 1.0f};
    style.tabActiveBackground = UI::UIColor{0.132f, 0.146f, 0.164f, 1.0f};
    style.tabInactiveBackground = UI::UIColor{0.082f, 0.092f, 0.104f, 1.0f};
    style.tabActiveBorder = theme.colors.accent.WithAlpha(0.68f);
    style.tabInactiveBorder = theme.colors.border.WithAlpha(0.42f);
    style.buttonBackground = theme.colors.surface;
    style.buttonHoverBackground = theme.colors.surfaceHover;
    style.buttonPressedBackground = theme.colors.surfaceActive;
    style.buttonDisabledBackground = theme.colors.surface.WithAlpha(0.35f);
    style.borderSubtle = theme.colors.border.WithAlpha(0.62f);
    style.borderStrong = theme.colors.border;
    style.splitter = theme.colors.border.WithAlpha(0.72f);
    style.accent = theme.colors.accent;
    style.textPrimary = theme.colors.text;
    style.textSecondary = theme.colors.textMuted;

    const EditorShellMetrics shellMetrics = EditorShellMetricPolicy::Resolve(theme);
    style.frameBorderWidth = std::max(1.0f, theme.metrics.borderWidth);
    style.titleBarHeight = shellMetrics.panelTitleBarHeight;
    style.tabHeight = shellMetrics.panelTabHeight;
    style.tabMinWidth = 78.0f;
    style.tabMaxWidth = 156.0f;
    style.tabGap = 1.0f;
    style.tabAccentHeight = std::max(2.0f, theme.metrics.borderWidth * 2.0f);
    style.panelPadding = shellMetrics.contentPadding;
    style.buttonSize = shellMetrics.panelChromeButtonSize;
    return style;
}

} // namespace RVX::Editor
