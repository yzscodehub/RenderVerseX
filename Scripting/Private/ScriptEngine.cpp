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

    ScriptEngine::ScriptEngine() = default;

    ScriptEngine::~ScriptEngine()
    {
        Deinitialize();
    }

    // =========================================================================
    // ISubsystem Implementation
    // =========================================================================

    void ScriptEngine::Initialize()
    {
        RVX_CORE_INFO("ScriptEngine::Initialize");

        // Initialize Lua state
        if (!m_luaState.Initialize(m_config.luaConfig))
        {
            RVX_CORE_ERROR("ScriptEngine - Failed to initialize Lua state");
            return;
        }

        // Add scripts directory to search path
        m_luaState.AddSearchPath(m_config.scriptsDirectory);

        // Register all bindings
        RegisterBindings();

        RVX_CORE_INFO("ScriptEngine initialized with scripts directory: {}", 
                      m_config.scriptsDirectory.string());
    }

    void ScriptEngine::Deinitialize()
    {
        RVX_CORE_INFO("ScriptEngine::Deinitialize");

        // Clear all cached scripts
        m_scripts.clear();
        m_pathToHandle.clear();

        // Clear component references
        m_components.clear();

        // Shutdown Lua state
        m_luaState.Shutdown();
    }

    void ScriptEngine::Tick(float deltaTime)
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

    void ScriptEngine::Configure(const ScriptEngineConfig& config)
    {
        m_config = config;
    }

    // =========================================================================
    // Script Loading
    // =========================================================================

    ScriptHandle ScriptEngine::LoadScript(const std::filesystem::path& relativePath)
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
            RVX_CORE_ERROR("ScriptEngine::LoadScript - File not found: {}", fullPath.string());
            return InvalidScriptHandle;
        }

        // Read file contents
        std::ifstream file(fullPath);
        if (!file.is_open())
        {
            RVX_CORE_ERROR("ScriptEngine::LoadScript - Failed to open: {}", fullPath.string());
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

        RVX_CORE_INFO("ScriptEngine::LoadScript - Loaded: {}", relativePath.string());
        return handle;
    }

    ScriptHandle ScriptEngine::LoadScriptString(const std::string& source, const std::string& name)
    {
        ScriptHandle handle = AllocateHandle();
        CachedScript& script = m_scripts[handle];
        script.handle = handle;
        script.filePath = name;
        script.source = source;
        script.isValid = true;

        return handle;
    }

    ScriptResult ScriptEngine::ExecuteScript(ScriptHandle handle)
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

    bool ScriptEngine::ReloadScript(ScriptHandle handle)
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

        RVX_CORE_INFO("ScriptEngine::ReloadScript - Reloaded: {}", script.filePath.string());
        return true;
    }

    void ScriptEngine::UnloadScript(ScriptHandle handle)
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

    const CachedScript* ScriptEngine::GetScript(ScriptHandle handle) const
    {
        auto it = m_scripts.find(handle);
        return it != m_scripts.end() ? &it->second : nullptr;
    }

    // =========================================================================
    // Direct Execution
    // =========================================================================

    ScriptResult ScriptEngine::ExecuteString(std::string_view script)
    {
        return m_luaState.ExecuteString(script);
    }

    ScriptResult ScriptEngine::ExecuteFile(const std::filesystem::path& filePath)
    {
        return m_luaState.ExecuteFile(filePath);
    }

    // =========================================================================
    // Function Calls
    // =========================================================================

    bool ScriptEngine::HasGlobalFunction(const std::string& functionName) const
    {
        return m_luaState.HasFunction(functionName);
    }

    // =========================================================================
    // Binding Registration
    // =========================================================================

    void ScriptEngine::RegisterBindings()
    {
        RVX_CORE_INFO("ScriptEngine::RegisterBindings - Registering engine bindings");

        // Register core bindings
        Bindings::RegisterCoreBindings(m_luaState);

        // Register math bindings
        Bindings::RegisterMathBindings(m_luaState);

        // Register scene bindings
        Bindings::RegisterSceneBindings(m_luaState);

        // Register input bindings
        Bindings::RegisterInputBindings(m_luaState);

        RVX_CORE_INFO("ScriptEngine::RegisterBindings - All bindings registered");
    }

    sol::table ScriptEngine::GetOrCreateNamespace(const std::string& name)
    {
        return m_luaState.GetOrCreateNamespace(name);
    }

    // =========================================================================
    // Component Management
    // =========================================================================

    void ScriptEngine::RegisterComponent(ScriptComponent* component)
    {
        if (component && std::find(m_components.begin(), m_components.end(), component) == m_components.end())
        {
            m_components.push_back(component);
        }
    }

    void ScriptEngine::UnregisterComponent(ScriptComponent* component)
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

    void ScriptEngine::CheckForHotReload()
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
                RVX_CORE_INFO("ScriptEngine - Hot reload detected: {}", script.filePath.string());
                
                if (ReloadScript(handle))
                {
                    // Re-execute the script
                    auto result = ExecuteScript(handle);
                    if (!result)
                    {
                        RVX_CORE_ERROR("ScriptEngine - Hot reload execution failed: {}", 
                                      result.errorMessage);
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

    ScriptHandle ScriptEngine::AllocateHandle()
    {
        return m_nextHandle++;
    }

} // namespace RVX
