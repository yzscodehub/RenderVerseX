/**
 * @file Layout.cpp
 * @brief Layout implementations
 */

#include "UI/Layout/Layout.h"

namespace RVX::UI
{

// ============================================================================
// BoxLayout
// ============================================================================

void BoxLayout::Layout()
{
    float offset = (m_direction == LayoutDirection::Horizontal) 
        ? m_padding.left 
        : m_padding.top;

    for (auto& child : m_children)
    {
        if (child->GetVisibility() == Visibility::Collapsed)
            continue;

        Vec2 childPos = child->GetPosition();
        if (m_direction == LayoutDirection::Horizontal)
        {
            childPos.x = offset;
            childPos.y = m_padding.top;
            offset += child->GetWidth() + m_spacing;
        }
        else
        {
            childPos.x = m_padding.left;
            childPos.y = offset;
            offset += child->GetHeight() + m_spacing;
        }
        child->SetPosition(childPos);
        child->Layout();
    }

    m_layoutDirty = false;
}

// ============================================================================
// GridLayout
// ============================================================================

void GridLayout::Layout()
{
    if (m_columns == 0) m_columns = 1;

    float cellWidth = (m_size.x - m_padding.Horizontal() - 
                       (m_columns - 1) * m_columnSpacing) / m_columns;

    uint32 row = 0;
    uint32 col = 0;

    for (auto& child : m_children)
    {
        if (child->GetVisibility() == Visibility::Collapsed)
            continue;

        float x = m_padding.left + col * (cellWidth + m_columnSpacing);
        float y = m_padding.top + row * (child->GetHeight() + m_rowSpacing);

        child->SetPosition(x, y);
        child->Layout();

        col++;
        if (col >= m_columns)
        {
            col = 0;
            row++;
        }
    }

    m_layoutDirty = false;
}

// ============================================================================
// StackLayout
// ============================================================================

void StackLayout::Layout()
{
    for (auto& child : m_children)
    {
        child->SetPosition(m_padding.left, m_padding.top);
        child->SetSize(
            m_size.x - m_padding.Horizontal(),
            m_size.y - m_padding.Vertical()
        );
        child->Layout();
    }

    m_layoutDirty = false;
}

} // namespace RVX::UI
