#include "Scripting/Bindings/CoreBindings.h"
#include "Scripting/LuaState.h"
#include "Core/Log.h"
#include "Core/Services.h"

#include <sstream>

namespace RVX::Bindings
{
    namespace
    {
        // =====================================================================
        // Logging Helpers
        // =====================================================================

        std::string FormatLuaArgs(sol::variadic_args va)
        {
            std::ostringstream oss;
            bool first = true;
            for (auto v : va)
            {
                if (!first) oss << " ";
                first = false;

                if (v.is<std::string>())
                {
                    oss << v.as<std::string>();
                }
                else if (v.is<double>())
                {
                    oss << v.as<double>();
                }
                else if (v.is<bool>())
                {
                    oss << (v.as<bool>() ? "true" : "false");
                }
                else if (v.is<sol::nil_t>())
                {
                    oss << "nil";
                }
                else if (v.is<sol::table>())
                {
                    oss << "table";
                }
                else if (v.is<sol::function>())
                {
                    oss << "function";
                }
                else
                {
                    oss << "userdata";
                }
            }
            return oss.str();
        }

        void LuaLogInfo(sol::variadic_args va)
        {
            RVX_CORE_INFO("[Lua] {}", FormatLuaArgs(va));
        }

        void LuaLogWarn(sol::variadic_args va)
        {
            RVX_CORE_WARN("[Lua] {}", FormatLuaArgs(va));
        }

        void LuaLogError(sol::variadic_args va)
        {
            RVX_CORE_ERROR("[Lua] {}", FormatLuaArgs(va));
        }

        void LuaLogDebug(sol::variadic_args va)
        {
            RVX_CORE_DEBUG("[Lua] {}", FormatLuaArgs(va));
        }

        // =====================================================================
        // Time Variables (updated by ScriptEngine each frame)
        // =====================================================================

        float s_deltaTime = 0.0f;
        float s_totalTime = 0.0f;

        float GetDeltaTime() { return s_deltaTime; }
        float GetTotalTime() { return s_totalTime; }

    } // anonymous namespace

    // =========================================================================
    // Public API
    // =========================================================================

    void RegisterCoreBindings(LuaState& lua)
    {
        sol::state& state = lua.GetState();

        // Create RVX namespace
        sol::table rvx = lua.GetOrCreateNamespace("RVX");

        // =====================================================================
        // Log table
        // =====================================================================
        sol::table log = state.create_table();
        log["Info"] = &LuaLogInfo;
        log["Warn"] = &LuaLogWarn;
        log["Error"] = &LuaLogError;
        log["Debug"] = &LuaLogDebug;
        rvx["Log"] = log;

        // Override global print to use our logging
        state["print"] = &LuaLogInfo;

        // =====================================================================
        // Time table
        // =====================================================================
        sol::table time = state.create_table();
        time["GetDeltaTime"] = &GetDeltaTime;
        time["GetTotalTime"] = &GetTotalTime;
        rvx["Time"] = time;

        // =====================================================================
        // Utility functions
        // =====================================================================

        // Type checking helpers
        rvx["IsNil"] = [](sol::object obj) { return obj.is<sol::nil_t>(); };
        rvx["IsNumber"] = [](sol::object obj) { return obj.is<double>(); };
        rvx["IsString"] = [](sol::object obj) { return obj.is<std::string>(); };
        rvx["IsBool"] = [](sol::object obj) { return obj.is<bool>(); };
        rvx["IsTable"] = [](sol::object obj) { return obj.is<sol::table>(); };
        rvx["IsFunction"] = [](sol::object obj) { return obj.is<sol::function>(); };

        // Class creation helper
        state.script(R"(
            function RVX.Class(name, base)
                local cls = {}
                cls.__name = name
                cls.__index = cls
                
                if base then
                    setmetatable(cls, { __index = base })
                end
                
                function cls:new(...)
                    local instance = setmetatable({}, cls)
                    if instance.Init then
                        instance:Init(...)
                    end
                    return instance
                end
                
                return cls
            end
        )");

        RVX_CORE_INFO("CoreBindings registered");
    }

    // =========================================================================
    // Time Update (called by ScriptingSubsystem)
    // =========================================================================

    namespace
    {
        // These can be called from ScriptingSubsystem to update time values
    }

} // namespace RVX::Bindings
