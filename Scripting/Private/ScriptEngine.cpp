#include "Scripting/ScriptEngine.h"
#include "Scripting/ScriptComponent.h"
#include "Scripting/Bindings/CoreBindings.h"
#include "Scripting/Bindings/MathBindings.h"
#include "Scripting/Bindings/SceneBindings.h"
#include "Scripting/Bindings/InputBindings.h"

#include <fstream>
#include <sstream>

namespace RVX
{
    // =========================================================================
    // Construction
    // =========================================================================

    ScriptingSubsystem::ScriptingSubsystem() = default;

    ScriptingSubsystem::~ScriptingSubsystem()
    {
        Deinitialize();
    }

    // =========================================================================
    // ISubsystem Implementation
    // =========================================================================

    void ScriptingSubsystem::Initialize()
    {
        RVX_CORE_INFO("ScriptingSubsystem::Initialize");

        // Initialize Lua state
        if (!m_luaState.Initialize(m_config.luaConfig))
        {
            RVX_CORE_ERROR("ScriptingSubsystem - Failed to initialize Lua state");
            return;
        }

        // Add scripts directory to search path
        m_luaState.AddSearchPath(m_config.scriptsDirectory);

        // Register all bindings
        RegisterBindings();

        RVX_CORE_INFO("ScriptingSubsystem initialized with scripts directory: {}", 
                      m_config.scriptsDirectory.string());
    }

    void ScriptingSubsystem::Deinitialize()
    {
        RVX_CORE_INFO("ScriptingSubsystem::Deinitialize");

        // Clear all cached scripts
        m_scripts.clear();
        m_pathToHandle.clear();

        // Clear component references
        m_components.clear();

        // Shutdown Lua state
        m_luaState.Shutdown();
    }

    void ScriptingSubsystem::Tick(float deltaTime)
    {
        if (!m_config.enableHotReload)
        {
            return;
        }

        m_timeSinceLastCheck += deltaTime;
        if (m_timeSinceLastCheck >= m_config.hotReloadInterval)
        {
            m_timeSinceLastCheck = 0.0f;
            CheckForHotReload();
        }
    }

    // =========================================================================
    // Configuration
    // =========================================================================

    void ScriptingSubsystem::Configure(const ScriptingSubsystemConfig& config)
    {
        m_config = config;
    }

    // =========================================================================
    // Script Loading
    // =========================================================================

    ScriptHandle ScriptingSubsystem::LoadScript(const std::filesystem::path& relativePath)
    {
        std::string pathKey = relativePath.string();

        // Check if already loaded
        auto it = m_pathToHandle.find(pathKey);
        if (it != m_pathToHandle.end())
        {
            return it->second;
        }

        // Construct full path
        std::filesystem::path fullPath = m_config.scriptsDirectory / relativePath;
        
        if (!std::filesystem::exists(fullPath))
        {
            RVX_CORE_ERROR("ScriptingSubsystem::LoadScript - File not found: {}", fullPath.string());
            return InvalidScriptHandle;
        }

        // Read file contents
        std::ifstream file(fullPath);
        if (!file.is_open())
        {
            RVX_CORE_ERROR("ScriptingSubsystem::LoadScript - Failed to open: {}", fullPath.string());
            return InvalidScriptHandle;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string source = buffer.str();

        // Create cached script
        ScriptHandle handle = AllocateHandle();
        CachedScript& script = m_scripts[handle];
        script.handle = handle;
        script.filePath = fullPath;
        script.source = std::move(source);
        script.lastModified = std::filesystem::last_write_time(fullPath);
        script.isValid = true;

        m_pathToHandle[pathKey] = handle;

        RVX_CORE_INFO("ScriptingSubsystem::LoadScript - Loaded: {}", relativePath.string());
        return handle;
    }

    ScriptHandle ScriptingSubsystem::LoadScriptString(const std::string& source, const std::string& name)
    {
        ScriptHandle handle = AllocateHandle();
        CachedScript& script = m_scripts[handle];
        script.handle = handle;
        script.filePath = name;
        script.source = source;
        script.isValid = true;

        return handle;
    }

    ScriptResult ScriptingSubsystem::ExecuteScript(ScriptHandle handle)
    {
        auto it = m_scripts.find(handle);
        if (it == m_scripts.end())
        {
            return ScriptResult::Failure("Invalid script handle");
        }

        const CachedScript& script = it->second;
        if (!script.isValid)
        {
            return ScriptResult::Failure("Script is not valid");
        }

        return m_luaState.ExecuteString(script.source, script.filePath.string());
    }

    bool ScriptingSubsystem::ReloadScript(ScriptHandle handle)
    {
        auto it = m_scripts.find(handle);
        if (it == m_scripts.end())
        {
            return false;
        }

        CachedScript& script = it->second;
        if (script.filePath.empty() || !std::filesystem::exists(script.filePath))
        {
            return false;
        }

        // Read file contents
        std::ifstream file(script.filePath);
        if (!file.is_open())
        {
            return false;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        script.source = buffer.str();
        script.lastModified = std::filesystem::last_write_time(script.filePath);
        script.isValid = true;

        RVX_CORE_INFO("ScriptingSubsystem::ReloadScript - Reloaded: {}", script.filePath.string());
        return true;
    }

    void ScriptingSubsystem::UnloadScript(ScriptHandle handle)
    {
        auto it = m_scripts.find(handle);
        if (it != m_scripts.end())
        {
            // Remove from path map
            std::string pathKey = it->second.filePath.string();
            m_pathToHandle.erase(pathKey);

            // Remove from scripts
            m_scripts.erase(it);
        }
    }

    const CachedScript* ScriptingSubsystem::GetScript(ScriptHandle handle) const
    {
        auto it = m_scripts.find(handle);
        return it != m_scripts.end() ? &it->second : nullptr;
    }

    // =========================================================================
    // Direct Execution
    // =========================================================================

    ScriptResult ScriptingSubsystem::ExecuteString(std::string_view script)
    {
        return m_luaState.ExecuteString(script);
    }

    ScriptResult ScriptingSubsystem::ExecuteFile(const std::filesystem::path& filePath)
    {
        return m_luaState.ExecuteFile(filePath);
    }

    // =========================================================================
    // Function Calls
    // =========================================================================

    bool ScriptingSubsystem::HasGlobalFunction(const std::string& functionName) const
    {
        return m_luaState.HasFunction(functionName);
    }

    // =========================================================================
    // Binding Registration
    // =========================================================================

    void ScriptingSubsystem::RegisterBindings()
    {
        RVX_CORE_INFO("ScriptingSubsystem::RegisterBindings - Registering engine bindings");

        // Register core bindings
        Bindings::RegisterCoreBindings(m_luaState);

        // Register math bindings
        Bindings::RegisterMathBindings(m_luaState);

        // Register scene bindings
        Bindings::RegisterSceneBindings(m_luaState);

        // Register input bindings
        Bindings::RegisterInputBindings(m_luaState);

        RVX_CORE_INFO("ScriptingSubsystem::RegisterBindings - All bindings registered");
    }

    sol::table ScriptingSubsystem::GetOrCreateNamespace(const std::string& name)
    {
        return m_luaState.GetOrCreateNamespace(name);
    }

    // =========================================================================
    // Component Management
    // =========================================================================

    void ScriptingSubsystem::RegisterComponent(ScriptComponent* component)
    {
        if (component && std::find(m_components.begin(), m_components.end(), component) == m_components.end())
        {
            m_components.push_back(component);
        }
    }

    void ScriptingSubsystem::UnregisterComponent(ScriptComponent* component)
    {
        auto it = std::find(m_components.begin(), m_components.end(), component);
        if (it != m_components.end())
        {
            m_components.erase(it);
        }
    }

    // =========================================================================
    // Private Methods
    // =========================================================================

    void ScriptingSubsystem::CheckForHotReload()
    {
        for (auto& [handle, script] : m_scripts)
        {
            if (script.filePath.empty() || !std::filesystem::exists(script.filePath))
            {
                continue;
            }

            auto currentModTime = std::filesystem::last_write_time(script.filePath);
            if (currentModTime > script.lastModified)
            {
                RVX_CORE_INFO("ScriptingSubsystem - Hot reload detected: {}", script.filePath.string());
                
                if (ReloadScript(handle))
                {
                    // Re-execute the script
                    auto result = ExecuteScript(handle);
                    if (!result)
                    {
                        RVX_CORE_ERROR("ScriptingSubsystem - Hot reload execution failed: {}", result.errorMessage);
                    }

                    // Notify components that use this script
                    for (ScriptComponent* comp : m_components)
                    {
                        if (comp && comp->GetScriptHandle() == handle)
                        {
                            comp->OnScriptReloaded();
                        }
                    }
                }
            }
        }
    }

    ScriptHandle ScriptingSubsystem::AllocateHandle()
    {
        return m_nextHandle++;
    }

} // namespace RVX
