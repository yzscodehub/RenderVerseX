#pragma once

/**
 * @file MathBindings.h
 * @brief Math type bindings for Lua
 * 
 * Registers math types to Lua:
 * - Vec2, Vec3, Vec4
 * - Quat
 * - Mat4
 * - Math utility functions
 */

namespace RVX
{
    class LuaState;

    namespace Bindings
    {
        /**
         * @brief Register math bindings
         * 
         * Registers:
         * - Vec2, Vec3, Vec4 classes with operators
         * - Quat class with operators
         * - Mat4 class with operators
         * - RVX.Math namespace with utility functions
         * 
         * @param lua LuaState to register to
         */
        void RegisterMathBindings(LuaState& lua);

    } // namespace Bindings

} // namespace RVX
