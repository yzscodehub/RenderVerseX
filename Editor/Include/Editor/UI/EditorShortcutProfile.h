/**
 * @file EditorShortcutProfile.h
 * @brief Persistent editor shortcut profile import/export model
 */

#pragma once

#include "Editor/UI/EditorCommandRegistry.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace RVX::Editor
{

struct EditorShortcutProfileEntry
{
    std::string commandId;
    EditorShortcut shortcut;
    std::vector<EditorShortcut> secondaryShortcuts;
    uint32 shortcutScopeMask = RVX_EDITOR_COMMAND_SCOPE_GLOBAL_MASK;
};

struct EditorShortcutProfileApplyResult
{
    uint32 appliedCount = 0;
    uint32 missingCommandCount = 0;
    std::vector<std::string> missingCommandIds;

    bool Succeeded() const { return missingCommandCount == 0u; }
};

class EditorShortcutProfile
{
public:
    static EditorShortcutProfile CaptureFromRegistry(
        const EditorCommandRegistry& registry);

    bool AddOrReplaceEntry(EditorShortcutProfileEntry entry);
    void Clear() { m_entries.clear(); }

    const EditorShortcutProfileEntry* FindEntry(const std::string& commandId) const;
    EditorShortcutProfileEntry* FindEntry(const std::string& commandId);

    bool ApplyToRegistry(EditorCommandRegistry& registry,
                         EditorShortcutProfileApplyResult* result = nullptr) const;

    size_t GetEntryCount() const { return m_entries.size(); }
    const std::vector<EditorShortcutProfileEntry>& GetEntries() const
    {
        return m_entries;
    }

private:
    std::vector<EditorShortcutProfileEntry> m_entries;
};

class EditorShortcutProfileSerializer
{
public:
    static std::string Serialize(const EditorShortcutProfile& profile);
    static bool Deserialize(std::string_view text,
                            EditorShortcutProfile& profile,
                            std::string* error = nullptr);

    static bool SaveToFile(const EditorShortcutProfile& profile,
                           const std::filesystem::path& path,
                           std::string* error = nullptr);
    static bool LoadFromFile(const std::filesystem::path& path,
                             EditorShortcutProfile& profile,
                             std::string* error = nullptr);
};

} // namespace RVX::Editor
