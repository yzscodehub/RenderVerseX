#pragma once

/**
 * @file ScriptEngine.h
 * @brief Scripting subsystem for managing Lua scripts
 *
 * The ScriptingSubsystem is an EngineSubsystem that:
 * - Manages the global Lua state
 * - Handles script loading and caching
 * - Provides C++ to Lua bindings
 * - Manages script hot-reloading
 */

#include "Core/Subsystem/EngineSubsystem.h"
#include "Scripting/LuaState.h"

#include <unordered_map>
#include <filesystem>
#include <memory>

namespace RVX
{
    // Forward declarations
    class ScriptComponent;

    /**
     * @brief Handle to a loaded script
     */
    using ScriptHandle = uint32;
    constexpr ScriptHandle InvalidScriptHandle = RVX_INVALID_INDEX;

    /**
     * @brief Cached script data
     */
    struct CachedScript
    {
        ScriptHandle handle = InvalidScriptHandle;
        std::filesystem::path filePath;
        std::string source;
        std::filesystem::file_time_type lastModified;
        bool isValid = false;
    };

    /**
     * @brief Scripting subsystem configuration
     */
    struct ScriptingSubsystemConfig
    {
        LuaStateConfig luaConfig;
        std::filesystem::path scriptsDirectory = "Scripts";
        bool enableHotReload = true;
        float hotReloadInterval = 1.0f;     ///< Check interval in seconds
    };

    /**
     * @brief Engine subsystem for script management
     *
     * Usage:
     * @code
     * // Get from engine
     * auto* scriptingSubsystem = engine.GetSubsystem<ScriptingSubsystem>();
     *
     * // Load and execute a script
     * ScriptHandle handle = scriptingSubsystem->LoadScript("game.lua");
     * scriptingSubsystem->ExecuteScript(handle);
     *
     * // Call a Lua function
     * scriptingSubsystem->CallGlobalFunction<void>("OnGameStart");
     * @endcode
     */
    class ScriptingSubsystem : public EngineSubsystem
    {
    public:
        // =====================================================================
        // Construction
        // =====================================================================

        ScriptingSubsystem();
        ~ScriptingSubsystem() override;

        // =====================================================================
        // ISubsystem Implementation
        // =====================================================================

        const char* GetName() const override { return "ScriptingSubsystem"; }
        void Initialize() override;
        void Deinitialize() override;
        void Tick(float deltaTime) override;
        bool ShouldTick() const override { return m_config.enableHotReload; }
        TickPhase GetTickPhase() const override { return TickPhase::PreUpdate; }

        // =====================================================================
        // Configuration
        // =====================================================================

        /**
         * @brief Configure the scripting subsystem (call before Initialize)
         * @param config Configuration options
         */
        void Configure(const ScriptingSubsystemConfig& config);

        /**
         * @brief Get current configuration
         */
        const ScriptingSubsystemConfig& GetConfig() const { return m_config; }

        // =====================================================================
        // Script Loading
        // =====================================================================

        /**
         * @brief Load a script from file
         * @param relativePath Path relative to scripts directory
         * @return Script handle (InvalidScriptHandle on failure)
         */
        ScriptHandle LoadScript(const std::filesystem::path& relativePath);

        /**
         * @brief Load a script from string
         * @param source Lua source code
         * @param name Name for debugging
         * @return Script handle (InvalidScriptHandle on failure)
         */
        ScriptHandle LoadScriptString(const std::string& source, const std::string& name = "inline");

        /**
         * @brief Execute a loaded script
         * @param handle Script handle
         * @return Execution result
         */
        ScriptResult ExecuteScript(ScriptHandle handle);

        /**
         * @brief Reload a script from disk
         * @param handle Script handle
         * @return true if reload succeeded
         */
        bool ReloadScript(ScriptHandle handle);

        /**
         * @brief Unload a script
         * @param handle Script handle
         */
        void UnloadScript(ScriptHandle handle);

        /**
         * @brief Get cached script info
         * @param handle Script handle
         * @return Pointer to cached script or nullptr
         */
        const CachedScript* GetScript(ScriptHandle handle) const;

        // =====================================================================
        // Direct Execution
        // =====================================================================

        /**
         * @brief Execute a Lua string directly
         * @param script Lua code
         * @return Execution result
         */
        ScriptResult ExecuteString(std::string_view script);

        /**
         * @brief Execute a Lua file directly (not cached)
         * @param filePath Full path to file
         * @return Execution result
         */
        ScriptResult ExecuteFile(const std::filesystem::path& filePath);

        // =====================================================================
        // Function Calls
        // =====================================================================

        /**
         * @brief Call a global Lua function
         * @tparam Ret Return type
         * @tparam Args Argument types
         * @param functionName Function name
         * @param args Arguments
         * @return Optional return value
         */
        template<typename Ret, typename... Args>
        std::optional<Ret> CallGlobalFunction(const std::string& functionName, Args&&... args);

        /**
         * @brief Call a global Lua function (void return)
         * @tparam Args Argument types
         * @param functionName Function name
         * @param args Arguments
         * @return Execution result
         */
        template<typename... Args>
        ScriptResult CallGlobalFunctionVoid(const std::string& functionName, Args&&... args);

        /**
         * @brief Check if a global function exists
         * @param functionName Function name
         * @return true if exists
         */
        bool HasGlobalFunction(const std::string& functionName) const;

        // =====================================================================
        // Variable Access
        // =====================================================================

        /**
         * @brief Get a global variable
         * @tparam T Type
         * @param name Variable name
         * @return Optional value
         */
        template<typename T>
        std::optional<T> GetGlobal(const std::string& name) const;

        /**
         * @brief Set a global variable
         * @tparam T Type
         * @param name Variable name
         * @param value Value
         */
        template<typename T>
        void SetGlobal(const std::string& name, T&& value);

        // =====================================================================
        // Binding Registration
        // =====================================================================

        /**
         * @brief Register all engine bindings
         *
         * Called automatically during Initialize, but can be called
         * again if you need to re-register after modifying state.
         */
        void RegisterBindings();

        /**
         * @brief Register a custom type
         * @tparam T Type to register
         * @param name Name in Lua
         * @param args Constructor and member definitions
         * @return Usertype reference
         */
        template<typename T, typename... Args>
        sol::usertype<T> RegisterType(const std::string& name, Args&&... args);

        /**
         * @brief Get or create a namespace table
         * @param name Namespace name
         * @return Table reference
         */
        sol::table GetOrCreateNamespace(const std::string& name);

        // =====================================================================
        // State Access
        // =====================================================================

        /**
         * @brief Get the underlying LuaState
         */
        LuaState& GetLuaState() { return m_luaState; }
        const LuaState& GetLuaState() const { return m_luaState; }

        /**
         * @brief Get sol::state directly
         */
        sol::state& GetState() { return m_luaState.GetState(); }
        const sol::state& GetState() const { return m_luaState.GetState(); }

        // =====================================================================
        // Component Management
        // =====================================================================

        /**
         * @brief Register a script component (called by ScriptComponent)
         */
        void RegisterComponent(ScriptComponent* component);

        /**
         * @brief Unregister a script component
         */
        void UnregisterComponent(ScriptComponent* component);

        /**
         * @brief Get all registered script components
         */
        const std::vector<ScriptComponent*>& GetComponents() const { return m_components; }

    private:
        LuaState m_luaState;
        ScriptingSubsystemConfig m_config;

        // Script cache
        std::unordered_map<ScriptHandle, CachedScript> m_scripts;
        std::unordered_map<std::string, ScriptHandle> m_pathToHandle;
        ScriptHandle m_nextHandle = 1;

        // Registered components
        std::vector<ScriptComponent*> m_components;

        // Hot reload
        float m_timeSinceLastCheck = 0.0f;

        void CheckForHotReload();
        ScriptHandle AllocateHandle();
    };

    // =========================================================================
    // Template Implementations
    // =========================================================================

    template<typename Ret, typename... Args>
    std::optional<Ret> ScriptingSubsystem::CallGlobalFunction(const std::string& functionName, Args&&... args)
    {
        return m_luaState.Call<Ret>(functionName, std::forward<Args>(args)...);
    }

    template<typename... Args>
    ScriptResult ScriptingSubsystem::CallGlobalFunctionVoid(const std::string& functionName, Args&&... args)
    {
        return m_luaState.CallVoid(functionName, std::forward<Args>(args)...);
    }

    template<typename T>
    std::optional<T> ScriptingSubsystem::GetGlobal(const std::string& name) const
    {
        return m_luaState.Get<T>(name);
    }

    template<typename T>
    void ScriptingSubsystem::SetGlobal(const std::string& name, T&& value)
    {
        m_luaState.Set(name, std::forward<T>(value));
    }

    template<typename T, typename... Args>
    sol::usertype<T> ScriptingSubsystem::RegisterType(const std::string& name, Args&&... args)
    {
        return m_luaState.RegisterType<T>(name, std::forward<Args>(args)...);
    }

} // namespace RVX