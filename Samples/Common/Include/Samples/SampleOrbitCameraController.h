#pragma once

/** @file SampleOrbitCameraController.h @brief Input adapter for OrbitCameraRig. */

#include "Runtime/Camera/OrbitCameraRig.h"
#include "World/ECS/WorldEcsCameraService.h"

namespace RVX
{
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
     * @brief Keeps sample code to Input -> Intent -> Rig -> ECS camera values.
     *
     * All framing and collision decisions are implemented by OrbitCameraRig;
     * this adapter only observes sample input and writes its resolved pose.
     */
    class SampleOrbitCameraController final
    {
    public:
        void Initialize(const SampleOrbitCameraSettings& settings,
                        InputSubsystem* input = nullptr);
        [[nodiscard]] bool Update(InputSubsystem& input,
                                  WorldECS::WorldEcsCameraService& cameras,
                                  WorldECS::WorldEcsCameraRef camera);
        [[nodiscard]] bool ApplyInput(const SampleOrbitCameraInput& input,
                                      WorldECS::WorldEcsCameraService& cameras,
                                      WorldECS::WorldEcsCameraRef camera);
        [[nodiscard]] bool SetAspectRatio(float aspectRatio,
                                          WorldECS::WorldEcsCameraService& cameras,
                                          WorldECS::WorldEcsCameraRef camera);
        [[nodiscard]] bool SetFocus(const AABB& bounds,
                                    const Vec3& pivot,
                                    WorldECS::WorldEcsCameraService& cameras,
                                    WorldECS::WorldEcsCameraRef camera);
        [[nodiscard]] bool Fit(WorldECS::WorldEcsCameraService& cameras,
                               WorldECS::WorldEcsCameraRef camera);
        void CaptureResetAnchor();
        [[nodiscard]] bool Apply(WorldECS::WorldEcsCameraService& cameras,
                                 WorldECS::WorldEcsCameraRef camera);
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
        [[nodiscard]] static bool IsUsableCamera(
            const WorldECS::WorldEcsCameraService& cameras,
            WorldECS::WorldEcsCameraRef camera);

        OrbitCameraRig m_rig;
        float m_lastMouseX = 0.0f;
        float m_lastMouseY = 0.0f;
        float m_pendingAspectRatio = 1.0f;
        bool m_hasPendingAspectRatio = false;
    };
} // namespace RVX
