#pragma once

/**
 * @file HAL.h
 * @brief Hardware Abstraction Layer - Unified header
 * 
 * The HAL module provides platform-independent abstractions for:
 * - Window management
 * - Input handling (low-level backend)
 * - File system access
 * - Timer/Clock utilities
 */

#include "HAL/Window/IWindow.h"
#include "HAL/Window/WindowEvents.h"
#include "HAL/Input/IInputBackend.h"
#include "HAL/Input/Input.h"
#include "HAL/Input/InputState.h"
#include "HAL/Input/InputEvents.h"
#include "HAL/Input/KeyCodes.h"
