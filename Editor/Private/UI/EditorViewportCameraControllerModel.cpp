/**
 * @file EditorViewportCameraControllerModel.cpp
 * @brief Native viewport camera navigation controller implementation
 */

#include "Editor/UI/EditorViewportCameraControllerModel.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace RVX::Editor
{
namespace
{
    constexpr float RVX_CAMERA_CONTROLLER_EPSILON = 0.0001f;

    Vec3 SafeNormalize(const Vec3& value, const Vec3& fallback)
    {
        const float valueLength = length(value);
        return valueLength > RVX_CAMERA_CONTROLLER_EPSILON
                   ? value / valueLength
                   : fallback;
    }
} // namespace

void EditorViewportCameraControllerModel::ApplyOrbit(
    EditorViewportCameraControllerState& state,
    const EditorViewportCameraControllerInput& input)
{
    NormalizeFrame(state);
    m_lastStats = {};

    if (input.rotate)
    {
        state.yaw += input.mouseDelta.x * state.rotateSpeed;
        state.pitch -= input.mouseDelta.y * state.rotateSpeed;
        state.pitch = glm::clamp(state.pitch, -89.0f, 89.0f);
        m_lastStats.rotated = true;
    }

    const Vec3 forward = ComputeForward(state.yaw, state.pitch);
    const Vec3 right = SafeNormalize(glm::cross(forward, state.up),
                                     Vec3(1.0f, 0.0f, 0.0f));
    const Vec3 up = SafeNormalize(glm::cross(right, forward), state.up);

    if (input.pan)
    {
        const float panSpeed = state.distance * 0.002f;
        const Vec3 pan = -input.mouseDelta.x * panSpeed * right +
                         input.mouseDelta.y * panSpeed * up;
        state.target += pan;
        m_lastStats.panned = true;
    }

    if (std::abs(input.scrollDelta.y) > 0.0f)
    {
        const float zoomSpeed = state.distance * 0.1f;
        state.distance -= input.scrollDelta.y * zoomSpeed;
        state.distance = glm::clamp(state.distance,
                                    state.minDistance,
                                    state.maxDistance);
        m_lastStats.zoomed = true;
    }

    RebuildOrbitPosition(state);
    m_lastStats.forward = ComputeForward(state.yaw, state.pitch);
    m_lastStats.right = right;
    m_lastStats.up = up;
    m_lastStats.distance = state.distance;
}

void EditorViewportCameraControllerModel::ApplyFly(
    EditorViewportCameraControllerState& state,
    const EditorViewportCameraControllerInput& input)
{
    NormalizeFrame(state);
    m_lastStats = {};

    if (input.rotate)
    {
        state.yaw += input.mouseDelta.x * state.rotateSpeed;
        state.pitch -= input.mouseDelta.y * state.rotateSpeed;
        state.pitch = glm::clamp(state.pitch, -89.0f, 89.0f);
        m_lastStats.rotated = true;
    }

    const Vec3 forward = ComputeForward(state.yaw, state.pitch);
    const Vec3 right = SafeNormalize(glm::cross(forward, state.up),
                                     Vec3(1.0f, 0.0f, 0.0f));

    float speed = state.moveSpeed * std::max(0.0f, input.deltaTime);
    if (input.boost)
    {
        speed *= 3.0f;
    }

    Vec3 move(0.0f);
    if (input.moveForward)
    {
        move += forward;
    }
    if (input.moveBackward)
    {
        move -= forward;
    }
    if (input.moveLeft)
    {
        move -= right;
    }
    if (input.moveRight)
    {
        move += right;
    }
    if (input.moveDown)
    {
        move -= state.up;
    }
    if (input.moveUp)
    {
        move += state.up;
    }

    if (length(move) > RVX_CAMERA_CONTROLLER_EPSILON && speed > 0.0f)
    {
        state.position += SafeNormalize(move, Vec3(0.0f)) * speed;
        m_lastStats.moved = true;
    }

    state.target = state.position + forward * state.distance;
    m_lastStats.forward = forward;
    m_lastStats.right = right;
    m_lastStats.up = state.up;
    m_lastStats.distance = state.distance;
}

bool EditorViewportCameraControllerModel::AlignToViewDirection(
    EditorViewportCameraControllerState& state,
    const Vec3& directionFromTarget)
{
    if (length(directionFromTarget) <= RVX_CAMERA_CONTROLLER_EPSILON)
    {
        return false;
    }

    NormalizeFrame(state);
    Vec3 direction = SafeNormalize(directionFromTarget, Vec3(1.0f, 0.0f, 0.0f));
    state.distance = std::max(state.minDistance,
                              length(state.position - state.target));
    if (!std::isfinite(state.distance))
    {
        state.distance = 10.0f;
    }

    state.pitch =
        glm::degrees(std::asin(glm::clamp(direction.y, -0.999f, 0.999f)));
    state.yaw = glm::degrees(std::atan2(direction.z, direction.x));
    state.position = state.target + direction * state.distance;

    m_lastStats = {};
    m_lastStats.aligned = true;
    m_lastStats.forward = SafeNormalize(state.target - state.position,
                                        Vec3(-1.0f, 0.0f, 0.0f));
    m_lastStats.right =
        SafeNormalize(glm::cross(m_lastStats.forward, state.up),
                      Vec3(0.0f, 0.0f, -1.0f));
    m_lastStats.up =
        SafeNormalize(glm::cross(m_lastStats.right, m_lastStats.forward),
                      state.up);
    m_lastStats.distance = state.distance;
    return true;
}

Vec3 EditorViewportCameraControllerModel::ComputeForward(float yawDegrees,
                                                         float pitchDegrees)
{
    const float yawRad = glm::radians(yawDegrees);
    const float pitchRad = glm::radians(pitchDegrees);

    Vec3 forward;
    forward.x = std::cos(pitchRad) * std::cos(yawRad);
    forward.y = std::sin(pitchRad);
    forward.z = std::cos(pitchRad) * std::sin(yawRad);
    return SafeNormalize(forward, Vec3(0.0f, 0.0f, -1.0f));
}

void EditorViewportCameraControllerModel::NormalizeFrame(
    EditorViewportCameraControllerState& state)
{
    state.up = SafeNormalize(state.up, Vec3(0.0f, 1.0f, 0.0f));
    state.minDistance = std::max(RVX_CAMERA_CONTROLLER_EPSILON,
                                 state.minDistance);
    state.maxDistance = std::max(state.minDistance, state.maxDistance);
    if (!std::isfinite(state.distance) ||
        state.distance < state.minDistance)
    {
        state.distance = state.minDistance;
    }
    state.distance = std::min(state.distance, state.maxDistance);
}

void EditorViewportCameraControllerModel::RebuildOrbitPosition(
    EditorViewportCameraControllerState& state)
{
    const Vec3 direction = ComputeForward(state.yaw, state.pitch);
    state.position = state.target + direction * state.distance;
}

} // namespace RVX::Editor
