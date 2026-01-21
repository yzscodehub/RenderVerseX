#pragma once

/**
 * @file Runtime.h
 * @brief Runtime module - Unified header
 * 
 * Runtime provides engine subsystems for:
 * - Window management (WindowSubsystem)
 * - Input handling (InputSubsystem)
 * - Time/clock utilities (Time, TimeSubsystem)
 * - Camera and controllers
 */

// Window
#include "Runtime/Window/WindowSubsystem.h"

// Input
#include "Runtime/Input/InputSubsystem.h"

// Time
#include "Runtime/Time/Time.h"
#include "Runtime/Time/TimeSubsystem.h"

// Camera
#include "Runtime/Camera/Camera.h"
#include "Runtime/Camera/CameraController.h"
#include "Runtime/Camera/FPSController.h"
#include "Runtime/Camera/OrbitController.h"
#include "Runtime/Camera/CameraRegistry.h"
