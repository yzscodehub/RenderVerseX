/** @file SampleOrbitCameraController.cpp @brief Orbit camera implementation. */

#include "Samples/SampleOrbitCameraController.h"

#include "HAL/Input/KeyCodes.h"
#include "Runtime/Input/InputSubsystem.h"
#include "Samples/ModelCameraFraming.h"
#include "Scene/Components/CameraComponent.h"

#include <algorithm>
#include <cmath>

namespace RVX
{
    void SampleOrbitCameraController::Initialize(
        const SampleOrbitCameraSettings& settings,
        InputSubsystem* input)
    {
        m_initial = settings;
        m_initial.distance = std::max(settings.distance, 0.001f);
        m_initial.minDistance = std::max(settings.minDistance, 0.001f);
        m_initial.maxDistance =
            std::max(settings.maxDistance, m_initial.minDistance);
        m_initial.distance = std::clamp(m_initial.distance,
                                       m_initial.minDistance,
                                       m_initial.maxDistance);
        m_initial.pitch = std::clamp(settings.pitch,
                                    settings.minPitch,
                                    settings.maxPitch);
        if (!std::isfinite(m_initial.verticalFovRadians) ||
            m_initial.verticalFovRadians <= 0.0f ||
            !std::isfinite(m_initial.aspectRatio) ||
            m_initial.aspectRatio <= 0.0f ||
            !std::isfinite(m_initial.boundsRadius) ||
            m_initial.boundsRadius <= 0.0f)
        {
            m_initial.verticalFovRadians = 0.0f;
            m_initial.aspectRatio = 1.0f;
            m_initial.boundsRadius = 0.0f;
        }
        m_current = m_initial;
        if (input)
        {
            input->GetMousePosition(m_lastMouseX, m_lastMouseY);
        }
        m_initialized = true;
    }

    void SampleOrbitCameraController::Update(InputSubsystem& input,
                                             CameraComponent& camera)
    {
        if (!m_initialized)
        {
            return;
        }

        float mouseX = 0.0f;
        float mouseY = 0.0f;
        input.GetMousePosition(mouseX, mouseY);
        float scrollX = 0.0f;
        float scrollY = 0.0f;
        input.GetScrollDelta(scrollX, scrollY);
        static_cast<void>(scrollX);

        SampleOrbitCameraInput cameraInput;
        cameraInput.orbitActive =
            input.IsMouseButtonDown(MouseButton::Left);
        cameraInput.pointerDelta =
            Vec2(mouseX - m_lastMouseX, mouseY - m_lastMouseY);
        cameraInput.scrollDelta = scrollY;
        cameraInput.reset = input.IsKeyPressed(Key::R);

        m_lastMouseX = mouseX;
        m_lastMouseY = mouseY;
        ApplyInput(cameraInput, camera);
    }

    void SampleOrbitCameraController::ApplyInput(
        const SampleOrbitCameraInput& input,
        CameraComponent& camera)
    {
        if (!m_initialized)
        {
            return;
        }

        if (input.orbitActive)
        {
            m_current.yaw -= input.pointerDelta.x * m_current.orbitSpeed;
            m_current.pitch = std::clamp(
                m_current.pitch +
                    input.pointerDelta.y * m_current.orbitSpeed,
                m_current.minPitch,
                m_current.maxPitch);
        }
        if (input.scrollDelta != 0.0f)
        {
            m_current.distance = std::clamp(
                m_current.distance -
                    input.scrollDelta * m_current.zoomSpeed,
                m_current.minDistance,
                m_current.maxDistance);
        }
        if (input.reset)
        {
            Reset();
        }

        Apply(camera);
    }

    void SampleOrbitCameraController::SetAspectRatio(
        float aspectRatio,
        CameraComponent& camera)
    {
        if (!m_initialized || !std::isfinite(aspectRatio) ||
            aspectRatio <= 0.0f)
        {
            return;
        }
        m_initial.aspectRatio = aspectRatio;
        m_current.aspectRatio = aspectRatio;
        Apply(camera);
    }

    void SampleOrbitCameraController::Apply(CameraComponent& camera) const
    {
        if (!m_initialized)
        {
            return;
        }

        const float horizontalDistance =
            m_current.distance * std::cos(m_current.pitch);
        const Vec3 position = m_current.target +
                              Vec3(horizontalDistance * std::sin(m_current.yaw),
                                   m_current.distance * std::sin(m_current.pitch),
                                   horizontalDistance * std::cos(m_current.yaw));
        if (m_current.boundsRadius > 0.0f)
        {
            const ModelCameraClipRange clipRange = BuildModelCameraClipRange(
                m_current.distance,
                m_current.boundsRadius);
            if (clipRange.valid)
            {
                camera.SetPerspective(m_current.verticalFovRadians,
                                      m_current.aspectRatio,
                                      clipRange.nearPlane,
                                      clipRange.farPlane);
            }
        }
        camera.SetPosition(position);
        camera.LookAt(m_current.target);
    }

    void SampleOrbitCameraController::Reset()
    {
        if (m_initialized)
        {
            m_current = m_initial;
        }
    }
} // namespace RVX
