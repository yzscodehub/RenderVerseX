/**
 * @file EditorCommandRegistry.cpp
 * @brief Native editor command registry implementation
 */

#include "Editor/UI/EditorCommandRegistry.h"

#include <algorithm>
#include <utility>

namespace RVX::Editor
{
namespace
{
    bool ShortcutsEqual(const EditorShortcut& lhs, const EditorShortcut& rhs)
    {
        return lhs.valid && rhs.valid && lhs.keyCode == rhs.keyCode &&
               lhs.modifiers == rhs.modifiers;
    }

    bool ShortcutMatchesInput(const EditorShortcut& shortcut,
                              const UI::UIInputState& input)
    {
        return shortcut.valid && input.WasKeyPressed(shortcut.keyCode) &&
               input.current.modifiers == shortcut.modifiers;
    }

    void AppendCommandShortcuts(const EditorCommand& command,
                                std::vector<EditorShortcut>& shortcuts)
    {
        if (command.desc.shortcut.valid)
        {
            shortcuts.push_back(command.desc.shortcut);
        }
        for (const EditorShortcut& shortcut : command.desc.secondaryShortcuts)
        {
            if (shortcut.valid)
            {
                shortcuts.push_back(shortcut);
            }
        }
    }

    uint32 SanitizeCommandScopeMask(uint32 scopeMask)
    {
        return scopeMask & RVX_EDITOR_COMMAND_SCOPE_ALL_MASK;
    }

    bool CommandMatchesScope(const EditorCommand& command,
                             EditorCommandScope scope)
    {
        return (SanitizeCommandScopeMask(command.desc.shortcutScopeMask) &
                ToEditorCommandScopeMask(scope)) != 0u;
    }

    EditorCommandExecutionResult MakeCommandResult(
        EditorCommandExecutionStatus status,
        std::string commandId,
        EditorShortcut shortcut = {},
        uint64 executionCount = 0,
        bool routedShortcut = false)
    {
        EditorCommandExecutionResult result;
        result.status = status;
        result.commandId = std::move(commandId);
        result.shortcut = shortcut;
        result.executionCount = executionCount;
        result.routedShortcut = routedShortcut;
        return result;
    }

    EditorCommandExecutionResult RouteShortcutInScope(
        std::vector<EditorCommand>& commands,
        const UI::UIInputState& input,
        EditorCommandContext& context,
        EditorCommandScope scope,
        uint32 activeShortcutScopeMask,
        EditorCommandExecutionResult& lastExecutionResult)
    {
        EditorCommandExecutionResult firstFailure = MakeCommandResult(
            EditorCommandExecutionStatus::ShortcutNotMatched,
            {},
            {},
            0,
            true);

        for (EditorCommand& command : commands)
        {
            if (!CommandMatchesScope(command, scope))
            {
                continue;
            }

            std::vector<EditorShortcut> shortcuts;
            AppendCommandShortcuts(command, shortcuts);
            for (const EditorShortcut& shortcut : shortcuts)
            {
                if (!ShortcutMatchesInput(shortcut, input))
                {
                    continue;
                }

                if (!command.desc.enabled)
                {
                    if (firstFailure.status ==
                        EditorCommandExecutionStatus::ShortcutNotMatched)
                    {
                        firstFailure = MakeCommandResult(
                            EditorCommandExecutionStatus::Disabled,
                            command.desc.id,
                            shortcut,
                            command.executionCount,
                            true);
                    }
                    continue;
                }

                if (input.current.wantsKeyboardCapture &&
                    !command.desc.allowWhenKeyboardCaptured)
                {
                    if (firstFailure.status ==
                        EditorCommandExecutionStatus::ShortcutNotMatched)
                    {
                        firstFailure = MakeCommandResult(
                            EditorCommandExecutionStatus::BlockedByKeyboardCapture,
                            command.desc.id,
                            shortcut,
                            command.executionCount,
                            true);
                    }
                    continue;
                }

                if (!command.desc.callback)
                {
                    if (firstFailure.status ==
                        EditorCommandExecutionStatus::ShortcutNotMatched)
                    {
                        firstFailure = MakeCommandResult(
                            EditorCommandExecutionStatus::MissingCallback,
                            command.desc.id,
                            shortcut,
                            command.executionCount,
                            true);
                    }
                    continue;
                }

                context.input = &input;
                context.activeShortcutScopeMask = activeShortcutScopeMask;
                command.desc.callback(context);
                ++command.executionCount;
                lastExecutionResult = MakeCommandResult(
                    EditorCommandExecutionStatus::Succeeded,
                    command.desc.id,
                    shortcut,
                    command.executionCount,
                    true);
                return lastExecutionResult;
            }
        }

        return firstFailure;
    }
}

bool EditorCommandRegistry::RegisterCommand(EditorCommandDesc desc)
{
    if (desc.id.empty() || FindCommand(desc.id))
    {
        return false;
    }

    EditorCommand command;
    command.desc = std::move(desc);
    command.desc.shortcutScopeMask =
        SanitizeCommandScopeMask(command.desc.shortcutScopeMask);
    if (command.desc.shortcutScopeMask == 0u)
    {
        command.desc.shortcutScopeMask = RVX_EDITOR_COMMAND_SCOPE_GLOBAL_MASK;
    }
    m_commands.push_back(std::move(command));
    return true;
}

bool EditorCommandRegistry::UnregisterCommand(const std::string& id)
{
    const auto it = std::find_if(m_commands.begin(),
                                 m_commands.end(),
                                 [&id](const EditorCommand& command) {
                                     return command.desc.id == id;
                                 });
    if (it == m_commands.end())
    {
        return false;
    }

    m_commands.erase(it);
    return true;
}

bool EditorCommandRegistry::ExecuteCommand(const std::string& id, EditorCommandContext& context)
{
    return ExecuteCommandDetailed(id, context).Succeeded();
}

bool EditorCommandRegistry::RouteShortcut(const UI::UIInputState& input, EditorCommandContext& context)
{
    return RouteShortcutDetailed(input, context).Succeeded();
}

EditorCommandExecutionResult EditorCommandRegistry::ExecuteCommandDetailed(
    const std::string& id,
    EditorCommandContext& context)
{
    EditorCommand* command = FindCommand(id);
    if (!command)
    {
        m_lastExecutionResult =
            MakeCommandResult(EditorCommandExecutionStatus::NotFound, id);
        return m_lastExecutionResult;
    }
    if (!command->desc.enabled)
    {
        m_lastExecutionResult =
            MakeCommandResult(EditorCommandExecutionStatus::Disabled, id);
        return m_lastExecutionResult;
    }
    if (!command->desc.callback)
    {
        m_lastExecutionResult =
            MakeCommandResult(EditorCommandExecutionStatus::MissingCallback, id);
        return m_lastExecutionResult;
    }

    command->desc.callback(context);
    ++command->executionCount;
    m_lastExecutionResult = MakeCommandResult(
        EditorCommandExecutionStatus::Succeeded,
        id,
        {},
        command->executionCount);
    return m_lastExecutionResult;
}

EditorCommandExecutionResult EditorCommandRegistry::RouteShortcutDetailed(
    const UI::UIInputState& input,
    EditorCommandContext& context)
{
    return RouteShortcutDetailed(input,
                                 context,
                                 RVX_EDITOR_COMMAND_SCOPE_ALL_MASK);
}

EditorCommandExecutionResult EditorCommandRegistry::RouteShortcutDetailed(
    const UI::UIInputState& input,
    EditorCommandContext& context,
    uint32 activeShortcutScopeMask)
{
    const uint32 sanitizedScopeMask =
        SanitizeCommandScopeMask(activeShortcutScopeMask);
    if (sanitizedScopeMask == 0u)
    {
        m_lastExecutionResult = MakeCommandResult(
            EditorCommandExecutionStatus::ShortcutNotMatched,
            {},
            {},
            0,
            true);
        return m_lastExecutionResult;
    }

    constexpr EditorCommandScope priority[] = {
        EditorCommandScope::FocusedPanel,
        EditorCommandScope::Viewport,
        EditorCommandScope::Global,
    };

    EditorCommandExecutionResult firstFailure = MakeCommandResult(
        EditorCommandExecutionStatus::ShortcutNotMatched,
        {},
        {},
        0,
        true);

    for (EditorCommandScope scope : priority)
    {
        if ((sanitizedScopeMask & ToEditorCommandScopeMask(scope)) == 0u)
        {
            continue;
        }

        EditorCommandExecutionResult result =
            RouteShortcutInScope(m_commands,
                                 input,
                                 context,
                                 scope,
                                 sanitizedScopeMask,
                                 m_lastExecutionResult);
        if (result.Succeeded())
        {
            return m_lastExecutionResult;
        }
        if (result.status != EditorCommandExecutionStatus::ShortcutNotMatched)
        {
            m_lastExecutionResult = result;
            return m_lastExecutionResult;
        }
        if (firstFailure.status ==
            EditorCommandExecutionStatus::ShortcutNotMatched)
        {
            firstFailure = result;
        }
    }

    m_lastExecutionResult = firstFailure;
    return m_lastExecutionResult;
}

EditorCommand* EditorCommandRegistry::FindCommand(const std::string& id)
{
    const auto it = std::find_if(m_commands.begin(),
                                 m_commands.end(),
                                 [&id](const EditorCommand& command) {
                                     return command.desc.id == id;
                                 });
    return it == m_commands.end() ? nullptr : &(*it);
}

const EditorCommand* EditorCommandRegistry::FindCommand(const std::string& id) const
{
    const auto it = std::find_if(m_commands.begin(),
                                 m_commands.end(),
                                 [&id](const EditorCommand& command) {
                                     return command.desc.id == id;
                                 });
    return it == m_commands.end() ? nullptr : &(*it);
}

void EditorCommandRegistry::SetCommandEnabled(const std::string& id,
                                              bool enabled,
                                              std::string disabledReason)
{
    EditorCommandAvailability availability;
    availability.enabled = enabled;
    availability.disabledReason = std::move(disabledReason);
    SetCommandAvailability(id, std::move(availability));
}

void EditorCommandRegistry::SetCommandAvailability(
    const std::string& id,
    EditorCommandAvailability availability)
{
    if (EditorCommand* command = FindCommand(id))
    {
        command->desc.enabled = availability.enabled;
        command->desc.disabledReason =
            availability.enabled ? std::string()
                                 : std::move(availability.disabledReason);
    }
}

void EditorCommandRegistry::SetCommandPrecondition(
    const std::string& id,
    EditorCommandPrecondition precondition)
{
    SetCommandAvailability(id, precondition.ToAvailability());
}

void EditorCommandRegistry::SetCommandDisabledReason(
    const std::string& id,
    std::string disabledReason)
{
    if (EditorCommand* command = FindCommand(id))
    {
        command->desc.disabledReason = std::move(disabledReason);
    }
}

void EditorCommandRegistry::SetCommandChecked(const std::string& id, bool checked)
{
    if (EditorCommand* command = FindCommand(id))
    {
        command->desc.checked = checked;
    }
}

bool EditorCommandRegistry::SetCommandShortcuts(
    const std::string& id,
    EditorShortcut shortcut,
    std::vector<EditorShortcut> secondaryShortcuts,
    uint32 shortcutScopeMask)
{
    EditorCommand* command = FindCommand(id);
    if (!command)
    {
        return false;
    }

    command->desc.shortcut = shortcut;
    command->desc.secondaryShortcuts = std::move(secondaryShortcuts);
    command->desc.shortcutScopeMask =
        SanitizeCommandScopeMask(shortcutScopeMask);
    if (command->desc.shortcutScopeMask == 0u)
    {
        command->desc.shortcutScopeMask = RVX_EDITOR_COMMAND_SCOPE_GLOBAL_MASK;
    }
    return true;
}

std::vector<EditorCommandShortcutConflict>
EditorCommandRegistry::GetShortcutConflicts() const
{
    struct CommandShortcutBinding
    {
        EditorShortcut shortcut;
        std::string commandId;
        uint32 scopeMask = RVX_EDITOR_COMMAND_SCOPE_GLOBAL_MASK;
    };

    std::vector<CommandShortcutBinding> bindings;
    for (const EditorCommand& command : m_commands)
    {
        if (command.desc.id.empty())
        {
            continue;
        }

        if (command.desc.shortcut.valid)
        {
            bindings.push_back({command.desc.shortcut,
                                command.desc.id,
                                SanitizeCommandScopeMask(
                                    command.desc.shortcutScopeMask)});
        }
        for (const EditorShortcut& shortcut : command.desc.secondaryShortcuts)
        {
            if (shortcut.valid)
            {
                bindings.push_back({shortcut,
                                    command.desc.id,
                                    SanitizeCommandScopeMask(
                                        command.desc.shortcutScopeMask)});
            }
        }
    }

    std::vector<EditorCommandShortcutConflict> conflicts;
    for (size_t lhs = 0; lhs < bindings.size(); ++lhs)
    {
        for (size_t rhs = lhs + 1; rhs < bindings.size(); ++rhs)
        {
            if (bindings[lhs].commandId == bindings[rhs].commandId)
            {
                continue;
            }
            if (!ShortcutsEqual(bindings[lhs].shortcut, bindings[rhs].shortcut))
            {
                continue;
            }
            const uint32 scopeOverlapMask =
                bindings[lhs].scopeMask & bindings[rhs].scopeMask;
            if (scopeOverlapMask == 0u)
            {
                continue;
            }

            EditorCommandShortcutConflict conflict;
            conflict.shortcut = bindings[lhs].shortcut;
            conflict.firstCommandId = bindings[lhs].commandId;
            conflict.secondCommandId = bindings[rhs].commandId;
            conflict.scopeOverlapMask = scopeOverlapMask;
            conflicts.push_back(std::move(conflict));
        }
    }
    return conflicts;
}

} // namespace RVX::Editor
