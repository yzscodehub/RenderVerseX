/**
 * @file EditorPickerListView.h
 * @brief Reusable native editor picker/list view builder
 */

#pragma once

#include "Core/Types.h"
#include "Editor/UI/EditorPickerListModel.h"
#include "UI/UITypes.h"

#include <functional>
#include <string>
#include <vector>

namespace RVX::UI
{
    class Panel;
    class UIContext;
}

namespace RVX::Editor
{

struct EditorPickerListViewItemDesc
{
    std::string id;
    std::string text;
    std::string category;
    std::string secondaryText;
    std::string disabledReason;
    std::string tooltipText;
    bool enabled = true;
    std::function<void()> onClick;
};

struct EditorPickerListViewDesc
{
    UI::UIContext* ui = nullptr;
    UI::Panel* parent = nullptr;
    EditorPickerListModel* model = nullptr;
    std::string name;
    UI::Rect bounds;
    std::string searchText;
    std::string searchPlaceholder = "Search";
    std::string emptyText = "No results";
    std::vector<EditorPickerListViewItemDesc> items;
    std::function<void(const std::string&)> onSearchChanged;
    std::function<bool(uint32)> onKeyDown;
    float padding = 4.0f;
    float rowHeight = 22.0f;
    float categoryHeight = 20.0f;
    float maxListHeight = 220.0f;
};

struct EditorPickerListViewStats
{
    uint32 itemCount = 0;
    uint32 enabledItemCount = 0;
    uint32 secondaryTextItemCount = 0;
    uint32 disabledReasonItemCount = 0;
    uint32 categoryCount = 0;
    int32 selectedItemIndex = -1;
    float contentHeight = 0.0f;
    float listViewportHeight = 0.0f;
    float scrollOffsetY = 0.0f;
    bool empty = false;
    bool scrollable = false;
    UI::Rect bounds;
    UI::Rect listBounds;
};

class EditorPickerListView
{
public:
    // =========================================================================
    // Build
    // =========================================================================
    void Build(const EditorPickerListViewDesc& desc);

    // =========================================================================
    // Scrolling
    // =========================================================================
    void SetScrollOffsetY(float offsetY);
    float GetScrollOffsetY() const { return m_scrollOffsetY; }

    // =========================================================================
    // Stats
    // =========================================================================
    const EditorPickerListViewStats& GetLastBuildStats() const
    {
        return m_lastBuildStats;
    }

private:
    float m_scrollOffsetY = 0.0f;
    EditorPickerListViewStats m_lastBuildStats;
};

} // namespace RVX::Editor
