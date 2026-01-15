#pragma once

#include "Core/ISystem.h"
#include "Camera/Camera.h"
#include "Camera/CameraController.h"
#include "Engine/Systems/InputSystem.h"
#include <memory>

namespace RVX
{
    class CameraSystem : public ISystem
    {
    public:
        const char* GetName() const override { return "CameraSystem"; }

        void SetInputSystem(InputSystem* inputSystem) { m_inputSystem = inputSystem; }
        void SetController(std::unique_ptr<CameraController> controller) { m_controller = std::move(controller); }
        Camera& GetCamera() { return m_camera; }
        const Camera& GetCamera() const { return m_camera; }

        void OnUpdate(float deltaTime) override
        {
            if (m_controller && m_inputSystem)
            {
                m_controller->Update(m_camera, m_inputSystem->GetState(), deltaTime);
            }
        }

    private:
        InputSystem* m_inputSystem = nullptr;
        Camera m_camera;
        std::unique_ptr<CameraController> m_controller;
    };
} // namespace RVX
