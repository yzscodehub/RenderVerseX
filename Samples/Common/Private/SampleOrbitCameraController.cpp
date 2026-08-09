/** @file SampleOrbitCameraController.cpp @brief Input adapter for OrbitCameraRig. */

#include "Samples/SampleOrbitCameraController.h"

#include "HAL/Input/KeyCodes.h"
#include "Runtime/Input/InputSubsystem.h"
#include "Scene/Components/CameraComponent.h"

#include <cmath>

namespace RVX
{
    void SampleOrbitCameraController::Initialize(
        const SampleOrbitCameraSettings& settings,
        InputSubsystem* input)
    {
        SampleOrbitCameraSettings resolvedSettings = settings;
        if (m_hasPendingAspectRatio)
        {
            resolvedSettings.aspectRatio = m_pendingAspectRatio;
        }

        if (!m_rig.Initialize(resolvedSettings))
        {
            return;
        }
        static_cast<void>(m_rig.Fit());
        m_rig.CaptureResetAnchor();
        if (input)
        {
            input->GetMousePosition(m_lastMouseX, m_lastMouseY);
        }
    }

    void SampleOrbitCameraController::Update(InputSubsystem& input,
                                             CameraComponent& camera)
    {
        if (!m_rig.IsInitialized())
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
        cameraInput.orbitActive = input.IsMouseButtonDown(MouseButton::Left);
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
        OrbitCameraIntent intent;
        intent.orbitActive = input.orbitActive;
        intent.orbitDelta = input.pointerDelta;
        intent.zoomDelta = input.scrollDelta;
        intent.reset = input.reset;
        m_rig.ApplyIntent(intent);
        Apply(camera);
    }

    void SampleOrbitCameraController::SetAspectRatio(
        float aspectRatio,
        CameraComponent& camera)
    {
        if (!std::isfinite(aspectRatio) || aspectRatio <= 0.0f)
        {
            return;
        }

        m_pendingAspectRatio = aspectRatio;
        m_hasPendingAspectRatio = true;
        if (m_rig.IsInitialized() && m_rig.SetAspectRatio(aspectRatio))
        {
            Apply(camera);
        }
    }

    bool SampleOrbitCameraController::SetFocus(const AABB& bounds,
                                                const Vec3& pivot,
                                                CameraComponent& camera)
    {
        const bool changed = m_rig.SetFocus(bounds, pivot);
        if (changed)
        {
            Apply(camera);
        }
        return changed;
    }

    bool SampleOrbitCameraController::Fit(CameraComponent& camera)
    {
        const bool changed = m_rig.Fit();
        if (changed)
        {
            Apply(camera);
        }
        return changed;
    }

    void SampleOrbitCameraController::CaptureResetAnchor()
    {
        m_rig.CaptureResetAnchor();
    }

    void SampleOrbitCameraController::Apply(CameraComponent& camera)
    {
        const OrbitCameraRigPose pose = m_rig.GetPose();
        if (!pose.valid)
        {
            return;
        }

        camera.SetPerspective(pose.verticalFovRadians,
                              pose.aspectRatio,
                              pose.nearPlane,
                              pose.farPlane);
        camera.SetPosition(pose.position);
        camera.LookAt(pose.pivot);
        if (m_rig.ConsumeDiscontinuity())
        {
            camera.MarkCut();
        }
    }

    void SampleOrbitCameraController::Reset()
    {
        static_cast<void>(m_rig.Reset());
    }
} // namespace RVX
