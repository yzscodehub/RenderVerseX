#pragma once

/**
 * @file ScriptComponent.h
 * @brief Component for attaching Lua scripts to entities
 * 
 * The ScriptComponent allows entities to have custom behavior
 * defined in Lua scripts. Each component can have:
 * - A script file that defines behavior
 * - A Lua table instance for per-entity data
 * - Lifecycle callbacks (OnStart, OnUpdate, OnDestroy, etc.)
 */

#include "Scene/Component.h"
#include "Scripting/LuaState.h"
#include "Scripting/ScriptEngine.h"

#include <string>
#include <filesystem>

namespace RVX
{
    // Forward declarations
    class ScriptEngine;

    /**
     * @brief Component for attaching Lua scripts to entities
     * 
     * Usage:
     * @code
     * // In C++
     * auto* scriptComp = entity->AddComponent<ScriptComponent>();
     * scriptComp->SetScript("player.lua");
     * scriptComp->Start();
     * 
     * // In Lua (player.lua)
     * Player = {}
     * 
     * function Player:OnStart()
     *     print("Player started!")
     * end
     * 
     * function Player:OnUpdate(deltaTime)
     *     -- Update logic
     * end
     * 
     * return Player
     * @endcode
     */
    class ScriptComponent : public Component
    {
    public:
        // =====================================================================
        // Construction
        // =====================================================================

        ScriptComponent();
        explicit ScriptComponent(const std::filesystem::path& scriptPath);
        ~ScriptComponent() override;

        // =====================================================================
        // Component Implementation
        // =====================================================================

        const char* GetTypeName() const override { return "ScriptComponent"; }
        void OnAttach() override;
        void OnDetach() override;
        void Tick(float deltaTime) override;

        // =====================================================================
        // Script Management
        // =====================================================================

        /**
         * @brief Set the script file to use
         * @param relativePath Path relative to scripts directory
         * @return true if script loaded successfully
         */
        bool SetScript(const std::filesystem::path& relativePath);

        /**
         * @brief Set script from a string
         * @param source Lua source code
         * @param name Name for debugging
         * @return true if script loaded successfully
         */
        bool SetScriptString(const std::string& source, const std::string& name = "inline");

        /**
         * @brief Get the current script path
         */
        const std::filesystem::path& GetScriptPath() const { return m_scriptPath; }

        /**
         * @brief Get the script handle
         */
        ScriptHandle GetScriptHandle() const { return m_scriptHandle; }

        /**
         * @brief Check if script is loaded and valid
         */
        bool IsScriptValid() const { return m_scriptHandle != InvalidScriptHandle && m_instanceValid; }

        // =====================================================================
        // Lifecycle
        // =====================================================================

        /**
         * @brief Initialize and start the script
         * 
         * Calls the script's OnStart function if it exists.
         */
        void Start();

        /**
         * @brief Stop and cleanup the script
         * 
         * Calls the script's OnDestroy function if it exists.
         */
        void Stop();

        /**
         * @brief Check if script has started
         */
        bool IsStarted() const { return m_started; }

        /**
         * @brief Called when script is hot-reloaded
         */
        void OnScriptReloaded();

        // =====================================================================
        // Script Function Calls
        // =====================================================================

        /**
         * @brief Call a function on this script instance
         * @tparam Ret Return type
         * @tparam Args Argument types
         * @param functionName Function name
         * @param args Arguments
         * @return Optional return value
         */
        template<typename Ret, typename... Args>
        std::optional<Ret> CallFunction(const std::string& functionName, Args&&... args);

        /**
         * @brief Call a function on this script instance (void return)
         * @tparam Args Argument types
         * @param functionName Function name
         * @param args Arguments
         * @return Execution result
         */
        template<typename... Args>
        ScriptResult CallFunctionVoid(const std::string& functionName, Args&&... args);

        /**
         * @brief Check if the script instance has a function
         * @param functionName Function name
         * @return true if function exists
         */
        bool HasFunction(const std::string& functionName) const;

        // =====================================================================
        // Property Access
        // =====================================================================

        /**
         * @brief Get a property from the script instance
         * @tparam T Type
         * @param name Property name
         * @return Optional value
         */
        template<typename T>
        std::optional<T> GetProperty(const std::string& name) const;

        /**
         * @brief Set a property on the script instance
         * @tparam T Type
         * @param name Property name
         * @param value Value
         */
        template<typename T>
        void SetProperty(const std::string& name, T&& value);

        /**
         * @brief Get the Lua table instance for this component
         */
        sol::table& GetInstance() { return m_instance; }
        const sol::table& GetInstance() const { return m_instance; }

    private:
        ScriptEngine* m_engine = nullptr;
        std::filesystem::path m_scriptPath;
        ScriptHandle m_scriptHandle = InvalidScriptHandle;
        
        sol::table m_instance;      ///< Lua table instance for this component
        bool m_instanceValid = false;
        bool m_started = false;

        bool CreateInstance();
        void DestroyInstance();
        void CallLifecycleFunction(const std::string& name);
    };

    // =========================================================================
    // Template Implementations
    // =========================================================================

    template<typename Ret, typename... Args>
    std::optional<Ret> ScriptComponent::CallFunction(const std::string& functionName, Args&&... args)
    {
        if (!m_instanceValid)
        {
            return std::nullopt;
        }

        sol::object funcObj = m_instance[functionName];
        if (!funcObj.valid() || !funcObj.is<sol::function>())
        {
            return std::nullopt;
        }

        sol::function func = funcObj.as<sol::function>();
        sol::protected_function_result result = func(m_instance, std::forward<Args>(args)...);
        
        if (!result.valid())
        {
            sol::error err = result;
            RVX_CORE_ERROR("ScriptComponent::CallFunction - Error calling '{}': {}", 
                          functionName, err.what());
            return std::nullopt;
        }

        return result.get<Ret>();
    }

    template<typename... Args>
    ScriptResult ScriptComponent::CallFunctionVoid(const std::string& functionName, Args&&... args)
    {
        if (!m_instanceValid)
        {
            return ScriptResult::Failure("Script instance not valid");
        }

        sol::object funcObj = m_instance[functionName];
        if (!funcObj.valid() || !funcObj.is<sol::function>())
        {
            return ScriptResult::Failure("Function '" + functionName + "' not found");
        }

        sol::function func = funcObj.as<sol::function>();
        sol::protected_function_result result = func(m_instance, std::forward<Args>(args)...);
        
        if (!result.valid())
        {
            sol::error err = result;
            return ScriptResult::Failure(err.what());
        }

        return ScriptResult::Success();
    }

    template<typename T>
    std::optional<T> ScriptComponent::GetProperty(const std::string& name) const
    {
        if (!m_instanceValid)
        {
            return std::nullopt;
        }

        sol::object obj = m_instance[name];
        if (!obj.valid() || !obj.is<T>())
        {
            return std::nullopt;
        }

        return obj.as<T>();
    }

    template<typename T>
    void ScriptComponent::SetProperty(const std::string& name, T&& value)
    {
        if (m_instanceValid)
        {
            m_instance[name] = std::forward<T>(value);
        }
    }

} // namespace RVX
