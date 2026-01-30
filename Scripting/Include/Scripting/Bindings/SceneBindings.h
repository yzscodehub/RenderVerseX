#pragma once

/**
 * @file SceneBindings.h
 * @brief Scene system bindings for Lua
 * 
 * Registers scene-related types to Lua:
 * - SceneEntity
 * - Component (base)
 * - Transform access
 * - Hierarchy traversal
 */

namespace RVX
{
    class LuaState;

    namespace Bindings
    {
        /**
         * @brief Register scene bindings
         * 
         * Registers:
         * - SceneEntity class with transform and hierarchy methods
         * - Component base class
         * - SceneManager access
         * 
         * @param lua LuaState to register to
         */
        void RegisterSceneBindings(LuaState& lua);

    } // namespace Bindings

} // namespace RVX
