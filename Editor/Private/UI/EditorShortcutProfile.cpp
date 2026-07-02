/**
 * @file EditorShortcutProfile.cpp
 * @brief Persistent editor shortcut profile import/export model implementation
 */

#include "Editor/UI/EditorShortcutProfile.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>
#include <utility>

namespace RVX::Editor
{
namespace
{
    constexpr const char* RVX_EDITOR_SHORTCUT_PROFILE_MAGIC =
        "RenderVerseXEditorShortcutProfile";
    constexpr uint32 RVX_EDITOR_SHORTCUT_PROFILE_VERSION = 1;
    constexpr uint32 RVX_EDITOR_SHORTCUT_PROFILE_MAX_SECONDARY_SHORTCUTS = 16;

    void SetError(std::string* error, const std::string& message)
    {
        if (error)
        {
            *error = message;
        }
    }

    uint32 SanitizeShortcutScopeMask(uint32 scopeMask)
    {
        const uint32 sanitized = scopeMask & RVX_EDITOR_COMMAND_SCOPE_ALL_MASK;
        return sanitized != 0u ? sanitized : RVX_EDITOR_COMMAND_SCOPE_GLOBAL_MASK;
    }

    char ToHex(uint8 value)
    {
        return static_cast<char>(value < 10u ? ('0' + value)
                                             : ('A' + (value - 10u)));
    }

    int32 FromHex(char value)
    {
        if (value >= '0' && value <= '9')
        {
            return value - '0';
        }
        if (value >= 'a' && value <= 'f')
        {
            return value - 'a' + 10;
        }
        if (value >= 'A' && value <= 'F')
        {
            return value - 'A' + 10;
        }

        return -1;
    }

    std::string EscapeToken(const std::string& value)
    {
        std::string result;
        result.reserve(value.size());
        for (const unsigned char character : value)
        {
            if (std::isalnum(character) != 0 || character == '_' ||
                character == '-' || character == '.')
            {
                result.push_back(static_cast<char>(character));
                continue;
            }

            result.push_back('%');
            result.push_back(ToHex(static_cast<uint8>((character >> 4u) & 0x0Fu)));
            result.push_back(ToHex(static_cast<uint8>(character & 0x0Fu)));
        }

        return result;
    }

    bool UnescapeToken(const std::string& token, std::string& value)
    {
        value.clear();
        value.reserve(token.size());
        for (size_t index = 0; index < token.size(); ++index)
        {
            const char character = token[index];
            if (character != '%')
            {
                value.push_back(character);
                continue;
            }

            if (index + 2u >= token.size())
            {
                return false;
            }

            const int32 high = FromHex(token[index + 1u]);
            const int32 low = FromHex(token[index + 2u]);
            if (high < 0 || low < 0)
            {
                return false;
            }

            value.push_back(static_cast<char>((high << 4) | low));
            index += 2u;
        }

        return true;
    }

    bool IsBlankOrComment(const std::string& line)
    {
        for (const char character : line)
        {
            if (std::isspace(static_cast<unsigned char>(character)) != 0)
            {
                continue;
            }

            return character == '#';
        }

        return true;
    }

    bool TryParseUint32Token(const std::string& token, uint32& value)
    {
        if (token.empty() || token[0] == '-')
        {
            return false;
        }

        uint64 parsedValue = 0;
        std::istringstream tokenStream(token);
        std::string invalidToken;
        if (!(tokenStream >> parsedValue) || (tokenStream >> invalidToken) ||
            parsedValue > std::numeric_limits<uint32>::max())
        {
            return false;
        }

        value = static_cast<uint32>(parsedValue);
        return true;
    }

    bool ReadUint32(std::istringstream& stream, uint32& value)
    {
        std::string token;
        if (!(stream >> token))
        {
            return false;
        }

        return TryParseUint32Token(token, value);
    }

    void WriteShortcut(std::ostringstream& stream, const EditorShortcut& shortcut)
    {
        stream << shortcut.keyCode << " "
               << shortcut.modifiers << " "
               << (shortcut.valid ? 1u : 0u);
    }

    bool ReadShortcut(std::istringstream& stream, EditorShortcut& shortcut)
    {
        uint32 keyCode = 0;
        uint32 modifiers = 0;
        uint32 valid = 0;
        if (!ReadUint32(stream, keyCode) ||
            !ReadUint32(stream, modifiers) ||
            !ReadUint32(stream, valid) ||
            valid > 1u)
        {
            return false;
        }

        shortcut.keyCode = keyCode;
        shortcut.modifiers = modifiers;
        shortcut.valid = valid != 0u;
        return true;
    }
}

EditorShortcutProfile EditorShortcutProfile::CaptureFromRegistry(
    const EditorCommandRegistry& registry)
{
    EditorShortcutProfile profile;
    for (const EditorCommand& command : registry.GetCommands())
    {
        EditorShortcutProfileEntry entry;
        entry.commandId = command.desc.id;
        entry.shortcut = command.desc.shortcut;
        entry.secondaryShortcuts = command.desc.secondaryShortcuts;
        entry.shortcutScopeMask = command.desc.shortcutScopeMask;
        profile.AddOrReplaceEntry(std::move(entry));
    }
    return profile;
}

bool EditorShortcutProfile::AddOrReplaceEntry(EditorShortcutProfileEntry entry)
{
    if (entry.commandId.empty())
    {
        return false;
    }

    entry.shortcutScopeMask =
        SanitizeShortcutScopeMask(entry.shortcutScopeMask);
    if (EditorShortcutProfileEntry* existing = FindEntry(entry.commandId))
    {
        *existing = std::move(entry);
        return true;
    }

    m_entries.push_back(std::move(entry));
    return true;
}

const EditorShortcutProfileEntry* EditorShortcutProfile::FindEntry(
    const std::string& commandId) const
{
    const auto it = std::find_if(
        m_entries.begin(),
        m_entries.end(),
        [&commandId](const EditorShortcutProfileEntry& entry) {
            return entry.commandId == commandId;
        });
    return it == m_entries.end() ? nullptr : &(*it);
}

EditorShortcutProfileEntry* EditorShortcutProfile::FindEntry(
    const std::string& commandId)
{
    auto it = std::find_if(
        m_entries.begin(),
        m_entries.end(),
        [&commandId](const EditorShortcutProfileEntry& entry) {
            return entry.commandId == commandId;
        });
    return it == m_entries.end() ? nullptr : &(*it);
}

bool EditorShortcutProfile::ApplyToRegistry(
    EditorCommandRegistry& registry,
    EditorShortcutProfileApplyResult* result) const
{
    EditorShortcutProfileApplyResult localResult;
    for (const EditorShortcutProfileEntry& entry : m_entries)
    {
        if (registry.SetCommandShortcuts(entry.commandId,
                                         entry.shortcut,
                                         entry.secondaryShortcuts,
                                         entry.shortcutScopeMask))
        {
            ++localResult.appliedCount;
            continue;
        }

        ++localResult.missingCommandCount;
        localResult.missingCommandIds.push_back(entry.commandId);
    }

    if (result)
    {
        *result = std::move(localResult);
        return result->Succeeded();
    }

    return localResult.Succeeded();
}

std::string EditorShortcutProfileSerializer::Serialize(
    const EditorShortcutProfile& profile)
{
    std::ostringstream stream;
    stream << RVX_EDITOR_SHORTCUT_PROFILE_MAGIC << " "
           << RVX_EDITOR_SHORTCUT_PROFILE_VERSION << "\n";

    for (const EditorShortcutProfileEntry& entry : profile.GetEntries())
    {
        stream << "command "
               << EscapeToken(entry.commandId) << " "
               << SanitizeShortcutScopeMask(entry.shortcutScopeMask) << " ";
        WriteShortcut(stream, entry.shortcut);
        stream << " "
               << static_cast<uint32>(entry.secondaryShortcuts.size());
        for (const EditorShortcut& shortcut : entry.secondaryShortcuts)
        {
            stream << " ";
            WriteShortcut(stream, shortcut);
        }
        stream << "\n";
    }

    return stream.str();
}

bool EditorShortcutProfileSerializer::Deserialize(
    std::string_view text,
    EditorShortcutProfile& profile,
    std::string* error)
{
    EditorShortcutProfile parsedProfile;
    std::istringstream stream{std::string(text)};

    std::string magic;
    std::string versionToken;
    if (!(stream >> magic >> versionToken))
    {
        SetError(error, "Shortcut profile header is missing");
        return false;
    }

    uint32 version = 0;
    if (!TryParseUint32Token(versionToken, version))
    {
        SetError(error, "Shortcut profile version is invalid");
        return false;
    }

    if (magic != RVX_EDITOR_SHORTCUT_PROFILE_MAGIC)
    {
        SetError(error, "Shortcut profile magic is invalid");
        return false;
    }

    if (version != RVX_EDITOR_SHORTCUT_PROFILE_VERSION)
    {
        SetError(error, "Unsupported shortcut profile version");
        return false;
    }

    std::string line;
    std::getline(stream, line);
    uint32 lineNumber = 1;
    while (std::getline(stream, line))
    {
        ++lineNumber;
        if (IsBlankOrComment(line))
        {
            continue;
        }

        std::istringstream lineStream(line);
        std::string recordType;
        lineStream >> recordType;
        if (recordType != "command")
        {
            SetError(error,
                     "Unsupported shortcut profile record at line " +
                         std::to_string(lineNumber));
            return false;
        }

        std::string commandIdToken;
        if (!(lineStream >> commandIdToken))
        {
            SetError(error,
                     "Shortcut profile command id is missing at line " +
                         std::to_string(lineNumber));
            return false;
        }

        EditorShortcutProfileEntry entry;
        if (!UnescapeToken(commandIdToken, entry.commandId) ||
            entry.commandId.empty())
        {
            SetError(error,
                     "Shortcut profile command id is invalid at line " +
                         std::to_string(lineNumber));
            return false;
        }

        if (parsedProfile.FindEntry(entry.commandId))
        {
            SetError(error,
                     "Duplicate shortcut profile command id at line " +
                         std::to_string(lineNumber));
            return false;
        }

        uint32 shortcutScopeMask = 0;
        if (!ReadUint32(lineStream, shortcutScopeMask))
        {
            SetError(error,
                     "Shortcut profile scope mask is invalid at line " +
                         std::to_string(lineNumber));
            return false;
        }
        entry.shortcutScopeMask =
            SanitizeShortcutScopeMask(shortcutScopeMask);

        if (!ReadShortcut(lineStream, entry.shortcut))
        {
            SetError(error,
                     "Shortcut profile primary shortcut is invalid at line " +
                         std::to_string(lineNumber));
            return false;
        }

        uint32 secondaryCount = 0;
        if (!ReadUint32(lineStream, secondaryCount) ||
            secondaryCount > RVX_EDITOR_SHORTCUT_PROFILE_MAX_SECONDARY_SHORTCUTS)
        {
            SetError(error,
                     "Shortcut profile secondary shortcut count is invalid at line " +
                         std::to_string(lineNumber));
            return false;
        }

        entry.secondaryShortcuts.reserve(secondaryCount);
        for (uint32 index = 0; index < secondaryCount; ++index)
        {
            EditorShortcut shortcut;
            if (!ReadShortcut(lineStream, shortcut))
            {
                SetError(error,
                         "Shortcut profile secondary shortcut is invalid at line " +
                             std::to_string(lineNumber));
                return false;
            }
            entry.secondaryShortcuts.push_back(shortcut);
        }

        std::string trailingToken;
        if (lineStream >> trailingToken)
        {
            SetError(error,
                     "Shortcut profile line has trailing tokens at line " +
                         std::to_string(lineNumber));
            return false;
        }

        parsedProfile.AddOrReplaceEntry(std::move(entry));
    }

    profile = std::move(parsedProfile);
    if (error)
    {
        error->clear();
    }
    return true;
}

bool EditorShortcutProfileSerializer::SaveToFile(
    const EditorShortcutProfile& profile,
    const std::filesystem::path& path,
    std::string* error)
{
    if (path.empty())
    {
        SetError(error, "Shortcut profile path is empty");
        return false;
    }

    if (path.has_parent_path())
    {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec)
        {
            SetError(error,
                     "Could not create shortcut profile directory: " +
                         ec.message());
            return false;
        }
    }

    std::ofstream file(path, std::ios::binary);
    if (!file)
    {
        SetError(error, "Could not open shortcut profile file for writing");
        return false;
    }

    file << Serialize(profile);
    if (!file)
    {
        SetError(error, "Could not write shortcut profile file");
        return false;
    }

    if (error)
    {
        error->clear();
    }
    return true;
}

bool EditorShortcutProfileSerializer::LoadFromFile(
    const std::filesystem::path& path,
    EditorShortcutProfile& profile,
    std::string* error)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        SetError(error, "Could not open shortcut profile file for reading");
        return false;
    }

    std::ostringstream contents;
    contents << file.rdbuf();
    if (!file.good() && !file.eof())
    {
        SetError(error, "Could not read shortcut profile file");
        return false;
    }

    return Deserialize(contents.str(), profile, error);
}

} // namespace RVX::Editor
