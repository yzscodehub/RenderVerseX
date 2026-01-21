#pragma once

/**
 * @file Runtime.h
 * @brief Runtime module - Unified header
 * 
 * Runtime provides engine subsystems for:
 * - Window management (WindowSubsystem)
 * - Input handling (InputSubsystem)
 * - Time/clock utilities
 * - Camera (to be migrated from Camera module)
 */

// Window
#include "Runtime/Window/WindowSubsystem.h"

// Input
#include "Runtime/Input/InputSubsystem.h"

// Time
#include "Runtime/Time/Time.h"
