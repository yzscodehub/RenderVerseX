/**
 * @file EditorPickerListView.cpp
 * @brief Reusable native editor picker/list view builder implementation
 */

#include "Editor/UI/EditorPickerListView.h"

#include "Editor/UI/EditorTextInput.h"
#include "Editor/UI/EditorTypography.h"
#include "UI/UIContext.h"
#include "UI/Widgets/Button.h"
#include "UI/Widgets/Label.h"
#include "UI/Widgets/Panel.h"
#include "UI/Widgets/ScrollView.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace RVX::Editor
{
namespace
{
    struct PickerVisualItem
    {
        float top = 0.0f;
        float bottom = 0.0f;
    };

    class EditorPickerListRootPanel final : public UI::Panel
    {
    public:
        using KeyDownCallback = std::function<bool(uint32)>;

        void SetOnKeyDown(KeyDownCallback callback)
        {
            m_onKeyDown = std::move(callback);
        }

        bool HandleEvent(const UI::UIEvent& event) override
        {
            if (event.type == UI::UIEventType::KeyDown && m_onKeyDown &&
                m_onKeyDown(static_cast<uint32>(event.keyCode)))
            {
                return true;
            }
            return UI::Panel::HandleEvent(event);
        }

    private:
        KeyDownCallback m_onKeyDown;
    };

    std::string SanitizePickerWidgetId(const std::string& value)
    {
        std::string result;
        result.reserve(value.size());
        for (char ch : value)
        {
            const unsigned char code = static_cast<unsigned char>(ch);
            if (std::isalnum(code) || ch == '_' || ch == '-' || ch == '.')
            {
                result.push_back(ch);
            }
            else
            {
                result.push_back('_');
            }
        }
        return result.empty() ? "Item" : result;
    }

    UI::Label::Ptr CreatePickerLabel(const std::string& name,
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

    UI::Button::Ptr CreatePickerButton(const std::string& name,
                                       const std::string& text,
                                       const UI::UITheme& theme)
    {
        UI::Button::Ptr button = UI::Button::Create(text);
        button->SetName(name);
        button->GetStyle().fontSize = EditorTypography::GetFontSize(
            theme,
            EditorTypographyRole::ListItem);
        button->GetStyle().textColor = theme.colors.text;
        button->SetNormalColor(theme.colors.surface);
        button->SetHoverColor(theme.colors.surfaceHover);
        button->SetPressedColor(theme.colors.accent.WithAlpha(0.75f));
        button->SetDisabledColor(theme.colors.surface.WithAlpha(0.35f));
        button->SetOverflowMode(UI::TextOverflowMode::EndEllipsis);
        button->SetTooltipText(text);
        return button;
    }

    std::string BuildPickerItemTooltip(
        const EditorPickerListViewItemDesc& itemDesc)
    {
        std::string tooltip =
            itemDesc.tooltipText.empty() ? itemDesc.text : itemDesc.tooltipText;
        if (!itemDesc.secondaryText.empty())
        {
            if (!tooltip.empty())
            {
                tooltip += " ";
            }
            tooltip += "[" + itemDesc.secondaryText + "]";
        }
        if (!itemDesc.enabled && !itemDesc.disabledReason.empty())
        {
            if (!tooltip.empty())
            {
                tooltip += " ";
            }
            tooltip += "Unavailable: " + itemDesc.disabledReason;
        }
        return tooltip;
    }

    float ClampScrollOffset(float offsetY, float contentHeight, float viewportHeight)
    {
        const float maxOffset = std::max(0.0f, contentHeight - viewportHeight);
        return std::clamp(offsetY, 0.0f, maxOffset);
    }

    float EnsureVisualItemVisible(float offsetY,
                                  float viewportHeight,
                                  float contentHeight,
                                  const PickerVisualItem& item)
    {
        float targetOffset = offsetY;
        if (item.top < targetOffset)
        {
            targetOffset = item.top;
        }
        else if (item.bottom > targetOffset + viewportHeight)
        {
            targetOffset = item.bottom - viewportHeight;
        }

        return ClampScrollOffset(targetOffset, contentHeight, viewportHeight);
    }
}

void EditorPickerListView::Build(const EditorPickerListViewDesc& desc)
{
    m_lastBuildStats = {};
    if (!desc.ui || !desc.parent || !desc.model || desc.name.empty() ||
        desc.bounds.width <= 0.0f)
    {
        m_scrollOffsetY = 0.0f;
        return;
    }

    const UI::UITheme& theme = desc.ui->GetTheme();
    const float padding = std::max(0.0f, desc.padding);
    const float rowHeight = std::max(1.0f, desc.rowHeight);
    const float categoryHeight = std::max(1.0f, desc.categoryHeight);
    const float searchHeight = rowHeight;
    const float emptyHeight = categoryHeight;
    const float maxListHeight = std::max(rowHeight, desc.maxListHeight);
    const float innerWidth = std::max(0.0f, desc.bounds.width - padding * 2.0f);

    desc.model->NormalizeSelection();

    std::vector<PickerVisualItem> visualItems;
    visualItems.reserve(desc.items.size());

    uint32 categoryCount = 0;
    uint32 enabledItemCount = 0;
    uint32 secondaryTextItemCount = 0;
    uint32 disabledReasonItemCount = 0;
    std::string currentCategory;
    float contentHeight = 0.0f;
    for (uint32 index = 0; index < static_cast<uint32>(desc.items.size()); ++index)
    {
        const EditorPickerListViewItemDesc& item = desc.items[index];
        if (item.enabled)
        {
            ++enabledItemCount;
        }
        if (!item.secondaryText.empty())
        {
            ++secondaryTextItemCount;
        }
        if (!item.disabledReason.empty())
        {
            ++disabledReasonItemCount;
        }

        if (item.category != currentCategory)
        {
            currentCategory = item.category;
            ++categoryCount;
            contentHeight += categoryHeight;
        }

        visualItems.push_back({contentHeight, contentHeight + rowHeight});
        contentHeight += rowHeight;
    }

    if (desc.items.empty())
    {
        contentHeight = emptyHeight;
    }

    const float listViewportHeight = std::min(contentHeight, maxListHeight);
    const int32 selectedItemIndex = desc.model->GetSelectedIndex();
    if (selectedItemIndex >= 0 &&
        static_cast<size_t>(selectedItemIndex) < visualItems.size())
    {
        m_scrollOffsetY = EnsureVisualItemVisible(
            m_scrollOffsetY,
            listViewportHeight,
            contentHeight,
            visualItems[static_cast<size_t>(selectedItemIndex)]);
    }
    else
    {
        m_scrollOffsetY = ClampScrollOffset(
            m_scrollOffsetY,
            contentHeight,
            listViewportHeight);
    }

    const float panelHeight =
        padding * 3.0f + searchHeight + listViewportHeight;

    auto panel = std::make_shared<EditorPickerListRootPanel>();
    panel->SetName(desc.name);
    panel->SetOnKeyDown(desc.onKeyDown);
    panel->SetPosition(desc.bounds.x, desc.bounds.y);
    panel->SetSize(desc.bounds.width, panelHeight);
    panel->SetBackgroundColor(theme.colors.surface.WithAlpha(0.65f));
    panel->SetBorderColor(theme.colors.border);
    panel->SetBorderWidth(theme.metrics.borderWidth);

    float localY = padding;
    EditorTextInput::Ptr search = EditorTextInput::Create();
    search->SetName(desc.name + ".Search");
    search->ApplyTheme(theme);
    search->SetPlaceholder(desc.searchPlaceholder);
    search->SetText(desc.searchText);
    search->SetPosition(padding, localY);
    search->SetSize(innerWidth, searchHeight);
    search->SetOnTextChanged(desc.onSearchChanged);
    search->SetOnUnhandledKeyDown(desc.onKeyDown);
    panel->AddChild(search);
    localY += searchHeight + padding;

    UI::ScrollView::Ptr viewport = UI::ScrollView::Create();
    viewport->SetName(desc.name + ".Viewport");
    viewport->SetPosition(padding, localY);
    viewport->SetSize(innerWidth, listViewportHeight);
    viewport->SetBackgroundColor(theme.colors.windowBackground.WithAlpha(0.35f));
    viewport->SetBorderColor(theme.colors.border.WithAlpha(0.45f));
    viewport->SetBorderWidth(theme.metrics.borderWidth);
    viewport->SetScrollbarWidth(8.0f);
    viewport->SetScrollbarNamePrefix(desc.name + ".Scrollbar");
    viewport->SetWheelStep(rowHeight * 3.0f);
    viewport->SetContentSize(innerWidth, contentHeight);
    viewport->SetOnScrollChanged([this](const Vec2& offset) {
        m_scrollOffsetY = std::max(0.0f, offset.y);
    });
    viewport->SetScrollOffset(0.0f, m_scrollOffsetY);

    if (desc.items.empty())
    {
        UI::Label::Ptr empty = CreatePickerLabel(desc.name + ".Empty",
                                                 desc.emptyText,
                                                 theme,
                                                 theme.colors.textMuted);
        empty->SetPosition(padding, 0.0f);
        empty->SetSize(std::max(0.0f, innerWidth - padding * 2.0f), emptyHeight);
        viewport->AddContentChild(empty);
    }
    else
    {
        currentCategory.clear();
        float contentY = 0.0f;
        for (uint32 index = 0; index < static_cast<uint32>(desc.items.size()); ++index)
        {
            const EditorPickerListViewItemDesc& itemDesc = desc.items[index];
            if (itemDesc.category != currentCategory)
            {
                currentCategory = itemDesc.category;
                const std::string categoryName =
                    desc.name + ".Category." + SanitizePickerWidgetId(currentCategory);
                UI::Label::Ptr category = CreatePickerLabel(categoryName,
                                                            currentCategory,
                                                            theme,
                                                            theme.colors.textMuted);
                category->SetPosition(padding, contentY);
                category->SetSize(std::max(0.0f, innerWidth - padding * 2.0f),
                                  categoryHeight);
                viewport->AddContentChild(category);
                contentY += categoryHeight;
            }

            const std::string itemName =
                desc.name + ".Item." + SanitizePickerWidgetId(itemDesc.id);
            const bool hasMetadata = !itemDesc.secondaryText.empty() ||
                                     (!itemDesc.enabled &&
                                      !itemDesc.disabledReason.empty());
            const std::string metadataText =
                !itemDesc.enabled && !itemDesc.disabledReason.empty()
                    ? itemDesc.disabledReason
                    : itemDesc.secondaryText;
            UI::Button::Ptr item = CreatePickerButton(
                itemName,
                hasMetadata ? std::string() : itemDesc.text,
                theme);
            const bool selected =
                itemDesc.enabled && selectedItemIndex == static_cast<int32>(index);
            if (selected)
            {
                item->SetNormalColor(theme.colors.accent.WithAlpha(0.82f));
                item->SetHoverColor(theme.colors.accentHover.WithAlpha(0.88f));
                item->SetPressedColor(theme.colors.accentHover);
            }

            item->SetPosition(padding, contentY);
            const float itemWidth =
                std::max(0.0f, innerWidth - padding * 2.0f);
            item->SetSize(itemWidth, rowHeight);
            item->SetEnabled(itemDesc.enabled);
            item->SetTooltipText(BuildPickerItemTooltip(itemDesc));
            if (hasMetadata)
            {
                const float labelPadding = std::max(8.0f, padding);
                const float metadataGap = std::max(8.0f, padding);
                const float metadataMeasuredWidth =
                    EditorTypography::MeasureTextWidth(
                        metadataText,
                        theme,
                        EditorTypographyRole::ListItem);
                const float metadataWidth =
                    std::clamp(metadataMeasuredWidth + labelPadding,
                               std::min(96.0f, itemWidth * 0.38f),
                               std::max(96.0f, itemWidth * 0.46f));
                const float primaryWidth =
                    std::max(0.0f,
                             itemWidth - labelPadding * 2.0f - metadataGap -
                                 metadataWidth);
                UI::Label::Ptr primary =
                    CreatePickerLabel(itemName + ".Primary",
                                      itemDesc.text,
                                      theme,
                                      itemDesc.enabled
                                          ? theme.colors.text
                                          : theme.colors.textMuted.WithAlpha(0.58f));
                primary->SetPosition(labelPadding, 0.0f);
                primary->SetSize(primaryWidth, rowHeight);
                item->AddChild(primary);

                UI::Label::Ptr metadata =
                    CreatePickerLabel(itemName + ".Metadata",
                                      metadataText,
                                      theme,
                                      !itemDesc.enabled &&
                                              !itemDesc.disabledReason.empty()
                                          ? theme.colors.warning
                                          : theme.colors.textMuted);
                metadata->SetTextAlign(UI::TextAlign::Right);
                metadata->SetPosition(labelPadding + primaryWidth + metadataGap,
                                      0.0f);
                metadata->SetSize(std::max(0.0f, metadataWidth), rowHeight);
                item->AddChild(metadata);
            }
            if (itemDesc.enabled && itemDesc.onClick)
            {
                item->SetOnClick([callback = itemDesc.onClick](const UI::UIEvent& event) {
                    (void)event;
                    callback();
                });
            }
            viewport->AddContentChild(item);
            contentY += rowHeight;
        }
    }

    panel->AddChild(viewport);
    desc.parent->AddChild(panel);

    m_lastBuildStats.itemCount = static_cast<uint32>(desc.items.size());
    m_lastBuildStats.enabledItemCount = enabledItemCount;
    m_lastBuildStats.secondaryTextItemCount = secondaryTextItemCount;
    m_lastBuildStats.disabledReasonItemCount = disabledReasonItemCount;
    m_lastBuildStats.categoryCount = categoryCount;
    m_lastBuildStats.selectedItemIndex = selectedItemIndex;
    m_lastBuildStats.contentHeight = contentHeight;
    m_lastBuildStats.listViewportHeight = listViewportHeight;
    m_lastBuildStats.scrollOffsetY = m_scrollOffsetY;
    m_lastBuildStats.empty = desc.items.empty();
    m_lastBuildStats.scrollable = contentHeight > listViewportHeight + 0.001f;
    m_lastBuildStats.bounds =
        UI::Rect(desc.bounds.x, desc.bounds.y, desc.bounds.width, panelHeight);
    m_lastBuildStats.listBounds =
        UI::Rect(desc.bounds.x + padding,
                 desc.bounds.y + padding + searchHeight + padding,
                 innerWidth,
                 listViewportHeight);
}

void EditorPickerListView::SetScrollOffsetY(float offsetY)
{
    m_scrollOffsetY = std::max(0.0f, offsetY);
}

} // namespace RVX::Editor
