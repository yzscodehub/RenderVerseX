/** @file SampleOrbitCameraController.cpp @brief Input adapter for OrbitCameraRig. */

#include "Samples/SampleOrbitCameraController.h"

#include "HAL/Input/KeyCodes.h"
#include "Runtime/Input/InputSubsystem.h"

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

    bool SampleOrbitCameraController::Update(InputSubsystem& input,
                                             WorldECS::WorldEcsCameraService& cameras,
                                             WorldECS::WorldEcsCameraRef camera)
    {
        if (!m_rig.IsInitialized() || !IsUsableCamera(cameras, camera))
        {
            return false;
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
        return ApplyInput(cameraInput, cameras, camera);
    }

    bool SampleOrbitCameraController::ApplyInput(
        const SampleOrbitCameraInput& input,
        WorldECS::WorldEcsCameraService& cameras,
        WorldECS::WorldEcsCameraRef camera)
    {
        if (!IsUsableCamera(cameras, camera))
        {
            return false;
        }

        OrbitCameraIntent intent;
        intent.orbitActive = input.orbitActive;
        intent.orbitDelta = input.pointerDelta;
        intent.zoomDelta = input.scrollDelta;
        intent.reset = input.reset;
        m_rig.ApplyIntent(intent);
        return Apply(cameras, camera);
    }

    bool SampleOrbitCameraController::SetAspectRatio(
        float aspectRatio,
        WorldECS::WorldEcsCameraService& cameras,
        WorldECS::WorldEcsCameraRef camera)
    {
        if (!std::isfinite(aspectRatio) || aspectRatio <= 0.0f ||
            !IsUsableCamera(cameras, camera))
        {
            return false;
        }

        m_pendingAspectRatio = aspectRatio;
        m_hasPendingAspectRatio = true;
        if (m_rig.IsInitialized() && m_rig.SetAspectRatio(aspectRatio))
        {
            return Apply(cameras, camera);
        }
        return true;
    }

    bool SampleOrbitCameraController::SetFocus(const AABB& bounds,
                                                const Vec3& pivot,
                                                WorldECS::WorldEcsCameraService& cameras,
                                                WorldECS::WorldEcsCameraRef camera)
    {
        if (!IsUsableCamera(cameras, camera))
        {
            return false;
        }

        const bool changed = m_rig.SetFocus(bounds, pivot);
        return changed && Apply(cameras, camera);
    }

    bool SampleOrbitCameraController::Fit(WorldECS::WorldEcsCameraService& cameras,
                                           WorldECS::WorldEcsCameraRef camera)
    {
        if (!IsUsableCamera(cameras, camera))
        {
            return false;
        }

        const bool changed = m_rig.Fit();
        return changed && Apply(cameras, camera);
    }

    void SampleOrbitCameraController::CaptureResetAnchor()
    {
        m_rig.CaptureResetAnchor();
    }

    bool SampleOrbitCameraController::Apply(WorldECS::WorldEcsCameraService& cameras,
                                             WorldECS::WorldEcsCameraRef camera)
    {
        if (!IsUsableCamera(cameras, camera))
        {
            return false;
        }

        const OrbitCameraRigPose pose = m_rig.GetPose();
        if (!pose.valid)
        {
            return false;
        }

        if (!cameras.SetPerspective(camera,
                                    pose.verticalFovRadians,
                                    pose.aspectRatio,
                                    pose.nearPlane,
                                    pose.farPlane) ||
            !cameras.SetPose(camera, {.position = pose.position}) ||
            !cameras.LookAt(camera, pose.pivot))
        {
            return false;
        }
        return !m_rig.ConsumeDiscontinuity() || cameras.MarkCut(camera);
    }

    void SampleOrbitCameraController::Reset()
    {
        static_cast<void>(m_rig.Reset());
    }

    bool SampleOrbitCameraController::IsUsableCamera(
        const WorldECS::WorldEcsCameraService& cameras,
        WorldECS::WorldEcsCameraRef camera)
    {
        return camera.sceneRuntimeId == cameras.GetSceneRuntimeId() &&
               cameras.GetCamera(camera).has_value();
    }
} // namespace RVX
