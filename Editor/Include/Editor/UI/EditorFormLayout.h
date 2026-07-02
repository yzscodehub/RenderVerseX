/**
 * @file EditorFormLayout.h
 * @brief Shared native editor form layout helpers.
 */

#pragma once

#include "Editor/UI/EditorUIPanel.h"
#include "UI/Widgets/ScrollView.h"

#include <functional>
#include <string>

namespace RVX::Editor
{

struct EditorFormLayoutMetrics
{
    float padding = 0.0f;
    float spacing = 0.0f;
    float lineHeight = 0.0f;
    float rowHeight = 0.0f;
    float contentWidth = 0.0f;
    float viewportHeight = 0.0f;
};

struct EditorFormLayoutFrame
{
    UI::ScrollView::Ptr viewport;
    UI::Panel::Ptr contentPanel;
    UI::Panel* parentContainer = nullptr;
    EditorUIPanelFrameContext contentContext;
    EditorFormLayoutMetrics metrics;
    float requestedScrollOffsetY = 0.0f;

    explicit operator bool() const
    {
        return viewport != nullptr && contentPanel != nullptr &&
               contentContext.contentContainer != nullptr;
    }
};

struct EditorFormLayoutDesc
{
    EditorUIPanelFrameContext* context = nullptr;
    std::string namePrefix;
    float scrollOffsetY = 0.0f;
    UI::ScrollView::ScrollChangedCallback onScrollChanged;
};

/**
 * @brief Utility for building scrollable native form panels.
 */
class EditorFormLayout
{
public:
    static EditorFormLayoutFrame BeginScrollableFrame(
        EditorFormLayoutDesc desc);
    static void EndScrollableFrame(EditorFormLayoutFrame& frame,
                                   float contentHeight);
};

} // namespace RVX::Editor
