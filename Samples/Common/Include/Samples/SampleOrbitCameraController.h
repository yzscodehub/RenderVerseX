#pragma once

/** @file SampleOrbitCameraController.h @brief Backend-neutral orbit camera input. */

#include "Core/MathTypes.h"

namespace RVX
{
    class CameraComponent;
    class InputSubsystem;

    struct SampleOrbitCameraSettings
    {
        Vec3 target{0.0f};
        float distance = 5.0f;
        float yaw = 0.0f;
        float pitch = 0.35f;
        float minDistance = 0.01f;
        float maxDistance = 100.0f;
        float minPitch = -1.5f;
        float maxPitch = 1.5f;
        float orbitSpeed = 0.005f;
        float zoomSpeed = 0.5f;
        float verticalFovRadians = 0.0f;
        float aspectRatio = 1.0f;
        float boundsRadius = 0.0f;
    };

    /** @brief Backend-neutral orbit intent shared by live and deterministic input. */
    struct SampleOrbitCameraInput
    {
        bool orbitActive = false;
        Vec2 pointerDelta{0.0f};
        float scrollDelta = 0.0f;
        bool reset = false;
    };

    class SampleOrbitCameraController final
    {
    public:
        void Initialize(const SampleOrbitCameraSettings& settings,
                        InputSubsystem* input = nullptr);
        void Update(InputSubsystem& input, CameraComponent& camera);
        void ApplyInput(const SampleOrbitCameraInput& input,
                        CameraComponent& camera);
        void SetAspectRatio(float aspectRatio, CameraComponent& camera);
        void Apply(CameraComponent& camera) const;
        void Reset();

        [[nodiscard]] const SampleOrbitCameraSettings& GetSettings() const noexcept
        {
            return m_current;
        }

    private:
        SampleOrbitCameraSettings m_initial;
        SampleOrbitCameraSettings m_current;
        float m_lastMouseX = 0.0f;
        float m_lastMouseY = 0.0f;
        bool m_initialized = false;
    };
} // namespace RVX
