/**
 * @file EditorCommandIconResolver.cpp
 * @brief Central command id to native editor icon mapping implementation
 */

#include "Editor/UI/EditorCommandIconResolver.h"

#include "Editor/UI/EditorCommandCatalog.h"

namespace RVX::Editor
{

std::string EditorCommandIconResolver::ResolveIconName(
    const std::string& commandId,
    const std::string& explicitIconName)
{
    if (!explicitIconName.empty())
    {
        return explicitIconName;
    }

    return ResolveCommandIconName(commandId);
}

std::string EditorCommandIconResolver::ResolveCommandIconName(
    const std::string& commandId)
{
    if (commandId == EditorCommandIds::FileNewScene)
    {
        return "file-plus";
    }
    if (commandId == EditorCommandIds::FileOpenScene)
    {
        return "folder-open";
    }
    if (commandId == EditorCommandIds::FileSaveScene ||
        commandId == EditorCommandIds::FileSaveSceneAs)
    {
        return "save";
    }
    if (commandId == EditorCommandIds::FileExit)
    {
        return "close";
    }
    if (commandId == EditorCommandIds::EditUndo)
    {
        return "undo";
    }
    if (commandId == EditorCommandIds::EditRedo)
    {
        return "redo";
    }
    if (commandId == EditorCommandIds::EditDelete)
    {
        return "trash";
    }
    if (commandId == EditorCommandIds::EditPreferences)
    {
        return "settings";
    }
    if (commandId == EditorCommandIds::ViewCommandPalette)
    {
        return "command";
    }
    if (commandId == EditorCommandIds::ViewSaveLayout)
    {
        return "layout-save";
    }
    if (commandId == EditorCommandIds::ViewReloadLayout)
    {
        return "refresh";
    }
    if (commandId == EditorCommandIds::ViewResetLayout)
    {
        return "reset";
    }
    if (commandId == EditorCommandIds::ViewToggleBottomDrawer)
    {
        return "panel-bottom";
    }
    if (commandId == EditorCommandIds::GameObjectCreateEmpty)
    {
        return "cube-plus";
    }
    if (commandId == EditorCommandIds::GizmoTranslate)
    {
        return "move";
    }
    if (commandId == EditorCommandIds::GizmoRotate)
    {
        return "rotate-cw";
    }
    if (commandId == EditorCommandIds::GizmoScale)
    {
        return "scale";
    }

    return {};
}

} // namespace RVX::Editor
