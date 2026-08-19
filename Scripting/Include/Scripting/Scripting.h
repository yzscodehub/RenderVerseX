#pragma once

/**
 * @file Scripting.h
 * @brief Unified header for Scripting module
 * 
 * Include this file to access all scripting functionality:
 * - LuaState (Lua VM wrapper)
 * - ScriptingSubsystem (engine subsystem for script management)
 * - Bindings (C++ to Lua type bindings)
 */

#include "Scripting/LuaState.h"
#include "Scripting/ScriptEngine.h"
#include "Scripting/Bindings/CoreBindings.h"
#include "Scripting/Bindings/MathBindings.h"
#include "Scripting/Bindings/InputBindings.h"
