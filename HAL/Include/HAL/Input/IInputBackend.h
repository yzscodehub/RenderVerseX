#pragma once

/**
 * @file IInputBackend.h
 * @brief Low-level input backend interface
 */

#include "Core/Types.h"

namespace RVX::HAL
{
    struct InputState;

    /**
     * @brief Abstract input backend interface
     * 
     * Platform-specific implementations (GLFW, Win32, etc.) derive from this
     * to provide raw input polling.
     */
    class IInputBackend
    {
    public:
        virtual ~IInputBackend() = default;

        /**
         * @brief Poll input state from the platform
         * @param state Output state to fill with current input data
         */
        virtual void Poll(InputState& state) = 0;
    };

} // namespace RVX::HAL

// Backward compatibility
namespace RVX
{
    using InputBackend = HAL::IInputBackend;
}
