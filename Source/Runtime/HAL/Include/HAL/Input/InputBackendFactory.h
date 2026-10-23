#pragma once

/**
 * @file InputBackendFactory.h
 * @brief Public factory functions for platform input backends.
 */

#include "HAL/Input/GamepadState.h"
#include "HAL/Input/IInputBackend.h"

#include <memory>

namespace RVX::HAL
{
    /**
     * @brief Create the default keyboard/mouse backend for a platform window.
     * @param internalWindowHandle Opaque handle returned by IWindow::GetInternalHandle().
     */
    std::unique_ptr<IInputBackend> CreateInputBackendForWindow(void* internalWindowHandle);

    /**
     * @brief Create the default gamepad backend for the current platform.
     */
    std::unique_ptr<IGamepadBackend> CreateGamepadBackend();

} // namespace RVX::HAL
