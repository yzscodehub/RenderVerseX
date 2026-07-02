/**
 * @file EditorCommandRouter.cpp
 * @brief Native editor command shortcut routing policy implementation
 */

#include "Editor/UI/EditorCommandRouter.h"

namespace RVX::Editor
{
namespace
{
    bool HasShortcutInput(const UI::UIInputState& input)
    {
        for (bool keyPressed : input.keysPressed)
        {
            if (keyPressed)
            {
                return true;
            }
        }
        return false;
    }

    EditorCommandExecutionResult MakeShortcutNotMatchedResult()
    {
        EditorCommandExecutionResult result;
        result.status = EditorCommandExecutionStatus::ShortcutNotMatched;
        result.routedShortcut = true;
        return result;
    }
}

bool EditorCommandShortcutRoutingPolicy::HasPopupCapture() const
{
    return GetPopupBlockReason() != EditorShortcutRouteBlockReason::None;
}

EditorShortcutRouteBlockReason
EditorCommandShortcutRoutingPolicy::GetPopupBlockReason() const
{
    if (modalDialogOpen)
    {
        return EditorShortcutRouteBlockReason::ModalDialog;
    }
    if (filePickerOpen)
    {
        return EditorShortcutRouteBlockReason::FilePicker;
    }
    if (commandPaletteOpen)
    {
        return EditorShortcutRouteBlockReason::CommandPalette;
    }
    if (contextMenuOpen)
    {
        return EditorShortcutRouteBlockReason::ContextMenu;
    }
    if (commandSurfaceMenuOpen)
    {
        return EditorShortcutRouteBlockReason::CommandSurfaceMenu;
    }
    return EditorShortcutRouteBlockReason::None;
}

const EditorCommandShortcutRouteResult& EditorCommandRouter::RouteShortcut(
    const EditorCommandShortcutRouteDesc& desc)
{
    m_lastShortcutRouteResult = {};
    m_lastShortcutRouteResult.registryReady = desc.registry != nullptr;
    m_lastShortcutRouteResult.inputReady = desc.input != nullptr;
    m_lastShortcutRouteResult.contextReady = desc.context != nullptr;
    m_lastShortcutRouteResult.commandResult = MakeShortcutNotMatchedResult();
    m_lastShortcutRouteResult.activeShortcutScopeMask =
        desc.policy.activeShortcutScopeMask;

    if (!m_lastShortcutRouteResult.registryReady ||
        !m_lastShortcutRouteResult.inputReady ||
        !m_lastShortcutRouteResult.contextReady)
    {
        return m_lastShortcutRouteResult;
    }

    m_lastShortcutRouteResult.shortcutInputDetected =
        HasShortcutInput(*desc.input);
    if (!m_lastShortcutRouteResult.shortcutInputDetected)
    {
        return m_lastShortcutRouteResult;
    }

    const EditorShortcutRouteBlockReason popupBlockReason =
        desc.policy.GetPopupBlockReason();
    if (popupBlockReason != EditorShortcutRouteBlockReason::None)
    {
        m_lastShortcutRouteResult.blockReason = popupBlockReason;
        m_lastShortcutRouteResult.shortcutDispatchBlocked = true;
        return m_lastShortcutRouteResult;
    }

    m_lastShortcutRouteResult.shortcutDispatchAttempted = true;
    m_lastShortcutRouteResult.commandResult =
        desc.registry->RouteShortcutDetailed(
            *desc.input,
            *desc.context,
            desc.policy.activeShortcutScopeMask);
    if (m_lastShortcutRouteResult.commandResult.status ==
        EditorCommandExecutionStatus::BlockedByKeyboardCapture)
    {
        m_lastShortcutRouteResult.blockReason =
            EditorShortcutRouteBlockReason::KeyboardCapture;
        m_lastShortcutRouteResult.shortcutDispatchBlocked = true;
    }
    return m_lastShortcutRouteResult;
}

} // namespace RVX::Editor
