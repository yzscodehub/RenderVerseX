/**
 * @file CVarSystem.cpp
 * @brief Configuration variable system implementation
 */

#include "Debug/CVarSystem.h"
#include "Debug/Console.h"
#include "Core/Log.h"
#include <fstream>
#include <sstream>
#include <algorithm>

namespace RVX
{
    const std::string CVarSystem::s_emptyString;

    // =========================================================================
    // Singleton
    // =========================================================================

    CVarSystem& CVarSystem::Get()
    {
        static CVarSystem instance;
        return instance;
    }

    // =========================================================================
    // Lifecycle
    // =========================================================================

    void CVarSystem::Initialize()
    {
        if (m_initialized)
        {
            return;
        }

        m_cvars.reserve(256);

        m_initialized = true;
        RVX_CORE_INFO("CVarSystem initialized");

        // Register console commands if console is available
        if (Console::Get().IsInitialized())
        {
            RegisterConsoleCommands();
        }
    }

    void CVarSystem::Shutdown()
    {
        if (!m_initialized)
        {
            return;
        }

        m_cvars.clear();
        m_nameToIndex.clear();

        m_initialized = false;
        RVX_CORE_INFO("CVarSystem shutdown");
    }

    // =========================================================================
    // Registration
    // =========================================================================

    CVarRef CVarSystem::RegisterBool(
        const char* name,
        bool defaultValue,
        const char* description,
        CVarFlags flags)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Check if already exists
        auto it = m_nameToIndex.find(name);
        if (it != m_nameToIndex.end())
        {
            RVX_CORE_WARN("CVar '{}' already registered", name);
            return CVarRef(it->second);
        }

        uint32 index = static_cast<uint32>(m_cvars.size());

        CVarDef def;
        def.name = name;
        def.description = description;
        def.category = ExtractCategory(name);
        def.type = CVarType::Bool;
        def.flags = flags;
        def.defaultValue = defaultValue;
        def.currentValue = defaultValue;

        m_cvars.push_back(std::move(def));
        m_nameToIndex[name] = index;

        return CVarRef(index);
    }

    CVarRef CVarSystem::RegisterInt(
        const char* name,
        int32 defaultValue,
        const char* description,
        int32 minValue,
        int32 maxValue,
        CVarFlags flags)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_nameToIndex.find(name);
        if (it != m_nameToIndex.end())
        {
            RVX_CORE_WARN("CVar '{}' already registered", name);
            return CVarRef(it->second);
        }

        uint32 index = static_cast<uint32>(m_cvars.size());

        CVarDef def;
        def.name = name;
        def.description = description;
        def.category = ExtractCategory(name);
        def.type = CVarType::Int;
        def.flags = flags;
        def.defaultValue = defaultValue;
        def.currentValue = defaultValue;
        def.minValue = minValue;
        def.maxValue = maxValue;

        m_cvars.push_back(std::move(def));
        m_nameToIndex[name] = index;

        return CVarRef(index);
    }

    CVarRef CVarSystem::RegisterFloat(
        const char* name,
        float32 defaultValue,
        const char* description,
        float32 minValue,
        float32 maxValue,
        CVarFlags flags)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_nameToIndex.find(name);
        if (it != m_nameToIndex.end())
        {
            RVX_CORE_WARN("CVar '{}' already registered", name);
            return CVarRef(it->second);
        }

        uint32 index = static_cast<uint32>(m_cvars.size());

        CVarDef def;
        def.name = name;
        def.description = description;
        def.category = ExtractCategory(name);
        def.type = CVarType::Float;
        def.flags = flags;
        def.defaultValue = defaultValue;
        def.currentValue = defaultValue;
        def.minValue = minValue;
        def.maxValue = maxValue;

        m_cvars.push_back(std::move(def));
        m_nameToIndex[name] = index;

        return CVarRef(index);
    }

    CVarRef CVarSystem::RegisterString(
        const char* name,
        const char* defaultValue,
        const char* description,
        CVarFlags flags)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_nameToIndex.find(name);
        if (it != m_nameToIndex.end())
        {
            RVX_CORE_WARN("CVar '{}' already registered", name);
            return CVarRef(it->second);
        }

        uint32 index = static_cast<uint32>(m_cvars.size());

        CVarDef def;
        def.name = name;
        def.description = description;
        def.category = ExtractCategory(name);
        def.type = CVarType::String;
        def.flags = flags;
        def.defaultValue = std::string(defaultValue);
        def.currentValue = std::string(defaultValue);

        m_cvars.push_back(std::move(def));
        m_nameToIndex[name] = index;

        return CVarRef(index);
    }

    void CVarSystem::Unregister(const char* name)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_nameToIndex.find(name);
        if (it != m_nameToIndex.end())
        {
            // Mark as invalid (we don't actually remove to keep indices stable)
            m_cvars[it->second].name.clear();
            m_nameToIndex.erase(it);
        }
    }

    // =========================================================================
    // Access
    // =========================================================================

    CVarRef CVarSystem::Find(const char* name) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_nameToIndex.find(name);
        if (it != m_nameToIndex.end())
        {
            return CVarRef(it->second);
        }

        return CVarRef();
    }

    bool CVarSystem::Exists(const char* name) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_nameToIndex.find(name) != m_nameToIndex.end();
    }

    const CVarDef* CVarSystem::GetDef(CVarRef ref) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!ref.IsValid() || ref.GetIndex() >= m_cvars.size())
        {
            return nullptr;
        }

        return &m_cvars[ref.GetIndex()];
    }

    const CVarDef* CVarSystem::GetDef(const char* name) const
    {
        return GetDef(Find(name));
    }

    std::vector<CVarRef> CVarSystem::GetByCategory(const std::string& category) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        std::vector<CVarRef> result;

        for (uint32 i = 0; i < static_cast<uint32>(m_cvars.size()); ++i)
        {
            if (m_cvars[i].category == category)
            {
                result.push_back(CVarRef(i));
            }
        }

        return result;
    }

    std::vector<std::string> CVarSystem::GetAllNames() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        std::vector<std::string> names;
        names.reserve(m_nameToIndex.size());

        for (const auto& [name, index] : m_nameToIndex)
        {
            names.push_back(name);
        }

        std::sort(names.begin(), names.end());
        return names;
    }

    // =========================================================================
    // Value Getters
    // =========================================================================

    bool CVarSystem::GetBool(const char* name) const
    {
        return GetBool(Find(name));
    }

    bool CVarSystem::GetBool(CVarRef ref) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!ref.IsValid() || ref.GetIndex() >= m_cvars.size())
        {
            return false;
        }

        const auto& def = m_cvars[ref.GetIndex()];
        if (def.type != CVarType::Bool)
        {
            RVX_CORE_WARN("CVar '{}' is not a bool", def.name);
            return false;
        }

        return std::get<bool>(def.currentValue);
    }

    int32 CVarSystem::GetInt(const char* name) const
    {
        return GetInt(Find(name));
    }

    int32 CVarSystem::GetInt(CVarRef ref) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!ref.IsValid() || ref.GetIndex() >= m_cvars.size())
        {
            return 0;
        }

        const auto& def = m_cvars[ref.GetIndex()];
        if (def.type != CVarType::Int)
        {
            RVX_CORE_WARN("CVar '{}' is not an int", def.name);
            return 0;
        }

        return std::get<int32>(def.currentValue);
    }

    float32 CVarSystem::GetFloat(const char* name) const
    {
        return GetFloat(Find(name));
    }

    float32 CVarSystem::GetFloat(CVarRef ref) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!ref.IsValid() || ref.GetIndex() >= m_cvars.size())
        {
            return 0.0f;
        }

        const auto& def = m_cvars[ref.GetIndex()];
        if (def.type != CVarType::Float)
        {
            RVX_CORE_WARN("CVar '{}' is not a float", def.name);
            return 0.0f;
        }

        return std::get<float32>(def.currentValue);
    }

    const std::string& CVarSystem::GetString(const char* name) const
    {
        return GetString(Find(name));
    }

    const std::string& CVarSystem::GetString(CVarRef ref) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!ref.IsValid() || ref.GetIndex() >= m_cvars.size())
        {
            return s_emptyString;
        }

        const auto& def = m_cvars[ref.GetIndex()];
        if (def.type != CVarType::String)
        {
            RVX_CORE_WARN("CVar '{}' is not a string", def.name);
            return s_emptyString;
        }

        return std::get<std::string>(def.currentValue);
    }

    // =========================================================================
    // Value Setters
    // =========================================================================

    bool CVarSystem::SetBool(const char* name, bool value)
    {
        return SetBool(Find(name), value);
    }

    bool CVarSystem::SetBool(CVarRef ref, bool value)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!ref.IsValid() || ref.GetIndex() >= m_cvars.size())
        {
            return false;
        }

        auto& def = m_cvars[ref.GetIndex()];

        if (HasFlag(def.flags, CVarFlags::ReadOnly))
        {
            RVX_CORE_WARN("CVar '{}' is read-only", def.name);
            return false;
        }

        if (HasFlag(def.flags, CVarFlags::Cheat) && !m_cheatsEnabled)
        {
            RVX_CORE_WARN("CVar '{}' requires cheats to be enabled", def.name);
            return false;
        }

        CVarValue oldValue = def.currentValue;
        def.currentValue = value;

        NotifyCallbacks(def, oldValue);
        return true;
    }

    bool CVarSystem::SetInt(const char* name, int32 value)
    {
        return SetInt(Find(name), value);
    }

    bool CVarSystem::SetInt(CVarRef ref, int32 value)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!ref.IsValid() || ref.GetIndex() >= m_cvars.size())
        {
            return false;
        }

        auto& def = m_cvars[ref.GetIndex()];

        if (HasFlag(def.flags, CVarFlags::ReadOnly))
        {
            RVX_CORE_WARN("CVar '{}' is read-only", def.name);
            return false;
        }

        // Clamp to range
        int32 minVal = std::get<int32>(def.minValue);
        int32 maxVal = std::get<int32>(def.maxValue);
        value = std::clamp(value, minVal, maxVal);

        CVarValue oldValue = def.currentValue;
        def.currentValue = value;

        NotifyCallbacks(def, oldValue);
        return true;
    }

    bool CVarSystem::SetFloat(const char* name, float32 value)
    {
        return SetFloat(Find(name), value);
    }

    bool CVarSystem::SetFloat(CVarRef ref, float32 value)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!ref.IsValid() || ref.GetIndex() >= m_cvars.size())
        {
            return false;
        }

        auto& def = m_cvars[ref.GetIndex()];

        if (HasFlag(def.flags, CVarFlags::ReadOnly))
        {
            RVX_CORE_WARN("CVar '{}' is read-only", def.name);
            return false;
        }

        // Clamp to range
        float32 minVal = std::get<float32>(def.minValue);
        float32 maxVal = std::get<float32>(def.maxValue);
        value = std::clamp(value, minVal, maxVal);

        CVarValue oldValue = def.currentValue;
        def.currentValue = value;

        NotifyCallbacks(def, oldValue);
        return true;
    }

    bool CVarSystem::SetString(const char* name, const std::string& value)
    {
        return SetString(Find(name), value);
    }

    bool CVarSystem::SetString(CVarRef ref, const std::string& value)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!ref.IsValid() || ref.GetIndex() >= m_cvars.size())
        {
            return false;
        }

        auto& def = m_cvars[ref.GetIndex()];

        if (HasFlag(def.flags, CVarFlags::ReadOnly))
        {
            RVX_CORE_WARN("CVar '{}' is read-only", def.name);
            return false;
        }

        CVarValue oldValue = def.currentValue;
        def.currentValue = value;

        NotifyCallbacks(def, oldValue);
        return true;
    }

    bool CVarSystem::SetFromString(const char* name, const std::string& value)
    {
        CVarRef ref = Find(name);
        if (!ref.IsValid())
        {
            return false;
        }

        const CVarDef* def = GetDef(ref);
        if (!def)
        {
            return false;
        }

        switch (def->type)
        {
        case CVarType::Bool:
        {
            std::string lower = value;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            bool boolVal = (lower == "true" || lower == "1" || lower == "yes" || lower == "on");
            return SetBool(ref, boolVal);
        }
        case CVarType::Int:
        {
            try
            {
                int32 intVal = std::stoi(value);
                return SetInt(ref, intVal);
            }
            catch (...)
            {
                return false;
            }
        }
        case CVarType::Float:
        {
            try
            {
                float32 floatVal = std::stof(value);
                return SetFloat(ref, floatVal);
            }
            catch (...)
            {
                return false;
            }
        }
        case CVarType::String:
            return SetString(ref, value);

        default:
            return false;
        }
    }

    std::string CVarSystem::GetAsString(const char* name) const
    {
        return GetAsString(Find(name));
    }

    std::string CVarSystem::GetAsString(CVarRef ref) const
    {
        const CVarDef* def = GetDef(ref);
        if (!def)
        {
            return "";
        }

        switch (def->type)
        {
        case CVarType::Bool:
            return std::get<bool>(def->currentValue) ? "true" : "false";
        case CVarType::Int:
            return std::to_string(std::get<int32>(def->currentValue));
        case CVarType::Float:
            return std::to_string(std::get<float32>(def->currentValue));
        case CVarType::String:
            return std::get<std::string>(def->currentValue);
        default:
            return "";
        }
    }

    void CVarSystem::ResetToDefault(const char* name)
    {
        ResetToDefault(Find(name));
    }

    void CVarSystem::ResetToDefault(CVarRef ref)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!ref.IsValid() || ref.GetIndex() >= m_cvars.size())
        {
            return;
        }

        auto& def = m_cvars[ref.GetIndex()];

        if (HasFlag(def.flags, CVarFlags::ReadOnly))
        {
            return;
        }

        CVarValue oldValue = def.currentValue;
        def.currentValue = def.defaultValue;

        NotifyCallbacks(def, oldValue);
    }

    void CVarSystem::ResetAllToDefaults()
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        for (auto& def : m_cvars)
        {
            if (!HasFlag(def.flags, CVarFlags::ReadOnly))
            {
                CVarValue oldValue = def.currentValue;
                def.currentValue = def.defaultValue;
                NotifyCallbacks(def, oldValue);
            }
        }
    }

    // =========================================================================
    // Callbacks
    // =========================================================================

    void CVarSystem::RegisterCallback(const char* name, CVarCallback callback)
    {
        RegisterCallback(Find(name), std::move(callback));
    }

    void CVarSystem::RegisterCallback(CVarRef ref, CVarCallback callback)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!ref.IsValid() || ref.GetIndex() >= m_cvars.size())
        {
            return;
        }

        m_cvars[ref.GetIndex()].callbacks.push_back(std::move(callback));
    }

    void CVarSystem::NotifyCallbacks(CVarDef& def, const CVarValue& oldValue)
    {
        for (const auto& callback : def.callbacks)
        {
            callback(oldValue, def.currentValue);
        }
    }

    // =========================================================================
    // Persistence
    // =========================================================================

    bool CVarSystem::SaveToFile(const std::string& filepath) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        std::ofstream file(filepath);
        if (!file.is_open())
        {
            RVX_CORE_ERROR("Failed to open config file for writing: {}", filepath);
            return false;
        }

        file << "# RenderVerseX Configuration\n";
        file << "# Auto-generated file\n\n";

        std::string currentCategory;

        for (const auto& def : m_cvars)
        {
            if (!HasFlag(def.flags, CVarFlags::Archive))
            {
                continue;
            }

            // Write category header
            if (def.category != currentCategory)
            {
                if (!currentCategory.empty())
                {
                    file << "\n";
                }
                file << "# " << def.category << "\n";
                currentCategory = def.category;
            }

            // Write the cvar
            file << def.name << " = " << GetAsString(Find(def.name.c_str())) << "\n";
        }

        return true;
    }

    bool CVarSystem::LoadFromFile(const std::string& filepath)
    {
        std::ifstream file(filepath);
        if (!file.is_open())
        {
            RVX_CORE_WARN("Config file not found: {}", filepath);
            return false;
        }

        std::string line;
        int lineNum = 0;

        while (std::getline(file, line))
        {
            lineNum++;

            // Trim whitespace
            line.erase(0, line.find_first_not_of(" \t"));
            line.erase(line.find_last_not_of(" \t\r\n") + 1);

            // Skip empty lines and comments
            if (line.empty() || line[0] == '#')
            {
                continue;
            }

            // Parse "name = value"
            size_t eqPos = line.find('=');
            if (eqPos == std::string::npos)
            {
                RVX_CORE_WARN("Config parse error at line {}: no '=' found", lineNum);
                continue;
            }

            std::string name = line.substr(0, eqPos);
            std::string value = line.substr(eqPos + 1);

            // Trim
            name.erase(0, name.find_first_not_of(" \t"));
            name.erase(name.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);

            if (!SetFromString(name.c_str(), value))
            {
                RVX_CORE_WARN("Config: Failed to set '{}' to '{}'", name, value);
            }
        }

        RVX_CORE_INFO("Loaded config from: {}", filepath);
        return true;
    }

    // =========================================================================
    // Console Integration
    // =========================================================================

    void CVarSystem::RegisterConsoleCommands()
    {
        // List CVars command
        Console::Get().RegisterCommand({
            .name = "cvarlist",
            .description = "List all console variables",
            .usage = "cvarlist [filter]",
            .handler = [this](const CommandArgs& args) -> CommandResult {
                std::vector<std::string> lines;
                std::string filter = args.Count() > 0 ? args.GetString(0) : "";

                auto names = GetAllNames();
                for (const auto& name : names)
                {
                    if (!filter.empty() && name.find(filter) == std::string::npos)
                    {
                        continue;
                    }

                    const CVarDef* def = GetDef(name.c_str());
                    if (def && !HasFlag(def->flags, CVarFlags::Hidden))
                    {
                        std::string line = name + " = " + GetAsString(name.c_str());
                        if (!def->description.empty())
                        {
                            line += " // " + def->description;
                        }
                        lines.push_back(line);
                    }
                }

                if (lines.empty())
                {
                    return CommandResult::Success("No CVars found");
                }

                return CommandResult::Output(lines);
            }
        });

        // Set CVar command
        Console::Get().RegisterCommand({
            .name = "set",
            .description = "Set a console variable",
            .usage = "set <name> <value>",
            .handler = [this](const CommandArgs& args) -> CommandResult {
                if (args.Count() < 2)
                {
                    return CommandResult::Error("Usage: set <name> <value>");
                }

                const std::string& name = args.GetString(0);
                const std::string& value = args.GetString(1);

                if (!Exists(name.c_str()))
                {
                    return CommandResult::Error("Unknown CVar: " + name);
                }

                if (SetFromString(name.c_str(), value))
                {
                    return CommandResult::Success(name + " = " + GetAsString(name.c_str()));
                }
                else
                {
                    return CommandResult::Error("Failed to set " + name);
                }
            }
        });

        // Get CVar command
        Console::Get().RegisterCommand({
            .name = "get",
            .description = "Get a console variable value",
            .usage = "get <name>",
            .handler = [this](const CommandArgs& args) -> CommandResult {
                if (args.Count() < 1)
                {
                    return CommandResult::Error("Usage: get <name>");
                }

                const std::string& name = args.GetString(0);

                if (!Exists(name.c_str()))
                {
                    return CommandResult::Error("Unknown CVar: " + name);
                }

                const CVarDef* def = GetDef(name.c_str());
                std::string result = name + " = " + GetAsString(name.c_str());

                if (def && !def->description.empty())
                {
                    result += "\n  " + def->description;
                }

                return CommandResult::Success(result);
            }
        });

        // Reset command
        Console::Get().RegisterCommand({
            .name = "reset",
            .description = "Reset a console variable to default",
            .usage = "reset <name>",
            .handler = [this](const CommandArgs& args) -> CommandResult {
                if (args.Count() < 1)
                {
                    return CommandResult::Error("Usage: reset <name>");
                }

                const std::string& name = args.GetString(0);

                if (!Exists(name.c_str()))
                {
                    return CommandResult::Error("Unknown CVar: " + name);
                }

                ResetToDefault(name.c_str());
                return CommandResult::Success(name + " reset to " + GetAsString(name.c_str()));
            }
        });
    }

    // =========================================================================
    // Helpers
    // =========================================================================

    bool CVarSystem::ValidateValue(const CVarDef& def, const CVarValue& value) const
    {
        // Type check
        if (value.index() != static_cast<size_t>(def.type))
        {
            return false;
        }

        // Range check for numeric types
        if (def.type == CVarType::Int)
        {
            int32 val = std::get<int32>(value);
            int32 minVal = std::get<int32>(def.minValue);
            int32 maxVal = std::get<int32>(def.maxValue);
            return val >= minVal && val <= maxVal;
        }
        else if (def.type == CVarType::Float)
        {
            float32 val = std::get<float32>(value);
            float32 minVal = std::get<float32>(def.minValue);
            float32 maxVal = std::get<float32>(def.maxValue);
            return val >= minVal && val <= maxVal;
        }

        return true;
    }

    std::string CVarSystem::ExtractCategory(const std::string& name) const
    {
        size_t dotPos = name.find('.');
        if (dotPos != std::string::npos)
        {
            return name.substr(0, dotPos);
        }
        return "General";
    }

} // namespace RVX
