/**
 * @file EditorContextMenu.cpp
 * @brief Native editor context menu service implementation
 */

#include "Editor/UI/EditorContextMenu.h"

#include "Editor/UI/EditorCommandRegistry.h"
#include "Editor/UI/EditorPopupLayer.h"
#include "Editor/UI/EditorShellMetrics.h"
#include "Editor/UI/EditorTypography.h"
#include "Editor/UI/EditorUIHost.h"
#include "UI/UIContext.h"
#include "UI/Widgets/Button.h"
#include "UI/Widgets/Panel.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace RVX::Editor
{
namespace
{
    constexpr const char* RVX_EDITOR_CONTEXT_MENU_PREFIX = "Editor.ContextMenu";

    float EstimateMenuTextWidth(const std::string& text,
                                const UI::UITheme& theme,
                                float minWidth,
                                float maxWidth)
    {
        return EditorTypography::EstimatePaddedTextWidth(
            text,
            theme,
            EditorTypographyRole::ContextMenu,
            minWidth,
            maxWidth);
    }

    float ClampPopupAxis(float requested, float extent, float surfaceExtent)
    {
        if (surfaceExtent <= extent)
        {
            return 0.0f;
        }
        return std::clamp(requested, 0.0f, surfaceExtent - extent);
    }

    std::string MenuRootName(const std::string& id)
    {
        return std::string(RVX_EDITOR_CONTEXT_MENU_PREFIX) + "." +
               (id.empty() ? "Default" : id);
    }

    std::string ItemWidgetName(const std::string& menuId, const EditorContextMenuItem& item)
    {
        const std::string itemId = !item.id.empty()
                                       ? item.id
                                       : (!item.commandId.empty() ? item.commandId : "separator");
        return MenuRootName(menuId) + ".Item." + itemId;
    }

    std::string BuildUnavailableTooltip(const std::string& label,
                                        const std::string& disabledReason)
    {
        if (disabledReason.empty())
        {
            return {};
        }

        std::string tooltip = label;
        tooltip += tooltip.empty() ? std::string() : std::string("\n");
        tooltip += "Unavailable: " + disabledReason;
        return tooltip;
    }

    struct ResolvedContextMenuItem
    {
        const EditorCommand* command = nullptr;
        std::string text;
        std::string disabledReason;
        std::string tooltip;
        bool enabled = false;
    };

    ResolvedContextMenuItem ResolveContextMenuItem(
        const EditorContextMenuItem& item,
        const EditorUIHost& host)
    {
        ResolvedContextMenuItem resolved;
        resolved.text = item.text;
        resolved.disabledReason = item.disabledReason;
        resolved.enabled = item.enabled;

        if (item.type == EditorContextMenuItemType::Command)
        {
            resolved.command =
                host.GetCommandRegistry().FindCommand(item.commandId);
            resolved.enabled =
                item.enabled && resolved.command &&
                resolved.command->desc.enabled &&
                static_cast<bool>(resolved.command->desc.callback);
            if (resolved.text.empty() && resolved.command)
            {
                resolved.text = resolved.command->desc.displayName;
            }
            if (resolved.disabledReason.empty() && resolved.command)
            {
                resolved.disabledReason = resolved.command->desc.disabledReason;
            }
        }
        else if (item.type == EditorContextMenuItemType::Action)
        {
            resolved.enabled = item.enabled && static_cast<bool>(item.action);
        }
        else
        {
            resolved.enabled = false;
        }

        if (!resolved.enabled && !resolved.disabledReason.empty())
        {
            resolved.tooltip =
                BuildUnavailableTooltip(resolved.text, resolved.disabledReason);
        }

        return resolved;
    }

    void ApplyContextButtonStyle(UI::Button& button,
                                 const UI::UITheme& theme,
                                 bool enabled,
                                 bool selected)
    {
        button.GetStyle().fontSize = EditorTypography::GetFontSize(
            theme,
            EditorTypographyRole::ContextMenu);
        button.GetStyle().textColor = enabled ? theme.colors.text : theme.colors.textMuted;
        button.SetNormalColor(selected ? theme.colors.surfaceActive
                                       : theme.colors.panelBackground);
        button.SetHoverColor(selected ? theme.colors.surfaceActive
                                      : theme.colors.surfaceHover);
        button.SetPressedColor(theme.colors.surfaceActive);
        button.SetDisabledColor(theme.colors.surface.WithAlpha(0.35f));
        button.SetEnabled(enabled);
    }

    class EditorContextMenuPanel final : public UI::Panel
    {
    public:
        EditorContextMenuPanel(EditorContextMenu& menu, EditorUIHost& host)
            : m_menu(menu)
            , m_host(host)
        {
            SetInteractive(true);
        }

        bool HandleEvent(const UI::UIEvent& event) override
        {
            if (event.type == UI::UIEventType::KeyDown)
            {
                return m_menu.HandleKeyDown(static_cast<uint32>(event.keyCode), m_host);
            }

            return UI::Panel::HandleEvent(event);
        }

    private:
        EditorContextMenu& m_menu;
        EditorUIHost& m_host;
    };
}

EditorContextMenuItem EditorContextMenuItem::Action(std::string id,
                                                    std::string text,
                                                    EditorContextMenuAction action,
                                                    bool enabled,
                                                    std::string disabledReason)
{
    EditorContextMenuItem item;
    item.type = EditorContextMenuItemType::Action;
    item.id = std::move(id);
    item.text = std::move(text);
    item.disabledReason = std::move(disabledReason);
    item.action = std::move(action);
    item.enabled = enabled;
    return item;
}

EditorContextMenuItem EditorContextMenuItem::Command(std::string commandId,
                                                     std::string text,
                                                     bool enabled,
                                                     std::string disabledReason)
{
    EditorContextMenuItem item;
    item.type = EditorContextMenuItemType::Command;
    item.id = commandId;
    item.commandId = std::move(commandId);
    item.text = std::move(text);
    item.disabledReason = std::move(disabledReason);
    item.enabled = enabled;
    return item;
}

EditorContextMenuItem EditorContextMenuItem::Separator(std::string id)
{
    EditorContextMenuItem item;
    item.type = EditorContextMenuItemType::Separator;
    item.id = std::move(id);
    item.enabled = false;
    return item;
}

void EditorContextMenu::Open(EditorContextMenuDesc desc)
{
    if (desc.id.empty())
    {
        desc.id = "Default";
    }
    m_desc = std::move(desc);
    m_selectedItemIndex = -1;
    m_open = true;
}

void EditorContextMenu::Close()
{
    m_open = false;
}

void EditorContextMenu::Clear()
{
    m_desc = {};
    m_lastBuildStats = {};
    m_selectedItemIndex = -1;
    m_open = false;
}

std::string EditorContextMenu::GetRootWidgetName() const
{
    return MenuRootName(m_desc.id);
}

bool EditorContextMenu::IsExecutableItem(const EditorContextMenuItem& item,
                                         const EditorUIHost& host) const
{
    if (!item.visible || item.type == EditorContextMenuItemType::Separator)
    {
        return false;
    }

    return ResolveContextMenuItem(item, host).enabled;
}

bool EditorContextMenu::ExecuteItem(const EditorContextMenuItem& item, EditorUIHost& host)
{
    if (!IsExecutableItem(item, host))
    {
        return false;
    }

    if (item.type == EditorContextMenuItemType::Command)
    {
        host.ExecuteCommand(item.commandId);
    }
    else
    {
        item.action(host);
    }

    if (m_desc.closeOnExecute)
    {
        Close();
    }
    return true;
}

void EditorContextMenu::NormalizeSelectedItem(const EditorUIHost& host)
{
    if (m_selectedItemIndex >= 0 &&
        static_cast<size_t>(m_selectedItemIndex) < m_desc.items.size() &&
        IsExecutableItem(m_desc.items[static_cast<size_t>(m_selectedItemIndex)], host))
    {
        return;
    }

    m_selectedItemIndex = -1;
    for (size_t index = 0; index < m_desc.items.size(); ++index)
    {
        if (IsExecutableItem(m_desc.items[index], host))
        {
            m_selectedItemIndex = static_cast<int32>(index);
            return;
        }
    }
}

bool EditorContextMenu::MoveSelection(int32 delta, const EditorUIHost& host)
{
    if (m_desc.items.empty())
    {
        m_selectedItemIndex = -1;
        return false;
    }

    NormalizeSelectedItem(host);
    const int32 count = static_cast<int32>(m_desc.items.size());
    int32 cursor = m_selectedItemIndex;
    if (cursor < 0)
    {
        cursor = delta < 0 ? 0 : count - 1;
    }

    for (int32 step = 0; step < count; ++step)
    {
        cursor = (cursor + delta + count) % count;
        if (IsExecutableItem(m_desc.items[static_cast<size_t>(cursor)], host))
        {
            const bool changed = cursor != m_selectedItemIndex;
            m_selectedItemIndex = cursor;
            return changed;
        }
    }

    m_selectedItemIndex = -1;
    return false;
}

bool EditorContextMenu::HandleKeyDown(uint32 keyCode, EditorUIHost& host)
{
    if (!m_open)
    {
        return false;
    }

    if (keyCode == UI::RVX_UI_KEY_ESCAPE)
    {
        Close();
        return true;
    }
    if (keyCode == UI::RVX_UI_KEY_DOWN)
    {
        MoveSelection(1, host);
        return true;
    }
    if (keyCode == UI::RVX_UI_KEY_UP)
    {
        MoveSelection(-1, host);
        return true;
    }
    if (keyCode == UI::RVX_UI_KEY_ENTER || keyCode == UI::RVX_UI_KEY_SPACE)
    {
        NormalizeSelectedItem(host);
        if (m_selectedItemIndex >= 0 &&
            static_cast<size_t>(m_selectedItemIndex) < m_desc.items.size())
        {
            return ExecuteItem(m_desc.items[static_cast<size_t>(m_selectedItemIndex)],
                               host);
        }
        return true;
    }

    return false;
}

void EditorContextMenu::Build(UI::UIContext& ui,
                              EditorPopupLayer& popupLayer,
                              EditorUIHost& host)
{
    m_lastBuildStats = {};
    if (!m_open)
    {
        return;
    }

    const UI::UITheme& theme = ui.GetTheme();
    const EditorShellMetrics shellMetrics = EditorShellMetricPolicy::Resolve(theme);
    const float itemHeight = shellMetrics.panelControlHeight;
    const float separatorHeight = std::max(6.0f, shellMetrics.compactGap);
    const float horizontalPadding = shellMetrics.contentPadding;
    const float verticalPadding = shellMetrics.compactGap;

    std::vector<const EditorContextMenuItem*> visibleItems;
    visibleItems.reserve(m_desc.items.size());
    float width = std::clamp(m_desc.minWidth, 1.0f, std::max(1.0f, m_desc.maxWidth));
    float height = verticalPadding * 2.0f;

    for (const EditorContextMenuItem& item : m_desc.items)
    {
        if (!item.visible)
        {
            continue;
        }

        visibleItems.push_back(&item);
        ++m_lastBuildStats.itemCount;

        if (item.type == EditorContextMenuItemType::Separator)
        {
            ++m_lastBuildStats.separatorCount;
            height += separatorHeight;
            continue;
        }

        const ResolvedContextMenuItem resolved =
            ResolveContextMenuItem(item, host);
        if (item.type == EditorContextMenuItemType::Command)
        {
            ++m_lastBuildStats.commandItemCount;
        }
        else
        {
            ++m_lastBuildStats.actionItemCount;
        }

        if (resolved.enabled)
        {
            ++m_lastBuildStats.executableItemCount;
        }
        else if (!resolved.disabledReason.empty())
        {
            ++m_lastBuildStats.disabledReasonItemCount;
        }
        width = std::max(width,
                         EstimateMenuTextWidth(resolved.text,
                                               theme,
                                               m_desc.minWidth,
                                               m_desc.maxWidth));
        height += itemHeight;
    }

    if (visibleItems.empty())
    {
        return;
    }
    NormalizeSelectedItem(host);

    const float x = ClampPopupAxis(m_desc.anchor.x,
                                   width,
                                   static_cast<float>(ui.GetWidth()));
    const float y = ClampPopupAxis(m_desc.anchor.y,
                                   height,
                                   static_cast<float>(ui.GetHeight()));

    UI::Panel::Ptr menu = std::make_shared<EditorContextMenuPanel>(*this, host);
    menu->SetName(GetRootWidgetName());
    menu->SetPosition(x, y);
    menu->SetSize(width, height);
    menu->SetBackgroundColor(theme.colors.panelBackground);
    menu->SetBorderColor(theme.colors.border);
    menu->SetBorderWidth(theme.metrics.borderWidth);

    float cursorY = verticalPadding;
    uint32 separatorIndex = 0;
    for (const EditorContextMenuItem* item : visibleItems)
    {
        if (!item)
        {
            continue;
        }

        if (item->type == EditorContextMenuItemType::Separator)
        {
            UI::Panel::Ptr separator = UI::Panel::Create();
            separator->SetName(ItemWidgetName(m_desc.id, *item) + "." +
                               std::to_string(separatorIndex++));
            separator->SetInteractive(false);
            separator->SetPosition(horizontalPadding, cursorY + separatorHeight * 0.5f);
            separator->SetSize(std::max(0.0f, width - horizontalPadding * 2.0f),
                               theme.metrics.borderWidth);
            separator->SetBackgroundColor(theme.colors.border);
            separator->SetBorderWidth(0.0f);
            menu->AddChild(separator);
            cursorY += separatorHeight;
            continue;
        }

        const ResolvedContextMenuItem resolved =
            ResolveContextMenuItem(*item, host);

        const bool selected =
            m_selectedItemIndex >= 0 &&
            static_cast<size_t>(m_selectedItemIndex) <
            m_desc.items.size() &&
            &m_desc.items[static_cast<size_t>(m_selectedItemIndex)] == item;
        UI::Button::Ptr button = UI::Button::Create(resolved.text);
        button->SetName(ItemWidgetName(m_desc.id, *item));
        button->SetPosition(horizontalPadding * 0.5f, cursorY);
        button->SetSize(std::max(0.0f, width - horizontalPadding), itemHeight);
        ApplyContextButtonStyle(*button, theme, resolved.enabled, selected);
        button->SetTooltipText(resolved.tooltip);
        if (resolved.enabled)
        {
            if (item->type == EditorContextMenuItemType::Command)
            {
                button->SetOnClick([this,
                                    commandId = item->commandId,
                                    closeOnExecute = m_desc.closeOnExecute,
                                    hostPtr = &host](const UI::UIEvent& event) {
                    (void)event;
                    if (hostPtr)
                    {
                        hostPtr->ExecuteCommand(commandId);
                    }
                    if (closeOnExecute)
                    {
                        Close();
                    }
                });
            }
            else
            {
                button->SetOnClick([this,
                                    action = item->action,
                                    closeOnExecute = m_desc.closeOnExecute,
                                    hostPtr = &host](const UI::UIEvent& event) {
                    (void)event;
                    if (hostPtr)
                    {
                        action(*hostPtr);
                    }
                    if (closeOnExecute)
                    {
                        Close();
                    }
                });
            }
        }

        menu->AddChild(button);
        cursorY += itemHeight;
    }

    m_lastBuildStats.open = true;
    m_lastBuildStats.selectedItemIndex = m_selectedItemIndex;
    m_lastBuildStats.bounds = UI::Rect(x, y, width, height);
    popupLayer.AddPopup(menu);
}

} // namespace RVX::Editor
