/**
 * @file EditorCommandSurfaceModel.h
 * @brief Command-driven native editor menu, toolbar, and status models
 */

#pragma once

#include "Core/Types.h"
#include "Editor/UI/EditorCommandRegistry.h"

#include <string>
#include <vector>

namespace RVX::Editor
{

enum class EditorCommandSurfaceItemType : uint8
{
    Command = 0,
    Separator
};

struct EditorCommandSurfaceItem
{
    EditorCommandSurfaceItemType type = EditorCommandSurfaceItemType::Command;
    std::string commandId;
    std::string displayNameOverride;
    std::string iconName;
    bool visible = true;

    static EditorCommandSurfaceItem Command(std::string commandId,
                                            std::string iconName = {},
                                            std::string displayNameOverride = {});
    static EditorCommandSurfaceItem Separator();
};

struct EditorMenuSurface
{
    std::string id;
    std::string title;
    std::vector<EditorCommandSurfaceItem> items;
};

struct EditorToolbarSurface
{
    std::string id;
    std::string title;
    std::vector<EditorCommandSurfaceItem> items;
};

struct EditorStatusSurfaceItem
{
    std::string id;
    std::string label;
    std::string value;
    uint32 priority = 0;
    bool visible = true;
};

class EditorCommandSurfaceModel
{
public:
    void Clear();
    void BuildDefaultEditorSurfaces();

    bool AddMenu(EditorMenuSurface menu);
    bool AddMenuItem(const std::string& menuId, EditorCommandSurfaceItem item);
    bool AddToolbar(EditorToolbarSurface toolbar);
    bool SetStatusItem(EditorStatusSurfaceItem item);

    const EditorMenuSurface* FindMenu(const std::string& id) const;
    const EditorToolbarSurface* FindToolbar(const std::string& id) const;
    const EditorStatusSurfaceItem* FindStatusItem(const std::string& id) const;

    const EditorCommand* ResolveCommand(const EditorCommandSurfaceItem& item,
                                        const EditorCommandRegistry& registry) const;
    std::string GetItemDisplayName(const EditorCommandSurfaceItem& item,
                                   const EditorCommandRegistry& registry) const;
    bool IsItemEnabled(const EditorCommandSurfaceItem& item,
                       const EditorCommandRegistry& registry) const;

    const std::vector<EditorMenuSurface>& GetMenus() const { return m_menus; }
    const std::vector<EditorToolbarSurface>& GetToolbars() const { return m_toolbars; }
    const std::vector<EditorStatusSurfaceItem>& GetStatusItems() const { return m_statusItems; }

private:
    std::vector<EditorMenuSurface> m_menus;
    std::vector<EditorToolbarSurface> m_toolbars;
    std::vector<EditorStatusSurfaceItem> m_statusItems;
};

} // namespace RVX::Editor
