#pragma once

/**
 * @file Debug.h
 * @brief Debug Module - Unified header for profiling and debugging tools
 * 
 * This module provides:
 * - CPUProfiler: High-resolution CPU timing and scope profiling
 * - MemoryTracker: Allocation tracking and leak detection
 * - Console: Command execution system
 * - CVarSystem: Configuration variables
 * - StatsHUD: Runtime statistics display
 * - DebugSubsystem: Engine integration subsystem
 */

// Core profiling
#include "Debug/CPUProfiler.h"
#include "Debug/MemoryTracker.h"

// Console system
#include "Debug/Console.h"
#include "Debug/CVarSystem.h"

// Statistics display
#include "Debug/StatsHUD.h"

// Engine integration
#include "Debug/DebugSubsystem.h"
