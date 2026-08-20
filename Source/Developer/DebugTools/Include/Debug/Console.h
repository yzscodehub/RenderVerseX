#pragma once

/**
 * @file Console.h
 * @brief Command console system for runtime debugging
 * 
 * Features:
 * - Command registration with arguments
 * - Command history and auto-complete
 * - Argument parsing and validation
 * - Built-in help system
 */

#include "Core/Types.h"
#include <spdlog/fmt/fmt.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <variant>
#include <optional>

namespace RVX
{
    /**
     * @brief Command argument value types
     */
    using CommandArgValue = std::variant<
        std::monostate,     // No value / void
        bool,               // Boolean
        int64,              // Integer
        float64,            // Floating point
        std::string         // String
    >;

    /**
     * @brief Parsed command arguments
     */
    class CommandArgs
    {
    public:
        CommandArgs() = default;
        explicit CommandArgs(const std::vector<std::string>& args);

        // =========================================================================
        // Access
        // =========================================================================

        /**
         * @brief Get the number of arguments
         */
        size_t Count() const { return m_args.size(); }

        /**
         * @brief Check if empty
         */
        bool Empty() const { return m_args.empty(); }

        /**
         * @brief Get raw string argument
         */
        const std::string& GetString(size_t index) const;

        /**
         * @brief Get argument as integer
         */
        std::optional<int64> GetInt(size_t index) const;

        /**
         * @brief Get argument as float
         */
        std::optional<float64> GetFloat(size_t index) const;

        /**
         * @brief Get argument as boolean
         */
        std::optional<bool> GetBool(size_t index) const;

        /**
         * @brief Get argument with default value
         */
        template<typename T>
        T GetOr(size_t index, T defaultValue) const;

        /**
         * @brief Get all arguments as a single string
         */
        std::string GetRemainder(size_t startIndex = 0) const;

    private:
        std::vector<std::string> m_args;
        static const std::string s_emptyString;
    };

    /**
     * @brief Result of command execution
     */
    struct CommandResult
    {
        bool success = true;
        std::string message;
        std::vector<std::string> output;

        static CommandResult Success(const std::string& msg = "")
        {
            return { true, msg, {} };
        }

        static CommandResult Error(const std::string& msg)
        {
            return { false, msg, {} };
        }

        static CommandResult Output(const std::vector<std::string>& lines)
        {
            return { true, "", lines };
        }
    };

    /**
     * @brief Command handler function type
     */
    using CommandHandler = std::function<CommandResult(const CommandArgs&)>;

    /**
     * @brief Command definition
     */
    struct CommandDef
    {
        std::string name;
        std::string description;
        std::string usage;
        CommandHandler handler;
        std::vector<std::string> aliases;
        bool hidden = false;  // Hidden from help listing
    };

    /**
     * @brief Output handler for console messages
     */
    using ConsoleOutputHandler = std::function<void(const std::string& message, bool isError)>;

    /**
     * @brief Command console for runtime debugging
     * 
     * Usage:
     * @code
     * // Register a command
     * Console::Get().RegisterCommand({
     *     .name = "quit",
     *     .description = "Exit the application",
     *     .usage = "quit",
     *     .handler = [](const CommandArgs&) {
     *         // Quit logic
     *         return CommandResult::Success("Goodbye!");
     *     }
     * });
     * 
     * // Execute a command
     * Console::Get().Execute("quit");
     * @endcode
     */
    class Console
    {
    public:
        // =========================================================================
        // Singleton Access
        // =========================================================================

        /**
         * @brief Get the global console instance
         */
        static Console& Get();

        // =========================================================================
        // Lifecycle
        // =========================================================================

        /**
         * @brief Initialize the console with built-in commands
         */
        void Initialize();

        /**
         * @brief Shutdown the console
         */
        void Shutdown();

        /**
         * @brief Check if initialized
         */
        bool IsInitialized() const { return m_initialized; }

        // =========================================================================
        // Command Registration
        // =========================================================================

        /**
         * @brief Register a command
         * @param def Command definition
         * @return true if registered successfully
         */
        bool RegisterCommand(const CommandDef& def);

        /**
         * @brief Register a simple command with just name, description, and handler
         */
        bool RegisterCommand(
            const std::string& name,
            const std::string& description,
            CommandHandler handler
        );

        /**
         * @brief Unregister a command
         */
        void UnregisterCommand(const std::string& name);

        /**
         * @brief Check if a command exists
         */
        bool HasCommand(const std::string& name) const;

        /**
         * @brief Get all registered commands
         */
        std::vector<const CommandDef*> GetCommands() const;

        // =========================================================================
        // Execution
        // =========================================================================

        /**
         * @brief Execute a command string
         * @param commandLine Full command line (e.g., "set r.vsync 1")
         * @return Execution result
         */
        CommandResult Execute(const std::string& commandLine);

        /**
         * @brief Execute a command with parsed arguments
         * @param name Command name
         * @param args Pre-parsed arguments
         */
        CommandResult Execute(const std::string& name, const CommandArgs& args);

        // =========================================================================
        // Auto-Complete
        // =========================================================================

        /**
         * @brief Get auto-complete suggestions
         * @param partial Partial command string
         * @return List of matching command names
         */
        std::vector<std::string> GetCompletions(const std::string& partial) const;

        // =========================================================================
        // History
        // =========================================================================

        /**
         * @brief Add a command to history
         */
        void AddToHistory(const std::string& command);

        /**
         * @brief Get command history
         */
        const std::vector<std::string>& GetHistory() const { return m_history; }

        /**
         * @brief Clear command history
         */
        void ClearHistory() { m_history.clear(); }

        /**
         * @brief Set maximum history size
         */
        void SetMaxHistorySize(size_t size) { m_maxHistorySize = size; }

        /**
         * @brief Get a history entry (0 = most recent)
         */
        const std::string& GetHistoryEntry(size_t index) const;

        // =========================================================================
        // Output
        // =========================================================================

        /**
         * @brief Print a message to the console
         */
        void Print(const std::string& message);

        /**
         * @brief Print an error message
         */
        void PrintError(const std::string& message);

        /**
         * @brief Print formatted message
         */
        template<typename... Args>
        void Printf(const char* format, Args&&... args);

        /**
         * @brief Set output handler
         */
        void SetOutputHandler(ConsoleOutputHandler handler) { m_outputHandler = handler; }

        /**
         * @brief Get recent output lines
         */
        const std::vector<std::string>& GetOutputBuffer() const { return m_outputBuffer; }

        /**
         * @brief Clear output buffer
         */
        void ClearOutput() { m_outputBuffer.clear(); }

        /**
         * @brief Set maximum output buffer size
         */
        void SetMaxOutputSize(size_t size) { m_maxOutputSize = size; }

    private:
        Console() = default;
        ~Console() { Shutdown(); }

        // Non-copyable
        Console(const Console&) = delete;
        Console& operator=(const Console&) = delete;

        void RegisterBuiltinCommands();
        std::vector<std::string> ParseCommandLine(const std::string& line) const;
        const CommandDef* FindCommand(const std::string& name) const;

        // =========================================================================
        // Internal State
        // =========================================================================

        bool m_initialized = false;

        // Commands
        std::unordered_map<std::string, CommandDef> m_commands;
        std::unordered_map<std::string, std::string> m_aliases;  // alias -> real name

        // History
        std::vector<std::string> m_history;
        size_t m_maxHistorySize = 100;

        // Output
        std::vector<std::string> m_outputBuffer;
        size_t m_maxOutputSize = 1000;
        ConsoleOutputHandler m_outputHandler;
    };

    // =========================================================================
    // Template Implementations
    // =========================================================================

    template<typename T>
    T CommandArgs::GetOr(size_t index, T defaultValue) const
    {
        if (index >= m_args.size())
        {
            return defaultValue;
        }

        if constexpr (std::is_same_v<T, int64> || std::is_same_v<T, int32> || std::is_same_v<T, int>)
        {
            auto val = GetInt(index);
            return val.has_value() ? static_cast<T>(val.value()) : defaultValue;
        }
        else if constexpr (std::is_same_v<T, float64> || std::is_same_v<T, float32> || std::is_same_v<T, float>)
        {
            auto val = GetFloat(index);
            return val.has_value() ? static_cast<T>(val.value()) : defaultValue;
        }
        else if constexpr (std::is_same_v<T, bool>)
        {
            auto val = GetBool(index);
            return val.has_value() ? val.value() : defaultValue;
        }
        else if constexpr (std::is_same_v<T, std::string>)
        {
            return GetString(index);
        }
        else
        {
            return defaultValue;
        }
    }

    template<typename... Args>
    void Console::Printf(const char* format, Args&&... args)
    {
        // Use fmt-style formatting
        Print(fmt::format(format, std::forward<Args>(args)...));
    }

} // namespace RVX

// =============================================================================
// Console Macros
// =============================================================================

/**
 * @brief Register a simple console command
 */
#define RVX_CONSOLE_COMMAND(name, description, handler) \
    ::RVX::Console::Get().RegisterCommand(name, description, handler)
