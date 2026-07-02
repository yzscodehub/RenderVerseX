/**
 * @file EditorTreeList.cpp
 * @brief Native editor tree/list row virtualization helper implementation
 */

#include "Editor/UI/EditorTreeList.h"

#include "Editor/UI/EditorTypography.h"
#include "UI/UIContext.h"
#include "UI/Widgets/Label.h"
#include "UI/Widgets/Panel.h"
#include "UI/Widgets/ScrollView.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace RVX::Editor
{
namespace
{
    UI::Label::Ptr CreateRowLabel(const std::string& name,
                                  const std::string& text,
                                  const UI::UITheme& theme,
                                  const UI::UIColor& color)
    {
        UI::Label::Ptr label = UI::Label::Create(text);
        label->SetName(name);
        label->SetFontSize(EditorTypography::GetFontSize(
            theme,
            EditorTypographyRole::ListItem));
        label->SetTextColor(color);
        label->SetVerticalAlign(UI::VerticalAlign::Middle);
        label->SetOverflowMode(UI::TextOverflowMode::EndEllipsis);
        label->SetTooltipText(text);
        label->SetInteractive(false);
        return label;
    }

    std::string RowWidgetName(const std::string& listName, const std::string& rowId)
    {
        return listName + ".Row." + rowId;
    }

    std::string BranchPrefix(const EditorTreeListRowDesc& row)
    {
        if (!row.expandable)
        {
            return "- ";
        }
        return row.expanded ? "v " : "> ";
    }
}

void EditorTreeList::Build(const EditorTreeListBuildDesc& desc)
{
    m_lastBuildStats = {};
    if (!desc.ui || !desc.parent || desc.name.empty() || desc.rowHeight <= 0.0f)
    {
        m_scrollOffsetRows = 0;
        return;
    }

    const UI::UITheme& theme = desc.ui->GetTheme();
    const float scrollbarWidth = 8.0f;
    const float rowHeight = std::max(1.0f, desc.rowHeight);
    const uint32 totalRows = static_cast<uint32>(desc.rows.size());
    const uint32 visibleCapacity =
        desc.bounds.height > 0.0f
            ? std::max(1u, static_cast<uint32>(std::floor(desc.bounds.height / rowHeight)))
            : 0u;
    const uint32 maxScrollOffset =
        totalRows > visibleCapacity ? totalRows - visibleCapacity : 0u;
    m_scrollOffsetRows = std::min(m_scrollOffsetRows, maxScrollOffset);

    m_lastBuildStats.totalRowCount = totalRows;
    m_lastBuildStats.firstVisibleRow = m_scrollOffsetRows;
    m_lastBuildStats.maxScrollOffsetRows = maxScrollOffset;
    m_lastBuildStats.scrollOffsetRows = m_scrollOffsetRows;
    m_lastBuildStats.scrollbarVisible = maxScrollOffset > 0u;

    auto viewport = UI::ScrollView::Create();
    viewport->SetName(desc.name + ".Viewport");
    viewport->SetPosition(desc.bounds.x, desc.bounds.y);
    viewport->SetSize(desc.bounds.width, desc.bounds.height);
    viewport->SetBackgroundColor(UI::UIColor::Transparent());
    viewport->SetBorderWidth(0.0f);
    viewport->SetScrollbarWidth(scrollbarWidth);
    viewport->SetScrollbarNamePrefix(desc.name + ".Scrollbar");
    viewport->SetWheelStep(rowHeight * 3.0f);
    viewport->SetContentSize(std::max(0.0f,
                                      desc.bounds.width -
                                          (m_lastBuildStats.scrollbarVisible ? scrollbarWidth : 0.0f)),
                             static_cast<float>(totalRows) * rowHeight);
    viewport->SetOnScrollChanged([this, maxScrollOffset, rowHeight](const Vec2& offset) {
        const uint32 rowOffset = static_cast<uint32>(
            std::floor((offset.y + rowHeight * 0.5f) / rowHeight));
        m_scrollOffsetRows = std::min(rowOffset, maxScrollOffset);
    });
    viewport->SetScrollOffset(0.0f, static_cast<float>(m_scrollOffsetRows) * rowHeight);

    const float contentWidth =
        std::max(0.0f, desc.bounds.width - (m_lastBuildStats.scrollbarVisible ? scrollbarWidth : 0.0f));
    const uint32 endRow = std::min(totalRows, m_scrollOffsetRows + visibleCapacity);
    for (uint32 rowIndex = m_scrollOffsetRows; rowIndex < endRow; ++rowIndex)
    {
        const EditorTreeListRowDesc& rowDesc = desc.rows[rowIndex];
        const std::string rowName = RowWidgetName(desc.name, rowDesc.id);

        UI::Panel::Ptr row = UI::Panel::Create();
        row->SetName(rowName);
        row->SetPosition(0.0f, static_cast<float>(rowIndex) * rowHeight);
        row->SetSize(contentWidth, rowHeight);
        row->SetBackgroundColor(rowDesc.selected
                                    ? theme.colors.surfaceActive
                                    : ((rowIndex % 2u == 0u)
                                           ? theme.colors.panelBackground
                                           : theme.colors.windowBackground.WithAlpha(0.35f)));
        row->SetBorderWidth(rowDesc.selected ? 1.0f : 0.0f);
        row->SetBorderColor(rowDesc.selected ? theme.colors.accent : UI::UIColor::Transparent());
        if (rowDesc.onClick || rowDesc.onContextMenu)
        {
            row->SetOnClick([click = rowDesc.onClick,
                             contextMenu = rowDesc.onContextMenu](const UI::UIEvent& event) {
                if (event.button == static_cast<int>(UI::UIMouseButton::Right))
                {
                    if (contextMenu)
                    {
                        contextMenu(event.position);
                    }
                    return;
                }

                if (event.button == static_cast<int>(UI::UIMouseButton::Left) && click)
                {
                    click();
                }
            });
        }

        const float indent = static_cast<float>(rowDesc.depth) * desc.indentWidth;
        const UI::UIColor textColor = rowDesc.muted ? theme.colors.textMuted : theme.colors.text;
        UI::Label::Ptr label = CreateRowLabel(rowName + ".Text",
                                              BranchPrefix(rowDesc) + rowDesc.text,
                                              theme,
                                              textColor);
        label->SetPosition(8.0f + indent, 0.0f);
        label->SetSize(std::max(0.0f, contentWidth - indent - 16.0f), rowHeight);
        row->AddChild(label);
        viewport->AddContentChild(row);

        ++m_lastBuildStats.visibleRowCount;
    }

    if (m_lastBuildStats.visibleRowCount > 0u)
    {
        m_lastBuildStats.lastVisibleRow =
            m_lastBuildStats.firstVisibleRow + m_lastBuildStats.visibleRowCount - 1u;
    }

    desc.parent->AddChild(viewport);
}

void EditorTreeList::SetScrollOffsetRows(uint32 offsetRows)
{
    m_scrollOffsetRows = offsetRows;
}

void EditorTreeList::ScrollByRows(int32 deltaRows)
{
    if (deltaRows < 0)
    {
        const uint32 amount = static_cast<uint32>(-deltaRows);
        m_scrollOffsetRows = amount > m_scrollOffsetRows ? 0u : m_scrollOffsetRows - amount;
        return;
    }

    m_scrollOffsetRows =
        std::min(m_scrollOffsetRows + static_cast<uint32>(deltaRows),
                 m_lastBuildStats.maxScrollOffsetRows);
}

} // namespace RVX::Editor
