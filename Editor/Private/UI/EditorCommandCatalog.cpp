/**
 * @file EditorCommandCatalog.cpp
 * @brief Built-in production editor command catalog implementation
 */

#include "Editor/UI/EditorCommandCatalog.h"

#include <utility>

namespace RVX::Editor
{
namespace
{
    EditorCommandCallback MakeCommandCallback(const std::function<void()>& callback)
    {
        if (!callback)
        {
            return {};
        }

        return [callback](EditorCommandContext& context) {
            (void)context;
            callback();
        };
    }

    EditorCommandDesc MakeCommand(const char* id,
                                  const char* displayName,
                                  const char* category,
                                  const char* tooltip,
                                  EditorShortcut shortcut,
                                  const std::function<void()>& callback,
                                  bool allowWhenKeyboardCaptured = false)
    {
        EditorCommandDesc desc;
        desc.id = id;
        desc.displayName = displayName;
        desc.category = category;
        desc.tooltip = tooltip;
        desc.shortcut = shortcut;
        desc.callback = MakeCommandCallback(callback);
        desc.allowWhenKeyboardCaptured = allowWhenKeyboardCaptured;
        return desc;
    }

    EditorCommandDesc MakeViewportCommand(const char* id,
                                          const char* displayName,
                                          const char* category,
                                          const char* tooltip,
                                          EditorShortcut shortcut,
                                          const std::function<void()>& callback)
    {
        EditorCommandDesc desc =
            MakeCommand(id, displayName, category, tooltip, shortcut, callback);
        desc.shortcutScopeMask = RVX_EDITOR_COMMAND_SCOPE_VIEWPORT_MASK;
        desc.allowWhenKeyboardCaptured = true;
        return desc;
    }

    bool Register(EditorCommandRegistry& registry, EditorCommandDesc desc)
    {
        return registry.RegisterCommand(std::move(desc));
    }
}

bool RegisterEditorCommandCatalog(EditorCommandRegistry& registry,
                                  const EditorCommandCatalogCallbacks& callbacks)
{
    bool success = true;
    const uint32 ctrl = UI::ToMask(UI::UIInputModifier::Ctrl);
    const uint32 shift = UI::ToMask(UI::UIInputModifier::Shift);

    success &= Register(registry,
                        MakeCommand(EditorCommandIds::FileNewScene,
                                    "New Scene",
                                    "File",
                                    "Create a new empty editor scene.",
                                    EditorShortcut::Key('N', ctrl),
                                    callbacks.newScene));
    success &= Register(registry,
                        MakeCommand(EditorCommandIds::FileOpenScene,
                                    "Open Scene",
                                    "File",
                                    "Open an editor scene from disk.",
                                    EditorShortcut::Key('O', ctrl),
                                    callbacks.openScene));
    success &= Register(registry,
                        MakeCommand(EditorCommandIds::FileSaveScene,
                                    "Save Scene",
                                    "File",
                                    "Save the active editor scene.",
                                    EditorShortcut::Key('S', ctrl),
                                    callbacks.saveScene));
    success &= Register(registry,
                        MakeCommand(EditorCommandIds::FileSaveSceneAs,
                                    "Save Scene As",
                                    "File",
                                    "Save the active editor scene to a new path.",
                                    EditorShortcut::Key('S', ctrl | shift),
                                    callbacks.saveSceneAs));
    success &= Register(registry,
                        MakeCommand(EditorCommandIds::FileExit,
                                    "Exit",
                                    "File",
                                    "Close the editor.",
                                    {},
                                    callbacks.exit));
    success &= Register(registry,
                        MakeCommand(EditorCommandIds::EditUndo,
                                    "Undo",
                                    "Edit",
                                    "Undo the last editor operation.",
                                    EditorShortcut::Key('Z', ctrl),
                                    callbacks.undo));
    success &= Register(registry,
                        MakeCommand(EditorCommandIds::EditRedo,
                                    "Redo",
                                    "Edit",
                                    "Redo the last undone editor operation.",
                                    EditorShortcut::Key('Y', ctrl),
                                    callbacks.redo));
    if (EditorCommand* redoCommand = registry.FindCommand(EditorCommandIds::EditRedo))
    {
        redoCommand->desc.secondaryShortcuts.push_back(
            EditorShortcut::Key('Z', ctrl | shift));
    }
    success &= Register(registry,
                        MakeCommand(EditorCommandIds::EditDelete,
                                    "Delete",
                                    "Edit",
                                    "Delete the current selection.",
                                    EditorShortcut::Key(127u),
                                    callbacks.deleteSelection));
    success &= Register(registry,
                        MakeCommand(EditorCommandIds::EditPreferences,
                                    "Preferences",
                                    "Edit",
                                    "Open editor preferences.",
                                    {},
                                    callbacks.preferences));
    success &= Register(registry,
                        MakeCommand(EditorCommandIds::ViewCommandPalette,
                                    "Command Palette",
                                    "View",
                                    "Open the editor command palette.",
                                    EditorShortcut::Key('P', ctrl | shift),
                                    callbacks.commandPalette,
                                    true));
    success &= Register(registry,
                        MakeCommand(EditorCommandIds::ViewSaveLayout,
                                    "Save Layout",
                                    "View",
                                    "Save the current editor workspace layout.",
                                    {},
                                    callbacks.saveLayout));
    success &= Register(registry,
                        MakeCommand(EditorCommandIds::ViewReloadLayout,
                                    "Reload Saved Layout",
                                    "View",
                                    "Reload the saved editor workspace layout.",
                                    {},
                                    callbacks.reloadLayout));
    success &= Register(registry,
                        MakeCommand(EditorCommandIds::ViewResetLayout,
                                    "Reset Layout",
                                    "View",
                                    "Restore the default editor layout.",
                                    {},
                                    callbacks.resetLayout));
    success &= Register(registry,
                        MakeCommand(EditorCommandIds::ViewToggleBottomDrawer,
                                    "Collapse Bottom Drawer",
                                    "View",
                                    "Collapse or expand the bottom editor drawer.",
                                    {},
                                    callbacks.toggleBottomDrawer));
    if (EditorCommand* drawerCommand =
            registry.FindCommand(EditorCommandIds::ViewToggleBottomDrawer))
    {
        drawerCommand->desc.checkable = true;
        drawerCommand->desc.checked = false;
    }
    success &= Register(registry,
                        MakeCommand(EditorCommandIds::GameObjectCreateEmpty,
                                    "Create Empty",
                                    "GameObject",
                                    "Create an empty scene entity.",
                                    EditorShortcut::Key('N', ctrl | shift),
                                    callbacks.createEmpty));
    success &= Register(registry,
                        MakeViewportCommand(
                            EditorCommandIds::GizmoTranslate,
                            "Translate",
                            "Gizmo",
                            "Switch the viewport gizmo to translate mode.",
                            EditorShortcut::Key('W'),
                            callbacks.gizmoTranslate));
    success &= Register(registry,
                        MakeViewportCommand(
                            EditorCommandIds::GizmoRotate,
                            "Rotate",
                            "Gizmo",
                            "Switch the viewport gizmo to rotate mode.",
                            EditorShortcut::Key('E'),
                            callbacks.gizmoRotate));
    success &= Register(registry,
                        MakeViewportCommand(
                            EditorCommandIds::GizmoScale,
                            "Scale",
                            "Gizmo",
                            "Switch the viewport gizmo to scale mode.",
                            EditorShortcut::Key('R'),
                            callbacks.gizmoScale));

    return success;
}

} // namespace RVX::Editor
