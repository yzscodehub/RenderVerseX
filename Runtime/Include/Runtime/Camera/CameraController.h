#pragma once

/**
 * @file CameraController.h
 * @brief Camera controller interface
 */

#include "Runtime/Camera/Camera.h"
#include "HAL/Input/InputState.h"

namespace RVX
{
    /**
     * @brief Abstract camera controller interface
     * 
     * Camera controllers update camera position/rotation based on input.
     */
    class CameraController
    {
    public:
        virtual ~CameraController() = default;
        
        /**
         * @brief Update camera based on input
         * @param camera Camera to update
         * @param input Current input state
         * @param deltaTime Time since last frame
         */
        virtual void Update(Camera& camera, const HAL::InputState& input, float deltaTime) = 0;
    };
} // namespace RVX
