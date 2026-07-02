/**
 * @file EditorShortcutBindingModel.h
 * @brief Native editor shortcut binding edit model.
 */

#pragma once

#include "Core/Types.h"
#include "Editor/UI/EditorCommandRegistry.h"

#include <array>
#include <string>
#include <vector>

namespace RVX::Editor
{

inline constexpr uint32 RVX_EDITOR_SHORTCUT_BINDING_SECONDARY_SLOT_COUNT = 2u;

enum class EditorShortcutBindingApplyStatus : uint8
{
    None = 0,
    WaitingForKey,
    Applied,
    Cleared,
    Cancelled,
    CommandNotFound,
    Conflict
};

enum class EditorShortcutBindingSlotType : uint8
{
    Primary = 0,
    Secondary
};

struct EditorShortcutBindingSlot
{
    EditorShortcutBindingSlotType type =
        EditorShortcutBindingSlotType::Primary;
    uint32 secondaryIndex = 0;

    static EditorShortcutBindingSlot Primary()
    {
        return {};
    }

    static EditorShortcutBindingSlot Secondary(uint32 index)
    {
        EditorShortcutBindingSlot slot;
        slot.type = EditorShortcutBindingSlotType::Secondary;
        slot.secondaryIndex = index;
        return slot;
    }

    bool IsPrimary() const
    {
        return type == EditorShortcutBindingSlotType::Primary;
    }
};

struct EditorShortcutBindingRow
{
    std::string commandId;
    std::string displayName;
    std::string category;
    std::string primaryShortcutText;
    std::array<std::string,
               RVX_EDITOR_SHORTCUT_BINDING_SECONDARY_SLOT_COUNT>
        secondaryShortcutTexts;
    std::array<bool, RVX_EDITOR_SHORTCUT_BINDING_SECONDARY_SLOT_COUNT>
        hasSecondaryShortcuts{};
    std::array<bool, RVX_EDITOR_SHORTCUT_BINDING_SECONDARY_SLOT_COUNT>
        isSecondaryCaptureTarget{};
    uint32 secondaryShortcutCount = 0;
    uint32 conflictCount = 0;
    bool hasPrimaryShortcut = false;
    bool isCaptureTarget = false;
    bool isPrimaryCaptureTarget = false;
};

struct EditorShortcutBindingApplyResult
{
    EditorShortcutBindingApplyStatus status =
        EditorShortcutBindingApplyStatus::None;
    std::string commandId;
    EditorShortcutBindingSlot slot;
    EditorShortcut shortcut;
    uint32 conflictCount = 0;
    std::string statusText;

    bool Succeeded() const
    {
        return status == EditorShortcutBindingApplyStatus::Applied;
    }

    bool Cleared() const
    {
        return status == EditorShortcutBindingApplyStatus::Cleared;
    }
};

class EditorShortcutBindingModel
{
public:
    void Rebuild(const EditorCommandRegistry& registry);

    bool BeginCapture(const std::string& commandId,
                      EditorShortcutBindingSlot slot);
    bool BeginPrimaryCapture(const std::string& commandId);
    bool BeginSecondaryCapture(const std::string& commandId,
                               uint32 secondaryIndex);
    void CancelCapture();

    EditorShortcutBindingApplyResult CaptureFromInput(
        const UI::UIInputState& input,
        EditorCommandRegistry& registry);
    EditorShortcutBindingApplyResult ClearShortcut(
        const std::string& commandId,
        EditorShortcutBindingSlot slot,
        EditorCommandRegistry& registry);
    EditorShortcutBindingApplyResult ClearPrimaryShortcut(
        const std::string& commandId,
        EditorCommandRegistry& registry);
    EditorShortcutBindingApplyResult ClearSecondaryShortcut(
        const std::string& commandId,
        uint32 secondaryIndex,
        EditorCommandRegistry& registry);

    bool IsCapturing() const { return !m_captureCommandId.empty(); }
    const std::string& GetCaptureCommandId() const
    {
        return m_captureCommandId;
    }
    EditorShortcutBindingSlot GetCaptureSlot() const { return m_captureSlot; }

    const std::vector<EditorShortcutBindingRow>& GetRows() const
    {
        return m_rows;
    }

    const EditorShortcutBindingApplyResult& GetLastApplyResult() const
    {
        return m_lastApplyResult;
    }

    uint32 GetConflictRowCount() const;

    static std::string FormatShortcut(const EditorShortcut& shortcut);
    static std::string FormatSlotLabel(EditorShortcutBindingSlot slot);
    static std::string FormatCommandWidgetToken(const std::string& commandId);

private:
    static uint32 FindPressedKey(const UI::UIInputState& input);
    static uint32 CountConflictsForCommand(
        const std::vector<EditorCommandShortcutConflict>& conflicts,
        const std::string& commandId);
    static bool HasCommandLocalShortcutDuplicate(
        const EditorShortcut& primaryShortcut,
        const std::vector<EditorShortcut>& secondaryShortcuts);

    std::vector<EditorShortcutBindingRow> m_rows;
    std::string m_captureCommandId;
    EditorShortcutBindingSlot m_captureSlot;
    EditorShortcutBindingApplyResult m_lastApplyResult;
};

} // namespace RVX::Editor
