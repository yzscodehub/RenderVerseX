/**
 * @file EditorPickerListModel.h
 * @brief Reusable native editor picker/list selection model
 */

#pragma once

#include "Core/Types.h"

#include <string>
#include <vector>

namespace RVX::Editor
{

struct EditorPickerListItemState
{
    std::string id;
    bool enabled = true;
};

class EditorPickerListModel
{
public:
    // =========================================================================
    // Item Data
    // =========================================================================
    void SetItems(std::vector<EditorPickerListItemState> items);
    void ClearItems();

    uint32 GetItemCount() const { return static_cast<uint32>(m_items.size()); }
    uint32 GetEnabledItemCount() const;

    bool IsItemEnabled(int32 index) const;

    // =========================================================================
    // Selection
    // =========================================================================
    bool HasExecutableSelection() const { return IsItemEnabled(m_selectedIndex); }

    int32 GetSelectedIndex() const { return m_selectedIndex; }
    bool SetSelectedIndex(int32 index);
    bool ClearSelection();
    bool NormalizeSelection();
    bool MoveSelection(int32 delta);

    // =========================================================================
    // Scrolling
    // =========================================================================
    uint32 ClampScrollOffsetRows(uint32 offsetRows, uint32 visibleRowCount) const;
    uint32 ScrollOffsetForSelection(uint32 offsetRows, uint32 visibleRowCount) const;

private:
    std::vector<EditorPickerListItemState> m_items;
    int32 m_selectedIndex = -1;
};

} // namespace RVX::Editor
