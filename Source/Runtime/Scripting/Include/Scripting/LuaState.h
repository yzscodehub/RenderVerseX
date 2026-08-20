#pragma once

/**
 * @file LuaState.h
 * @brief Lua state wrapper using sol2
 * 
 * Provides a safe and convenient wrapper around the Lua VM with:
 * - RAII-managed state lifetime
 * - Script execution and error handling
 * - Type-safe variable access
 * - Standard library loading
 */

#include "Core/Types.h"
#include "Core/Log.h"

#include <sol/sol.hpp>
#include <string>
#include <string_view>
#include <functional>
#include <optional>
#include <filesystem>

namespace RVX
{
    /**
     * @brief Result of script execution
     */
    struct ScriptResult
    {
        bool success = false;
        std::string errorMessage;

        explicit operator bool() const { return success; }

        static ScriptResult Success() { return { true, "" }; }
        static ScriptResult Failure(const std::string& msg) { return { false, msg }; }
    };

    /**
     * @brief Lua library flags for selective loading
     */
    enum class LuaLibrary : uint32
    {
        None        = 0,
        Base        = 1 << 0,
        Package     = 1 << 1,
        Coroutine   = 1 << 2,
        String      = 1 << 3,
        OS          = 1 << 4,
        Math        = 1 << 5,
        Table       = 1 << 6,
        Debug       = 1 << 7,
        IO          = 1 << 8,
        UTF8        = 1 << 9,

        // Presets
        Safe        = Base | String | Math | Table | UTF8,  // No IO, OS, Debug
        All         = 0xFFFFFFFF
    };

    inline LuaLibrary operator|(LuaLibrary a, LuaLibrary b)
    {
        return static_cast<LuaLibrary>(static_cast<uint32>(a) | static_cast<uint32>(b));
    }

    inline LuaLibrary operator&(LuaLibrary a, LuaLibrary b)
    {
        return static_cast<LuaLibrary>(static_cast<uint32>(a) & static_cast<uint32>(b));
    }

    inline bool HasFlag(LuaLibrary flags, LuaLibrary flag)
    {
        return (static_cast<uint32>(flags) & static_cast<uint32>(flag)) != 0;
    }

    /**
     * @brief Configuration for LuaState
     */
    struct LuaStateConfig
    {
        LuaLibrary libraries = LuaLibrary::Safe;
        bool enablePanic = true;            ///< Install panic handler
        uint32 memoryLimitMB = 0;           ///< Memory limit (0 = unlimited)
        uint32 instructionLimit = 0;        ///< Instruction limit per call (0 = unlimited)
    };

    /**
     * @brief RAII wrapper for Lua VM state
     * 
     * Usage:
     * @code
     * LuaState lua;
     * lua.Initialize();
     * 
     * // Execute script
     * auto result = lua.ExecuteString("print('Hello from Lua!')");
     * if (!result) {
     *     RVX_CORE_ERROR("Script error: {}", result.errorMessage);
     * }
     * 
     * // Execute file
     * lua.ExecuteFile("scripts/game.lua");
     * 
     * // Call Lua function
     * auto sum = lua.Call<int>("add", 5, 3);
     * @endcode
     */
    class LuaState
    {
    public:
        // =====================================================================
        // Construction
        // =====================================================================

        LuaState();
        ~LuaState();

        // Non-copyable, movable
        LuaState(const LuaState&) = delete;
        LuaState& operator=(const LuaState&) = delete;
        LuaState(LuaState&&) noexcept;
        LuaState& operator=(LuaState&&) noexcept;

        // =====================================================================
        // Initialization
        // =====================================================================

        /**
         * @brief Initialize the Lua state with configuration
         * @param config Configuration options
         * @return true if initialization succeeded
         */
        bool Initialize(const LuaStateConfig& config = {});

        /**
         * @brief Shutdown and clean up the Lua state
         */
        void Shutdown();

        /**
         * @brief Check if state is initialized
         */
        bool IsInitialized() const { return m_initialized; }

        // =====================================================================
        // Script Execution
        // =====================================================================

        /**
         * @brief Execute a Lua string
         * @param script The Lua code to execute
         * @param chunkName Optional name for error messages
         * @return Execution result
         */
        ScriptResult ExecuteString(std::string_view script, 
                                   const std::string& chunkName = "string");

        /**
         * @brief Execute a Lua file
         * @param filePath Path to the script file
         * @return Execution result
         */
        ScriptResult ExecuteFile(const std::filesystem::path& filePath);

        /**
         * @brief Load a script without executing it
         * @param script The Lua code to load
         * @param chunkName Optional name for error messages
         * @return Loaded function or nil on error
         */
        sol::load_result LoadString(std::string_view script,
                                    const std::string& chunkName = "string");

        // =====================================================================
        // Function Calls
        // =====================================================================

        /**
         * @brief Call a global Lua function
         * @tparam Ret Return type
         * @tparam Args Argument types
         * @param functionName Name of the global function
         * @param args Arguments to pass
         * @return Optional return value (empty on error)
         */
        template<typename Ret, typename... Args>
        std::optional<Ret> Call(const std::string& functionName, Args&&... args);

        /**
         * @brief Call a global Lua function (void return)
         * @tparam Args Argument types
         * @param functionName Name of the global function
         * @param args Arguments to pass
         * @return Execution result
         */
        template<typename... Args>
        ScriptResult CallVoid(const std::string& functionName, Args&&... args);

        /**
         * @brief Check if a global function exists
         * @param functionName Name to check
         * @return true if function exists
         */
        bool HasFunction(const std::string& functionName) const;

        // =====================================================================
        // Variable Access
        // =====================================================================

        /**
         * @brief Get a global variable
         * @tparam T Type to retrieve
         * @param name Variable name
         * @return Optional value (empty if not found or wrong type)
         */
        template<typename T>
        std::optional<T> Get(const std::string& name) const;

        /**
         * @brief Set a global variable
         * @tparam T Type to set
         * @param name Variable name
         * @param value Value to set
         */
        template<typename T>
        void Set(const std::string& name, T&& value);

        // =====================================================================
        // Table/Usertype Registration
        // =====================================================================

        /**
         * @brief Register a new usertype (C++ class)
         * @tparam T Type to register
         * @param name Name in Lua
         * @param args Constructor and member definitions
         */
        template<typename T, typename... Args>
        sol::usertype<T> RegisterType(const std::string& name, Args&&... args);

        /**
         * @brief Get or create a namespace table
         * @param name Namespace name
         * @return Table reference
         */
        sol::table GetOrCreateNamespace(const std::string& name);

        /**
         * @brief Add a search path for require()
         * @param path Directory path
         */
        void AddSearchPath(const std::filesystem::path& path);

        // =====================================================================
        // Direct State Access
        // =====================================================================

        /**
         * @brief Get the underlying sol::state
         * @return Reference to sol state
         */
        sol::state& GetState() { return m_state; }
        const sol::state& GetState() const { return m_state; }

        /**
         * @brief Get raw Lua state pointer
         * @return lua_State pointer
         */
        lua_State* GetRawState() { return m_state.lua_state(); }

        // =====================================================================
        // Memory Management
        // =====================================================================

        /**
         * @brief Get current memory usage in bytes
         */
        size_t GetMemoryUsage() const;

        /**
         * @brief Run garbage collector
         */
        void CollectGarbage();

    private:
        sol::state m_state;
        bool m_initialized = false;
        LuaStateConfig m_config;

        void LoadLibraries(LuaLibrary libs);
        static int PanicHandler(lua_State* L);
    };

    // =========================================================================
    // Template Implementations
    // =========================================================================

    template<typename Ret, typename... Args>
    std::optional<Ret> LuaState::Call(const std::string& functionName, Args&&... args)
    {
        if (!m_initialized)
        {
            RVX_CORE_ERROR("LuaState::Call - State not initialized");
            return std::nullopt;
        }

        sol::function func = m_state[functionName];
        if (!func.valid())
        {
            RVX_CORE_ERROR("LuaState::Call - Function '{}' not found", functionName);
            return std::nullopt;
        }

        sol::protected_function_result result = func(std::forward<Args>(args)...);
        if (!result.valid())
        {
            sol::error err = result;
            RVX_CORE_ERROR("LuaState::Call - Error calling '{}': {}", 
                          functionName, err.what());
            return std::nullopt;
        }

        return result.get<Ret>();
    }

    template<typename... Args>
    ScriptResult LuaState::CallVoid(const std::string& functionName, Args&&... args)
    {
        if (!m_initialized)
        {
            return ScriptResult::Failure("State not initialized");
        }

        sol::function func = m_state[functionName];
        if (!func.valid())
        {
            return ScriptResult::Failure("Function '" + functionName + "' not found");
        }

        sol::protected_function_result result = func(std::forward<Args>(args)...);
        if (!result.valid())
        {
            sol::error err = result;
            return ScriptResult::Failure(err.what());
        }

        return ScriptResult::Success();
    }

    template<typename T>
    std::optional<T> LuaState::Get(const std::string& name) const
    {
        if (!m_initialized)
        {
            return std::nullopt;
        }

        sol::object obj = m_state[name];
        if (!obj.valid() || !obj.is<T>())
        {
            return std::nullopt;
        }

        return obj.as<T>();
    }

    template<typename T>
    void LuaState::Set(const std::string& name, T&& value)
    {
        if (m_initialized)
        {
            m_state[name] = std::forward<T>(value);
        }
    }

    template<typename T, typename... Args>
    sol::usertype<T> LuaState::RegisterType(const std::string& name, Args&&... args)
    {
        return m_state.new_usertype<T>(name, std::forward<Args>(args)...);
    }

} // namespace RVX
