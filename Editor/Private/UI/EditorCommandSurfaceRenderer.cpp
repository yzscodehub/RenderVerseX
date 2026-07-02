/**
 * @file EditorCommandSurfaceRenderer.cpp
 * @brief Native editor command surface widget builder implementation
 */

#include "Editor/UI/EditorCommandSurfaceRenderer.h"

#include "Editor/UI/EditorCommandIconResolver.h"
#include "Editor/UI/EditorIconButton.h"
#include "Editor/UI/EditorPopupLayer.h"
#include "Editor/UI/EditorShellMetrics.h"
#include "Editor/UI/EditorShortcutBindingModel.h"
#include "Editor/UI/EditorTypography.h"
#include "Editor/UI/EditorUIHost.h"
#include "Editor/UI/EditorVectorIcon.h"
#include "UI/UICanvas.h"
#include "UI/UIContext.h"
#include "UI/UIRenderer.h"
#include "UI/Widget.h"
#include "UI/Widgets/Button.h"
#include "UI/Widgets/Label.h"
#include "UI/Widgets/Panel.h"
#include "UI/Widgets/ScrollView.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace RVX::Editor
{
namespace
{
    constexpr const char* RVX_EDITOR_SURFACE_MENU_BAR = "Editor.CommandSurface.MenuBar";
    constexpr const char* RVX_EDITOR_SURFACE_TOOLBAR = "Editor.CommandSurface.ToolbarBar";
    constexpr const char* RVX_EDITOR_SURFACE_STATUS_BAR = "Editor.CommandSurface.StatusBar";
    constexpr const char* RVX_EDITOR_SURFACE_MENU_DROPDOWN =
        "Editor.CommandSurface.MenuDropdown";
    constexpr const char* RVX_EDITOR_SURFACE_TOOLBAR_OVERFLOW_DROPDOWN =
        "Editor.CommandSurface.Toolbar.OverflowDropdown";

    float MeasureTextWidthForLayout(const std::string& text,
                                    const UI::UITheme& theme,
                                    EditorTypographyRole role)
    {
        return EditorTypography::MeasureTextWidth(text, theme, role);
    }

    float EstimateTextWidth(const std::string& text,
                            const UI::UITheme& theme,
                            EditorTypographyRole role,
                            float minWidth,
                            float maxWidth)
    {
        return EditorTypography::EstimatePaddedTextWidth(text,
                                                         theme,
                                                         role,
                                                         minWidth,
                                                         maxWidth);
    }

    std::string BuildStatusText(const EditorStatusSurfaceItem& item)
    {
        return item.value.empty() ? item.label : (item.label + ": " + item.value);
    }

    struct StatusLayoutItem
    {
        std::string id;
        std::string text;
        uint32 priority = 0;
        UI::TextAlign align = UI::TextAlign::Left;
        float x = 0.0f;
        float width = 0.0f;
        float desiredWidth = 0.0f;
        float minWidth = 0.0f;
    };


    enum class StatusLayoutZone : uint8
    {
        Left = 0,
        Center,
        Right
    };

    StatusLayoutZone ResolveStatusLayoutZone(const StatusLayoutItem& item)
    {
        if (item.priority == 0u)
        {
            return StatusLayoutZone::Left;
        }
        if (item.priority >= 10u)
        {
            return StatusLayoutZone::Right;
        }
        return StatusLayoutZone::Center;
    }

    float MeasureStatusGroupWidth(const std::vector<StatusLayoutItem*>& group,
                                  float itemGap,
                                  bool desired)
    {
        if (group.empty())
        {
            return 0.0f;
        }

        float width = itemGap * static_cast<float>(group.size() - 1u);
        for (const StatusLayoutItem* item : group)
        {
            if (!item)
            {
                continue;
            }
            width += desired ? item->desiredWidth : item->minWidth;
        }
        return width;
    }

    void FitStatusGroup(std::vector<StatusLayoutItem*>& group,
                        float availableWidth,
                        float itemGap)
    {
        if (group.empty())
        {
            return;
        }

        const float availableContentWidth =
            std::max(0.0f,
                     availableWidth -
                         itemGap * static_cast<float>(group.size() - 1u));
        float desiredTotal = 0.0f;
        float minTotal = 0.0f;
        for (StatusLayoutItem* item : group)
        {
            if (!item)
            {
                continue;
            }
            item->width = item->desiredWidth;
            desiredTotal += item->desiredWidth;
            minTotal += item->minWidth;
        }

        if (desiredTotal <= availableContentWidth)
        {
            return;
        }

        if (minTotal > availableContentWidth && minTotal > 0.0f)
        {
            const float scale = availableContentWidth / minTotal;
            for (StatusLayoutItem* item : group)
            {
                if (item)
                {
                    item->width = std::max(0.0f, item->minWidth * scale);
                }
            }
            return;
        }

        const float shrinkTotal = desiredTotal - availableContentWidth;
        float shrinkCapacity = 0.0f;
        for (const StatusLayoutItem* item : group)
        {
            if (item)
            {
                shrinkCapacity +=
                    std::max(0.0f, item->desiredWidth - item->minWidth);
            }
        }
        if (shrinkCapacity <= 0.0f)
        {
            return;
        }

        for (StatusLayoutItem* item : group)
        {
            if (!item)
            {
                continue;
            }
            const float capacity =
                std::max(0.0f, item->desiredWidth - item->minWidth);
            const float reduction = shrinkTotal * (capacity / shrinkCapacity);
            item->width = std::max(item->minWidth,
                                   item->desiredWidth - reduction);
        }
    }

    void PlaceStatusGroup(std::vector<StatusLayoutItem*>& group,
                          float startX,
                          float itemGap)
    {
        float cursorX = startX;
        for (StatusLayoutItem* item : group)
        {
            if (!item)
            {
                continue;
            }
            item->x = cursorX;
            cursorX += item->width + itemGap;
        }
    }

    std::vector<StatusLayoutItem> BuildStatusLayout(
        const std::vector<EditorStatusSurfaceItem>& items,
        float statusBarWidth,
        const UI::UITheme& theme,
        float outerPadding,
        float itemGap)
    {
        std::vector<StatusLayoutItem> layout;
        layout.reserve(items.size());

        const EditorTypographyStyle statusText =
            EditorTypography::Resolve(theme, EditorTypographyRole::Status);
        const float textPadding = std::max(18.0f, statusText.fontSize * 1.35f);
        for (const EditorStatusSurfaceItem& item : items)
        {
            if (!item.visible)
            {
                continue;
            }

            StatusLayoutItem layoutItem;
            layoutItem.id = item.id;
            layoutItem.text = BuildStatusText(item);
            layoutItem.priority = item.priority;
            const float labelWidth =
                MeasureTextWidthForLayout(item.label,
                                          theme,
                                          EditorTypographyRole::Status);
            const float textWidth =
                MeasureTextWidthForLayout(layoutItem.text,
                                          theme,
                                          EditorTypographyRole::Status);
            layoutItem.desiredWidth =
                std::max(0.0f, textWidth + textPadding);
            layoutItem.minWidth =
                std::min(layoutItem.desiredWidth,
                         std::max(58.0f, labelWidth + textPadding));
            layoutItem.width = layoutItem.desiredWidth;
            layout.push_back(std::move(layoutItem));
        }

        if (layout.empty())
        {
            return layout;
        }

        const float availableWidth = std::max(
            0.0f,
            statusBarWidth - outerPadding * 2.0f);

        std::vector<StatusLayoutItem*> leftGroup;
        std::vector<StatusLayoutItem*> centerGroup;
        std::vector<StatusLayoutItem*> rightGroup;
        leftGroup.reserve(layout.size());
        centerGroup.reserve(layout.size());
        rightGroup.reserve(layout.size());
        for (StatusLayoutItem& item : layout)
        {
            switch (ResolveStatusLayoutZone(item))
            {
                case StatusLayoutZone::Left:
                    leftGroup.push_back(&item);
                    break;
                case StatusLayoutZone::Center:
                    centerGroup.push_back(&item);
                    break;
                case StatusLayoutZone::Right:
                    item.align = UI::TextAlign::Right;
                    rightGroup.push_back(&item);
                    break;
            }
        }

        const uint32 visibleGroupCount =
            static_cast<uint32>(!leftGroup.empty()) +
            static_cast<uint32>(!centerGroup.empty()) +
            static_cast<uint32>(!rightGroup.empty());
        const float groupGap = itemGap * 1.5f;
        const float availableGroupWidth =
            std::max(0.0f,
                     availableWidth -
                         groupGap * static_cast<float>(
                             visibleGroupCount > 0u ? visibleGroupCount - 1u : 0u));

        const float leftDesired =
            MeasureStatusGroupWidth(leftGroup, itemGap, true);
        const float rightDesired =
            MeasureStatusGroupWidth(rightGroup, itemGap, true);
        const float leftMin = MeasureStatusGroupWidth(leftGroup, itemGap, false);
        const float rightMin = MeasureStatusGroupWidth(rightGroup, itemGap, false);

        const float sideCap = availableGroupWidth * 0.24f;
        float leftAreaWidth = leftGroup.empty()
                                  ? 0.0f
                                  : std::min(leftDesired,
                                             std::max(leftMin, sideCap));
        float rightAreaWidth = rightGroup.empty()
                                   ? 0.0f
                                   : std::min(rightDesired,
                                              std::max(rightMin, sideCap));
        if (leftAreaWidth + rightAreaWidth > availableGroupWidth)
        {
            const float scale =
                availableGroupWidth /
                std::max(1.0f, leftAreaWidth + rightAreaWidth);
            leftAreaWidth *= scale;
            rightAreaWidth *= scale;
        }

        const float centerAreaWidth =
            std::max(0.0f,
                     availableGroupWidth - leftAreaWidth - rightAreaWidth);

        FitStatusGroup(leftGroup, leftAreaWidth, itemGap);
        FitStatusGroup(centerGroup, centerAreaWidth, itemGap);
        FitStatusGroup(rightGroup, rightAreaWidth, itemGap);

        const float leftX = outerPadding;
        const float rightX =
            std::max(leftX,
                     statusBarWidth - outerPadding - rightAreaWidth);
        const float centerX =
            leftX + leftAreaWidth +
            (leftGroup.empty() || centerGroup.empty() ? 0.0f : groupGap);

        PlaceStatusGroup(leftGroup, leftX, itemGap);
        PlaceStatusGroup(centerGroup, centerX, itemGap);
        PlaceStatusGroup(rightGroup, rightX, itemGap);

        return layout;
    }

    void RemoveSurfaceWidget(UI::UICanvas& canvas, const char* name)
    {
        UI::Widget::Ptr widget = canvas.FindWidget(name);
        if (widget)
        {
            canvas.RemoveWidget(std::move(widget));
        }
    }

    void ClearSurfaceWidgets(UI::UICanvas& canvas)
    {
        RemoveSurfaceWidget(canvas, RVX_EDITOR_SURFACE_MENU_BAR);
        RemoveSurfaceWidget(canvas, RVX_EDITOR_SURFACE_TOOLBAR);
        RemoveSurfaceWidget(canvas, RVX_EDITOR_SURFACE_STATUS_BAR);
    }

    std::string CommandWidgetName(const char* prefix,
                                  const std::string& ownerId,
                                  const std::string& commandId)
    {
        return std::string(prefix) + "." + ownerId + ".Item." + commandId;
    }

    bool IsVisibleToolbarCommand(const EditorCommandSurfaceItem& item)
    {
        return item.visible && item.type == EditorCommandSurfaceItemType::Command &&
               !item.commandId.empty();
    }

    uint32 CountVisibleToolbarCommands(const EditorToolbarSurface& toolbar,
                                       size_t startItemIndex)
    {
        uint32 count = 0;
        for (size_t itemIndex = startItemIndex; itemIndex < toolbar.items.size();
             ++itemIndex)
        {
            if (IsVisibleToolbarCommand(toolbar.items[itemIndex]))
            {
                ++count;
            }
        }
        return count;
    }

    uint32 CountVisibleToolbarCommands(
        const std::vector<EditorToolbarSurface>& toolbars,
        size_t startToolbarIndex,
        size_t startItemIndex)
    {
        uint32 count = 0;
        for (size_t toolbarIndex = startToolbarIndex; toolbarIndex < toolbars.size();
             ++toolbarIndex)
        {
            const size_t itemStart =
                toolbarIndex == startToolbarIndex ? startItemIndex : 0u;
            count += CountVisibleToolbarCommands(toolbars[toolbarIndex], itemStart);
        }
        return count;
    }

    void RecordToolbarIconStats(EditorCommandSurfaceRenderStats& stats,
                                const EditorCommandSurfaceItem& item,
                                const std::string& iconName)
    {
        ++stats.toolbarIconItemCount;
        if (item.iconName.empty() && !iconName.empty())
        {
            ++stats.toolbarResolvedIconItemCount;
        }
        if (iconName.empty())
        {
            ++stats.toolbarEmptyIconItemCount;
            ++stats.toolbarTextFallbackRiskCount;
        }
        else if (EditorVectorIconLibrary::IsKnownIcon(iconName))
        {
            ++stats.toolbarKnownIconItemCount;
        }
        else
        {
            ++stats.toolbarUnknownIconItemCount;
            ++stats.toolbarTextFallbackRiskCount;
        }
    }

    void ApplyButtonStyle(UI::Button& button,
                          const UI::UITheme& theme,
                          EditorTypographyRole role,
                          bool subtle)
    {
        button.GetStyle().fontSize = EditorTypography::GetFontSize(theme, role);
        button.GetStyle().textColor = theme.colors.text;
        if (subtle)
        {
            button.SetNormalColor(theme.colors.panelBackground);
            button.SetHoverColor(theme.colors.surfaceHover);
            button.SetPressedColor(theme.colors.surfaceActive);
        }
        else
        {
            button.SetNormalColor(theme.colors.surface);
            button.SetHoverColor(theme.colors.surfaceHover);
            button.SetPressedColor(theme.colors.surfaceActive);
        }
        button.SetDisabledColor(theme.colors.surface.WithAlpha(0.35f));
    }

    void ApplyIconButtonStyle(EditorIconButton& button,
                              const UI::UITheme& theme,
                              bool subtle)
    {
        button.GetStyle().fontSize =
            EditorTypography::GetFontSize(theme, EditorTypographyRole::Toolbar);
        button.GetStyle().textColor = theme.colors.text;
        button.SetShowText(false);
        button.SetIconSize(std::max(14.0f, theme.metrics.controlHeight * 0.50f));
        if (subtle)
        {
            button.SetSurfaceColors(theme.colors.panelBackground,
                                    theme.colors.surfaceHover,
                                    theme.colors.surfaceActive,
                                    theme.colors.panelBackground.WithAlpha(0.45f));
        }
        else
        {
            button.SetSurfaceColors(theme.colors.surface,
                                    theme.colors.surfaceHover,
                                    theme.colors.surfaceActive,
                                    theme.colors.surface.WithAlpha(0.35f));
        }
        button.SetIconColors(theme.colors.text,
                             theme.colors.text,
                             theme.colors.text,
                             theme.colors.textMuted.WithAlpha(0.55f));
        button.SetFocusIndicator(theme.colors.accent, 2.0f);
    }

    std::string BuildCommandTooltip(const EditorCommandSurfaceItem& item,
                                    const EditorCommandRegistry& registry,
                                    const EditorCommandSurfaceModel& surfaceModel,
                                    const std::string& fallbackLabel)
    {
        const EditorCommand* command = surfaceModel.ResolveCommand(item, registry);
        if (!command)
        {
            return fallbackLabel;
        }

        const std::string displayName = fallbackLabel.empty()
                                            ? command->desc.displayName
                                            : fallbackLabel;
        std::string tooltip;
        if (command->desc.tooltip.empty())
        {
            tooltip = displayName;
        }
        else if (displayName.empty())
        {
            tooltip = command->desc.tooltip;
        }
        else
        {
            tooltip = displayName + ": " + command->desc.tooltip;
        }

        if (!command->desc.enabled && !command->desc.disabledReason.empty())
        {
            tooltip += tooltip.empty() ? std::string() : std::string("\n");
            tooltip += "Unavailable: " + command->desc.disabledReason;
        }
        return tooltip;
    }

    std::string BuildCommandShortcutText(
        const EditorCommandSurfaceItem& item,
        const EditorCommandRegistry& registry,
        const EditorCommandSurfaceModel& surfaceModel)
    {
        const EditorCommand* command = surfaceModel.ResolveCommand(item, registry);
        if (!command || !command->desc.shortcut.valid)
        {
            return {};
        }

        return EditorShortcutBindingModel::FormatShortcut(command->desc.shortcut);
    }

    bool IsCommandCheckable(const EditorCommandSurfaceItem& item,
                            const EditorCommandRegistry& registry,
                            const EditorCommandSurfaceModel& surfaceModel)
    {
        const EditorCommand* command = surfaceModel.ResolveCommand(item, registry);
        return command && command->desc.checkable;
    }

    bool IsCommandChecked(const EditorCommandSurfaceItem& item,
                          const EditorCommandRegistry& registry,
                          const EditorCommandSurfaceModel& surfaceModel)
    {
        const EditorCommand* command = surfaceModel.ResolveCommand(item, registry);
        return command && command->desc.checkable && command->desc.checked;
    }

    bool MenuRequiresStateColumn(const EditorMenuSurface& menu,
                                 const EditorCommandRegistry& registry,
                                 const EditorCommandSurfaceModel& surfaceModel)
    {
        return std::any_of(
            menu.items.begin(),
            menu.items.end(),
            [&registry, &surfaceModel](const EditorCommandSurfaceItem& item) {
                return item.visible &&
                       item.type == EditorCommandSurfaceItemType::Command &&
                       IsCommandCheckable(item, registry, surfaceModel);
            });
    }

    bool IsExecutableMenuItem(const EditorCommandSurfaceItem& item,
                              const EditorCommandRegistry& registry,
                              const EditorCommandSurfaceModel& surfaceModel)
    {
        return item.visible && item.type == EditorCommandSurfaceItemType::Command &&
               surfaceModel.IsItemEnabled(item, registry);
    }

    class EditorCommandSurfaceDropdownPanel final : public UI::Panel
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

    class EditorCommandMenuStateIcon final : public UI::Widget
    {
    public:
        using Ptr = std::shared_ptr<EditorCommandMenuStateIcon>;

        static Ptr Create(bool checked,
                          const UI::UIColor& color,
                          float iconInset)
        {
            Ptr icon = std::make_shared<EditorCommandMenuStateIcon>();
            icon->SetChecked(checked);
            icon->SetColor(color);
            icon->SetIconInset(iconInset);
            icon->SetInteractive(false);
            return icon;
        }

        const char* GetTypeName() const override
        {
            return "EditorCommandMenuStateIcon";
        }

        bool IsChecked() const
        {
            return m_checked;
        }

        void SetChecked(bool checked)
        {
            m_checked = checked;
        }

        void SetColor(const UI::UIColor& color)
        {
            m_color = color;
        }

        void SetIconInset(float iconInset)
        {
            m_iconInset = std::max(0.0f, iconInset);
        }

    protected:
        void OnRender(UI::UIRenderer& renderer) override
        {
            if (!m_checked)
            {
                return;
            }

            const UI::Rect bounds = GetGlobalRect();
            const float iconSize = std::max(
                0.0f,
                std::min(bounds.width, bounds.height) - m_iconInset * 2.0f);
            if (iconSize <= 0.0f)
            {
                return;
            }

            EditorVectorIconDrawDesc iconDesc;
            iconDesc.name = "check";
            iconDesc.bounds = UI::Rect(bounds.x + (bounds.width - iconSize) * 0.5f,
                                       bounds.y + (bounds.height - iconSize) * 0.5f,
                                       iconSize,
                                       iconSize);
            iconDesc.color = m_color;
            (void)EditorVectorIconLibrary::Draw(renderer, iconDesc);
        }

    private:
        bool m_checked = false;
        UI::UIColor m_color{0.18f, 0.55f, 0.95f, 1.0f};
        float m_iconInset = 2.0f;
    };

    class EditorCommandToolbarOverflowIcon final : public UI::Widget
    {
    public:
        using Ptr = std::shared_ptr<EditorCommandToolbarOverflowIcon>;

        static Ptr Create(std::string iconName,
                          std::string fallbackLabel,
                          const UI::UIColor& color,
                          float iconInset)
        {
            Ptr icon = std::make_shared<EditorCommandToolbarOverflowIcon>();
            icon->m_iconName = std::move(iconName);
            icon->m_fallbackLabel = std::move(fallbackLabel);
            icon->m_color = color;
            icon->m_iconInset = std::max(0.0f, iconInset);
            icon->SetInteractive(false);
            return icon;
        }

        const char* GetTypeName() const override
        {
            return "EditorCommandToolbarOverflowIcon";
        }

        const EditorVectorIconDrawStats& GetLastIconDrawStats() const
        {
            return m_lastIconDrawStats;
        }

    protected:
        void OnRender(UI::UIRenderer& renderer) override
        {
            const UI::Rect bounds = GetGlobalRect();
            const float iconSize = std::max(
                0.0f,
                std::min(bounds.width, bounds.height) - m_iconInset * 2.0f);
            if (iconSize <= 0.0f)
            {
                return;
            }

            EditorVectorIconDrawDesc iconDesc;
            iconDesc.name = m_iconName;
            iconDesc.bounds = UI::Rect(bounds.x + (bounds.width - iconSize) * 0.5f,
                                       bounds.y + (bounds.height - iconSize) * 0.5f,
                                       iconSize,
                                       iconSize);
            iconDesc.color = m_color;
            iconDesc.fallbackLabel = m_fallbackLabel;
            iconDesc.fallbackFontSize = std::max(12.0f, iconSize * 0.72f);
            m_lastIconDrawStats = EditorVectorIconLibrary::Draw(renderer, iconDesc);
        }

    private:
        std::string m_iconName;
        std::string m_fallbackLabel;
        UI::UIColor m_color{0.90f, 0.92f, 0.94f, 1.0f};
        float m_iconInset = 3.0f;
        EditorVectorIconDrawStats m_lastIconDrawStats;
    };

    UI::Label::Ptr CreateLabel(const std::string& name,
                               const std::string& text,
                               const UI::UITheme& theme,
                               EditorTypographyRole role,
                               float x,
                               float y,
                               float width,
                               float height)
    {
        UI::Label::Ptr label = UI::Label::Create(text);
        label->SetName(name);
        label->SetPosition(x, y);
        label->SetSize(width, height);
        label->SetFontSize(EditorTypography::GetFontSize(theme, role));
        label->SetTextColor(role == EditorTypographyRole::Status
                                ? theme.colors.text
                                : theme.colors.textMuted);
        label->SetVerticalAlign(UI::VerticalAlign::Middle);
        label->SetInteractive(false);
        return label;
    }

    UI::Button::Ptr CreateCommandButton(const std::string& name,
                                        const std::string& text,
                                        const std::string& commandId,
                                        bool enabled,
                                        bool subtle,
                                        EditorUIHost* host,
                                        const UI::UITheme& theme,
                                        EditorTypographyRole role,
                                        std::function<void()> afterExecute = {})
    {
        UI::Button::Ptr button = UI::Button::Create(text);
        button->SetName(name);
        button->SetEnabled(enabled);
        ApplyButtonStyle(*button, theme, role, subtle);
        if (!commandId.empty())
        {
            button->SetOnClick([host,
                                commandId,
                                afterExecute = std::move(afterExecute)](
                                   const UI::UIEvent& event) {
                (void)event;
                if (host)
                {
                    host->ExecuteCommand(commandId);
                }
                if (afterExecute)
                {
                    afterExecute();
                }
            });
        }
        return button;
    }

    EditorIconButton::Ptr CreateCommandIconButton(
        const std::string& name,
        const std::string& text,
        const std::string& iconName,
        const std::string& commandId,
        bool enabled,
        bool subtle,
        EditorUIHost* host,
        const UI::UITheme& theme,
        std::function<void()> afterExecute = {})
    {
        EditorIconButton::Ptr button = EditorIconButton::Create(text, iconName);
        button->SetName(name);
        button->SetEnabled(enabled);
        ApplyIconButtonStyle(*button, theme, subtle);
        if (!commandId.empty())
        {
            button->SetOnClick([host,
                                commandId,
                                afterExecute = std::move(afterExecute)](
                                   const UI::UIEvent& event) {
                (void)event;
                if (host)
                {
                    host->ExecuteCommand(commandId);
                }
                if (afterExecute)
                {
                    afterExecute();
                }
            });
        }
        return button;
    }
}

bool EditorCommandSurfaceRenderer::Build(const EditorCommandSurfaceRenderDesc& desc)
{
    m_lastBuildStats = {};
    if (!desc.ui || !desc.commandRegistry || !desc.surfaceModel)
    {
        return false;
    }

    UI::UICanvas& canvas = desc.ui->GetCanvas();
    ClearSurfaceWidgets(canvas);

    const UI::UITheme& theme = desc.ui->GetTheme();
    const EditorShellMetrics shellMetrics = EditorShellMetricPolicy::Resolve(theme);
    const float width = static_cast<float>(desc.ui->GetWidth());
    const float height = static_cast<float>(desc.ui->GetHeight());
    const float menuHeight = shellMetrics.menuBarHeight;
    const float toolbarHeight = shellMetrics.commandToolbarHeight;
    const float statusHeight = shellMetrics.statusBarHeight;
    const float padding = shellMetrics.contentPadding;
    const float compactItemGap = shellMetrics.compactGap;
    const float statusItemGap = shellMetrics.statusItemGap;
    m_lastBuildStats.menuBarHeight = desc.buildMenuBar ? menuHeight : 0.0f;
    m_lastBuildStats.toolbarHeight = desc.buildToolbars ? toolbarHeight : 0.0f;
    m_lastBuildStats.statusBarHeight = desc.buildStatusBar ? statusHeight : 0.0f;
    m_lastBuildStats.topReservedHeight =
        m_lastBuildStats.menuBarHeight + m_lastBuildStats.toolbarHeight;
    m_lastBuildStats.bottomReservedHeight = m_lastBuildStats.statusBarHeight;

    const EditorMenuSurface* openMenuSurface = nullptr;
    float openMenuX = padding;
    float openMenuWidth = 0.0f;
    std::vector<EditorCommandToolbarOverflowEntry> toolbarOverflowEntries;
    UI::Rect toolbarOverflowAnchorRect;

    if (desc.buildMenuBar)
    {
        UI::Panel::Ptr menuBar = UI::Panel::Create();
        menuBar->SetName(RVX_EDITOR_SURFACE_MENU_BAR);
        menuBar->SetPosition(0.0f, 0.0f);
        menuBar->SetSize(width, menuHeight);
        menuBar->SetBackgroundColor(theme.colors.panelBackground);
        menuBar->SetBorderColor(theme.colors.border);
        menuBar->SetBorderWidth(theme.metrics.borderWidth);

        float x = padding;
        for (const EditorMenuSurface& menu : desc.surfaceModel->GetMenus())
        {
            ++m_lastBuildStats.menuCount;
            const float titleWidth =
                EstimateTextWidth(menu.title,
                                  theme,
                                  EditorTypographyRole::Menu,
                                  42.0f,
                                  180.0f);
            const bool open = m_openMenuId == menu.id;
            UI::Button::Ptr title =
                UI::Button::Create(menu.title);
            title->SetName("Editor.CommandSurface.Menu." + menu.id + ".Title");
            ApplyButtonStyle(*title, theme, EditorTypographyRole::Menu, true);
            if (open)
            {
                title->SetNormalColor(theme.colors.surfaceActive);
            }
            const float menuItemHeight =
                std::min(shellMetrics.panelControlHeight, menuHeight - 6.0f);
            const float menuItemY = std::max(3.0f,
                                             (menuHeight - menuItemHeight) * 0.5f);
            title->SetPosition(x, menuItemY);
            title->SetSize(titleWidth, menuItemHeight);
            title->SetOnClick([this, menuId = menu.id](const UI::UIEvent& event) {
                (void)event;
                CloseToolbarOverflow();
                if (m_openMenuId == menuId)
                {
                    CloseOpenMenu();
                }
                else
                {
                    m_openMenuId = menuId;
                    m_openMenuSelectedItemIndex = -1;
                }
            });
            menuBar->AddChild(title);

            for (const EditorCommandSurfaceItem& item : menu.items)
            {
                if (!item.visible)
                {
                    continue;
                }
                if (item.type == EditorCommandSurfaceItemType::Command)
                {
                    ++m_lastBuildStats.menuItemCount;
                    const EditorCommand* command =
                        desc.surfaceModel->ResolveCommand(item,
                                                           *desc.commandRegistry);
                    if (command && command->desc.enabled)
                    {
                        ++m_lastBuildStats.executableItemCount;
                    }
                    else if (command && !command->desc.disabledReason.empty())
                    {
                        ++m_lastBuildStats.menuDisabledReasonItemCount;
                    }
                }
            }

            if (open)
            {
                openMenuSurface = &menu;
                openMenuX = x;
                openMenuWidth = std::max(titleWidth, 180.0f);
            }

            x += titleWidth + padding * 0.5f;
        }

        if (!openMenuSurface && !m_openMenuId.empty())
        {
            CloseOpenMenu();
        }

        canvas.AddWidget(menuBar);
    }
    else
    {
        m_openMenuId.clear();
    }

    if (desc.buildToolbars)
    {
        UI::Panel::Ptr toolbar = UI::Panel::Create();
        toolbar->SetName(RVX_EDITOR_SURFACE_TOOLBAR);
        toolbar->SetPosition(0.0f, menuHeight);
        toolbar->SetSize(width, toolbarHeight);
        toolbar->SetBackgroundColor(theme.colors.windowBackground);
        toolbar->SetBorderColor(theme.colors.border);
        toolbar->SetBorderWidth(theme.metrics.borderWidth);

        float x = padding;
        const float toolbarRightLimit = std::max(padding, width - padding);
        const float iconButtonSize =
            std::max(24.0f, toolbarHeight - shellMetrics.contentPadding);
        const float iconButtonY =
            std::max(3.0f, (toolbarHeight - iconButtonSize) * 0.5f);
        EditorIconButton::Ptr overflowButton;
        bool overflowActive = false;

        const auto hasToolbarRoom = [&](float itemWidth, float reservedWidth) {
            return x + itemWidth + reservedWidth <= toolbarRightLimit + 0.5f;
        };
        const auto addOverflowIndicator = [&]() {
            if (overflowButton || m_lastBuildStats.toolbarOverflowIndicatorClippedCount > 0u)
            {
                return;
            }

            const float overflowX =
                std::min(x, toolbarRightLimit - iconButtonSize);
            if (overflowX < padding ||
                overflowX + iconButtonSize > toolbarRightLimit + 0.5f)
            {
                ++m_lastBuildStats.toolbarOverflowIndicatorClippedCount;
                return;
            }

            overflowButton = CreateCommandIconButton(
                "Editor.CommandSurface.Toolbar.Overflow",
                "More",
                "more-horizontal",
                "",
                true,
                false,
                desc.host,
                theme);
            overflowButton->SetPosition(overflowX, iconButtonY);
            overflowButton->SetSize(iconButtonSize, iconButtonSize);
            overflowButton->SetOnClick([this](const UI::UIEvent& event) {
                (void)event;
                CloseOpenMenu();
                if (m_toolbarOverflowOpen)
                {
                    CloseToolbarOverflow();
                }
                else
                {
                    m_toolbarOverflowOpen = true;
                }
            });
            toolbar->AddChild(overflowButton);
            ++m_lastBuildStats.toolbarOverflowIndicatorCount;
            toolbarOverflowAnchorRect =
                UI::Rect(overflowX, menuHeight + iconButtonY, iconButtonSize, iconButtonSize);
            x = overflowX + iconButtonSize + compactItemGap;
        };

        const std::vector<EditorToolbarSurface>& toolbars =
            desc.surfaceModel->GetToolbars();
        for (size_t toolbarIndex = 0; toolbarIndex < toolbars.size(); ++toolbarIndex)
        {
            const EditorToolbarSurface& toolbarSurface = toolbars[toolbarIndex];
            ++m_lastBuildStats.toolbarCount;
            const float titleWidth = EstimateTextWidth(toolbarSurface.title,
                                                       theme,
                                                       EditorTypographyRole::Toolbar,
                                                       42.0f,
                                                       140.0f);
            const uint32 toolbarCommandCount =
                CountVisibleToolbarCommands(toolbarSurface, 0u);
            const float reserveAfterTitle =
                toolbarCommandCount == 0u
                    ? 0.0f
                    : compactItemGap + iconButtonSize +
                          (toolbarCommandCount > 1u
                               ? compactItemGap + iconButtonSize
                               : 0.0f);
            if (!overflowActive &&
                hasToolbarRoom(titleWidth, reserveAfterTitle))
            {
                UI::Label::Ptr title =
                    CreateLabel("Editor.CommandSurface.Toolbar." + toolbarSurface.id,
                                toolbarSurface.title,
                                theme,
                                EditorTypographyRole::Toolbar,
                                x,
                                0.0f,
                                titleWidth,
                                toolbarHeight);
                toolbar->AddChild(title);
                x += title->GetWidth() + compactItemGap;
            }
            else if (!toolbarSurface.title.empty())
            {
                ++m_lastBuildStats.toolbarTitleHiddenCount;
            }

            for (size_t itemIndex = 0; itemIndex < toolbarSurface.items.size();
                 ++itemIndex)
            {
                const EditorCommandSurfaceItem& item = toolbarSurface.items[itemIndex];
                if (!item.visible)
                {
                    continue;
                }
                if (item.type == EditorCommandSurfaceItemType::Separator)
                {
                    const uint32 remainingCommands =
                        CountVisibleToolbarCommands(toolbars,
                                                    toolbarIndex,
                                                    itemIndex + 1u);
                    const float reservedWidth =
                        remainingCommands > 0u
                            ? compactItemGap + iconButtonSize
                            : 0.0f;
                    if (!overflowActive && hasToolbarRoom(padding, reservedWidth))
                    {
                        x += padding;
                    }
                    else
                    {
                        ++m_lastBuildStats.toolbarSeparatorCollapsedCount;
                    }
                    continue;
                }

                ++m_lastBuildStats.toolbarItemCount;
                const std::string label =
                    desc.surfaceModel->GetItemDisplayName(item,
                                                          *desc.commandRegistry);
                if (label.empty())
                {
                    continue;
                }

                const bool enabled = desc.surfaceModel->IsItemEnabled(item, *desc.commandRegistry);
                const std::string iconName =
                    EditorCommandIconResolver::ResolveIconName(item.commandId,
                                                               item.iconName);
                const std::string shortcutText =
                    BuildCommandShortcutText(item,
                                             *desc.commandRegistry,
                                             *desc.surfaceModel);
                RecordToolbarIconStats(m_lastBuildStats, item, iconName);

                const uint32 remainingCommands =
                    CountVisibleToolbarCommands(toolbars,
                                                toolbarIndex,
                                                itemIndex + 1u);
                const float reservedWidth =
                    remainingCommands > 0u
                        ? compactItemGap + iconButtonSize
                        : 0.0f;
                if (overflowActive || !hasToolbarRoom(iconButtonSize, reservedWidth))
                {
                    ++m_lastBuildStats.toolbarOverflowItemCount;
                    EditorCommandToolbarOverflowEntry overflowEntry;
                    overflowEntry.item = item;
                    overflowEntry.label = label;
                    overflowEntry.iconName = iconName;
                    overflowEntry.shortcutText = shortcutText;
                    overflowEntry.enabled = enabled;
                    toolbarOverflowEntries.push_back(std::move(overflowEntry));
                    if (!overflowActive)
                    {
                        overflowActive = true;
                        addOverflowIndicator();
                    }
                    continue;
                }

                EditorIconButton::Ptr button = CreateCommandIconButton(
                    CommandWidgetName("Editor.CommandSurface.Toolbar",
                                      toolbarSurface.id,
                                      item.commandId),
                    label,
                    iconName,
                    item.commandId,
                    enabled,
                    false,
                    desc.host,
                    theme);
                button->SetPosition(x, iconButtonY);
                button->SetSize(iconButtonSize, iconButtonSize);
                button->SetTooltipText(BuildCommandTooltip(item,
                                                           *desc.commandRegistry,
                                                           *desc.surfaceModel,
                                                           label));
                toolbar->AddChild(button);
                x += button->GetWidth() + compactItemGap;
                ++m_lastBuildStats.toolbarVisibleIconItemCount;
                if (enabled)
                {
                    ++m_lastBuildStats.executableItemCount;
                }
            }

            if (!overflowActive)
            {
                x += padding;
            }
        }

        if (overflowButton)
        {
            overflowButton->SetTooltipText(
                std::to_string(m_lastBuildStats.toolbarOverflowItemCount) +
                " toolbar actions hidden at this width.");
        }

        canvas.AddWidget(toolbar);
    }
    else
    {
        m_toolbarOverflowOpen = false;
    }

    if (desc.buildStatusBar)
    {
        UI::Panel::Ptr statusBar = UI::Panel::Create();
        statusBar->SetName(RVX_EDITOR_SURFACE_STATUS_BAR);
        statusBar->SetPosition(0.0f, std::max(0.0f, height - statusHeight));
        statusBar->SetSize(width, statusHeight);
        statusBar->SetBackgroundColor(theme.colors.panelBackground);
        statusBar->SetBorderColor(theme.colors.border);
        statusBar->SetBorderWidth(theme.metrics.borderWidth);

        const std::vector<StatusLayoutItem> statusLayout =
            BuildStatusLayout(desc.surfaceModel->GetStatusItems(),
                              width,
                              theme,
                              padding,
                              statusItemGap);
        m_lastBuildStats.statusItemCount =
            static_cast<uint32>(statusLayout.size());

        for (const StatusLayoutItem& item : statusLayout)
        {
            if (item.width <= 0.0f)
            {
                continue;
            }
            switch (ResolveStatusLayoutZone(item))
            {
                case StatusLayoutZone::Left:
                    ++m_lastBuildStats.statusLeftItemCount;
                    break;
                case StatusLayoutZone::Center:
                    ++m_lastBuildStats.statusCenterItemCount;
                    break;
                case StatusLayoutZone::Right:
                    ++m_lastBuildStats.statusRightItemCount;
                    break;
            }

            UI::Label::Ptr label = CreateLabel("Editor.CommandSurface.Status." + item.id,
                                               item.text,
                                               theme,
                                               EditorTypographyRole::Status,
                                               item.x,
                                               0.0f,
                                               item.width,
                                               statusHeight);
            label->SetOverflowMode(UI::TextOverflowMode::EndEllipsis);
            label->SetTextAlign(item.align);
            label->SetTooltipText(item.text);
            statusBar->AddChild(label);
        }

        canvas.AddWidget(statusBar);
    }

    if (m_toolbarOverflowOpen && toolbarOverflowEntries.empty())
    {
        CloseToolbarOverflow();
    }
    m_toolbarOverflowEntries = toolbarOverflowEntries;

    if (m_toolbarOverflowOpen && !toolbarOverflowEntries.empty())
    {
        const float itemHeight = shellMetrics.panelControlHeight;
        const float horizontalPadding = padding;
        const float verticalPadding = shellMetrics.compactGap;
        const float iconColumnWidth = std::max(26.0f, itemHeight * 0.78f);
        const float textInset = std::max(8.0f, padding);
        const float shortcutGap = std::max(18.0f, padding * 2.0f);

        float dropdownWidth = 220.0f;
        for (const EditorCommandToolbarOverflowEntry& entry :
             toolbarOverflowEntries)
        {
            const float labelWidth =
                EstimateTextWidth(entry.label,
                                  theme,
                                  EditorTypographyRole::Menu,
                                  120.0f,
                                  280.0f);
            const float shortcutWidth =
                entry.shortcutText.empty()
                    ? 0.0f
                    : MeasureTextWidthForLayout(entry.shortcutText,
                                                theme,
                                                EditorTypographyRole::Menu);
            dropdownWidth = std::max(dropdownWidth,
                                     iconColumnWidth + textInset + labelWidth +
                                         (entry.shortcutText.empty()
                                              ? 0.0f
                                              : shortcutGap + shortcutWidth) +
                                         textInset);
        }
        dropdownWidth = std::min(360.0f,
                                 std::min(dropdownWidth,
                                          std::max(160.0f, width - padding * 2.0f)));
        const float availableBelow =
            std::max(itemHeight,
                     height - (menuHeight + toolbarHeight) - statusHeight -
                         padding);
        const uint32 maxVisibleRowsByHeight = static_cast<uint32>(std::max(
            1.0f,
            std::floor((availableBelow - verticalPadding * 2.0f) / itemHeight)));
        const uint32 maxVisibleRows =
            std::max(1u, std::min<uint32>(8u, maxVisibleRowsByHeight));
        const uint32 visibleRows =
            std::min<uint32>(static_cast<uint32>(toolbarOverflowEntries.size()),
                             maxVisibleRows);
        if (m_toolbarOverflowSelectedItemIndex < 0 ||
            static_cast<size_t>(m_toolbarOverflowSelectedItemIndex) >=
                toolbarOverflowEntries.size() ||
            !toolbarOverflowEntries[static_cast<size_t>(
                 m_toolbarOverflowSelectedItemIndex)]
                 .enabled)
        {
            m_toolbarOverflowSelectedItemIndex = -1;
            for (uint32 index = 0u;
                 index < static_cast<uint32>(toolbarOverflowEntries.size());
                 ++index)
            {
                if (toolbarOverflowEntries[index].enabled)
                {
                    m_toolbarOverflowSelectedItemIndex =
                        static_cast<int32>(index);
                    break;
                }
            }
        }
        if (m_toolbarOverflowSelectedItemIndex >= 0)
        {
            const uint32 selectedIndex =
                static_cast<uint32>(m_toolbarOverflowSelectedItemIndex);
            if (selectedIndex < m_toolbarOverflowScrollOffsetRows)
            {
                m_toolbarOverflowScrollOffsetRows = selectedIndex;
            }
            else if (selectedIndex >=
                     m_toolbarOverflowScrollOffsetRows + visibleRows)
            {
                m_toolbarOverflowScrollOffsetRows =
                    selectedIndex + 1u - visibleRows;
            }
        }
        const uint32 maxScrollRows =
            static_cast<uint32>(toolbarOverflowEntries.size()) > visibleRows
                ? static_cast<uint32>(toolbarOverflowEntries.size()) - visibleRows
                : 0u;
        m_toolbarOverflowScrollOffsetRows =
            std::min(m_toolbarOverflowScrollOffsetRows, maxScrollRows);

        const float viewportHeight = itemHeight * static_cast<float>(visibleRows);
        const float contentHeight =
            itemHeight * static_cast<float>(toolbarOverflowEntries.size());
        const float dropdownHeight = verticalPadding * 2.0f + viewportHeight;
        const float dropdownX =
            std::clamp(toolbarOverflowAnchorRect.x,
                       padding,
                       std::max(padding, width - dropdownWidth - padding));
        const float dropdownY = menuHeight + toolbarHeight;

        UI::Panel::Ptr dropdown = std::make_shared<EditorCommandSurfaceDropdownPanel>();
        dropdown->SetName(RVX_EDITOR_SURFACE_TOOLBAR_OVERFLOW_DROPDOWN);
        dropdown->SetPosition(dropdownX, dropdownY);
        dropdown->SetSize(dropdownWidth, dropdownHeight);
        dropdown->SetBackgroundColor(theme.colors.panelBackground);
        dropdown->SetBorderColor(theme.colors.border);
        dropdown->SetBorderWidth(theme.metrics.borderWidth);
        static_cast<EditorCommandSurfaceDropdownPanel*>(dropdown.get())
            ->SetOnKeyDown([this, host = desc.host](uint32 keyCode) {
                if (!host)
                {
                    return false;
                }
                return HandleToolbarOverflowKeyDown(keyCode, *host);
            });

        UI::ScrollView::Ptr viewport = UI::ScrollView::Create();
        viewport->SetName("Editor.CommandSurface.Toolbar.Overflow.Viewport");
        viewport->SetPosition(0.0f, verticalPadding);
        viewport->SetSize(dropdownWidth, viewportHeight);
        viewport->SetBackgroundColor(UI::UIColor::Transparent());
        viewport->SetBorderWidth(0.0f);
        viewport->SetScrollbarNamePrefix(
            "Editor.CommandSurface.Toolbar.Overflow.Scrollbar");
        viewport->SetWheelStep(itemHeight);
        viewport->SetContentSize(dropdownWidth, contentHeight);
        viewport->SetScrollOffset(
            0.0f,
            static_cast<float>(m_toolbarOverflowScrollOffsetRows) * itemHeight);
        viewport->SetOnScrollChanged(
            [this, itemHeight, maxScrollRows](const Vec2& offset) {
                const uint32 rowOffset = static_cast<uint32>(
                    std::floor((offset.y / std::max(1.0f, itemHeight)) + 0.5f));
                m_toolbarOverflowScrollOffsetRows =
                    std::min(rowOffset, maxScrollRows);
            });

        for (uint32 entryIndex = 0u;
             entryIndex < static_cast<uint32>(toolbarOverflowEntries.size());
             ++entryIndex)
        {
            const EditorCommandToolbarOverflowEntry& entry =
                toolbarOverflowEntries[entryIndex];
            UI::Button::Ptr button = CreateCommandButton(
                std::string("Editor.CommandSurface.Toolbar.Overflow.Item.") +
                    entry.item.commandId,
                "",
                entry.item.commandId,
                entry.enabled,
                true,
                desc.host,
                theme,
                EditorTypographyRole::Menu,
                [this]() {
                    CloseToolbarOverflow();
                });
            const bool selected =
                m_toolbarOverflowSelectedItemIndex ==
                static_cast<int32>(entryIndex);
            if (selected)
            {
                button->SetNormalColor(theme.colors.surfaceActive);
                button->SetHoverColor(theme.colors.surfaceActive);
            }
            button->SetPosition(horizontalPadding * 0.5f,
                                itemHeight * static_cast<float>(entryIndex));
            button->SetSize(dropdownWidth - horizontalPadding, itemHeight);
            button->SetTooltipText(BuildCommandTooltip(entry.item,
                                                       *desc.commandRegistry,
                                                       *desc.surfaceModel,
                                                       entry.label));

            EditorCommandToolbarOverflowIcon::Ptr icon =
                EditorCommandToolbarOverflowIcon::Create(
                    entry.iconName,
                    entry.label,
                    entry.enabled ? theme.colors.text
                                  : theme.colors.textMuted.WithAlpha(0.45f),
                    std::max(3.0f, itemHeight * 0.24f));
            icon->SetName(button->GetName() + ".Icon");
            icon->SetPosition(textInset, 0.0f);
            icon->SetSize(iconColumnWidth, itemHeight);
            button->AddChild(icon);
            ++m_lastBuildStats.toolbarOverflowMenuIconCount;
            if (entry.iconName.empty() ||
                !EditorVectorIconLibrary::IsKnownIcon(entry.iconName))
            {
                ++m_lastBuildStats.toolbarOverflowMenuTextFallbackRiskCount;
            }

            const float shortcutWidth =
                entry.shortcutText.empty()
                    ? 0.0f
                    : MeasureTextWidthForLayout(entry.shortcutText,
                                                theme,
                                                EditorTypographyRole::Menu);
            const float labelX = textInset + iconColumnWidth;
            const float labelWidth =
                std::max(0.0f,
                         dropdownWidth - horizontalPadding - labelX - textInset -
                             (entry.shortcutText.empty()
                                  ? 0.0f
                                  : shortcutGap + shortcutWidth));
            UI::Label::Ptr label = CreateLabel(button->GetName() + ".Label",
                                               entry.label,
                                               theme,
                                               EditorTypographyRole::Menu,
                                               labelX,
                                               0.0f,
                                               labelWidth,
                                               itemHeight);
            label->SetTextColor(entry.enabled
                                    ? theme.colors.text
                                    : theme.colors.textMuted.WithAlpha(0.55f));
            label->SetOverflowMode(UI::TextOverflowMode::EndEllipsis);
            button->AddChild(label);

            if (!entry.shortcutText.empty())
            {
                UI::Label::Ptr shortcutLabel =
                    CreateLabel(button->GetName() + ".Shortcut",
                                entry.shortcutText,
                                theme,
                                EditorTypographyRole::Menu,
                                std::max(labelX,
                                         dropdownWidth - horizontalPadding -
                                             textInset - shortcutWidth),
                                0.0f,
                                shortcutWidth,
                                itemHeight);
                shortcutLabel->SetTextAlign(UI::TextAlign::Right);
                shortcutLabel->SetTextColor(entry.enabled
                                                ? theme.colors.textMuted
                                                : theme.colors.textMuted.WithAlpha(0.45f));
                button->AddChild(shortcutLabel);
                ++m_lastBuildStats.menuShortcutItemCount;
            }

            viewport->AddContentChild(button);
            ++m_lastBuildStats.toolbarOverflowMenuItemCount;
            if (entry.enabled)
            {
                ++m_lastBuildStats.executableItemCount;
            }
        }
        dropdown->AddChild(viewport);

        ++m_lastBuildStats.dropdownCount;
        m_lastBuildStats.toolbarOverflowMenuVisibleItemCount = visibleRows;
        m_lastBuildStats.toolbarOverflowMenuMaxVisibleItemCount = maxVisibleRows;
        m_lastBuildStats.toolbarOverflowMenuScrollOffsetRows =
            m_toolbarOverflowScrollOffsetRows;
        m_lastBuildStats.toolbarOverflowMenuSelectedItemIndex =
            m_toolbarOverflowSelectedItemIndex;
        if (desc.popupLayer)
        {
            desc.popupLayer->AddPopup(dropdown);
        }
        else
        {
            canvas.AddWidget(dropdown);
        }
    }

    if (openMenuSurface)
    {
        const float itemHeight = shellMetrics.panelControlHeight;
        const float separatorHeight = std::max(6.0f, shellMetrics.compactGap);
        const float horizontalPadding = padding;
        const float verticalPadding = shellMetrics.compactGap;
        const float menuTextInset = std::max(8.0f, padding);
        const float shortcutGap = std::max(24.0f, padding * 3.0f);
        const bool hasStateColumn =
            MenuRequiresStateColumn(*openMenuSurface,
                                    *desc.commandRegistry,
                                    *desc.surfaceModel);
        const float stateColumnWidth =
            hasStateColumn ? std::max(18.0f, itemHeight * 0.72f) : 0.0f;

        float dropdownWidth = openMenuWidth;
        float dropdownHeight = verticalPadding * 2.0f;
        for (const EditorCommandSurfaceItem& item : openMenuSurface->items)
        {
            if (!item.visible)
            {
                continue;
            }
            if (item.type == EditorCommandSurfaceItemType::Separator)
            {
                dropdownHeight += separatorHeight;
                continue;
            }

            const std::string label = desc.surfaceModel->GetItemDisplayName(
                item,
                *desc.commandRegistry);
            if (label.empty())
            {
                continue;
            }
            const std::string shortcutText = BuildCommandShortcutText(
                item,
                *desc.commandRegistry,
                *desc.surfaceModel);
            const float labelWidth = EstimateTextWidth(label,
                                                       theme,
                                                       EditorTypographyRole::Menu,
                                                       150.0f,
                                                       360.0f);
            const float shortcutWidth =
                shortcutText.empty()
                    ? 0.0f
                    : MeasureTextWidthForLayout(shortcutText,
                                                theme,
                                                EditorTypographyRole::Menu);
            const float desiredWidth =
                stateColumnWidth + labelWidth +
                (shortcutText.empty() ? 0.0f : shortcutGap + shortcutWidth) +
                menuTextInset;
            dropdownWidth = std::max(dropdownWidth, std::min(520.0f, desiredWidth));
            dropdownHeight += itemHeight;
        }

        NormalizeOpenMenuSelection(*openMenuSurface,
                                   *desc.commandRegistry,
                                   *desc.surfaceModel);

        UI::Panel::Ptr dropdown = std::make_shared<EditorCommandSurfaceDropdownPanel>();
        dropdown->SetName(RVX_EDITOR_SURFACE_MENU_DROPDOWN);
        dropdown->SetPosition(openMenuX, menuHeight);
        dropdown->SetSize(dropdownWidth + horizontalPadding, dropdownHeight);
        dropdown->SetBackgroundColor(theme.colors.panelBackground);
        dropdown->SetBorderColor(theme.colors.border);
        dropdown->SetBorderWidth(theme.metrics.borderWidth);
        static_cast<EditorCommandSurfaceDropdownPanel*>(dropdown.get())
            ->SetOnKeyDown([this,
                            host = desc.host,
                            registry = desc.commandRegistry,
                            surfaceModel = desc.surfaceModel](uint32 keyCode) {
                if (!host || !registry || !surfaceModel)
                {
                    return false;
                }
                return HandleOpenMenuKeyDown(keyCode,
                                             *host,
                                             *registry,
                                             *surfaceModel);
            });

        float y = verticalPadding;
        uint32 separatorIndex = 0;
        for (size_t itemIndex = 0; itemIndex < openMenuSurface->items.size(); ++itemIndex)
        {
            const EditorCommandSurfaceItem& item = openMenuSurface->items[itemIndex];
            if (!item.visible)
            {
                continue;
            }
            if (item.type == EditorCommandSurfaceItemType::Separator)
            {
                UI::Panel::Ptr separator = UI::Panel::Create();
                separator->SetName("Editor.CommandSurface.Menu." + openMenuSurface->id +
                                   ".Separator." + std::to_string(separatorIndex++));
                separator->SetInteractive(false);
                separator->SetPosition(horizontalPadding * 0.5f,
                                       y + separatorHeight * 0.5f);
                separator->SetSize(dropdownWidth, theme.metrics.borderWidth);
                separator->SetBackgroundColor(theme.colors.border);
                separator->SetBorderWidth(0.0f);
                dropdown->AddChild(separator);
                y += separatorHeight;
                continue;
            }

            const std::string label = desc.surfaceModel->GetItemDisplayName(
                item,
                *desc.commandRegistry);
            if (label.empty())
            {
                continue;
            }

            const bool enabled = desc.surfaceModel->IsItemEnabled(item, *desc.commandRegistry);
            const bool checkable = IsCommandCheckable(item,
                                                      *desc.commandRegistry,
                                                      *desc.surfaceModel);
            const bool checked = IsCommandChecked(item,
                                                  *desc.commandRegistry,
                                                  *desc.surfaceModel);
            const bool selected =
                enabled &&
                m_openMenuSelectedItemIndex == static_cast<int32>(itemIndex);
            UI::Button::Ptr button = CreateCommandButton(
                CommandWidgetName("Editor.CommandSurface.Menu",
                                  openMenuSurface->id,
                                  item.commandId),
                "",
                item.commandId,
                enabled,
                true,
                desc.host,
                theme,
                EditorTypographyRole::Menu,
                [this]() {
                    CloseOpenMenu();
                });
            if (selected)
            {
                button->SetNormalColor(theme.colors.surfaceActive);
                button->SetHoverColor(theme.colors.surfaceActive);
            }
            button->SetPosition(horizontalPadding * 0.5f, y);
            button->SetSize(dropdownWidth, itemHeight);
            button->SetTooltipText(BuildCommandTooltip(item,
                                                       *desc.commandRegistry,
                                                       *desc.surfaceModel,
                                                       label));

            const std::string shortcutText = BuildCommandShortcutText(
                item,
                *desc.commandRegistry,
                *desc.surfaceModel);
            const float shortcutWidth =
                shortcutText.empty()
                    ? 0.0f
                    : MeasureTextWidthForLayout(shortcutText,
                                                theme,
                                                EditorTypographyRole::Menu);
            const float labelWidth = std::max(
                0.0f,
                dropdownWidth - menuTextInset * 2.0f - stateColumnWidth -
                    (shortcutText.empty() ? 0.0f : shortcutGap + shortcutWidth));

            if (hasStateColumn)
            {
                EditorCommandMenuStateIcon::Ptr stateIcon =
                    EditorCommandMenuStateIcon::Create(
                        checked,
                        enabled ? theme.colors.accent
                                : theme.colors.textMuted.WithAlpha(0.45f),
                        std::max(2.0f, itemHeight * 0.18f));
                stateIcon->SetName(button->GetName() + ".State");
                stateIcon->SetPosition(menuTextInset, 0.0f);
                stateIcon->SetSize(stateColumnWidth, itemHeight);
                button->AddChild(stateIcon);
                if (checkable && checked)
                {
                    ++m_lastBuildStats.menuCheckedItemCount;
                }
            }

            UI::Label::Ptr itemLabel = CreateLabel(button->GetName() + ".Label",
                                                   label,
                                                   theme,
                                                   EditorTypographyRole::Menu,
                                                   menuTextInset + stateColumnWidth,
                                                   0.0f,
                                                   labelWidth,
                                                   itemHeight);
            itemLabel->SetTextColor(enabled ? theme.colors.text
                                            : theme.colors.textMuted.WithAlpha(0.55f));
            button->AddChild(itemLabel);

            if (!shortcutText.empty())
            {
                UI::Label::Ptr shortcutLabel =
                    CreateLabel(button->GetName() + ".Shortcut",
                                shortcutText,
                                theme,
                                EditorTypographyRole::Menu,
                                std::max(menuTextInset,
                                         dropdownWidth - menuTextInset - shortcutWidth),
                                0.0f,
                                shortcutWidth,
                                itemHeight);
                shortcutLabel->SetTextAlign(UI::TextAlign::Right);
                shortcutLabel->SetTextColor(enabled ? theme.colors.textMuted
                                                    : theme.colors.textMuted.WithAlpha(0.45f));
                button->AddChild(shortcutLabel);
                ++m_lastBuildStats.menuShortcutItemCount;
            }

            dropdown->AddChild(button);
            ++m_lastBuildStats.openMenuItemCount;
            y += itemHeight;
        }

        ++m_lastBuildStats.dropdownCount;
        if (desc.popupLayer)
        {
            desc.popupLayer->AddPopup(dropdown);
        }
        else
        {
            canvas.AddWidget(dropdown);
        }
    }

    return true;
}

std::string EditorCommandSurfaceRenderer::GetOpenMenuRootWidgetName() const
{
    return m_openMenuId.empty() ? std::string() : RVX_EDITOR_SURFACE_MENU_DROPDOWN;
}

std::string EditorCommandSurfaceRenderer::GetToolbarOverflowRootWidgetName() const
{
    return m_toolbarOverflowOpen ? RVX_EDITOR_SURFACE_TOOLBAR_OVERFLOW_DROPDOWN
                                 : std::string();
}

void EditorCommandSurfaceRenderer::CloseOpenMenu()
{
    m_openMenuId.clear();
    m_openMenuSelectedItemIndex = -1;
}

void EditorCommandSurfaceRenderer::CloseToolbarOverflow()
{
    m_toolbarOverflowOpen = false;
    m_toolbarOverflowEntries.clear();
    m_toolbarOverflowSelectedItemIndex = -1;
    m_toolbarOverflowScrollOffsetRows = 0;
}

bool EditorCommandSurfaceRenderer::OpenMenu(
    const std::string& menuId,
    const EditorCommandRegistry& registry,
    const EditorCommandSurfaceModel& surfaceModel)
{
    const EditorMenuSurface* menu = surfaceModel.FindMenu(menuId);
    if (!menu)
    {
        CloseOpenMenu();
        return false;
    }

    CloseToolbarOverflow();
    m_openMenuId = menu->id;
    m_openMenuSelectedItemIndex = -1;
    NormalizeOpenMenuSelection(*menu, registry, surfaceModel);
    return true;
}

void EditorCommandSurfaceRenderer::NormalizeOpenMenuSelection(
    const EditorMenuSurface& menu,
    const EditorCommandRegistry& registry,
    const EditorCommandSurfaceModel& surfaceModel)
{
    if (m_openMenuSelectedItemIndex >= 0 &&
        static_cast<size_t>(m_openMenuSelectedItemIndex) < menu.items.size() &&
        IsExecutableMenuItem(menu.items[static_cast<size_t>(m_openMenuSelectedItemIndex)],
                             registry,
                             surfaceModel))
    {
        return;
    }

    m_openMenuSelectedItemIndex = -1;
    for (size_t index = 0; index < menu.items.size(); ++index)
    {
        if (IsExecutableMenuItem(menu.items[index], registry, surfaceModel))
        {
            m_openMenuSelectedItemIndex = static_cast<int32>(index);
            return;
        }
    }
}

void EditorCommandSurfaceRenderer::MoveOpenMenuSelection(
    int32 delta,
    const EditorMenuSurface& menu,
    const EditorCommandRegistry& registry,
    const EditorCommandSurfaceModel& surfaceModel)
{
    if (menu.items.empty())
    {
        m_openMenuSelectedItemIndex = -1;
        return;
    }

    NormalizeOpenMenuSelection(menu, registry, surfaceModel);
    const int32 count = static_cast<int32>(menu.items.size());
    int32 cursor = m_openMenuSelectedItemIndex;
    if (cursor < 0)
    {
        cursor = delta < 0 ? 0 : count - 1;
    }

    for (int32 step = 0; step < count; ++step)
    {
        cursor = (cursor + delta + count) % count;
        if (IsExecutableMenuItem(menu.items[static_cast<size_t>(cursor)],
                                 registry,
                                 surfaceModel))
        {
            m_openMenuSelectedItemIndex = cursor;
            return;
        }
    }

    m_openMenuSelectedItemIndex = -1;
}

bool EditorCommandSurfaceRenderer::MoveOpenMenuSurface(
    int32 delta,
    const EditorCommandRegistry& registry,
    const EditorCommandSurfaceModel& surfaceModel)
{
    const std::vector<EditorMenuSurface>& menus = surfaceModel.GetMenus();
    if (menus.empty() || delta == 0)
    {
        return false;
    }

    const auto currentIt = std::find_if(menus.begin(),
                                        menus.end(),
                                        [this](const EditorMenuSurface& menu) {
                                            return menu.id == m_openMenuId;
                                        });
    if (currentIt == menus.end())
    {
        CloseOpenMenu();
        return false;
    }

    const int32 count = static_cast<int32>(menus.size());
    const int32 currentIndex =
        static_cast<int32>(std::distance(menus.begin(), currentIt));
    const int32 nextIndex = (currentIndex + delta + count) % count;
    const EditorMenuSurface& nextMenu = menus[static_cast<size_t>(nextIndex)];
    m_openMenuId = nextMenu.id;
    m_openMenuSelectedItemIndex = -1;
    NormalizeOpenMenuSelection(nextMenu, registry, surfaceModel);
    return true;
}

bool EditorCommandSurfaceRenderer::ExecuteOpenMenuSelection(
    EditorUIHost& host,
    const EditorMenuSurface& menu,
    const EditorCommandRegistry& registry,
    const EditorCommandSurfaceModel& surfaceModel)
{
    NormalizeOpenMenuSelection(menu, registry, surfaceModel);
    if (m_openMenuSelectedItemIndex < 0 ||
        static_cast<size_t>(m_openMenuSelectedItemIndex) >= menu.items.size())
    {
        return true;
    }

    const EditorCommandSurfaceItem& item =
        menu.items[static_cast<size_t>(m_openMenuSelectedItemIndex)];
    if (!IsExecutableMenuItem(item, registry, surfaceModel))
    {
        return true;
    }

    host.ExecuteCommand(item.commandId);
    CloseOpenMenu();
    return true;
}

bool EditorCommandSurfaceRenderer::HandleOpenMenuKeyDown(
    uint32 keyCode,
    EditorUIHost& host,
    const EditorCommandRegistry& registry,
    const EditorCommandSurfaceModel& surfaceModel)
{
    if (m_openMenuId.empty())
    {
        return false;
    }

    const EditorMenuSurface* menu = surfaceModel.FindMenu(m_openMenuId);
    if (!menu)
    {
        CloseOpenMenu();
        return false;
    }

    if (keyCode == UI::RVX_UI_KEY_ESCAPE)
    {
        CloseOpenMenu();
        return true;
    }
    if (keyCode == UI::RVX_UI_KEY_RIGHT)
    {
        return MoveOpenMenuSurface(1, registry, surfaceModel);
    }
    if (keyCode == UI::RVX_UI_KEY_LEFT)
    {
        return MoveOpenMenuSurface(-1, registry, surfaceModel);
    }
    if (keyCode == UI::RVX_UI_KEY_DOWN)
    {
        MoveOpenMenuSelection(1, *menu, registry, surfaceModel);
        return true;
    }
    if (keyCode == UI::RVX_UI_KEY_UP)
    {
        MoveOpenMenuSelection(-1, *menu, registry, surfaceModel);
        return true;
    }
    if (keyCode == UI::RVX_UI_KEY_ENTER || keyCode == UI::RVX_UI_KEY_SPACE)
    {
        return ExecuteOpenMenuSelection(host, *menu, registry, surfaceModel);
    }

    return false;
}

bool EditorCommandSurfaceRenderer::HandleToolbarOverflowKeyDown(
    uint32 keyCode,
    EditorUIHost& host)
{
    if (!m_toolbarOverflowOpen)
    {
        return false;
    }

    if (keyCode == UI::RVX_UI_KEY_ESCAPE)
    {
        CloseToolbarOverflow();
        return true;
    }

    if (m_toolbarOverflowEntries.empty())
    {
        CloseToolbarOverflow();
        return false;
    }

    auto normalizeSelection = [this]() {
        if (m_toolbarOverflowSelectedItemIndex >= 0 &&
            static_cast<size_t>(m_toolbarOverflowSelectedItemIndex) <
                m_toolbarOverflowEntries.size() &&
            m_toolbarOverflowEntries[static_cast<size_t>(
                m_toolbarOverflowSelectedItemIndex)]
                .enabled)
        {
            return;
        }

        m_toolbarOverflowSelectedItemIndex = -1;
        for (size_t index = 0; index < m_toolbarOverflowEntries.size(); ++index)
        {
            if (m_toolbarOverflowEntries[index].enabled)
            {
                m_toolbarOverflowSelectedItemIndex = static_cast<int32>(index);
                return;
            }
        }
    };

    auto moveSelection = [this, &normalizeSelection](int32 delta) {
        normalizeSelection();
        if (m_toolbarOverflowSelectedItemIndex < 0)
        {
            return;
        }

        const int32 count =
            static_cast<int32>(m_toolbarOverflowEntries.size());
        int32 index = m_toolbarOverflowSelectedItemIndex;
        for (int32 attempts = 0; attempts < count; ++attempts)
        {
            index = (index + delta + count) % count;
            if (m_toolbarOverflowEntries[static_cast<size_t>(index)].enabled)
            {
                m_toolbarOverflowSelectedItemIndex = index;
                return;
            }
        }
    };

    if (keyCode == UI::RVX_UI_KEY_DOWN)
    {
        moveSelection(1);
        return true;
    }
    if (keyCode == UI::RVX_UI_KEY_UP)
    {
        moveSelection(-1);
        return true;
    }
    if (keyCode == UI::RVX_UI_KEY_ENTER || keyCode == UI::RVX_UI_KEY_SPACE)
    {
        normalizeSelection();
        if (m_toolbarOverflowSelectedItemIndex < 0 ||
            static_cast<size_t>(m_toolbarOverflowSelectedItemIndex) >=
                m_toolbarOverflowEntries.size())
        {
            return true;
        }

        const EditorCommandToolbarOverflowEntry& entry =
            m_toolbarOverflowEntries[static_cast<size_t>(
                m_toolbarOverflowSelectedItemIndex)];
        if (entry.enabled)
        {
            host.ExecuteCommand(entry.item.commandId);
            CloseToolbarOverflow();
        }
        return true;
    }

    return false;
}

void EditorCommandSurfaceRenderer::Clear(UI::UIContext& ui)
{
    ClearSurfaceWidgets(ui.GetCanvas());
    m_lastBuildStats = {};
    CloseOpenMenu();
    CloseToolbarOverflow();
}

} // namespace RVX::Editor
