#pragma once

/**
 * @file InputBindings.h
 * @brief Input system bindings for Lua
 * 
 * Registers input-related types to Lua:
 * - Key codes
 * - Mouse button codes
 * - Input polling functions
 * - Input action map (if available)
 */

namespace RVX
{
    class LuaState;
    class InputSubsystem;

    namespace Bindings
    {
        /**
         * @brief Register input bindings
         *
         * Registers:
         * - RVX.Input namespace with polling functions
         * - Key enum with all key codes
         * - MouseButton enum
         *
         * @param lua LuaState to register to
         */
        void RegisterInputBindings(LuaState& lua);

        // Input Sync API (exposed to ScriptEngine)
        void SyncInputCache(const InputSubsystem* input);

    } // namespace Bindings

} // namespace RVX
