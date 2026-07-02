/**
 * @file EditorViewportCameraControllerModel.h
 * @brief Native viewport camera navigation controller model
 */

#pragma once

#include "Core/MathTypes.h"
#include "Core/Types.h"

namespace RVX::Editor
{

struct EditorViewportCameraControllerState
{
    Vec3 position{0.0f, 5.0f, 10.0f};
    Vec3 target{0.0f, 0.0f, 0.0f};
    Vec3 up{0.0f, 1.0f, 0.0f};
    float yaw = -90.0f;
    float pitch = -15.0f;
    float distance = 10.0f;
    float moveSpeed = 10.0f;
    float rotateSpeed = 0.3f;
    float minDistance = 0.1f;
    float maxDistance = 1000.0f;
};

struct EditorViewportCameraControllerInput
{
    Vec2 mouseDelta{0.0f};
    Vec2 scrollDelta{0.0f};
    float deltaTime = 0.0f;
    bool rotate = false;
    bool pan = false;
    bool boost = false;
    bool moveForward = false;
    bool moveBackward = false;
    bool moveLeft = false;
    bool moveRight = false;
    bool moveDown = false;
    bool moveUp = false;
};

struct EditorViewportCameraControllerStats
{
    bool rotated = false;
    bool panned = false;
    bool zoomed = false;
    bool moved = false;
    bool aligned = false;
    Vec3 forward{0.0f, 0.0f, -1.0f};
    Vec3 right{1.0f, 0.0f, 0.0f};
    Vec3 up{0.0f, 1.0f, 0.0f};
    float distance = 10.0f;
};

class EditorViewportCameraControllerModel
{
public:
    EditorViewportCameraControllerModel() = default;

    void ApplyOrbit(EditorViewportCameraControllerState& state,
                    const EditorViewportCameraControllerInput& input);
    void ApplyFly(EditorViewportCameraControllerState& state,
                  const EditorViewportCameraControllerInput& input);
    bool AlignToViewDirection(EditorViewportCameraControllerState& state,
                              const Vec3& directionFromTarget);

    const EditorViewportCameraControllerStats& GetLastStats() const
    {
        return m_lastStats;
    }

    static Vec3 ComputeForward(float yawDegrees, float pitchDegrees);

private:
    static void NormalizeFrame(EditorViewportCameraControllerState& state);
    static void RebuildOrbitPosition(EditorViewportCameraControllerState& state);

    EditorViewportCameraControllerStats m_lastStats;
};

} // namespace RVX::Editor
