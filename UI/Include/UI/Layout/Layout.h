/**
 * @file Layout.h
 * @brief Layout components for automatic widget arrangement
 */

#pragma once

#include "UI/Widget.h"

namespace RVX::UI
{

/**
 * @brief Layout direction
 */
enum class LayoutDirection : uint8
{
    Horizontal,
    Vertical
};

/**
 * @brief Content alignment within layout
 */
enum class LayoutAlign : uint8
{
    Start,
    Center,
    End,
    SpaceBetween,
    SpaceAround,
    SpaceEvenly
};

/**
 * @brief Base layout component
 */
class Layout : public Widget
{
public:
    using Ptr = std::shared_ptr<Layout>;

    const char* GetTypeName() const override { return "Layout"; }

    // =========================================================================
    // Spacing
    // =========================================================================

    float GetSpacing() const { return m_spacing; }
    void SetSpacing(float spacing) { m_spacing = spacing; MarkLayoutDirty(); }

    // =========================================================================
    // Padding
    // =========================================================================

    const EdgeInsets& GetPadding() const { return m_padding; }
    void SetPadding(const EdgeInsets& padding) { m_padding = padding; MarkLayoutDirty(); }
    void SetPadding(float all) { SetPadding(EdgeInsets(all)); }

protected:
    float m_spacing = 0.0f;
    EdgeInsets m_padding;
};

/**
 * @brief Horizontal or vertical box layout
 */
class BoxLayout : public Layout
{
public:
    using Ptr = std::shared_ptr<BoxLayout>;

    BoxLayout() = default;
    explicit BoxLayout(LayoutDirection direction) : m_direction(direction) {}

    const char* GetTypeName() const override { return "BoxLayout"; }

    LayoutDirection GetDirection() const { return m_direction; }
    void SetDirection(LayoutDirection direction) { m_direction = direction; MarkLayoutDirty(); }

    LayoutAlign GetAlign() const { return m_align; }
    void SetAlign(LayoutAlign align) { m_align = align; MarkLayoutDirty(); }

    void Layout() override;

    static Ptr CreateHorizontal() { return std::make_shared<BoxLayout>(LayoutDirection::Horizontal); }
    static Ptr CreateVertical() { return std::make_shared<BoxLayout>(LayoutDirection::Vertical); }

private:
    LayoutDirection m_direction = LayoutDirection::Vertical;
    LayoutAlign m_align = LayoutAlign::Start;
};

/**
 * @brief Grid layout with rows and columns
 */
class GridLayout : public Layout
{
public:
    using Ptr = std::shared_ptr<GridLayout>;

    const char* GetTypeName() const override { return "GridLayout"; }

    uint32 GetColumns() const { return m_columns; }
    void SetColumns(uint32 columns) { m_columns = columns; MarkLayoutDirty(); }

    float GetColumnSpacing() const { return m_columnSpacing; }
    void SetColumnSpacing(float spacing) { m_columnSpacing = spacing; MarkLayoutDirty(); }

    float GetRowSpacing() const { return m_rowSpacing; }
    void SetRowSpacing(float spacing) { m_rowSpacing = spacing; MarkLayoutDirty(); }

    void Layout() override;

    static Ptr Create(uint32 columns = 2) 
    { 
        auto grid = std::make_shared<GridLayout>();
        grid->SetColumns(columns);
        return grid;
    }

private:
    uint32 m_columns = 2;
    float m_columnSpacing = 0.0f;
    float m_rowSpacing = 0.0f;
};

/**
 * @brief Stack layout where children overlap
 */
class StackLayout : public Layout
{
public:
    using Ptr = std::shared_ptr<StackLayout>;

    const char* GetTypeName() const override { return "StackLayout"; }

    void Layout() override;

    static Ptr Create() { return std::make_shared<StackLayout>(); }
};

} // namespace RVX::UI
