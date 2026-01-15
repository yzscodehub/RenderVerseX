#pragma once

#include "Camera/Camera.h"
#include "Input/InputState.h"

namespace RVX
{
    class CameraController
    {
    public:
        virtual ~CameraController() = default;
        virtual void Update(Camera& camera, const InputState& input, float deltaTime) = 0;
    };
} // namespace RVX
