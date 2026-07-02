/**
 * @file EditorCommandRouter.h
 * @brief Native editor command shortcut routing policy
 */

#pragma once

#include "Core/Types.h"
#include "Editor/UI/EditorCommandRegistry.h"
#include "UI/UIContext.h"

namespace RVX::Editor
{

enum class EditorShortcutRouteBlockReason : uint8
{
    None = 0,
    CommandSurfaceMenu,
    ContextMenu,
    CommandPalette,
    FilePicker,
    ModalDialog,
    KeyboardCapture
};

struct EditorCommandShortcutRoutingPolicy
{
    bool commandSurfaceMenuOpen = false;
    bool contextMenuOpen = false;
    bool commandPaletteOpen = false;
    bool filePickerOpen = false;
    bool modalDialogOpen = false;
    uint32 activeShortcutScopeMask = RVX_EDITOR_COMMAND_SCOPE_GLOBAL_MASK;

    bool HasPopupCapture() const;
    EditorShortcutRouteBlockReason GetPopupBlockReason() const;
};

struct EditorCommandShortcutRouteDesc
{
    EditorCommandRegistry* registry = nullptr;
    const UI::UIInputState* input = nullptr;
    EditorCommandContext* context = nullptr;
    EditorCommandShortcutRoutingPolicy policy;
};

struct EditorCommandShortcutRouteResult
{
    EditorCommandExecutionResult commandResult;
    EditorShortcutRouteBlockReason blockReason =
        EditorShortcutRouteBlockReason::None;
    bool registryReady = false;
    bool inputReady = false;
    bool contextReady = false;
    bool shortcutInputDetected = false;
    bool shortcutDispatchAttempted = false;
    bool shortcutDispatchBlocked = false;
    uint32 activeShortcutScopeMask = RVX_EDITOR_COMMAND_SCOPE_GLOBAL_MASK;
};

class EditorCommandRouter
{
public:
    const EditorCommandShortcutRouteResult& RouteShortcut(
        const EditorCommandShortcutRouteDesc& desc);

    const EditorCommandShortcutRouteResult& GetLastShortcutRouteResult() const
    {
        return m_lastShortcutRouteResult;
    }

private:
    EditorCommandShortcutRouteResult m_lastShortcutRouteResult;
};

} // namespace RVX::Editor
