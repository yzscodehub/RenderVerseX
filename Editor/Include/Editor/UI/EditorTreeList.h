/**
 * @file EditorTreeList.h
 * @brief Native editor tree/list row virtualization helper
 */

#pragma once

#include "Core/Types.h"
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

struct EditorTreeListRowDesc
{
    std::string id;
    std::string text;
    uint32 depth = 0;
    bool selected = false;
    bool muted = false;
    bool expandable = false;
    bool expanded = true;
    std::function<void()> onClick;
    std::function<void(const Vec2&)> onContextMenu;
};

struct EditorTreeListBuildDesc
{
    UI::UIContext* ui = nullptr;
    UI::Panel* parent = nullptr;
    std::string name;
    UI::Rect bounds;
    float rowHeight = 20.0f;
    float indentWidth = 14.0f;
    std::vector<EditorTreeListRowDesc> rows;
};

struct EditorTreeListBuildStats
{
    uint32 totalRowCount = 0;
    uint32 visibleRowCount = 0;
    uint32 firstVisibleRow = 0;
    uint32 lastVisibleRow = 0;
    uint32 maxScrollOffsetRows = 0;
    uint32 scrollOffsetRows = 0;
    bool scrollbarVisible = false;
};

class EditorTreeList
{
public:
    void Build(const EditorTreeListBuildDesc& desc);

    void SetScrollOffsetRows(uint32 offsetRows);
    uint32 GetScrollOffsetRows() const { return m_scrollOffsetRows; }

    const EditorTreeListBuildStats& GetLastBuildStats() const { return m_lastBuildStats; }

private:
    void ScrollByRows(int32 deltaRows);

    EditorTreeListBuildStats m_lastBuildStats;
    uint32 m_scrollOffsetRows = 0;
};

} // namespace RVX::Editor
