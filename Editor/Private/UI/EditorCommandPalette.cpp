/**
 * @file EditorCommandPalette.cpp
 * @brief Native editor command palette service implementation
 */

#include "Editor/UI/EditorCommandPalette.h"

#include "Editor/UI/EditorCommandRegistry.h"
#include "Editor/UI/EditorPopupLayer.h"
#include "Editor/UI/EditorShellMetrics.h"
#include "Editor/UI/EditorShortcutBindingModel.h"
#include "Editor/UI/EditorUIHost.h"
#include "UI/UIContext.h"
#include "UI/Widgets/Panel.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace RVX::Editor
{
namespace
{
    constexpr const char* RVX_EDITOR_COMMAND_PALETTE_ROOT = "Editor.CommandPalette";
    constexpr const char* RVX_EDITOR_COMMAND_PALETTE_POPUP = "Editor.CommandPalette.Popup";

    std::string CommandDisplayName(const EditorCommand& command)
    {
        return command.desc.displayName.empty() ? command.desc.id
                                                : command.desc.displayName;
    }

    bool IsCommandExecutable(const EditorCommand& command)
    {
        return command.desc.enabled && static_cast<bool>(command.desc.callback);
    }

    std::string ShortcutToken(const EditorShortcut& shortcut)
    {
        if (!shortcut.valid)
        {
            return {};
        }

        return EditorShortcutBindingModel::FormatShortcut(shortcut);
    }

    std::string CommandPrimaryShortcutText(const EditorCommand& command)
    {
        if (command.desc.shortcut.valid)
        {
            return ShortcutToken(command.desc.shortcut);
        }

        for (const EditorShortcut& shortcut : command.desc.secondaryShortcuts)
        {
            if (shortcut.valid)
            {
                return ShortcutToken(shortcut);
            }
        }

        return {};
    }

    std::string CommandPaletteTooltip(const EditorCommand& command)
    {
        std::string tooltip =
            command.desc.tooltip.empty() ? CommandDisplayName(command)
                                         : command.desc.tooltip;
        if (!command.desc.disabledReason.empty())
        {
            if (!tooltip.empty())
            {
                tooltip += " ";
            }
            tooltip += "Unavailable: " + command.desc.disabledReason;
        }
        return tooltip;
    }

    float CommandPaletteWidth(float surfaceWidth)
    {
        if (surfaceWidth <= 0.0f)
        {
            return 0.0f;
        }

        const float available = std::max(0.0f, surfaceWidth - 32.0f);
        return std::clamp(available, std::min(available, 240.0f), 560.0f);
    }
}

void EditorCommandPalette::Open(std::string initialFilter)
{
    m_filterText = std::move(initialFilter);
    m_visibleCommandIds.clear();
    m_listModel.ClearItems();
    m_listView.SetScrollOffsetY(0.0f);
    m_open = true;
}

void EditorCommandPalette::Close()
{
    m_open = false;
}

void EditorCommandPalette::Clear()
{
    m_filterText.clear();
    m_visibleCommandIds.clear();
    m_filterModel.SetItems({});
    m_filterModel.SetFilterText({});
    m_filterModel.Rebuild();
    m_listModel.ClearItems();
    m_listView.SetScrollOffsetY(0.0f);
    m_lastBuildStats = {};
    m_open = false;
}

std::string EditorCommandPalette::GetRootWidgetName() const
{
    return RVX_EDITOR_COMMAND_PALETTE_ROOT;
}

std::string EditorCommandPalette::GetFocusWidgetName() const
{
    return GetRootWidgetName() + ".Search";
}

void EditorCommandPalette::RebuildCommandItems(const EditorUIHost& host)
{
    const std::vector<EditorCommand>& commands =
        host.GetCommandRegistry().GetCommands();

    std::vector<EditorPickerFilterItemDesc> filterItems;
    filterItems.reserve(commands.size());
    for (const EditorCommand& command : commands)
    {
        if (command.desc.id.empty())
        {
            continue;
        }

        EditorPickerFilterItemDesc item;
        item.id = command.desc.id;
        item.text = CommandDisplayName(command);
        item.category = command.desc.category.empty() ? "Commands"
                                                      : command.desc.category;
        item.keywords.push_back(command.desc.id);
        item.keywords.push_back(command.desc.category);
        item.keywords.push_back(command.desc.tooltip);
        item.keywords.push_back(ShortcutToken(command.desc.shortcut));
        for (const EditorShortcut& shortcut : command.desc.secondaryShortcuts)
        {
            item.keywords.push_back(ShortcutToken(shortcut));
        }
        item.enabled = IsCommandExecutable(command);
        filterItems.push_back(std::move(item));
    }

    EditorPickerFilterOptions filterOptions;
    filterOptions.groupByCategory = true;
    filterOptions.sortMatchesByScore = true;
    m_filterModel.SetOptions(filterOptions);
    m_filterModel.SetItems(std::move(filterItems));
    m_filterModel.SetFilterText(m_filterText);
    m_filterModel.Rebuild();

    const std::vector<EditorPickerFilterItemDesc>& sourceItems =
        m_filterModel.GetItems();
    const std::vector<EditorPickerFilterResult>& results =
        m_filterModel.GetResults();

    std::vector<EditorPickerListItemState> listItems;
    listItems.reserve(results.size());
    m_visibleCommandIds.clear();
    m_visibleCommandIds.reserve(results.size());
    for (const EditorPickerFilterResult& result : results)
    {
        if (result.sourceIndex >= sourceItems.size())
        {
            continue;
        }

        const EditorPickerFilterItemDesc& item = sourceItems[result.sourceIndex];
        m_visibleCommandIds.push_back(item.id);
        listItems.push_back({item.id, item.enabled});
    }

    m_listModel.SetItems(std::move(listItems));
}

bool EditorCommandPalette::IsVisibleCommandExecutable(int32 visibleIndex,
                                                      const EditorUIHost& host) const
{
    if (visibleIndex < 0 ||
        static_cast<size_t>(visibleIndex) >= m_visibleCommandIds.size())
    {
        return false;
    }

    const EditorCommand* command = host.GetCommandRegistry().FindCommand(
        m_visibleCommandIds[static_cast<size_t>(visibleIndex)]);
    return command && IsCommandExecutable(*command);
}

bool EditorCommandPalette::ExecuteSelectedCommand(EditorUIHost& host)
{
    m_listModel.NormalizeSelection();
    const int32 selectedIndex = m_listModel.GetSelectedIndex();
    if (!IsVisibleCommandExecutable(selectedIndex, host))
    {
        return false;
    }

    const std::string commandId = m_visibleCommandIds[static_cast<size_t>(selectedIndex)];
    const bool executed = host.ExecuteCommand(commandId);
    if (executed)
    {
        Close();
    }
    return executed;
}

bool EditorCommandPalette::HandleKeyDown(uint32 keyCode, EditorUIHost& host)
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
        m_listModel.MoveSelection(1);
        return true;
    }
    if (keyCode == UI::RVX_UI_KEY_UP)
    {
        m_listModel.MoveSelection(-1);
        return true;
    }
    if (keyCode == UI::RVX_UI_KEY_ENTER || keyCode == UI::RVX_UI_KEY_SPACE)
    {
        ExecuteSelectedCommand(host);
        return true;
    }

    return false;
}

void EditorCommandPalette::Build(UI::UIContext& ui,
                                 EditorPopupLayer& popupLayer,
                                 EditorUIHost& host)
{
    m_lastBuildStats = {};
    if (!m_open)
    {
        return;
    }

    RebuildCommandItems(host);

    const UI::UITheme& theme = ui.GetTheme();
    const float surfaceWidth = static_cast<float>(ui.GetWidth());
    const float surfaceHeight = static_cast<float>(ui.GetHeight());
    const float width = CommandPaletteWidth(surfaceWidth);
    if (width <= 0.0f || surfaceHeight <= 0.0f)
    {
        return;
    }

    const float x = std::max(8.0f, (surfaceWidth - width) * 0.5f);
    const float y = std::clamp(surfaceHeight * 0.12f, 12.0f, 96.0f);
    const float maxListHeight =
        std::max(40.0f, std::min(320.0f, surfaceHeight - y - 24.0f));

    UI::Panel::Ptr popup = UI::Panel::Create();
    popup->SetName(RVX_EDITOR_COMMAND_PALETTE_POPUP);
    popup->SetPosition(x, y);
    popup->SetSize(width, 1.0f);
    popup->SetBackgroundColor(UI::UIColor::Transparent());
    popup->SetBorderWidth(0.0f);

    std::vector<EditorPickerListViewItemDesc> viewItems;
    const std::vector<EditorPickerFilterItemDesc>& sourceItems =
        m_filterModel.GetItems();
    const std::vector<EditorPickerFilterResult>& results =
        m_filterModel.GetResults();
    viewItems.reserve(results.size());
    for (const EditorPickerFilterResult& result : results)
    {
        if (result.sourceIndex >= sourceItems.size())
        {
            continue;
        }

        const EditorPickerFilterItemDesc& sourceItem = sourceItems[result.sourceIndex];
        EditorPickerListViewItemDesc item;
        item.id = sourceItem.id;
        item.text = sourceItem.text;
        item.category = sourceItem.category;
        item.enabled = sourceItem.enabled;
        if (const EditorCommand* command =
                host.GetCommandRegistry().FindCommand(sourceItem.id))
        {
            item.secondaryText = CommandPrimaryShortcutText(*command);
            item.disabledReason = command->desc.disabledReason;
            item.tooltipText = CommandPaletteTooltip(*command);
        }
        item.onClick = [this, &host, commandId = sourceItem.id]() {
            if (host.ExecuteCommand(commandId))
            {
                Close();
            }
        };
        viewItems.push_back(std::move(item));
    }

    EditorPickerListViewDesc pickerDesc;
    pickerDesc.ui = &ui;
    pickerDesc.parent = popup.get();
    pickerDesc.model = &m_listModel;
    pickerDesc.name = RVX_EDITOR_COMMAND_PALETTE_ROOT;
    pickerDesc.bounds = UI::Rect(0.0f, 0.0f, width, 0.0f);
    pickerDesc.searchText = m_filterText;
    pickerDesc.searchPlaceholder = "Search commands";
    pickerDesc.emptyText = "No commands";
    pickerDesc.items = std::move(viewItems);
    pickerDesc.onSearchChanged = [this](const std::string& text) {
        m_filterText = text;
        m_listModel.ClearSelection();
        m_listView.SetScrollOffsetY(0.0f);
    };
    pickerDesc.onKeyDown = [this, &host](uint32 keyCode) {
        return HandleKeyDown(keyCode, host);
    };
    const EditorShellMetrics shellMetrics = EditorShellMetricPolicy::Resolve(theme);
    pickerDesc.padding = shellMetrics.contentPadding;
    pickerDesc.rowHeight = shellMetrics.pickerRowHeight;
    pickerDesc.categoryHeight = shellMetrics.pickerCategoryHeight;
    pickerDesc.maxListHeight = maxListHeight;

    m_listView.Build(pickerDesc);
    const EditorPickerListViewStats& pickerStats = m_listView.GetLastBuildStats();
    popup->SetSize(width, std::max(1.0f, pickerStats.bounds.height));
    popupLayer.AddPopup(std::move(popup));

    m_lastBuildStats.open = true;
    m_lastBuildStats.commandCount =
        m_filterModel.GetLastBuildStats().sourceItemCount;
    m_lastBuildStats.resultCount =
        m_filterModel.GetLastBuildStats().resultCount;
    m_lastBuildStats.executableItemCount = m_listModel.GetEnabledItemCount();
    m_lastBuildStats.shortcutMetadataItemCount =
        pickerStats.secondaryTextItemCount;
    m_lastBuildStats.disabledReasonItemCount =
        pickerStats.disabledReasonItemCount;
    m_lastBuildStats.selectedItemIndex = m_listModel.GetSelectedIndex();
    m_lastBuildStats.bounds =
        UI::Rect(x, y, width, std::max(1.0f, pickerStats.bounds.height));
    m_lastBuildStats.filterStats = m_filterModel.GetLastBuildStats();
    m_lastBuildStats.pickerStats = pickerStats;
}

} // namespace RVX::Editor
