/**
 * @file Console.cpp
 * @brief Command console implementation
 */

#include "Debug/Console.h"
#include "Core/Log.h"
#include <sstream>
#include <algorithm>
#include <cctype>

namespace RVX
{
    // =========================================================================
    // CommandArgs Implementation
    // =========================================================================

    const std::string CommandArgs::s_emptyString;

    CommandArgs::CommandArgs(const std::vector<std::string>& args)
        : m_args(args)
    {
    }

    const std::string& CommandArgs::GetString(size_t index) const
    {
        if (index >= m_args.size())
        {
            return s_emptyString;
        }
        return m_args[index];
    }

    std::optional<int64> CommandArgs::GetInt(size_t index) const
    {
        if (index >= m_args.size())
        {
            return std::nullopt;
        }

        try
        {
            return std::stoll(m_args[index]);
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::optional<float64> CommandArgs::GetFloat(size_t index) const
    {
        if (index >= m_args.size())
        {
            return std::nullopt;
        }

        try
        {
            return std::stod(m_args[index]);
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::optional<bool> CommandArgs::GetBool(size_t index) const
    {
        if (index >= m_args.size())
        {
            return std::nullopt;
        }

        const std::string& str = m_args[index];

        // Convert to lowercase for comparison
        std::string lower = str;
        std::transform(lower.begin(), lower.end(), lower.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (lower == "true" || lower == "1" || lower == "yes" || lower == "on")
        {
            return true;
        }
        if (lower == "false" || lower == "0" || lower == "no" || lower == "off")
        {
            return false;
        }

        return std::nullopt;
    }

    std::string CommandArgs::GetRemainder(size_t startIndex) const
    {
        if (startIndex >= m_args.size())
        {
            return "";
        }

        std::ostringstream ss;
        for (size_t i = startIndex; i < m_args.size(); ++i)
        {
            if (i > startIndex)
            {
                ss << " ";
            }
            ss << m_args[i];
        }

        return ss.str();
    }

    // =========================================================================
    // Console Singleton
    // =========================================================================

    Console& Console::Get()
    {
        static Console instance;
        return instance;
    }

    // =========================================================================
    // Lifecycle
    // =========================================================================

    void Console::Initialize()
    {
        if (m_initialized)
        {
            return;
        }

        RegisterBuiltinCommands();

        m_initialized = true;
        RVX_CORE_INFO("Console initialized");
    }

    void Console::Shutdown()
    {
        if (!m_initialized)
        {
            return;
        }

        m_commands.clear();
        m_aliases.clear();
        m_history.clear();
        m_outputBuffer.clear();

        m_initialized = false;
        RVX_CORE_INFO("Console shutdown");
    }

    void Console::RegisterBuiltinCommands()
    {
        // Help command
        RegisterCommand({
            .name = "help",
            .description = "Display help for commands",
            .usage = "help [command]",
            .handler = [this](const CommandArgs& args) -> CommandResult {
                if (args.Count() == 0)
                {
                    // List all commands
                    std::vector<std::string> lines;
                    lines.push_back("Available commands:");

                    std::vector<const CommandDef*> commands = GetCommands();
                    std::sort(commands.begin(), commands.end(),
                        [](const CommandDef* a, const CommandDef* b) {
                            return a->name < b->name;
                        });

                    for (const auto* cmd : commands)
                    {
                        if (!cmd->hidden)
                        {
                            lines.push_back("  " + cmd->name + " - " + cmd->description);
                        }
                    }

                    lines.push_back("");
                    lines.push_back("Type 'help <command>' for detailed usage.");

                    return CommandResult::Output(lines);
                }
                else
                {
                    // Show help for specific command
                    const std::string& cmdName = args.GetString(0);
                    const CommandDef* cmd = FindCommand(cmdName);

                    if (cmd == nullptr)
                    {
                        return CommandResult::Error("Unknown command: " + cmdName);
                    }

                    std::vector<std::string> lines;
                    lines.push_back("Command: " + cmd->name);
                    lines.push_back("Description: " + cmd->description);
                    lines.push_back("Usage: " + cmd->usage);

                    if (!cmd->aliases.empty())
                    {
                        std::string aliasStr = "Aliases: ";
                        for (size_t i = 0; i < cmd->aliases.size(); ++i)
                        {
                            if (i > 0) aliasStr += ", ";
                            aliasStr += cmd->aliases[i];
                        }
                        lines.push_back(aliasStr);
                    }

                    return CommandResult::Output(lines);
                }
            }
        });

        // Clear command
        RegisterCommand({
            .name = "clear",
            .description = "Clear the console output",
            .usage = "clear",
            .handler = [this](const CommandArgs&) -> CommandResult {
                ClearOutput();
                return CommandResult::Success();
            }
        });

        // Echo command
        RegisterCommand({
            .name = "echo",
            .description = "Print a message",
            .usage = "echo <message>",
            .handler = [](const CommandArgs& args) -> CommandResult {
                return CommandResult::Success(args.GetRemainder(0));
            }
        });

        // History command
        RegisterCommand({
            .name = "history",
            .description = "Show command history",
            .usage = "history [count]",
            .handler = [this](const CommandArgs& args) -> CommandResult {
                size_t count = args.GetOr<int64>(0, static_cast<int64>(m_history.size()));
                count = std::min(count, m_history.size());

                std::vector<std::string> lines;
                size_t start = m_history.size() > count ? m_history.size() - count : 0;

                for (size_t i = start; i < m_history.size(); ++i)
                {
                    lines.push_back(std::to_string(i) + ": " + m_history[i]);
                }

                return CommandResult::Output(lines);
            }
        });

        // Alias command
        RegisterCommand({
            .name = "alias",
            .description = "Create an alias for a command",
            .usage = "alias <name> <command>",
            .handler = [this](const CommandArgs& args) -> CommandResult {
                if (args.Count() < 2)
                {
                    return CommandResult::Error("Usage: alias <name> <command>");
                }

                const std::string& aliasName = args.GetString(0);
                const std::string& cmdName = args.GetString(1);

                if (!HasCommand(cmdName))
                {
                    return CommandResult::Error("Unknown command: " + cmdName);
                }

                m_aliases[aliasName] = cmdName;
                return CommandResult::Success("Alias created: " + aliasName + " -> " + cmdName);
            }
        });
    }

    // =========================================================================
    // Command Registration
    // =========================================================================

    bool Console::RegisterCommand(const CommandDef& def)
    {
        if (def.name.empty() || !def.handler)
        {
            RVX_CORE_ERROR("Console: Invalid command definition");
            return false;
        }

        if (m_commands.find(def.name) != m_commands.end())
        {
            RVX_CORE_WARN("Console: Command '{}' already registered, replacing", def.name);
        }

        m_commands[def.name] = def;

        // Register aliases
        for (const auto& alias : def.aliases)
        {
            m_aliases[alias] = def.name;
        }

        return true;
    }

    bool Console::RegisterCommand(
        const std::string& name,
        const std::string& description,
        CommandHandler handler)
    {
        return RegisterCommand({
            .name = name,
            .description = description,
            .usage = name,
            .handler = handler
        });
    }

    void Console::UnregisterCommand(const std::string& name)
    {
        auto it = m_commands.find(name);
        if (it != m_commands.end())
        {
            // Remove aliases
            for (const auto& alias : it->second.aliases)
            {
                m_aliases.erase(alias);
            }

            m_commands.erase(it);
        }
    }

    bool Console::HasCommand(const std::string& name) const
    {
        return FindCommand(name) != nullptr;
    }

    std::vector<const CommandDef*> Console::GetCommands() const
    {
        std::vector<const CommandDef*> result;
        result.reserve(m_commands.size());

        for (const auto& [name, def] : m_commands)
        {
            result.push_back(&def);
        }

        return result;
    }

    const CommandDef* Console::FindCommand(const std::string& name) const
    {
        // Check direct command
        auto it = m_commands.find(name);
        if (it != m_commands.end())
        {
            return &it->second;
        }

        // Check alias
        auto aliasIt = m_aliases.find(name);
        if (aliasIt != m_aliases.end())
        {
            it = m_commands.find(aliasIt->second);
            if (it != m_commands.end())
            {
                return &it->second;
            }
        }

        return nullptr;
    }

    // =========================================================================
    // Execution
    // =========================================================================

    std::vector<std::string> Console::ParseCommandLine(const std::string& line) const
    {
        std::vector<std::string> tokens;
        std::string current;
        bool inQuotes = false;
        char quoteChar = 0;

        for (size_t i = 0; i < line.size(); ++i)
        {
            char c = line[i];

            if (inQuotes)
            {
                if (c == quoteChar)
                {
                    inQuotes = false;
                }
                else
                {
                    current += c;
                }
            }
            else
            {
                if (c == '"' || c == '\'')
                {
                    inQuotes = true;
                    quoteChar = c;
                }
                else if (std::isspace(static_cast<unsigned char>(c)))
                {
                    if (!current.empty())
                    {
                        tokens.push_back(current);
                        current.clear();
                    }
                }
                else
                {
                    current += c;
                }
            }
        }

        if (!current.empty())
        {
            tokens.push_back(current);
        }

        return tokens;
    }

    CommandResult Console::Execute(const std::string& commandLine)
    {
        // Skip empty lines
        std::string trimmed = commandLine;
        trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r"));
        trimmed.erase(trimmed.find_last_not_of(" \t\n\r") + 1);

        if (trimmed.empty())
        {
            return CommandResult::Success();
        }

        // Add to history
        AddToHistory(trimmed);

        // Parse command line
        auto tokens = ParseCommandLine(trimmed);
        if (tokens.empty())
        {
            return CommandResult::Success();
        }

        // First token is the command name
        std::string cmdName = tokens[0];
        tokens.erase(tokens.begin());

        // Create args from remaining tokens
        CommandArgs args(tokens);

        return Execute(cmdName, args);
    }

    CommandResult Console::Execute(const std::string& name, const CommandArgs& args)
    {
        const CommandDef* cmd = FindCommand(name);

        if (cmd == nullptr)
        {
            // Check if it's a CVar
            // TODO: Integrate with CVarSystem

            return CommandResult::Error("Unknown command: " + name);
        }

        try
        {
            return cmd->handler(args);
        }
        catch (const std::exception& e)
        {
            return CommandResult::Error("Command failed: " + std::string(e.what()));
        }
    }

    // =========================================================================
    // Auto-Complete
    // =========================================================================

    std::vector<std::string> Console::GetCompletions(const std::string& partial) const
    {
        std::vector<std::string> completions;

        // Convert to lowercase for case-insensitive matching
        std::string lowerPartial = partial;
        std::transform(lowerPartial.begin(), lowerPartial.end(), lowerPartial.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        // Check commands
        for (const auto& [name, def] : m_commands)
        {
            std::string lowerName = name;
            std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            if (lowerName.find(lowerPartial) == 0)
            {
                completions.push_back(name);
            }
        }

        // Check aliases
        for (const auto& [alias, target] : m_aliases)
        {
            std::string lowerAlias = alias;
            std::transform(lowerAlias.begin(), lowerAlias.end(), lowerAlias.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            if (lowerAlias.find(lowerPartial) == 0)
            {
                completions.push_back(alias);
            }
        }

        // Sort alphabetically
        std::sort(completions.begin(), completions.end());

        return completions;
    }

    // =========================================================================
    // History
    // =========================================================================

    void Console::AddToHistory(const std::string& command)
    {
        // Don't add duplicate of last command
        if (!m_history.empty() && m_history.back() == command)
        {
            return;
        }

        m_history.push_back(command);

        // Trim if too long
        while (m_history.size() > m_maxHistorySize)
        {
            m_history.erase(m_history.begin());
        }
    }

    const std::string& Console::GetHistoryEntry(size_t index) const
    {
        static const std::string empty;

        if (index >= m_history.size())
        {
            return empty;
        }

        // Index 0 = most recent
        return m_history[m_history.size() - 1 - index];
    }

    // =========================================================================
    // Output
    // =========================================================================

    void Console::Print(const std::string& message)
    {
        m_outputBuffer.push_back(message);

        // Trim if too long
        while (m_outputBuffer.size() > m_maxOutputSize)
        {
            m_outputBuffer.erase(m_outputBuffer.begin());
        }

        // Notify handler
        if (m_outputHandler)
        {
            m_outputHandler(message, false);
        }

        // Also log
        RVX_CORE_DEBUG("[Console] {}", message);
    }

    void Console::PrintError(const std::string& message)
    {
        std::string errorMsg = "[ERROR] " + message;
        m_outputBuffer.push_back(errorMsg);

        // Trim if too long
        while (m_outputBuffer.size() > m_maxOutputSize)
        {
            m_outputBuffer.erase(m_outputBuffer.begin());
        }

        // Notify handler
        if (m_outputHandler)
        {
            m_outputHandler(message, true);
        }

        // Also log
        RVX_CORE_ERROR("[Console] {}", message);
    }

} // namespace RVX
