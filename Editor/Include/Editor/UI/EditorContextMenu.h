/**
 * @file EditorContextMenu.h
 * @brief Native editor context menu service
 */

#pragma once

#include "Core/Types.h"
#include "UI/UITypes.h"

#include <functional>
#include <string>
#include <vector>

namespace RVX::UI
{
    class UIContext;
}

namespace RVX::Editor
{

class EditorPopupLayer;
class EditorUIHost;

enum class EditorContextMenuItemType : uint8
{
    Action = 0,
    Command,
    Separator
};

using EditorContextMenuAction = std::function<void(EditorUIHost&)>;

struct EditorContextMenuItem
{
    EditorContextMenuItemType type = EditorContextMenuItemType::Action;
    std::string id;
    std::string text;
    std::string commandId;
    std::string disabledReason;
    EditorContextMenuAction action;
    bool enabled = true;
    bool visible = true;

    static EditorContextMenuItem Action(std::string id,
                                        std::string text,
                                        EditorContextMenuAction action,
                                        bool enabled = true,
                                        std::string disabledReason = {});
    static EditorContextMenuItem Command(std::string commandId,
                                         std::string text = {},
                                         bool enabled = true,
                                         std::string disabledReason = {});
    static EditorContextMenuItem Separator(std::string id = {});
};

struct EditorContextMenuDesc
{
    std::string id = "Default";
    Vec2 anchor{0.0f};
    std::vector<EditorContextMenuItem> items;
    float minWidth = 160.0f;
    float maxWidth = 280.0f;
    bool closeOnExecute = true;
    bool focusOnOpen = true;
};

struct EditorContextMenuStats
{
    bool open = false;
    uint32 itemCount = 0;
    uint32 actionItemCount = 0;
    uint32 commandItemCount = 0;
    uint32 separatorCount = 0;
    uint32 executableItemCount = 0;
    uint32 disabledReasonItemCount = 0;
    int32 selectedItemIndex = -1;
    UI::Rect bounds;
};

/**
 * @brief Builds command/action context menus through the editor popup layer.
 */
class EditorContextMenu
{
public:
    void Open(EditorContextMenuDesc desc);
    void Close();
    void Clear();

    bool IsOpen() const { return m_open; }
    const std::string& GetOpenMenuId() const { return m_desc.id; }
    std::string GetRootWidgetName() const;
    bool WantsKeyboardFocus() const { return m_open && m_desc.focusOnOpen; }

    void Build(UI::UIContext& ui, EditorPopupLayer& popupLayer, EditorUIHost& host);
    bool HandleKeyDown(uint32 keyCode, EditorUIHost& host);

    const EditorContextMenuStats& GetLastBuildStats() const
    {
        return m_lastBuildStats;
    }

private:
    bool IsExecutableItem(const EditorContextMenuItem& item,
                          const EditorUIHost& host) const;
    bool ExecuteItem(const EditorContextMenuItem& item, EditorUIHost& host);
    void NormalizeSelectedItem(const EditorUIHost& host);
    bool MoveSelection(int32 delta, const EditorUIHost& host);

    EditorContextMenuDesc m_desc;
    EditorContextMenuStats m_lastBuildStats;
    int32 m_selectedItemIndex = -1;
    bool m_open = false;
};

} // namespace RVX::Editor
