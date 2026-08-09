#pragma once

/**
 * @file OrbitCameraRig.h
 * @brief Backend-neutral data model for an orbit inspection camera.
 */

#include "Core/Math/AABB.h"
#include "Core/Types.h"

namespace RVX
{
    /** @brief Selects whether the orbit camera must remain outside its bounds. */
    enum class OrbitCameraMode : uint8
    {
        ExteriorInspect = 0,
        FreeOrbit
    };

    /** @brief Projection and interaction state owned by an orbit camera rig. */
    struct OrbitCameraRigSettings
    {
        OrbitCameraMode mode = OrbitCameraMode::ExteriorInspect;
        AABB bounds;
        Vec3 pivot{0.0f};
        float distance = 5.0f;
        float yaw = 0.0f;
        float pitch = 0.35f;
        float minDistance = 0.001f;
        float maxDistance = 100.0f;
        float minPitch = -1.5f;
        float maxPitch = 1.5f;
        float orbitRadiansPerPixel = 0.005f;
        float zoomExponent = 0.08f;
        float verticalFovRadians = 0.78539816339f;
        float aspectRatio = 1.0f;
        float fitMargin = 1.10f;
        float exteriorMarginScale = 0.02f;
    };

    /** @brief Semantic interaction event consumed by OrbitCameraRig. */
    struct OrbitCameraIntent
    {
        bool orbitActive = false;
        Vec2 orbitDelta{0.0f};
        float zoomDelta = 0.0f;
        bool reset = false;
    };

    /** @brief World-space view basis for the rig's current yaw and pitch. */
    struct OrbitCameraViewBasis
    {
        Vec3 forward{0.0f, 0.0f, -1.0f};
        Vec3 right{1.0f, 0.0f, 0.0f};
        Vec3 up{0.0f, 1.0f, 0.0f};
    };

    /** @brief Fully resolved camera state ready to be written to a component. */
    struct OrbitCameraRigPose
    {
        Vec3 position{0.0f};
        Vec3 pivot{0.0f};
        OrbitCameraViewBasis viewBasis;
        float distance = 0.0f;
        float verticalFovRadians = 0.0f;
        float aspectRatio = 1.0f;
        float nearPlane = 0.1f;
        float farPlane = 1000.0f;
        bool valid = false;
    };

    /** @brief Scale-aware clipping range for an orbit camera. */
    struct OrbitCameraClipRange
    {
        float nearPlane = 0.1f;
        float farPlane = 1000.0f;
        bool valid = false;
    };

    /**
     * @brief Generates aspect-aware orbit poses from a model bound and intent.
     *
     * The rig owns no scene or input objects. Consumers provide semantic input,
     * then write GetPose() to their camera representation.
     */
    class OrbitCameraRig final
    {
    public:
        bool Initialize(const OrbitCameraRigSettings& settings);

        /** @brief Update yaw, pitch, and exponential zoom from an input intent. */
        void ApplyIntent(const OrbitCameraIntent& intent);

        /** @brief Update the viewport aspect without creating a camera cut. */
        bool SetAspectRatio(float aspectRatio);

        /** @brief Set a new bounds/pivot focus and fit it in the current basis. */
        bool SetFocus(const AABB& bounds, const Vec3& pivot);
        bool SetFocus(const AABB& bounds);

        /** @brief Fit the current bounds in the current view basis. */
        bool Fit();

        /** @brief Save the current state as the target for a later Reset(). */
        void CaptureResetAnchor();

        /** @brief Compatibility spelling for CaptureResetAnchor(). */
        void CaptureAnchor() { CaptureResetAnchor(); }

        /** @brief Restore the last captured reset anchor. */
        bool Reset();

        /** @brief Return and clear a coalesced Fit/Focus/Reset discontinuity. */
        bool ConsumeDiscontinuity();

        [[nodiscard]] bool IsInitialized() const noexcept { return m_initialized; }
        [[nodiscard]] const OrbitCameraRigSettings& GetSettings() const noexcept
        {
            return m_current;
        }
        [[nodiscard]] OrbitCameraViewBasis GetViewBasis() const;
        [[nodiscard]] OrbitCameraRigPose GetPose() const;

        /** @brief Current lower zoom bound, including exterior support when active. */
        [[nodiscard]] float GetMinimumDistance() const;

        /** @brief Build scale-aware near/far planes for an orbit camera. */
        static OrbitCameraClipRange BuildClipRange(
            float distance,
            float boundsRadius,
            float nearRadiusMargin = 1.10f,
            float farRadiusMargin = 2.0f);

    private:
        [[nodiscard]] bool HasFiniteBounds() const;
        [[nodiscard]] float GetBoundsRadius() const;
        [[nodiscard]] float GetExteriorMinimumDistance(
            const OrbitCameraViewBasis& basis) const;
        [[nodiscard]] float ClampDistance(float distance) const;
        [[nodiscard]] bool FitInternal();
        void RequestDiscontinuity();

        OrbitCameraRigSettings m_current;
        OrbitCameraRigSettings m_resetAnchor;
        bool m_initialized = false;
        bool m_discontinuityPending = false;
    };
} // namespace RVX
