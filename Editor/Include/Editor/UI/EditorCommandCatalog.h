/**
 * @file EditorCommandCatalog.h
 * @brief Built-in production editor command catalog
 */

#pragma once

#include "Editor/UI/EditorCommandRegistry.h"

#include <functional>

namespace RVX::Editor
{

namespace EditorCommandIds
{
    inline constexpr const char* FileNewScene = "file.newScene";
    inline constexpr const char* FileOpenScene = "file.openScene";
    inline constexpr const char* FileSaveScene = "file.saveScene";
    inline constexpr const char* FileSaveSceneAs = "file.saveSceneAs";
    inline constexpr const char* FileExit = "file.exit";
    inline constexpr const char* EditUndo = "edit.undo";
    inline constexpr const char* EditRedo = "edit.redo";
    inline constexpr const char* EditDelete = "edit.delete";
    inline constexpr const char* EditPreferences = "edit.preferences";
    inline constexpr const char* ViewCommandPalette = "view.commandPalette";
    inline constexpr const char* ViewSaveLayout = "view.saveLayout";
    inline constexpr const char* ViewReloadLayout = "view.reloadLayout";
    inline constexpr const char* ViewResetLayout = "view.resetLayout";
    inline constexpr const char* ViewToggleBottomDrawer = "view.toggleBottomDrawer";
    inline constexpr const char* GameObjectCreateEmpty = "gameObject.createEmpty";
    inline constexpr const char* GizmoTranslate = "gizmo.translate";
    inline constexpr const char* GizmoRotate = "gizmo.rotate";
    inline constexpr const char* GizmoScale = "gizmo.scale";
}

struct EditorCommandCatalogCallbacks
{
    std::function<void()> newScene;
    std::function<void()> openScene;
    std::function<void()> saveScene;
    std::function<void()> saveSceneAs;
    std::function<void()> exit;
    std::function<void()> undo;
    std::function<void()> redo;
    std::function<void()> deleteSelection;
    std::function<void()> preferences;
    std::function<void()> commandPalette;
    std::function<void()> saveLayout;
    std::function<void()> reloadLayout;
    std::function<void()> resetLayout;
    std::function<void()> toggleBottomDrawer;
    std::function<void()> createEmpty;
    std::function<void()> gizmoTranslate;
    std::function<void()> gizmoRotate;
    std::function<void()> gizmoScale;
};

bool RegisterEditorCommandCatalog(EditorCommandRegistry& registry,
                                  const EditorCommandCatalogCallbacks& callbacks = {});

} // namespace RVX::Editor
