/**
 * @file EditorCommandSurfaceModel.cpp
 * @brief Command-driven native editor surface model implementation
 */

#include "Editor/UI/EditorCommandSurfaceModel.h"
#include "Editor/UI/EditorCommandCatalog.h"

#include <algorithm>
#include <utility>

namespace RVX::Editor
{

EditorCommandSurfaceItem EditorCommandSurfaceItem::Command(std::string commandId,
                                                           std::string iconName,
                                                           std::string displayNameOverride)
{
    EditorCommandSurfaceItem item;
    item.type = EditorCommandSurfaceItemType::Command;
    item.commandId = std::move(commandId);
    item.iconName = std::move(iconName);
    item.displayNameOverride = std::move(displayNameOverride);
    return item;
}

EditorCommandSurfaceItem EditorCommandSurfaceItem::Separator()
{
    EditorCommandSurfaceItem item;
    item.type = EditorCommandSurfaceItemType::Separator;
    return item;
}

void EditorCommandSurfaceModel::Clear()
{
    m_menus.clear();
    m_toolbars.clear();
    m_statusItems.clear();
}

void EditorCommandSurfaceModel::BuildDefaultEditorSurfaces()
{
    Clear();

    EditorMenuSurface fileMenu;
    fileMenu.id = "file";
    fileMenu.title = "File";
    fileMenu.items.push_back(EditorCommandSurfaceItem::Command(EditorCommandIds::FileNewScene));
    fileMenu.items.push_back(EditorCommandSurfaceItem::Command(EditorCommandIds::FileOpenScene));
    fileMenu.items.push_back(EditorCommandSurfaceItem::Command(EditorCommandIds::FileSaveScene));
    fileMenu.items.push_back(EditorCommandSurfaceItem::Command(EditorCommandIds::FileSaveSceneAs));
    fileMenu.items.push_back(EditorCommandSurfaceItem::Separator());
    fileMenu.items.push_back(EditorCommandSurfaceItem::Command(EditorCommandIds::FileExit));
    AddMenu(std::move(fileMenu));

    EditorMenuSurface editMenu;
    editMenu.id = "edit";
    editMenu.title = "Edit";
    editMenu.items.push_back(EditorCommandSurfaceItem::Command(EditorCommandIds::EditUndo));
    editMenu.items.push_back(EditorCommandSurfaceItem::Command(EditorCommandIds::EditRedo));
    editMenu.items.push_back(EditorCommandSurfaceItem::Separator());
    editMenu.items.push_back(EditorCommandSurfaceItem::Command(EditorCommandIds::EditDelete));
    editMenu.items.push_back(EditorCommandSurfaceItem::Separator());
    editMenu.items.push_back(EditorCommandSurfaceItem::Command(EditorCommandIds::EditPreferences));
    AddMenu(std::move(editMenu));

    EditorMenuSurface gameObjectMenu;
    gameObjectMenu.id = "gameObject";
    gameObjectMenu.title = "GameObject";
    gameObjectMenu.items.push_back(EditorCommandSurfaceItem::Command(EditorCommandIds::GameObjectCreateEmpty));
    AddMenu(std::move(gameObjectMenu));

    EditorMenuSurface viewMenu;
    viewMenu.id = "view";
    viewMenu.title = "View";
    viewMenu.items.push_back(EditorCommandSurfaceItem::Command(EditorCommandIds::ViewCommandPalette));
    viewMenu.items.push_back(EditorCommandSurfaceItem::Separator());
    viewMenu.items.push_back(EditorCommandSurfaceItem::Command(EditorCommandIds::ViewSaveLayout));
    viewMenu.items.push_back(EditorCommandSurfaceItem::Command(EditorCommandIds::ViewReloadLayout));
    viewMenu.items.push_back(EditorCommandSurfaceItem::Command(EditorCommandIds::ViewResetLayout));
    viewMenu.items.push_back(EditorCommandSurfaceItem::Separator());
    viewMenu.items.push_back(EditorCommandSurfaceItem::Command(EditorCommandIds::ViewToggleBottomDrawer));
    AddMenu(std::move(viewMenu));

    EditorToolbarSurface mainToolbar;
    mainToolbar.id = "main";
    mainToolbar.title = "Main";
    mainToolbar.items.push_back(EditorCommandSurfaceItem::Command(
        EditorCommandIds::FileNewScene,
        "file-plus",
        "New"));
    mainToolbar.items.push_back(EditorCommandSurfaceItem::Command(
        EditorCommandIds::FileOpenScene,
        "folder-open",
        "Open"));
    mainToolbar.items.push_back(EditorCommandSurfaceItem::Command(
        EditorCommandIds::FileSaveScene,
        "save",
        "Save"));
    mainToolbar.items.push_back(EditorCommandSurfaceItem::Separator());
    mainToolbar.items.push_back(EditorCommandSurfaceItem::Command(
        EditorCommandIds::EditUndo,
        "undo",
        "Undo"));
    mainToolbar.items.push_back(EditorCommandSurfaceItem::Command(
        EditorCommandIds::EditRedo,
        "redo",
        "Redo"));
    AddToolbar(std::move(mainToolbar));

    EditorToolbarSurface gizmoToolbar;
    gizmoToolbar.id = "gizmo";
    gizmoToolbar.title = "Gizmo";
    gizmoToolbar.items.push_back(EditorCommandSurfaceItem::Command(
        EditorCommandIds::GizmoTranslate,
        "move",
        "Move"));
    gizmoToolbar.items.push_back(EditorCommandSurfaceItem::Command(
        EditorCommandIds::GizmoRotate,
        "rotate-cw",
        "Rotate"));
    gizmoToolbar.items.push_back(EditorCommandSurfaceItem::Command(
        EditorCommandIds::GizmoScale,
        "scale",
        "Scale"));
    AddToolbar(std::move(gizmoToolbar));

    SetStatusItem({"editor.state", "State", "Ready", 0, true});
    SetStatusItem({"editor.frameTime", "Frame", "--", 10, true});
}

bool EditorCommandSurfaceModel::AddMenu(EditorMenuSurface menu)
{
    if (menu.id.empty() || FindMenu(menu.id))
    {
        return false;
    }

    m_menus.push_back(std::move(menu));
    return true;
}

bool EditorCommandSurfaceModel::AddMenuItem(const std::string& menuId,
                                            EditorCommandSurfaceItem item)
{
    if (menuId.empty())
    {
        return false;
    }

    const auto menuIt = std::find_if(m_menus.begin(),
                                     m_menus.end(),
                                     [&menuId](const EditorMenuSurface& menu) {
                                         return menu.id == menuId;
                                     });
    if (menuIt == m_menus.end())
    {
        return false;
    }

    if (item.type == EditorCommandSurfaceItemType::Command && item.commandId.empty())
    {
        return false;
    }

    if (item.type == EditorCommandSurfaceItemType::Command)
    {
        const auto itemIt = std::find_if(menuIt->items.begin(),
                                         menuIt->items.end(),
                                         [&item](const EditorCommandSurfaceItem& existing) {
                                             return existing.type ==
                                                        EditorCommandSurfaceItemType::Command &&
                                                    existing.commandId == item.commandId;
                                         });
        if (itemIt != menuIt->items.end())
        {
            *itemIt = std::move(item);
            return true;
        }
    }

    menuIt->items.push_back(std::move(item));
    return true;
}

bool EditorCommandSurfaceModel::AddToolbar(EditorToolbarSurface toolbar)
{
    if (toolbar.id.empty() || FindToolbar(toolbar.id))
    {
        return false;
    }

    m_toolbars.push_back(std::move(toolbar));
    return true;
}

bool EditorCommandSurfaceModel::SetStatusItem(EditorStatusSurfaceItem item)
{
    if (item.id.empty())
    {
        return false;
    }

    const auto it = std::find_if(m_statusItems.begin(),
                                 m_statusItems.end(),
                                 [&item](const EditorStatusSurfaceItem& existing) {
                                     return existing.id == item.id;
                                 });
    if (it != m_statusItems.end())
    {
        *it = std::move(item);
        return true;
    }

    m_statusItems.push_back(std::move(item));
    std::stable_sort(m_statusItems.begin(),
                     m_statusItems.end(),
                     [](const EditorStatusSurfaceItem& lhs,
                        const EditorStatusSurfaceItem& rhs) {
                         return lhs.priority < rhs.priority;
                     });
    return true;
}

const EditorMenuSurface* EditorCommandSurfaceModel::FindMenu(const std::string& id) const
{
    const auto it = std::find_if(m_menus.begin(),
                                 m_menus.end(),
                                 [&id](const EditorMenuSurface& menu) {
                                     return menu.id == id;
                                 });
    return it == m_menus.end() ? nullptr : &(*it);
}

const EditorToolbarSurface* EditorCommandSurfaceModel::FindToolbar(const std::string& id) const
{
    const auto it = std::find_if(m_toolbars.begin(),
                                 m_toolbars.end(),
                                 [&id](const EditorToolbarSurface& toolbar) {
                                     return toolbar.id == id;
                                 });
    return it == m_toolbars.end() ? nullptr : &(*it);
}

const EditorStatusSurfaceItem* EditorCommandSurfaceModel::FindStatusItem(const std::string& id) const
{
    const auto it = std::find_if(m_statusItems.begin(),
                                 m_statusItems.end(),
                                 [&id](const EditorStatusSurfaceItem& item) {
                                     return item.id == id;
                                 });
    return it == m_statusItems.end() ? nullptr : &(*it);
}

const EditorCommand* EditorCommandSurfaceModel::ResolveCommand(
    const EditorCommandSurfaceItem& item,
    const EditorCommandRegistry& registry) const
{
    if (item.type != EditorCommandSurfaceItemType::Command || item.commandId.empty())
    {
        return nullptr;
    }

    return registry.FindCommand(item.commandId);
}

std::string EditorCommandSurfaceModel::GetItemDisplayName(
    const EditorCommandSurfaceItem& item,
    const EditorCommandRegistry& registry) const
{
    const EditorCommand* command = ResolveCommand(item, registry);
    if (!command)
    {
        return item.displayNameOverride;
    }

    const std::string displayName = item.displayNameOverride.empty()
                                        ? command->desc.displayName
                                        : item.displayNameOverride;
    return displayName;
}

bool EditorCommandSurfaceModel::IsItemEnabled(const EditorCommandSurfaceItem& item,
                                              const EditorCommandRegistry& registry) const
{
    if (!item.visible)
    {
        return false;
    }
    if (item.type == EditorCommandSurfaceItemType::Separator)
    {
        return true;
    }

    const EditorCommand* command = ResolveCommand(item, registry);
    return command && command->desc.enabled;
}

} // namespace RVX::Editor
