/**
 * @file EditorShortcutBindingModel.cpp
 * @brief Native editor shortcut binding edit model implementation.
 */

#include "Editor/UI/EditorShortcutBindingModel.h"

#include "UI/UIContext.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace RVX::Editor
{
namespace
{
    std::string KeyName(uint32 keyCode)
    {
        if (keyCode >= static_cast<uint32>('A') &&
            keyCode <= static_cast<uint32>('Z'))
        {
            return std::string(1, static_cast<char>(keyCode));
        }
        if (keyCode >= static_cast<uint32>('0') &&
            keyCode <= static_cast<uint32>('9'))
        {
            return std::string(1, static_cast<char>(keyCode));
        }

        switch (keyCode)
        {
            case UI::RVX_UI_KEY_BACKSPACE: return "Backspace";
            case UI::RVX_UI_KEY_ENTER: return "Enter";
            case UI::RVX_UI_KEY_ESCAPE: return "Escape";
            case UI::RVX_UI_KEY_SPACE: return "Space";
            case UI::RVX_UI_KEY_DELETE: return "Delete";
            case UI::RVX_UI_KEY_LEFT: return "Left";
            case UI::RVX_UI_KEY_RIGHT: return "Right";
            case UI::RVX_UI_KEY_UP: return "Up";
            case UI::RVX_UI_KEY_DOWN: return "Down";
            case UI::RVX_UI_KEY_HOME: return "Home";
            case UI::RVX_UI_KEY_END: return "End";
            default: break;
        }

        return "Key" + std::to_string(keyCode);
    }

    bool ShortcutsEqual(const EditorShortcut& lhs,
                        const EditorShortcut& rhs)
    {
        return lhs.valid && rhs.valid && lhs.keyCode == rhs.keyCode &&
               lhs.modifiers == rhs.modifiers;
    }

    void TrimTrailingInvalidSecondaryShortcuts(
        std::vector<EditorShortcut>& shortcuts)
    {
        while (!shortcuts.empty() && !shortcuts.back().valid)
        {
            shortcuts.pop_back();
        }
    }

    std::string BuildStatusText(const char* prefix,
                                const std::string& commandId,
                                EditorShortcutBindingSlot slot,
                                const EditorShortcut& shortcut)
    {
        return std::string(prefix) + " " + commandId + " " +
               EditorShortcutBindingModel::FormatSlotLabel(slot) + " -> " +
               EditorShortcutBindingModel::FormatShortcut(shortcut) + ".";
    }

    std::string BuildClearStatusText(const char* prefix,
                                     const std::string& commandId,
                                     EditorShortcutBindingSlot slot)
    {
        return std::string(prefix) + " " + commandId + " " +
               EditorShortcutBindingModel::FormatSlotLabel(slot) + ".";
    }
} // namespace

void EditorShortcutBindingModel::Rebuild(
    const EditorCommandRegistry& registry)
{
    const std::vector<EditorCommandShortcutConflict> conflicts =
        registry.GetShortcutConflicts();

    m_rows.clear();
    m_rows.reserve(registry.GetCommandCount());
    for (const EditorCommand& command : registry.GetCommands())
    {
        EditorShortcutBindingRow row;
        row.commandId = command.desc.id;
        row.displayName = command.desc.displayName.empty()
                              ? command.desc.id
                              : command.desc.displayName;
        row.category = command.desc.category;
        row.hasPrimaryShortcut = command.desc.shortcut.valid;
        row.primaryShortcutText = FormatShortcut(command.desc.shortcut);
        row.secondaryShortcutCount =
            static_cast<uint32>(command.desc.secondaryShortcuts.size());
        for (uint32 index = 0;
             index < RVX_EDITOR_SHORTCUT_BINDING_SECONDARY_SLOT_COUNT;
             ++index)
        {
            const bool hasShortcut =
                index < command.desc.secondaryShortcuts.size() &&
                command.desc.secondaryShortcuts[index].valid;
            row.hasSecondaryShortcuts[index] = hasShortcut;
            row.secondaryShortcutTexts[index] =
                hasShortcut ? FormatShortcut(command.desc.secondaryShortcuts[index])
                            : FormatShortcut({});
            row.isSecondaryCaptureTarget[index] =
                IsCapturing() && row.commandId == m_captureCommandId &&
                !m_captureSlot.IsPrimary() &&
                m_captureSlot.secondaryIndex == index;
        }
        row.conflictCount = CountConflictsForCommand(conflicts, row.commandId);
        row.isCaptureTarget = IsCapturing() && row.commandId == m_captureCommandId &&
                              m_captureSlot.IsPrimary();
        row.isPrimaryCaptureTarget = row.isCaptureTarget;
        m_rows.push_back(std::move(row));
    }
}

bool EditorShortcutBindingModel::BeginCapture(
    const std::string& commandId,
    EditorShortcutBindingSlot slot)
{
    if (commandId.empty())
    {
        return false;
    }

    m_captureCommandId = commandId;
    m_captureSlot = slot;
    m_lastApplyResult = {};
    m_lastApplyResult.status = EditorShortcutBindingApplyStatus::WaitingForKey;
    m_lastApplyResult.commandId = commandId;
    m_lastApplyResult.slot = slot;
    m_lastApplyResult.statusText =
        "Press a key for " + commandId + " " + FormatSlotLabel(slot) +
        ". Escape cancels.";
    return true;
}

bool EditorShortcutBindingModel::BeginPrimaryCapture(
    const std::string& commandId)
{
    return BeginCapture(commandId, EditorShortcutBindingSlot::Primary());
}

bool EditorShortcutBindingModel::BeginSecondaryCapture(
    const std::string& commandId,
    uint32 secondaryIndex)
{
    return BeginCapture(commandId,
                        EditorShortcutBindingSlot::Secondary(secondaryIndex));
}

void EditorShortcutBindingModel::CancelCapture()
{
    if (m_captureCommandId.empty())
    {
        return;
    }

    m_lastApplyResult = {};
    m_lastApplyResult.status = EditorShortcutBindingApplyStatus::Cancelled;
    m_lastApplyResult.commandId = m_captureCommandId;
    m_lastApplyResult.slot = m_captureSlot;
    m_lastApplyResult.statusText =
        "Shortcut capture cancelled for " + m_captureCommandId + " " +
        FormatSlotLabel(m_captureSlot) + ".";
    m_captureCommandId.clear();
}

EditorShortcutBindingApplyResult EditorShortcutBindingModel::CaptureFromInput(
    const UI::UIInputState& input,
    EditorCommandRegistry& registry)
{
    if (!IsCapturing())
    {
        m_lastApplyResult = {};
        return m_lastApplyResult;
    }

    if (input.WasKeyPressed(UI::RVX_UI_KEY_ESCAPE))
    {
        CancelCapture();
        return m_lastApplyResult;
    }

    const uint32 keyCode = FindPressedKey(input);
    if (keyCode == 0u)
    {
        m_lastApplyResult.status =
            EditorShortcutBindingApplyStatus::WaitingForKey;
        m_lastApplyResult.commandId = m_captureCommandId;
        m_lastApplyResult.slot = m_captureSlot;
        m_lastApplyResult.statusText =
            "Press a key for " + m_captureCommandId + " " +
            FormatSlotLabel(m_captureSlot) + ". Escape cancels.";
        return m_lastApplyResult;
    }

    const std::string commandId = m_captureCommandId;
    const EditorShortcutBindingSlot slot = m_captureSlot;
    EditorCommand* command = registry.FindCommand(commandId);
    if (!command)
    {
        m_captureCommandId.clear();
        m_lastApplyResult = {};
        m_lastApplyResult.status =
            EditorShortcutBindingApplyStatus::CommandNotFound;
        m_lastApplyResult.commandId = commandId;
        m_lastApplyResult.slot = slot;
        m_lastApplyResult.statusText =
            "Shortcut capture failed: command not found.";
        return m_lastApplyResult;
    }

    const EditorShortcut previousPrimary = command->desc.shortcut;
    std::vector<EditorShortcut> previousSecondary =
        command->desc.secondaryShortcuts;
    const uint32 previousScopeMask = command->desc.shortcutScopeMask;

    EditorShortcut shortcut =
        EditorShortcut::Key(keyCode, input.current.modifiers);
    EditorShortcut primaryShortcut = command->desc.shortcut;
    std::vector<EditorShortcut> secondaryShortcuts =
        command->desc.secondaryShortcuts;
    if (slot.IsPrimary())
    {
        primaryShortcut = shortcut;
    }
    else
    {
        if (secondaryShortcuts.size() <= slot.secondaryIndex)
        {
            secondaryShortcuts.resize(static_cast<size_t>(slot.secondaryIndex) +
                                      1u);
        }
        secondaryShortcuts[slot.secondaryIndex] = shortcut;
    }
    TrimTrailingInvalidSecondaryShortcuts(secondaryShortcuts);

    registry.SetCommandShortcuts(commandId,
                                 primaryShortcut,
                                 secondaryShortcuts,
                                 command->desc.shortcutScopeMask);

    const std::vector<EditorCommandShortcutConflict> conflicts =
        registry.GetShortcutConflicts();
    const uint32 commandConflictCount =
        CountConflictsForCommand(conflicts, commandId);
    const bool hasLocalDuplicate =
        HasCommandLocalShortcutDuplicate(primaryShortcut, secondaryShortcuts);
    if (commandConflictCount > 0u || hasLocalDuplicate)
    {
        registry.SetCommandShortcuts(commandId,
                                     previousPrimary,
                                     std::move(previousSecondary),
                                     previousScopeMask);
        m_captureCommandId.clear();
        m_lastApplyResult = {};
        m_lastApplyResult.status = EditorShortcutBindingApplyStatus::Conflict;
        m_lastApplyResult.commandId = commandId;
        m_lastApplyResult.slot = slot;
        m_lastApplyResult.shortcut = shortcut;
        m_lastApplyResult.conflictCount =
            commandConflictCount + (hasLocalDuplicate ? 1u : 0u);
        m_lastApplyResult.statusText =
            BuildStatusText("Shortcut conflict rejected for",
                            commandId,
                            slot,
                            shortcut);
        return m_lastApplyResult;
    }

    m_captureCommandId.clear();
    m_lastApplyResult = {};
    m_lastApplyResult.status = EditorShortcutBindingApplyStatus::Applied;
    m_lastApplyResult.commandId = commandId;
    m_lastApplyResult.slot = slot;
    m_lastApplyResult.shortcut = shortcut;
    m_lastApplyResult.statusText =
        BuildStatusText("Shortcut applied for", commandId, slot, shortcut);
    return m_lastApplyResult;
}

EditorShortcutBindingApplyResult EditorShortcutBindingModel::ClearShortcut(
    const std::string& commandId,
    EditorShortcutBindingSlot slot,
    EditorCommandRegistry& registry)
{
    m_captureCommandId.clear();
    m_lastApplyResult = {};
    m_lastApplyResult.commandId = commandId;
    m_lastApplyResult.slot = slot;

    EditorCommand* command = registry.FindCommand(commandId);
    if (!command)
    {
        m_lastApplyResult.status =
            EditorShortcutBindingApplyStatus::CommandNotFound;
        m_lastApplyResult.statusText =
            "Shortcut clear failed: command not found.";
        return m_lastApplyResult;
    }

    EditorShortcut primaryShortcut = command->desc.shortcut;
    std::vector<EditorShortcut> secondaryShortcuts =
        command->desc.secondaryShortcuts;
    if (slot.IsPrimary())
    {
        primaryShortcut = {};
    }
    else if (slot.secondaryIndex < secondaryShortcuts.size())
    {
        secondaryShortcuts[slot.secondaryIndex] = {};
        TrimTrailingInvalidSecondaryShortcuts(secondaryShortcuts);
    }

    registry.SetCommandShortcuts(commandId,
                                 primaryShortcut,
                                 std::move(secondaryShortcuts),
                                 command->desc.shortcutScopeMask);

    m_lastApplyResult.status = EditorShortcutBindingApplyStatus::Cleared;
    m_lastApplyResult.statusText =
        BuildClearStatusText("Shortcut cleared for", commandId, slot);
    return m_lastApplyResult;
}

EditorShortcutBindingApplyResult
EditorShortcutBindingModel::ClearPrimaryShortcut(
    const std::string& commandId,
    EditorCommandRegistry& registry)
{
    return ClearShortcut(commandId,
                         EditorShortcutBindingSlot::Primary(),
                         registry);
}

EditorShortcutBindingApplyResult
EditorShortcutBindingModel::ClearSecondaryShortcut(
    const std::string& commandId,
    uint32 secondaryIndex,
    EditorCommandRegistry& registry)
{
    return ClearShortcut(commandId,
                         EditorShortcutBindingSlot::Secondary(secondaryIndex),
                         registry);
}

uint32 EditorShortcutBindingModel::GetConflictRowCount() const
{
    uint32 count = 0;
    for (const EditorShortcutBindingRow& row : m_rows)
    {
        if (row.conflictCount > 0u)
        {
            ++count;
        }
    }
    return count;
}

std::string EditorShortcutBindingModel::FormatShortcut(
    const EditorShortcut& shortcut)
{
    if (!shortcut.valid)
    {
        return "Unassigned";
    }

    std::string result;
    if ((shortcut.modifiers & UI::ToMask(UI::UIInputModifier::Ctrl)) != 0u)
    {
        result += "Ctrl+";
    }
    if ((shortcut.modifiers & UI::ToMask(UI::UIInputModifier::Shift)) != 0u)
    {
        result += "Shift+";
    }
    if ((shortcut.modifiers & UI::ToMask(UI::UIInputModifier::Alt)) != 0u)
    {
        result += "Alt+";
    }
    if ((shortcut.modifiers & UI::ToMask(UI::UIInputModifier::Super)) != 0u)
    {
        result += "Super+";
    }
    result += KeyName(shortcut.keyCode);
    return result;
}

std::string EditorShortcutBindingModel::FormatSlotLabel(
    EditorShortcutBindingSlot slot)
{
    if (slot.IsPrimary())
    {
        return "primary";
    }

    return "secondary " + std::to_string(slot.secondaryIndex + 1u);
}

std::string EditorShortcutBindingModel::FormatCommandWidgetToken(
    const std::string& commandId)
{
    std::string token;
    token.reserve(commandId.size());
    for (const unsigned char character : commandId)
    {
        if (std::isalnum(character) != 0 || character == '_' ||
            character == '-' || character == '.')
        {
            token.push_back(static_cast<char>(character));
            continue;
        }
        token.push_back('_');
    }
    return token;
}

uint32 EditorShortcutBindingModel::FindPressedKey(
    const UI::UIInputState& input)
{
    for (uint32 keyCode = 1u; keyCode < UI::RVX_UI_MAX_KEY_COUNT; ++keyCode)
    {
        if (input.WasKeyPressed(keyCode))
        {
            return keyCode;
        }
    }
    return 0u;
}

uint32 EditorShortcutBindingModel::CountConflictsForCommand(
    const std::vector<EditorCommandShortcutConflict>& conflicts,
    const std::string& commandId)
{
    uint32 count = 0;
    for (const EditorCommandShortcutConflict& conflict : conflicts)
    {
        if (conflict.firstCommandId == commandId ||
            conflict.secondCommandId == commandId)
        {
            ++count;
        }
    }
    return count;
}

bool EditorShortcutBindingModel::HasCommandLocalShortcutDuplicate(
    const EditorShortcut& primaryShortcut,
    const std::vector<EditorShortcut>& secondaryShortcuts)
{
    for (const EditorShortcut& secondaryShortcut : secondaryShortcuts)
    {
        if (ShortcutsEqual(primaryShortcut, secondaryShortcut))
        {
            return true;
        }
    }

    for (size_t lhs = 0; lhs < secondaryShortcuts.size(); ++lhs)
    {
        for (size_t rhs = lhs + 1; rhs < secondaryShortcuts.size(); ++rhs)
        {
            if (ShortcutsEqual(secondaryShortcuts[lhs],
                               secondaryShortcuts[rhs]))
            {
                return true;
            }
        }
    }

    return false;
}

} // namespace RVX::Editor
