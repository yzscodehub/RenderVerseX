/**
 * @file EditorSettings.cpp
 * @brief Persistent user editor settings implementation.
 */

#include "Editor/EditorSettings.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

namespace RVX::Editor
{
namespace
{
    constexpr uint32 RVX_EDITOR_SETTINGS_SCHEMA_VERSION = 1;
    constexpr uint32 RVX_EDITOR_MAX_RECENT_SCENE_DIRECTORIES_LIMIT = 32;
    constexpr float RVX_EDITOR_MIN_UI_SCALE_FACTOR = 0.75f;
    constexpr float RVX_EDITOR_MAX_UI_SCALE_FACTOR = 2.0f;
    constexpr float RVX_EDITOR_MIN_SHORTCUT_AUTOSAVE_DEBOUNCE_SECONDS = 0.0f;
    constexpr float RVX_EDITOR_MAX_SHORTCUT_AUTOSAVE_DEBOUNCE_SECONDS = 60.0f;

    float ClampUIScaleFactor(float scaleFactor)
    {
        if (!std::isfinite(scaleFactor))
        {
            return RVX_EDITOR_DEFAULT_UI_SCALE_FACTOR;
        }
        return std::clamp(scaleFactor,
                          RVX_EDITOR_MIN_UI_SCALE_FACTOR,
                          RVX_EDITOR_MAX_UI_SCALE_FACTOR);
    }

    std::string EscapeJsonString(const std::string& value)
    {
        std::ostringstream out;
        for (char ch : value)
        {
            switch (ch)
            {
                case '\\': out << "\\\\"; break;
                case '"': out << "\\\""; break;
                case '\n': out << "\\n"; break;
                case '\r': out << "\\r"; break;
                case '\t': out << "\\t"; break;
                default: out << ch; break;
            }
        }
        return out.str();
    }

    bool HasJsonField(const std::string& contents, const std::string& fieldName)
    {
        return contents.find("\"" + fieldName + "\"") != std::string::npos;
    }

    std::string GetEnvironmentValue(const char* name)
    {
#if defined(_WIN32)
        char* value = nullptr;
        size_t valueLength = 0;
        if (_dupenv_s(&value, &valueLength, name) != 0 || !value)
        {
            return {};
        }

        std::string result(value, valueLength > 0 ? valueLength - 1u : 0u);
        std::free(value);
        return result;
#else
        const char* value = std::getenv(name);
        return value ? std::string(value) : std::string{};
#endif
    }

    bool FindJsonArrayContents(const std::string& contents,
                               const std::string& fieldName,
                               std::string& outContents)
    {
        const std::string needle = "\"" + fieldName + "\"";
        const size_t namePos = contents.find(needle);
        if (namePos == std::string::npos)
        {
            return false;
        }

        const size_t colonPos = contents.find(':', namePos + needle.size());
        if (colonPos == std::string::npos)
        {
            return false;
        }

        const size_t openPos = contents.find('[', colonPos + 1);
        if (openPos == std::string::npos)
        {
            return false;
        }

        bool inString = false;
        bool escaped = false;
        uint32 depth = 0;
        const size_t payloadStart = openPos + 1;
        for (size_t i = openPos; i < contents.size(); ++i)
        {
            const char ch = contents[i];
            if (escaped)
            {
                escaped = false;
                continue;
            }

            if (inString && ch == '\\')
            {
                escaped = true;
                continue;
            }

            if (ch == '"')
            {
                inString = !inString;
                continue;
            }

            if (inString)
            {
                continue;
            }

            if (ch == '[')
            {
                ++depth;
                continue;
            }

            if (ch == ']')
            {
                if (depth == 0)
                {
                    return false;
                }

                --depth;
                if (depth == 0)
                {
                    outContents = contents.substr(payloadStart, i - payloadStart);
                    return true;
                }
            }
        }

        return false;
    }

    bool ParseJsonStringArray(const std::string& payload,
                              std::vector<std::string>& outValues)
    {
        size_t index = 0;
        while (index < payload.size())
        {
            while (index < payload.size() &&
                   (std::isspace(static_cast<unsigned char>(payload[index])) ||
                    payload[index] == ','))
            {
                ++index;
            }

            if (index >= payload.size())
            {
                break;
            }

            if (payload[index] != '"')
            {
                return false;
            }
            ++index;

            std::string value;
            bool closed = false;
            while (index < payload.size())
            {
                const char ch = payload[index++];
                if (ch == '"')
                {
                    closed = true;
                    break;
                }

                if (ch != '\\')
                {
                    value.push_back(ch);
                    continue;
                }

                if (index >= payload.size())
                {
                    return false;
                }

                const char escaped = payload[index++];
                switch (escaped)
                {
                    case '\\': value.push_back('\\'); break;
                    case '"': value.push_back('"'); break;
                    case 'n': value.push_back('\n'); break;
                    case 'r': value.push_back('\r'); break;
                    case 't': value.push_back('\t'); break;
                    default: value.push_back(escaped); break;
                }
            }

            if (!closed)
            {
                return false;
            }

            outValues.push_back(std::move(value));
        }

        return true;
    }

    bool FindJsonUnsigned(const std::string& contents,
                          const std::string& fieldName,
                          uint32& outValue)
    {
        const std::string needle = "\"" + fieldName + "\"";
        const size_t namePos = contents.find(needle);
        if (namePos == std::string::npos)
        {
            return false;
        }

        const size_t colonPos = contents.find(':', namePos + needle.size());
        if (colonPos == std::string::npos)
        {
            return false;
        }

        size_t index = colonPos + 1;
        while (index < contents.size() &&
               std::isspace(static_cast<unsigned char>(contents[index])))
        {
            ++index;
        }

        if (index >= contents.size() ||
            !std::isdigit(static_cast<unsigned char>(contents[index])))
        {
            return false;
        }

        uint64 value = 0;
        while (index < contents.size() &&
               std::isdigit(static_cast<unsigned char>(contents[index])))
        {
            value = value * 10u + static_cast<uint64>(contents[index] - '0');
            if (value > std::numeric_limits<uint32>::max())
            {
                return false;
            }
            ++index;
        }

        outValue = static_cast<uint32>(value);
        return true;
    }

    bool FindJsonBool(const std::string& contents,
                      const std::string& fieldName,
                      bool& outValue)
    {
        const std::string needle = "\"" + fieldName + "\"";
        const size_t namePos = contents.find(needle);
        if (namePos == std::string::npos)
        {
            return false;
        }

        const size_t colonPos = contents.find(':', namePos + needle.size());
        if (colonPos == std::string::npos)
        {
            return false;
        }

        size_t index = colonPos + 1;
        while (index < contents.size() &&
               std::isspace(static_cast<unsigned char>(contents[index])))
        {
            ++index;
        }

        if (contents.compare(index, 4, "true") == 0)
        {
            outValue = true;
            return true;
        }
        if (contents.compare(index, 5, "false") == 0)
        {
            outValue = false;
            return true;
        }

        return false;
    }

    bool FindJsonFloat(const std::string& contents,
                       const std::string& fieldName,
                       float& outValue)
    {
        const std::string needle = "\"" + fieldName + "\"";
        const size_t namePos = contents.find(needle);
        if (namePos == std::string::npos)
        {
            return false;
        }

        const size_t colonPos = contents.find(':', namePos + needle.size());
        if (colonPos == std::string::npos)
        {
            return false;
        }

        size_t index = colonPos + 1;
        while (index < contents.size() &&
               std::isspace(static_cast<unsigned char>(contents[index])))
        {
            ++index;
        }

        size_t end = index;
        while (end < contents.size())
        {
            const char ch = contents[end];
            if (!(std::isdigit(static_cast<unsigned char>(ch)) ||
                  ch == '+' || ch == '-' || ch == '.' ||
                  ch == 'e' || ch == 'E'))
            {
                break;
            }
            ++end;
        }
        if (end == index)
        {
            return false;
        }

        try
        {
            size_t consumed = 0;
            const std::string valueText = contents.substr(index, end - index);
            const float value = std::stof(valueText, &consumed);
            if (consumed != valueText.size())
            {
                return false;
            }
            outValue = value;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool FindJsonString(const std::string& contents,
                        const std::string& fieldName,
                        std::string& outValue)
    {
        const std::string needle = "\"" + fieldName + "\"";
        const size_t namePos = contents.find(needle);
        if (namePos == std::string::npos)
        {
            return false;
        }

        const size_t colonPos = contents.find(':', namePos + needle.size());
        if (colonPos == std::string::npos)
        {
            return false;
        }

        size_t index = colonPos + 1;
        while (index < contents.size() &&
               std::isspace(static_cast<unsigned char>(contents[index])))
        {
            ++index;
        }

        if (index >= contents.size() || contents[index] != '"')
        {
            return false;
        }
        ++index;

        std::string value;
        while (index < contents.size())
        {
            const char ch = contents[index++];
            if (ch == '"')
            {
                outValue = std::move(value);
                return true;
            }

            if (ch != '\\')
            {
                value.push_back(ch);
                continue;
            }

            if (index >= contents.size())
            {
                return false;
            }

            const char escaped = contents[index++];
            switch (escaped)
            {
            case '\\': value.push_back('\\'); break;
            case '"': value.push_back('"'); break;
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            default: value.push_back(escaped); break;
            }
        }

        return false;
    }

    std::filesystem::path NormalizeExistingDirectory(
        const std::filesystem::path& directory)
    {
        std::error_code ec;
        std::filesystem::path normalized =
            std::filesystem::weakly_canonical(directory, ec);
        if (ec)
        {
            ec.clear();
            normalized = std::filesystem::absolute(directory, ec);
            if (ec)
            {
                normalized = directory;
            }
        }
        return normalized.lexically_normal();
    }

    bool AddRecentDirectoryToSettings(EditorSettings& settings,
                                      const std::filesystem::path& directory,
                                      std::string* error)
    {
        if (directory.empty())
        {
            if (error)
            {
                *error = "Recent scene directory is empty";
            }
            return false;
        }

        if (settings.filePicker.maxRecentSceneDirectories == 0u)
        {
            settings.filePicker.recentSceneDirectories.clear();
            if (error)
            {
                error->clear();
            }
            return false;
        }

        std::error_code ec;
        if (!std::filesystem::is_directory(directory, ec) || ec)
        {
            if (error)
            {
                *error = "Recent scene directory does not exist: " +
                         directory.string();
            }
            return false;
        }

        const std::filesystem::path normalizedDirectory =
            NormalizeExistingDirectory(directory);
        std::vector<std::filesystem::path>& directories =
            settings.filePicker.recentSceneDirectories;
        directories.erase(
            std::remove(directories.begin(),
                        directories.end(),
                        normalizedDirectory),
            directories.end());
        directories.insert(directories.begin(), normalizedDirectory);

        const size_t maxDirectoryCount =
            static_cast<size_t>(settings.filePicker.maxRecentSceneDirectories);
        if (directories.size() > maxDirectoryCount)
        {
            directories.resize(maxDirectoryCount);
        }

        if (error)
        {
            error->clear();
        }
        return true;
    }

    bool ParseSettings(const std::string& contents, EditorSettings& outSettings)
    {
        EditorSettings parsed;
        uint32 schemaVersion = 0;
        if (FindJsonUnsigned(contents, "schemaVersion", schemaVersion))
        {
            parsed.schemaVersion = schemaVersion;
        }

        uint32 maxRecentSceneDirectories = 0;
        if (FindJsonUnsigned(contents,
                             "maxRecentSceneDirectories",
                             maxRecentSceneDirectories))
        {
            parsed.filePicker.maxRecentSceneDirectories =
                std::min(maxRecentSceneDirectories,
                         RVX_EDITOR_MAX_RECENT_SCENE_DIRECTORIES_LIMIT);
        }

        if (HasJsonField(contents, "backendType"))
        {
            std::string backendTypeValue;
            if (!FindJsonString(contents, "backendType", backendTypeValue))
            {
                return false;
            }

            EditorUIBackendType backendType = GetDefaultEditorUIBackendType();
            if (!TryParseEditorUIBackendType(backendTypeValue, backendType))
            {
                return false;
            }
            parsed.ui.backendType = backendType;
        }

        if (HasJsonField(contents, "scaleFactor"))
        {
            float scaleFactor = RVX_EDITOR_DEFAULT_UI_SCALE_FACTOR;
            if (!FindJsonFloat(contents, "scaleFactor", scaleFactor))
            {
                return false;
            }
            parsed.ui.scaleFactor = ClampUIScaleFactor(scaleFactor);
        }

        if (HasJsonField(contents, "appendDefaultSystemFonts"))
        {
            bool appendDefaultSystemFonts = true;
            if (!FindJsonBool(contents,
                              "appendDefaultSystemFonts",
                              appendDefaultSystemFonts))
            {
                return false;
            }
            parsed.ui.appendDefaultSystemFonts = appendDefaultSystemFonts;
        }

        if (HasJsonField(contents, "fontPaths"))
        {
            std::string arrayContents;
            if (!FindJsonArrayContents(contents, "fontPaths", arrayContents))
            {
                return false;
            }

            std::vector<std::string> fontPaths;
            if (!ParseJsonStringArray(arrayContents, fontPaths))
            {
                return false;
            }

            parsed.ui.fontPaths.clear();
            for (const std::string& fontPath : fontPaths)
            {
                if (!fontPath.empty())
                {
                    parsed.ui.fontPaths.push_back(
                        std::filesystem::path(fontPath).lexically_normal());
                }
            }
        }

        if (HasJsonField(contents, "profileAutosaveEnabled"))
        {
            bool autosaveEnabled = false;
            if (!FindJsonBool(contents,
                              "profileAutosaveEnabled",
                              autosaveEnabled))
            {
                return false;
            }
            parsed.shortcuts.autosaveEnabled = autosaveEnabled;
        }

        if (HasJsonField(contents, "profileAutosaveDebounceSeconds"))
        {
            float debounceSeconds = 0.0f;
            if (!FindJsonFloat(contents,
                               "profileAutosaveDebounceSeconds",
                               debounceSeconds))
            {
                return false;
            }
            parsed.shortcuts.autosaveDebounceSeconds =
                std::clamp(debounceSeconds,
                           RVX_EDITOR_MIN_SHORTCUT_AUTOSAVE_DEBOUNCE_SECONDS,
                           RVX_EDITOR_MAX_SHORTCUT_AUTOSAVE_DEBOUNCE_SECONDS);
        }

        if (HasJsonField(contents, "recentSceneDirectories"))
        {
            std::string arrayContents;
            if (!FindJsonArrayContents(contents,
                                       "recentSceneDirectories",
                                       arrayContents))
            {
                return false;
            }

            std::vector<std::string> directories;
            if (!ParseJsonStringArray(arrayContents, directories))
            {
                return false;
            }

            parsed.filePicker.recentSceneDirectories.clear();
            for (auto it = directories.rbegin(); it != directories.rend(); ++it)
            {
                AddRecentDirectoryToSettings(parsed,
                                             std::filesystem::path(*it),
                                             nullptr);
            }
        }

        outSettings = std::move(parsed);
        return true;
    }
} // namespace

EditorSettingsService::EditorSettingsService()
    : m_settingsPath(ResolveDefaultSettingsPath())
{
}

EditorSettingsService::EditorSettingsService(std::filesystem::path settingsPath)
    : m_settingsPath(std::move(settingsPath))
{
}

void EditorSettingsService::SetSettingsPath(std::filesystem::path settingsPath)
{
    m_settingsPath = std::move(settingsPath);
}

bool EditorSettingsService::Load()
{
    ClearError();
    if (m_settingsPath.empty())
    {
        return Fail("Editor settings path is empty");
    }

    std::error_code ec;
    if (!std::filesystem::exists(m_settingsPath, ec))
    {
        if (ec)
        {
            return Fail("Could not inspect editor settings path: " + ec.message());
        }
        m_settings = EditorSettings{};
        return true;
    }

    std::ifstream file(m_settingsPath, std::ios::binary);
    if (!file)
    {
        return Fail("Could not open editor settings file: " +
                    m_settingsPath.string());
    }

    std::ostringstream contents;
    contents << file.rdbuf();

    EditorSettings loaded;
    if (!ParseSettings(contents.str(), loaded))
    {
        return Fail("Could not parse editor settings file: " +
                    m_settingsPath.string());
    }

    m_settings = std::move(loaded);
    return true;
}

bool EditorSettingsService::Save() const
{
    ClearError();
    if (m_settingsPath.empty())
    {
        return Fail("Editor settings path is empty");
    }

    if (m_settingsPath.has_parent_path())
    {
        std::error_code ec;
        std::filesystem::create_directories(m_settingsPath.parent_path(), ec);
        if (ec)
        {
            return Fail("Could not create editor settings directory: " +
                        ec.message());
        }
    }

    std::ofstream file(m_settingsPath, std::ios::binary);
    if (!file)
    {
        return Fail("Could not write editor settings file: " +
                    m_settingsPath.string());
    }

    file << "{\n";
    file << "  \"schemaVersion\": " << RVX_EDITOR_SETTINGS_SCHEMA_VERSION << ",\n";
    file << "  \"ui\": {\n";
    file << "    \"backendType\": \""
         << ToConfigString(m_settings.ui.backendType) << "\",\n";
    file << "    \"scaleFactor\": " << m_settings.ui.scaleFactor << ",\n";
    file << "    \"appendDefaultSystemFonts\": "
         << (m_settings.ui.appendDefaultSystemFonts ? "true" : "false")
         << ",\n";
    file << "    \"fontPaths\": [\n";

    const std::vector<std::filesystem::path>& fontPaths =
        m_settings.ui.fontPaths;
    for (size_t index = 0; index < fontPaths.size(); ++index)
    {
        file << "      \"" << EscapeJsonString(fontPaths[index].string())
             << "\"";
        if (index + 1u < fontPaths.size())
        {
            file << ",";
        }
        file << "\n";
    }

    file << "    ]\n";
    file << "  },\n";
    file << "  \"filePicker\": {\n";
    file << "    \"maxRecentSceneDirectories\": "
         << m_settings.filePicker.maxRecentSceneDirectories << ",\n";
    file << "    \"recentSceneDirectories\": [\n";

    const std::vector<std::filesystem::path>& directories =
        m_settings.filePicker.recentSceneDirectories;
    for (size_t index = 0; index < directories.size(); ++index)
    {
        file << "      \"" << EscapeJsonString(directories[index].string())
             << "\"";
        if (index + 1u < directories.size())
        {
            file << ",";
        }
        file << "\n";
    }

    file << "    ]\n";
    file << "  },\n";
    file << "  \"shortcuts\": {\n";
    file << "    \"profileAutosaveEnabled\": "
         << (m_settings.shortcuts.autosaveEnabled ? "true" : "false") << ",\n";
    file << "    \"profileAutosaveDebounceSeconds\": "
         << m_settings.shortcuts.autosaveDebounceSeconds << "\n";
    file << "  }\n";
    file << "}\n";

    if (!file)
    {
        return Fail("Could not finish writing editor settings file: " +
                    m_settingsPath.string());
    }

    return true;
}

void EditorSettingsService::SetUIBackendType(EditorUIBackendType type)
{
    m_settings.ui.backendType = type;
}

void EditorSettingsService::SetUIScaleFactor(float scaleFactor)
{
    m_settings.ui.scaleFactor = ClampUIScaleFactor(scaleFactor);
}

void EditorSettingsService::SetUIFontPaths(
    std::vector<std::filesystem::path> fontPaths)
{
    m_settings.ui.fontPaths.clear();
    for (std::filesystem::path& fontPath : fontPaths)
    {
        if (!fontPath.empty())
        {
            m_settings.ui.fontPaths.push_back(fontPath.lexically_normal());
        }
    }
}

void EditorSettingsService::SetAppendDefaultSystemFonts(
    bool appendDefaultSystemFonts)
{
    m_settings.ui.appendDefaultSystemFonts = appendDefaultSystemFonts;
}

void EditorSettingsService::SetMaxRecentSceneDirectories(uint32 maxDirectories)
{
    m_settings.filePicker.maxRecentSceneDirectories =
        std::min(maxDirectories, RVX_EDITOR_MAX_RECENT_SCENE_DIRECTORIES_LIMIT);
    std::vector<std::filesystem::path>& directories =
        m_settings.filePicker.recentSceneDirectories;
    if (directories.size() >
        static_cast<size_t>(m_settings.filePicker.maxRecentSceneDirectories))
    {
        directories.resize(
            static_cast<size_t>(m_settings.filePicker.maxRecentSceneDirectories));
    }
}

void EditorSettingsService::SetShortcutProfileAutosaveEnabled(bool enabled)
{
    m_settings.shortcuts.autosaveEnabled = enabled;
}

void EditorSettingsService::SetShortcutProfileAutosaveDebounceSeconds(
    float seconds)
{
    m_settings.shortcuts.autosaveDebounceSeconds =
        std::clamp(seconds,
                   RVX_EDITOR_MIN_SHORTCUT_AUTOSAVE_DEBOUNCE_SECONDS,
                   RVX_EDITOR_MAX_SHORTCUT_AUTOSAVE_DEBOUNCE_SECONDS);
}

bool EditorSettingsService::AddRecentSceneDirectory(
    const std::filesystem::path& directory)
{
    ClearError();
    return AddRecentDirectory(m_settings, directory, &m_lastError);
}

bool EditorSettingsService::AddRecentScenePath(
    const std::filesystem::path& scenePath)
{
    return AddRecentSceneDirectory(scenePath.parent_path());
}

std::filesystem::path EditorSettingsService::ResolveDefaultSettingsPath()
{
#if defined(_WIN32)
    const std::string appData = GetEnvironmentValue("APPDATA");
    if (!appData.empty())
    {
        return std::filesystem::path(appData) / "RenderVerseX" / "Editor" /
               "settings.json";
    }
#else
    const std::string xdgConfigHome = GetEnvironmentValue("XDG_CONFIG_HOME");
    if (!xdgConfigHome.empty())
    {
        return std::filesystem::path(xdgConfigHome) / "RenderVerseX" /
               "Editor" / "settings.json";
    }

    const std::string home = GetEnvironmentValue("HOME");
    if (!home.empty())
    {
        return std::filesystem::path(home) / ".config" / "RenderVerseX" /
               "Editor" / "settings.json";
    }
#endif

    return std::filesystem::current_path() / "RenderVerseX.Editor.settings.json";
}

bool EditorSettingsService::AddRecentDirectory(
    EditorSettings& settings,
    const std::filesystem::path& directory,
    std::string* error)
{
    return AddRecentDirectoryToSettings(settings, directory, error);
}

bool EditorSettingsService::Fail(std::string error) const
{
    m_lastError = std::move(error);
    return false;
}

void EditorSettingsService::ClearError() const
{
    m_lastError.clear();
}

} // namespace RVX::Editor
