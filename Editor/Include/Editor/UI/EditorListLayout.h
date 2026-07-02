/**
 * @file EditorListLayout.h
 * @brief Shared native editor scrollable list layout helpers.
 */

#pragma once

#include "Editor/UI/EditorUIPanel.h"
#include "UI/Widgets/ScrollView.h"

#include <functional>
#include <string>

namespace RVX::Editor
{

struct EditorListLayoutFrame
{
    UI::ScrollView::Ptr viewport;
    UI::Panel::Ptr contentPanel;
    UI::Panel* parentContainer = nullptr;
    float viewportHeight = 0.0f;
    float contentHeight = 0.0f;
    float requestedScrollOffsetY = 0.0f;

    explicit operator bool() const
    {
        return viewport != nullptr && contentPanel != nullptr;
    }
};

struct EditorListLayoutDesc
{
    EditorUIPanelFrameContext* context = nullptr;
    std::string namePrefix;
    UI::Rect bounds;
    float rowHeight = 0.0f;
    float contentHeight = 0.0f;
    float scrollOffsetY = 0.0f;
    UI::ScrollView::ScrollChangedCallback onScrollChanged;
};

/**
 * @brief Utility for building scrollable native list regions.
 */
class EditorListLayout
{
public:
    static EditorListLayoutFrame BeginScrollableList(EditorListLayoutDesc desc);
    static void EndScrollableList(EditorListLayoutFrame& frame);
};

} // namespace RVX::Editor
