#include "Scripting/LuaState.h"

#include <fstream>
#include <sstream>

namespace RVX
{
    // =========================================================================
    // Construction
    // =========================================================================

    LuaState::LuaState() = default;

    LuaState::~LuaState()
    {
        Shutdown();
    }

    LuaState::LuaState(LuaState&& other) noexcept
        : m_state(std::move(other.m_state))
        , m_initialized(other.m_initialized)
        , m_config(other.m_config)
    {
        other.m_initialized = false;
    }

    LuaState& LuaState::operator=(LuaState&& other) noexcept
    {
        if (this != &other)
        {
            Shutdown();
            m_state = std::move(other.m_state);
            m_initialized = other.m_initialized;
            m_config = other.m_config;
            other.m_initialized = false;
        }
        return *this;
    }

    // =========================================================================
    // Initialization
    // =========================================================================

    bool LuaState::Initialize(const LuaStateConfig& config)
    {
        if (m_initialized)
        {
            RVX_CORE_WARN("LuaState::Initialize - Already initialized");
            return true;
        }

        m_config = config;

        try
        {
            // Install panic handler if enabled
            if (config.enablePanic)
            {
                lua_atpanic(m_state.lua_state(), &LuaState::PanicHandler);
            }

            // Load requested libraries
            LoadLibraries(config.libraries);

            m_initialized = true;
            RVX_CORE_INFO("LuaState initialized successfully");
            return true;
        }
        catch (const std::exception& e)
        {
            RVX_CORE_ERROR("LuaState::Initialize - Failed: {}", e.what());
            return false;
        }
    }

    void LuaState::Shutdown()
    {
        if (!m_initialized)
        {
            return;
        }

        // Run garbage collection before shutdown
        CollectGarbage();

        m_initialized = false;
        RVX_CORE_INFO("LuaState shutdown");
    }

    void LuaState::LoadLibraries(LuaLibrary libs)
    {
        if (HasFlag(libs, LuaLibrary::Base))
        {
            m_state.open_libraries(sol::lib::base);
        }
        if (HasFlag(libs, LuaLibrary::Package))
        {
            m_state.open_libraries(sol::lib::package);
        }
        if (HasFlag(libs, LuaLibrary::Coroutine))
        {
            m_state.open_libraries(sol::lib::coroutine);
        }
        if (HasFlag(libs, LuaLibrary::String))
        {
            m_state.open_libraries(sol::lib::string);
        }
        if (HasFlag(libs, LuaLibrary::OS))
        {
            m_state.open_libraries(sol::lib::os);
        }
        if (HasFlag(libs, LuaLibrary::Math))
        {
            m_state.open_libraries(sol::lib::math);
        }
        if (HasFlag(libs, LuaLibrary::Table))
        {
            m_state.open_libraries(sol::lib::table);
        }
        if (HasFlag(libs, LuaLibrary::Debug))
        {
            m_state.open_libraries(sol::lib::debug);
        }
        if (HasFlag(libs, LuaLibrary::IO))
        {
            m_state.open_libraries(sol::lib::io);
        }
        if (HasFlag(libs, LuaLibrary::UTF8))
        {
            m_state.open_libraries(sol::lib::utf8);
        }
    }

    int LuaState::PanicHandler(lua_State* L)
    {
        const char* msg = lua_tostring(L, -1);
        RVX_CORE_CRITICAL("Lua PANIC: {}", msg ? msg : "Unknown error");
        return 0;
    }

    // =========================================================================
    // Script Execution
    // =========================================================================

    ScriptResult LuaState::ExecuteString(std::string_view script, const std::string& chunkName)
    {
        if (!m_initialized)
        {
            return ScriptResult::Failure("State not initialized");
        }

        try
        {
            sol::protected_function_result result = m_state.safe_script(
                script, 
                sol::script_pass_on_error,
                chunkName
            );

            if (!result.valid())
            {
                sol::error err = result;
                return ScriptResult::Failure(err.what());
            }

            return ScriptResult::Success();
        }
        catch (const std::exception& e)
        {
            return ScriptResult::Failure(e.what());
        }
    }

    ScriptResult LuaState::ExecuteFile(const std::filesystem::path& filePath)
    {
        if (!m_initialized)
        {
            return ScriptResult::Failure("State not initialized");
        }

        if (!std::filesystem::exists(filePath))
        {
            return ScriptResult::Failure("File not found: " + filePath.string());
        }

        try
        {
            sol::protected_function_result result = m_state.safe_script_file(
                filePath.string(),
                sol::script_pass_on_error
            );

            if (!result.valid())
            {
                sol::error err = result;
                return ScriptResult::Failure(err.what());
            }

            return ScriptResult::Success();
        }
        catch (const std::exception& e)
        {
            return ScriptResult::Failure(e.what());
        }
    }

    sol::load_result LuaState::LoadString(std::string_view script, const std::string& chunkName)
    {
        return m_state.load(script, chunkName);
    }

    bool LuaState::HasFunction(const std::string& functionName) const
    {
        if (!m_initialized)
        {
            return false;
        }

        sol::object obj = m_state[functionName];
        return obj.valid() && obj.is<sol::function>();
    }

    // =========================================================================
    // Namespace Management
    // =========================================================================

    sol::table LuaState::GetOrCreateNamespace(const std::string& name)
    {
        sol::object existing = m_state[name];
        if (existing.valid() && existing.is<sol::table>())
        {
            return existing.as<sol::table>();
        }

        sol::table newTable = m_state.create_table();
        m_state[name] = newTable;
        return newTable;
    }

    void LuaState::AddSearchPath(const std::filesystem::path& path)
    {
        if (!m_initialized)
        {
            return;
        }

        std::string pathStr = path.string();
        // Replace backslashes with forward slashes
        for (char& c : pathStr)
        {
            if (c == '\\') c = '/';
        }

        // Append to package.path
        std::string packagePath = m_state["package"]["path"];
        packagePath += ";" + pathStr + "/?.lua";
        packagePath += ";" + pathStr + "/?/init.lua";
        m_state["package"]["path"] = packagePath;
    }

    // =========================================================================
    // Memory Management
    // =========================================================================

    size_t LuaState::GetMemoryUsage() const
    {
        if (!m_initialized)
        {
            return 0;
        }

        return static_cast<size_t>(lua_gc(m_state.lua_state(), LUA_GCCOUNT, 0)) * 1024 +
               static_cast<size_t>(lua_gc(m_state.lua_state(), LUA_GCCOUNTB, 0));
    }

    void LuaState::CollectGarbage()
    {
        if (m_initialized)
        {
            lua_gc(m_state.lua_state(), LUA_GCCOLLECT, 0);
        }
    }

} // namespace RVX
