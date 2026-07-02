/**
 * @file EditorCommandRegistry.h
 * @brief Native editor command and shortcut routing infrastructure
 */

#pragma once

#include "Core/Types.h"
#include "UI/UIContext.h"

#include <functional>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace RVX::Editor
{

class EditorUIHost;

struct EditorShortcut
{
    uint32 keyCode = 0;
    uint32 modifiers = 0;
    bool valid = false;

    static EditorShortcut Key(uint32 keyCode, uint32 modifiers = 0)
    {
        EditorShortcut shortcut;
        shortcut.keyCode = keyCode;
        shortcut.modifiers = modifiers;
        shortcut.valid = true;
        return shortcut;
    }
};

enum class EditorCommandScope : uint8
{
    Global = 0,
    FocusedPanel,
    Viewport
};

inline constexpr uint32 ToEditorCommandScopeMask(EditorCommandScope scope)
{
    return 1u << static_cast<uint8>(scope);
}

inline constexpr uint32 RVX_EDITOR_COMMAND_SCOPE_GLOBAL_MASK =
    ToEditorCommandScopeMask(EditorCommandScope::Global);
inline constexpr uint32 RVX_EDITOR_COMMAND_SCOPE_FOCUSED_PANEL_MASK =
    ToEditorCommandScopeMask(EditorCommandScope::FocusedPanel);
inline constexpr uint32 RVX_EDITOR_COMMAND_SCOPE_VIEWPORT_MASK =
    ToEditorCommandScopeMask(EditorCommandScope::Viewport);
inline constexpr uint32 RVX_EDITOR_COMMAND_SCOPE_ALL_MASK =
    RVX_EDITOR_COMMAND_SCOPE_GLOBAL_MASK |
    RVX_EDITOR_COMMAND_SCOPE_FOCUSED_PANEL_MASK |
    RVX_EDITOR_COMMAND_SCOPE_VIEWPORT_MASK;

enum class EditorCommandExecutionStatus : uint8
{
    Succeeded = 0,
    NotFound,
    Disabled,
    MissingCallback,
    ShortcutNotMatched,
    BlockedByKeyboardCapture
};

struct EditorCommandExecutionResult
{
    EditorCommandExecutionStatus status =
        EditorCommandExecutionStatus::ShortcutNotMatched;
    std::string commandId;
    EditorShortcut shortcut;
    uint64 executionCount = 0;
    bool routedShortcut = false;

    bool Succeeded() const
    {
        return status == EditorCommandExecutionStatus::Succeeded;
    }

    explicit operator bool() const { return Succeeded(); }
};

struct EditorCommandShortcutConflict
{
    EditorShortcut shortcut;
    std::string firstCommandId;
    std::string secondCommandId;
    uint32 scopeOverlapMask = 0;
};

struct EditorCommandContext
{
    EditorUIHost* host = nullptr;
    const UI::UIInputState* input = nullptr;
    uint32 activeShortcutScopeMask = RVX_EDITOR_COMMAND_SCOPE_GLOBAL_MASK;
    float deltaTime = 0.0f;
};

struct EditorCommandAvailability
{
    bool enabled = true;
    std::string disabledReason;

    static EditorCommandAvailability Available()
    {
        return {};
    }

    static EditorCommandAvailability Unavailable(std::string reason)
    {
        EditorCommandAvailability availability;
        availability.enabled = false;
        availability.disabledReason = std::move(reason);
        return availability;
    }
};

struct EditorCommandPrecondition
{
    bool satisfied = true;
    std::string disabledReason;

    static EditorCommandPrecondition Passed()
    {
        return {};
    }

    static EditorCommandPrecondition Requires(bool condition,
                                              std::string reason)
    {
        EditorCommandPrecondition precondition;
        precondition.satisfied = condition;
        precondition.disabledReason =
            condition ? std::string() : std::move(reason);
        return precondition;
    }

    static EditorCommandPrecondition All(
        std::initializer_list<EditorCommandPrecondition> preconditions)
    {
        for (const EditorCommandPrecondition& precondition : preconditions)
        {
            if (!precondition.satisfied)
            {
                return precondition;
            }
        }
        return Passed();
    }

    EditorCommandAvailability ToAvailability() const
    {
        return satisfied
                   ? EditorCommandAvailability::Available()
                   : EditorCommandAvailability::Unavailable(disabledReason);
    }
};

using EditorCommandCallback = std::function<void(EditorCommandContext&)>;

struct EditorCommandDesc
{
    std::string id;
    std::string displayName;
    std::string category;
    std::string tooltip;
    std::string disabledReason;
    EditorShortcut shortcut;
    std::vector<EditorShortcut> secondaryShortcuts;
    EditorCommandCallback callback;
    bool enabled = true;
    bool checkable = false;
    bool checked = false;
    bool allowWhenKeyboardCaptured = false;
    uint32 shortcutScopeMask = RVX_EDITOR_COMMAND_SCOPE_GLOBAL_MASK;
};

struct EditorCommand
{
    EditorCommandDesc desc;
    uint64 executionCount = 0;
};

class EditorCommandRegistry
{
public:
    bool RegisterCommand(EditorCommandDesc desc);
    bool UnregisterCommand(const std::string& id);

    bool ExecuteCommand(const std::string& id, EditorCommandContext& context);
    bool RouteShortcut(const UI::UIInputState& input, EditorCommandContext& context);
    EditorCommandExecutionResult ExecuteCommandDetailed(
        const std::string& id,
        EditorCommandContext& context);
    EditorCommandExecutionResult RouteShortcutDetailed(
        const UI::UIInputState& input,
        EditorCommandContext& context);
    EditorCommandExecutionResult RouteShortcutDetailed(
        const UI::UIInputState& input,
        EditorCommandContext& context,
        uint32 activeShortcutScopeMask);

    EditorCommand* FindCommand(const std::string& id);
    const EditorCommand* FindCommand(const std::string& id) const;

    void SetCommandEnabled(const std::string& id,
                           bool enabled,
                           std::string disabledReason = {});
    void SetCommandAvailability(const std::string& id,
                                EditorCommandAvailability availability);
    void SetCommandPrecondition(const std::string& id,
                                EditorCommandPrecondition precondition);
    void SetCommandDisabledReason(const std::string& id,
                                  std::string disabledReason);
    void SetCommandChecked(const std::string& id, bool checked);
    bool SetCommandShortcuts(const std::string& id,
                             EditorShortcut shortcut,
                             std::vector<EditorShortcut> secondaryShortcuts,
                             uint32 shortcutScopeMask);
    size_t GetCommandCount() const { return m_commands.size(); }
    const std::vector<EditorCommand>& GetCommands() const { return m_commands; }
    std::vector<EditorCommandShortcutConflict> GetShortcutConflicts() const;
    const EditorCommandExecutionResult& GetLastExecutionResult() const
    {
        return m_lastExecutionResult;
    }

private:
    std::vector<EditorCommand> m_commands;
    EditorCommandExecutionResult m_lastExecutionResult;
};

} // namespace RVX::Editor
