#pragma once

/**
 * @file CoreBindings.h
 * @brief Core type bindings for Lua
 * 
 * Registers core engine types and utilities to Lua:
 * - Logging functions
 * - Time functions
 * - Utility functions
 */

namespace RVX
{
    class LuaState;

    namespace Bindings
    {
        /**
         * @brief Register core bindings
         * 
         * Registers:
         * - RVX.Log.Info/Warn/Error/Debug
         * - RVX.Time.GetDeltaTime/GetTotalTime
         * - print (redirected to logging)
         * 
         * @param lua LuaState to register to
         */
        void RegisterCoreBindings(LuaState& lua);

    } // namespace Bindings

} // namespace RVX
