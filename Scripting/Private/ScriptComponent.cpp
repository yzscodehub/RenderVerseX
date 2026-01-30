#include "Scripting/ScriptComponent.h"
#include "Scripting/ScriptEngine.h"
#include "Scene/SceneEntity.h"
#include "Core/Services.h"

namespace RVX
{
    // =========================================================================
    // Construction
    // =========================================================================

    ScriptComponent::ScriptComponent() = default;

    ScriptComponent::ScriptComponent(const std::filesystem::path& scriptPath)
        : m_scriptPath(scriptPath)
    {
    }

    ScriptComponent::~ScriptComponent()
    {
        Stop();
        DestroyInstance();
    }

    // =========================================================================
    // Component Implementation
    // =========================================================================

    void ScriptComponent::OnAttach()
    {
        // Try to get ScriptEngine from services
        m_engine = Services::Get<ScriptEngine>();
        
        if (m_engine)
        {
            m_engine->RegisterComponent(this);

            // If script path was set in constructor, load it now
            if (!m_scriptPath.empty())
            {
                SetScript(m_scriptPath);
            }
        }
        else
        {
            RVX_CORE_WARN("ScriptComponent::OnAttach - ScriptEngine not available");
        }
    }

    void ScriptComponent::OnDetach()
    {
        Stop();
        DestroyInstance();

        if (m_engine)
        {
            m_engine->UnregisterComponent(this);
            m_engine = nullptr;
        }
    }

    void ScriptComponent::Tick(float deltaTime)
    {
        if (!m_started || !m_instanceValid)
        {
            return;
        }

        // Call OnUpdate if it exists
        if (HasFunction("OnUpdate"))
        {
            CallFunctionVoid("OnUpdate", deltaTime);
        }
    }

    // =========================================================================
    // Script Management
    // =========================================================================

    bool ScriptComponent::SetScript(const std::filesystem::path& relativePath)
    {
        if (!m_engine)
        {
            RVX_CORE_ERROR("ScriptComponent::SetScript - No ScriptEngine available");
            return false;
        }

        // Stop current script if running
        if (m_started)
        {
            Stop();
        }

        // Destroy old instance
        DestroyInstance();

        m_scriptPath = relativePath;
        m_scriptHandle = m_engine->LoadScript(relativePath);

        if (m_scriptHandle == InvalidScriptHandle)
        {
            RVX_CORE_ERROR("ScriptComponent::SetScript - Failed to load: {}", relativePath.string());
            return false;
        }

        // Execute the script
        auto result = m_engine->ExecuteScript(m_scriptHandle);
        if (!result)
        {
            RVX_CORE_ERROR("ScriptComponent::SetScript - Execution failed: {}", result.errorMessage);
            return false;
        }

        // Create instance
        return CreateInstance();
    }

    bool ScriptComponent::SetScriptString(const std::string& source, const std::string& name)
    {
        if (!m_engine)
        {
            RVX_CORE_ERROR("ScriptComponent::SetScriptString - No ScriptEngine available");
            return false;
        }

        // Stop current script if running
        if (m_started)
        {
            Stop();
        }

        // Destroy old instance
        DestroyInstance();

        m_scriptPath = name;
        m_scriptHandle = m_engine->LoadScriptString(source, name);

        if (m_scriptHandle == InvalidScriptHandle)
        {
            return false;
        }

        // Execute the script
        auto result = m_engine->ExecuteScript(m_scriptHandle);
        if (!result)
        {
            RVX_CORE_ERROR("ScriptComponent::SetScriptString - Execution failed: {}", result.errorMessage);
            return false;
        }

        // Create instance
        return CreateInstance();
    }

    // =========================================================================
    // Lifecycle
    // =========================================================================

    void ScriptComponent::Start()
    {
        if (m_started || !m_instanceValid)
        {
            return;
        }

        m_started = true;

        // Set entity reference in the instance
        if (GetOwner())
        {
            m_instance["entity"] = GetOwner();
        }

        // Call OnStart
        CallLifecycleFunction("OnStart");

        RequestTick();
    }

    void ScriptComponent::Stop()
    {
        if (!m_started)
        {
            return;
        }

        // Call OnDestroy
        CallLifecycleFunction("OnDestroy");

        m_started = false;
    }

    void ScriptComponent::OnScriptReloaded()
    {
        RVX_CORE_INFO("ScriptComponent::OnScriptReloaded - {}", m_scriptPath.string());

        bool wasStarted = m_started;

        // Stop if running
        if (m_started)
        {
            Stop();
        }

        // Recreate instance
        DestroyInstance();
        
        if (CreateInstance() && wasStarted)
        {
            Start();
        }
    }

    // =========================================================================
    // Script Function Calls
    // =========================================================================

    bool ScriptComponent::HasFunction(const std::string& functionName) const
    {
        if (!m_instanceValid)
        {
            return false;
        }

        sol::object obj = m_instance[functionName];
        return obj.valid() && obj.is<sol::function>();
    }

    // =========================================================================
    // Private Methods
    // =========================================================================

    bool ScriptComponent::CreateInstance()
    {
        if (!m_engine)
        {
            return false;
        }

        sol::state& lua = m_engine->GetState();

        // Try to get the returned table from the script
        // The script should return a table that serves as the "class"
        // We create a new instance by copying it

        // Get the script name (without path and extension) as the global name
        std::string scriptName = m_scriptPath.stem().string();

        sol::object scriptClass = lua[scriptName];
        if (!scriptClass.valid())
        {
            // If no global with that name, create an empty instance
            m_instance = lua.create_table();
            m_instance["__name"] = scriptName;
            m_instanceValid = true;
            return true;
        }

        if (scriptClass.is<sol::table>())
        {
            // Create a new instance that inherits from the script class
            sol::table classTable = scriptClass.as<sol::table>();
            m_instance = lua.create_table();
            
            // Set metatable for inheritance
            sol::table metatable = lua.create_table();
            metatable["__index"] = classTable;
            m_instance[sol::metatable_key] = metatable;
            
            m_instance["__name"] = scriptName;
            m_instanceValid = true;

            RVX_CORE_INFO("ScriptComponent - Created instance of '{}'", scriptName);
            return true;
        }

        RVX_CORE_WARN("ScriptComponent - '{}' is not a table, creating empty instance", scriptName);
        m_instance = lua.create_table();
        m_instance["__name"] = scriptName;
        m_instanceValid = true;
        return true;
    }

    void ScriptComponent::DestroyInstance()
    {
        if (m_instanceValid)
        {
            // Clear the table
            m_instance = sol::table();
            m_instanceValid = false;
        }
    }

    void ScriptComponent::CallLifecycleFunction(const std::string& name)
    {
        if (!m_instanceValid)
        {
            return;
        }

        if (HasFunction(name))
        {
            auto result = CallFunctionVoid(name);
            if (!result)
            {
                RVX_CORE_ERROR("ScriptComponent::{} failed: {}", name, result.errorMessage);
            }
        }
    }

} // namespace RVX
