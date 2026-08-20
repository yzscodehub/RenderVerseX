/**
 * @file GLFWMetalLayerBridge.h
 * @brief Main-thread Apple presentation-layer attachment for GLFW windows
 */

#pragma once

#include <cstdint>

struct GLFWwindow;

namespace RVX::HAL
{
    uintptr_t AttachGLFWMetalLayer(GLFWwindow* window, uintptr_t& nativeWindow);
    void DetachGLFWMetalLayer(GLFWwindow* window);

} // namespace RVX::HAL
