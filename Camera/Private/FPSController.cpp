#include "Camera/FPSController.h"

namespace RVX
{
    void FPSController::Update(Camera& camera, const InputState& input, float deltaTime)
    {
        Vec3 rotation = camera.GetRotation();
        rotation.y += input.mouseDeltaX * m_lookSpeed * deltaTime;
        rotation.x += input.mouseDeltaY * m_lookSpeed * deltaTime;
        camera.SetRotation(rotation);

        Vec3 forward{0.0f, 0.0f, -1.0f};
        Vec3 right{1.0f, 0.0f, 0.0f};

        Vec3 move{0.0f, 0.0f, 0.0f};
        if (input.keys['W']) move = move + forward;
        if (input.keys['S']) move = move - forward;
        if (input.keys['A']) move = move - right;
        if (input.keys['D']) move = move + right;

        if (move.x != 0.0f || move.y != 0.0f || move.z != 0.0f)
        {
            move = normalize(move) * (m_moveSpeed * deltaTime);
            camera.SetPosition(camera.GetPosition() + move);
        }
    }
} // namespace RVX
