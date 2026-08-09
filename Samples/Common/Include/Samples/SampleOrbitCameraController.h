#pragma once

/** @file SampleOrbitCameraController.h @brief Input adapter for OrbitCameraRig. */

#include "Runtime/Camera/OrbitCameraRig.h"

namespace RVX
{
    class CameraComponent;
    class InputSubsystem;

    /** @brief Sample-facing alias for the backend-neutral rig configuration. */
    using SampleOrbitCameraSettings = OrbitCameraRigSettings;

    /** @brief Raw sample input before it is converted into a rig intent. */
    struct SampleOrbitCameraInput
    {
        bool orbitActive = false;
        Vec2 pointerDelta{0.0f};
        float scrollDelta = 0.0f;
        bool reset = false;
    };

    /**
     * @brief Keeps sample code to Input -> Intent -> Rig -> CameraComponent.
     *
     * All framing and collision decisions are implemented by OrbitCameraRig;
     * this adapter only observes sample input and writes its resolved pose.
     */
    class SampleOrbitCameraController final
    {
    public:
        void Initialize(const SampleOrbitCameraSettings& settings,
                        InputSubsystem* input = nullptr);
        void Update(InputSubsystem& input, CameraComponent& camera);
        void ApplyInput(const SampleOrbitCameraInput& input,
                        CameraComponent& camera);
        void SetAspectRatio(float aspectRatio, CameraComponent& camera);
        bool SetFocus(const AABB& bounds,
                      const Vec3& pivot,
                      CameraComponent& camera);
        bool Fit(CameraComponent& camera);
        void CaptureResetAnchor();
        void Apply(CameraComponent& camera);
        void Reset();

        [[nodiscard]] const SampleOrbitCameraSettings& GetSettings() const noexcept
        {
            return m_rig.GetSettings();
        }

        [[nodiscard]] OrbitCameraRigPose GetPose() const
        {
            return m_rig.GetPose();
        }

        [[nodiscard]] bool IsInitialized() const noexcept
        {
            return m_rig.IsInitialized();
        }

    private:
        OrbitCameraRig m_rig;
        float m_lastMouseX = 0.0f;
        float m_lastMouseY = 0.0f;
        float m_pendingAspectRatio = 1.0f;
        bool m_hasPendingAspectRatio = false;
    };
} // namespace RVX
