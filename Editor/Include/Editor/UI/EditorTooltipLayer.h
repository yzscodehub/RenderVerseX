/**
 * @file EditorTooltipLayer.h
 * @brief Native editor non-interactive tooltip overlay layer
 */

#pragma once

#include "Core/Types.h"
#include "UI/UITypes.h"

#include <string>

namespace RVX::UI
{
    class UIContext;
    class UICanvas;
    class Widget;
}

namespace RVX::Editor
{

struct EditorTooltipLayerStats
{
    bool visible = false;
    uint32 tooltipCount = 0;
    uint32 lineCount = 0;
    bool clipped = false;
    std::string sourceWidgetName;
    std::string text;
    UI::Rect bounds;
};

/**
 * @brief Builds the topmost non-interactive tooltip for the currently hovered widget.
 */
class EditorTooltipLayer
{
public:
    void Build(UI::UIContext& ui);
    void Clear(UI::UIContext& ui);

    const EditorTooltipLayerStats& GetLastBuildStats() const
    {
        return m_lastBuildStats;
    }

private:
    static const UI::Widget* FindTooltipSource(const UI::Widget* widget);
    static std::string ResolveTooltipText(const UI::Widget& widget);
    static void RemoveTooltipLayer(UI::UICanvas& canvas);

    EditorTooltipLayerStats m_lastBuildStats;
};

} // namespace RVX::Editor
