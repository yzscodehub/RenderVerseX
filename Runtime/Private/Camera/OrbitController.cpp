#include "Runtime/Camera/OrbitController.h"
#include "HAL/Input/KeyCodes.h"

namespace RVX
{
    void OrbitController::Update(Camera& camera, const HAL::InputState& input, float deltaTime)
    {
        // Rotate when left mouse button is held
        if (input.mouseButtons[HAL::MouseButton::Left])
        {
            Vec3 rotation = camera.GetRotation();
            rotation.y += input.mouseDeltaX * m_rotateSpeed * deltaTime;
            rotation.x += input.mouseDeltaY * m_rotateSpeed * deltaTime;
            camera.SetRotation(rotation);
        }

        // Zoom with scroll wheel
        if (input.mouseWheel != 0.0f)
        {
            m_distance -= input.mouseWheel * m_zoomSpeed * deltaTime;
            if (m_distance < 0.1f)
                m_distance = 0.1f;
        }

        // Calculate position based on orbit
        Vec3 rotation = camera.GetRotation();
        Vec3 offset{0.0f, 0.0f, m_distance};
        Mat4 rot = MakeRotationXYZ(rotation);
        
        // Extract Z column from rotation matrix (forward direction)
        Vec3 zAxis = Vec3(rot[2]);
        Vec3 position = zAxis * offset.z + m_target;
        
        camera.SetPosition(position);
    }
} // namespace RVX
