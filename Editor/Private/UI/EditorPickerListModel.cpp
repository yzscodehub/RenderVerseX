/**
 * @file EditorPickerListModel.cpp
 * @brief Reusable native editor picker/list selection model implementation
 */

#include "Editor/UI/EditorPickerListModel.h"

#include <algorithm>
#include <utility>

namespace RVX::Editor
{

void EditorPickerListModel::SetItems(std::vector<EditorPickerListItemState> items)
{
    m_items = std::move(items);
    NormalizeSelection();
}

void EditorPickerListModel::ClearItems()
{
    m_items.clear();
    m_selectedIndex = -1;
}

uint32 EditorPickerListModel::GetEnabledItemCount() const
{
    uint32 count = 0;
    for (const EditorPickerListItemState& item : m_items)
    {
        if (item.enabled)
        {
            ++count;
        }
    }
    return count;
}

bool EditorPickerListModel::IsItemEnabled(int32 index) const
{
    return index >= 0 &&
           static_cast<size_t>(index) < m_items.size() &&
           m_items[static_cast<size_t>(index)].enabled;
}

bool EditorPickerListModel::SetSelectedIndex(int32 index)
{
    if (!IsItemEnabled(index))
    {
        return NormalizeSelection();
    }

    if (m_selectedIndex == index)
    {
        return false;
    }

    m_selectedIndex = index;
    return true;
}

bool EditorPickerListModel::ClearSelection()
{
    if (m_selectedIndex < 0)
    {
        return false;
    }

    m_selectedIndex = -1;
    return true;
}

bool EditorPickerListModel::NormalizeSelection()
{
    if (IsItemEnabled(m_selectedIndex))
    {
        return false;
    }

    const int32 oldSelection = m_selectedIndex;
    m_selectedIndex = -1;
    for (size_t index = 0; index < m_items.size(); ++index)
    {
        const int32 candidate = static_cast<int32>(index);
        if (IsItemEnabled(candidate))
        {
            m_selectedIndex = candidate;
            break;
        }
    }

    return oldSelection != m_selectedIndex;
}

bool EditorPickerListModel::MoveSelection(int32 delta)
{
    if (m_items.empty())
    {
        return ClearSelection();
    }

    if (delta == 0)
    {
        return NormalizeSelection();
    }

    const int32 oldSelection = m_selectedIndex;
    const int32 count = static_cast<int32>(m_items.size());
    int32 cursor = m_selectedIndex;
    if (cursor < 0 || cursor >= count)
    {
        cursor = delta < 0 ? 0 : count - 1;
    }

    for (int32 step = 0; step < count; ++step)
    {
        cursor = (cursor + delta + count) % count;
        if (IsItemEnabled(cursor))
        {
            m_selectedIndex = cursor;
            return oldSelection != m_selectedIndex;
        }
    }

    m_selectedIndex = -1;
    return oldSelection != m_selectedIndex;
}

uint32 EditorPickerListModel::ClampScrollOffsetRows(uint32 offsetRows,
                                                    uint32 visibleRowCount) const
{
    if (visibleRowCount == 0u || m_items.empty())
    {
        return 0u;
    }

    const uint32 itemCount = GetItemCount();
    const uint32 maxOffset = itemCount > visibleRowCount
                                 ? itemCount - visibleRowCount
                                 : 0u;
    return std::min(offsetRows, maxOffset);
}

uint32 EditorPickerListModel::ScrollOffsetForSelection(uint32 offsetRows,
                                                       uint32 visibleRowCount) const
{
    if (visibleRowCount == 0u || m_selectedIndex < 0)
    {
        return ClampScrollOffsetRows(offsetRows, visibleRowCount);
    }

    uint32 targetOffset = offsetRows;
    const uint32 selected = static_cast<uint32>(m_selectedIndex);
    if (selected < targetOffset)
    {
        targetOffset = selected;
    }
    else if (selected >= targetOffset + visibleRowCount)
    {
        targetOffset = selected - visibleRowCount + 1u;
    }

    return ClampScrollOffsetRows(targetOffset, visibleRowCount);
}

} // namespace RVX::Editor
