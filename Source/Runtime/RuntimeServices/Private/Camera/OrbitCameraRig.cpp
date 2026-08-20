#include "Runtime/Camera/OrbitCameraRig.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace RVX
{
    namespace
    {
        constexpr float kPi = 3.14159265359f;
        constexpr float kMinimumDistance = 0.000001f;

        bool IsFiniteVector(const Vec3& value)
        {
            return std::isfinite(value.x) && std::isfinite(value.y) &&
                   std::isfinite(value.z);
        }

        float GetFiniteVectorLength(const Vec3& value)
        {
            if (!IsFiniteVector(value))
            {
                return 0.0f;
            }

            const float largestComponent = std::max(
                std::abs(value.x),
                std::max(std::abs(value.y), std::abs(value.z)));
            if (largestComponent <= 0.0f)
            {
                return 0.0f;
            }

            const Vec3 scaled = value / largestComponent;
            const float length = largestComponent * std::sqrt(
                dot(scaled, scaled));
            return std::isfinite(length) ? length : 0.0f;
        }

        bool IsSameVector(const Vec3& lhs, const Vec3& rhs)
        {
            return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
        }

        bool IsSameBounds(const AABB& lhs, const AABB& rhs)
        {
            return IsSameVector(lhs.GetMin(), rhs.GetMin()) &&
                   IsSameVector(lhs.GetMax(), rhs.GetMax());
        }
    } // namespace

    bool OrbitCameraRig::Initialize(const OrbitCameraRigSettings& settings)
    {
        if (!IsFiniteVector(settings.pivot) ||
            !std::isfinite(settings.distance) ||
            !std::isfinite(settings.yaw) || !std::isfinite(settings.pitch) ||
            !std::isfinite(settings.minDistance) ||
            !std::isfinite(settings.maxDistance) ||
            !std::isfinite(settings.minPitch) ||
            !std::isfinite(settings.maxPitch) ||
            !std::isfinite(settings.orbitRadiansPerPixel) ||
            !std::isfinite(settings.zoomExponent) ||
            !std::isfinite(settings.verticalFovRadians) ||
            !std::isfinite(settings.aspectRatio) ||
            !std::isfinite(settings.fitMargin) ||
            !std::isfinite(settings.exteriorMarginScale) ||
            !std::isfinite(settings.minimumFramingScale) ||
            !std::isfinite(settings.nearClipPaddingScale) ||
            !std::isfinite(settings.farClipPaddingScale) ||
            settings.verticalFovRadians <= 0.0f ||
            settings.verticalFovRadians >= kPi || settings.aspectRatio <= 0.0f ||
            settings.fitMargin < 1.0f || settings.exteriorMarginScale < 0.0f ||
            settings.minimumFramingScale < 0.0f ||
            settings.minimumFramingScale > 1.0f ||
            settings.nearClipPaddingScale < 0.0f ||
            settings.farClipPaddingScale < 0.0f)
        {
            return false;
        }

        m_current = settings;
        m_current.minDistance = std::max(settings.minDistance, kMinimumDistance);
        m_current.maxDistance = std::max(settings.maxDistance,
                                         m_current.minDistance);
        m_current.minPitch = std::min(settings.minPitch, settings.maxPitch);
        m_current.maxPitch = std::max(settings.minPitch, settings.maxPitch);
        const float pitchLimit = kPi * 0.5f - 0.001f;
        m_current.minPitch = std::max(m_current.minPitch, -pitchLimit);
        m_current.maxPitch = std::min(m_current.maxPitch, pitchLimit);
        if (m_current.minPitch > m_current.maxPitch)
        {
            return false;
        }
        m_current.pitch = std::clamp(m_current.pitch,
                                     m_current.minPitch,
                                     m_current.maxPitch);
        m_initialized = true;
        m_current.distance = ClampDistance(settings.distance);
        m_resetAnchor = m_current;
        m_discontinuityPending = false;
        return true;
    }

    void OrbitCameraRig::ApplyIntent(const OrbitCameraIntent& intent)
    {
        if (!m_initialized)
        {
            return;
        }

        if (intent.reset)
        {
            static_cast<void>(Reset());
            return;
        }

        if (intent.orbitActive && IsFiniteVector(Vec3(intent.orbitDelta, 0.0f)))
        {
            m_current.yaw -= intent.orbitDelta.x *
                             m_current.orbitRadiansPerPixel;
            m_current.pitch = std::clamp(
                m_current.pitch + intent.orbitDelta.y *
                                      m_current.orbitRadiansPerPixel,
                m_current.minPitch,
                m_current.maxPitch);
        }
        if (std::isfinite(intent.zoomDelta) && intent.zoomDelta != 0.0f)
        {
            const float exponent = std::clamp(
                -intent.zoomDelta * m_current.zoomExponent,
                -20.0f,
                20.0f);
            m_current.distance = ClampDistance(
                m_current.distance * std::exp(exponent));
        }
        else
        {
            m_current.distance = ClampDistance(m_current.distance);
        }
    }

    bool OrbitCameraRig::SetAspectRatio(float aspectRatio)
    {
        if (!m_initialized || !std::isfinite(aspectRatio) || aspectRatio <= 0.0f)
        {
            return false;
        }

        m_current.aspectRatio = aspectRatio;
        m_resetAnchor.aspectRatio = aspectRatio;
        if (m_current.mode == OrbitCameraMode::ExteriorInspect &&
            HasFiniteBounds())
        {
            const float preservedDistance = m_current.distance;
            if (!FitInternal())
            {
                return false;
            }
            m_current.distance = ClampDistance(
                std::max(preservedDistance, m_current.distance));
            m_resetAnchor.distance = std::max(m_resetAnchor.distance,
                                              m_current.distance);
        }
        return true;
    }

    bool OrbitCameraRig::SetFocus(const AABB& bounds, const Vec3& pivot)
    {
        if (!m_initialized || !bounds.IsValid() || !IsFiniteVector(bounds.GetMin()) ||
            !IsFiniteVector(bounds.GetMax()) || !IsFiniteVector(pivot))
        {
            return false;
        }
        if (IsSameBounds(m_current.bounds, bounds) &&
            IsSameVector(m_current.pivot, pivot))
        {
            return false;
        }

        const OrbitCameraRigSettings previous = m_current;
        m_current.bounds = bounds;
        m_current.pivot = pivot;
        if (!FitInternal())
        {
            m_current = previous;
            return false;
        }
        RequestDiscontinuity();
        return true;
    }

    bool OrbitCameraRig::SetFocus(const AABB& bounds)
    {
        return SetFocus(bounds, bounds.GetCenter());
    }

    bool OrbitCameraRig::Fit()
    {
        if (!m_initialized || !FitInternal())
        {
            return false;
        }

        RequestDiscontinuity();
        return true;
    }

    void OrbitCameraRig::CaptureResetAnchor()
    {
        if (m_initialized)
        {
            m_resetAnchor = m_current;
        }
    }

    bool OrbitCameraRig::Reset()
    {
        if (!m_initialized)
        {
            return false;
        }

        m_current = m_resetAnchor;
        m_current.distance = ClampDistance(m_current.distance);
        RequestDiscontinuity();
        return true;
    }

    bool OrbitCameraRig::ConsumeDiscontinuity()
    {
        const bool pending = m_discontinuityPending;
        m_discontinuityPending = false;
        return pending;
    }

    OrbitCameraViewBasis OrbitCameraRig::GetViewBasis() const
    {
        OrbitCameraViewBasis basis;
        if (!m_initialized)
        {
            return basis;
        }

        const float cosPitch = std::cos(m_current.pitch);
        basis.forward = normalize(Vec3(-cosPitch * std::sin(m_current.yaw),
                                       -std::sin(m_current.pitch),
                                       -cosPitch * std::cos(m_current.yaw)));
        basis.right = normalize(cross(basis.forward, Vec3(0.0f, 1.0f, 0.0f)));
        basis.up = normalize(cross(basis.right, basis.forward));
        return basis;
    }

    OrbitCameraRigPose OrbitCameraRig::GetPose() const
    {
        OrbitCameraRigPose pose;
        if (!m_initialized)
        {
            return pose;
        }

        pose.viewBasis = GetViewBasis();
        pose.pivot = m_current.pivot;
        pose.distance = ClampDistance(m_current.distance);
        pose.position = pose.pivot - pose.viewBasis.forward * pose.distance;
        pose.verticalFovRadians = m_current.verticalFovRadians;
        pose.aspectRatio = m_current.aspectRatio;
        OrbitCameraClipRange clipRange;
        if (HasFiniteBounds())
        {
            clipRange = BuildFocusedClipRange(
                m_current.bounds,
                pose.position,
                pose.viewBasis.forward,
                m_current.nearClipPaddingScale,
                m_current.farClipPaddingScale);
        }
        else
        {
            clipRange = BuildClipRange(pose.distance, GetBoundsRadius());
        }
        if (clipRange.valid)
        {
            pose.nearPlane = clipRange.nearPlane;
            pose.farPlane = clipRange.farPlane;
        }
        pose.valid = IsFiniteVector(pose.position) &&
                     IsFiniteVector(pose.pivot) &&
                     std::isfinite(pose.distance) && pose.distance > 0.0f &&
                     std::isfinite(pose.verticalFovRadians) &&
                     pose.verticalFovRadians > 0.0f &&
                     std::isfinite(pose.aspectRatio) && pose.aspectRatio > 0.0f;
        return pose;
    }

    float OrbitCameraRig::GetMinimumDistance() const
    {
        if (!m_initialized)
        {
            return 0.0f;
        }

        const OrbitCameraViewBasis basis = GetViewBasis();
        float minimum = GetBaseMinimumDistance(basis);
        if (m_current.mode == OrbitCameraMode::ExteriorInspect &&
            HasFiniteBounds() && m_current.minimumFramingScale > 0.0f)
        {
            const float fittedDistance = CalculateFitDistance(basis);
            if (std::isfinite(fittedDistance))
            {
                minimum = std::max(
                    minimum,
                    fittedDistance * m_current.minimumFramingScale);
            }
        }
        return minimum;
    }

    OrbitCameraClipRange OrbitCameraRig::BuildClipRange(
        float distance,
        float boundsRadius,
        float nearRadiusMargin,
        float farRadiusMargin)
    {
        OrbitCameraClipRange range;
        if (!std::isfinite(distance) || distance <= 0.0f ||
            !std::isfinite(boundsRadius) || boundsRadius <= 0.0f ||
            !std::isfinite(nearRadiusMargin) || nearRadiusMargin < 1.0f ||
            !std::isfinite(farRadiusMargin) || farRadiusMargin <= 0.0f)
        {
            return range;
        }

        const float scaleAwareNearFloor = std::max(
            boundsRadius * 0.001f,
            std::numeric_limits<float>::epsilon());
        range.nearPlane = std::max(scaleAwareNearFloor,
                                   distance - boundsRadius * nearRadiusMargin);
        range.farPlane = std::max(range.nearPlane + boundsRadius,
                                  distance + boundsRadius * farRadiusMargin);
        range.valid = std::isfinite(range.nearPlane) &&
                      std::isfinite(range.farPlane) && range.nearPlane > 0.0f &&
                      range.farPlane > range.nearPlane;
        return range;
    }

    OrbitCameraClipRange OrbitCameraRig::BuildFocusedClipRange(
        const AABB& bounds,
        const Vec3& cameraPosition,
        const Vec3& viewForward,
        float nearPaddingScale,
        float farPaddingScale)
    {
        OrbitCameraClipRange range;
        if (!bounds.IsValid() || !IsFiniteVector(bounds.GetMin()) ||
            !IsFiniteVector(bounds.GetMax()) ||
            !IsFiniteVector(cameraPosition) || !IsFiniteVector(viewForward) ||
            !std::isfinite(nearPaddingScale) || nearPaddingScale < 0.0f ||
            !std::isfinite(farPaddingScale) || farPaddingScale < 0.0f)
        {
            return range;
        }

        const float forwardLength = GetFiniteVectorLength(viewForward);
        const float boundsRadius = GetFiniteVectorLength(bounds.GetExtent());
        if (forwardLength <= kMinimumDistance ||
            boundsRadius <= kMinimumDistance)
        {
            return range;
        }

        const Vec3 forward = viewForward / forwardLength;
        const Vec3 min = bounds.GetMin();
        const Vec3 max = bounds.GetMax();
        const Vec3 corners[8] = {
            Vec3(min.x, min.y, min.z), Vec3(max.x, min.y, min.z),
            Vec3(min.x, max.y, min.z), Vec3(max.x, max.y, min.z),
            Vec3(min.x, min.y, max.z), Vec3(max.x, min.y, max.z),
            Vec3(min.x, max.y, max.z), Vec3(max.x, max.y, max.z)};

        float nearestDepth = std::numeric_limits<float>::infinity();
        float farthestDepth = -std::numeric_limits<float>::infinity();
        for (const Vec3& corner : corners)
        {
            const float depth = dot(corner - cameraPosition, forward);
            nearestDepth = std::min(nearestDepth, depth);
            farthestDepth = std::max(farthestDepth, depth);
        }
        if (!std::isfinite(nearestDepth) || !std::isfinite(farthestDepth))
        {
            return range;
        }

        const float scaleAwareNearFloor = std::max(
            boundsRadius * 0.001f,
            std::numeric_limits<float>::epsilon());
        const float nearPadding = std::max(
            boundsRadius * nearPaddingScale,
            scaleAwareNearFloor);
        if (nearestDepth > std::numeric_limits<float>::epsilon())
        {
            range.nearPlane = std::max(
                scaleAwareNearFloor,
                nearestDepth - nearPadding);
            if (range.nearPlane >= nearestDepth)
            {
                range.nearPlane = std::max(
                    std::numeric_limits<float>::epsilon(),
                    nearestDepth * 0.5f);
            }
        }
        else
        {
            // FreeOrbit may intentionally place the camera inside the focus.
            range.nearPlane = scaleAwareNearFloor;
        }

        const float farPadding = std::max(
            boundsRadius * farPaddingScale,
            scaleAwareNearFloor);
        const float minimumDepthSpan = std::max(
            boundsRadius * 0.01f,
            std::numeric_limits<float>::epsilon());
        range.farPlane = std::max(
            farthestDepth + farPadding,
            range.nearPlane + minimumDepthSpan);
        range.valid = std::isfinite(range.nearPlane) &&
                      std::isfinite(range.farPlane) &&
                      range.nearPlane > 0.0f &&
                      range.farPlane > range.nearPlane;
        return range;
    }

    bool OrbitCameraRig::HasFiniteBounds() const
    {
        return m_current.bounds.IsValid() &&
               IsFiniteVector(m_current.bounds.GetMin()) &&
               IsFiniteVector(m_current.bounds.GetMax()) &&
               GetBoundsRadius() > kMinimumDistance;
    }

    float OrbitCameraRig::GetBoundsRadius() const
    {
        if (!m_current.bounds.IsValid())
        {
            return 0.0f;
        }

        return GetFiniteVectorLength(m_current.bounds.GetExtent());
    }

    float OrbitCameraRig::GetBaseMinimumDistance(
        const OrbitCameraViewBasis& basis) const
    {
        float minimum = m_current.minDistance;
        if (m_current.mode == OrbitCameraMode::ExteriorInspect &&
            HasFiniteBounds())
        {
            minimum = std::max(minimum, GetExteriorMinimumDistance(basis));
        }
        return minimum;
    }

    float OrbitCameraRig::GetExteriorMinimumDistance(
        const OrbitCameraViewBasis& basis) const
    {
        if (!HasFiniteBounds())
        {
            return m_current.minDistance;
        }

        const Vec3 min = m_current.bounds.GetMin();
        const Vec3 max = m_current.bounds.GetMax();
        const Vec3 corners[8] = {
            Vec3(min.x, min.y, min.z), Vec3(max.x, min.y, min.z),
            Vec3(min.x, max.y, min.z), Vec3(max.x, max.y, min.z),
            Vec3(min.x, min.y, max.z), Vec3(max.x, min.y, max.z),
            Vec3(min.x, max.y, max.z), Vec3(max.x, max.y, max.z)};

        float support = -std::numeric_limits<float>::infinity();
        for (const Vec3& corner : corners)
        {
            support = std::max(support,
                               dot(corner - m_current.pivot,
                                   -basis.forward));
        }

        const float radius = GetBoundsRadius();
        const float margin = std::max(radius * m_current.exteriorMarginScale,
                                      radius * 0.001f);
        return std::max(m_current.minDistance, support + margin);
    }

    float OrbitCameraRig::ClampDistance(float distance) const
    {
        const float minimum = GetMinimumDistance();
        const float maximum = std::max(m_current.maxDistance, minimum);
        return std::clamp(std::max(distance, kMinimumDistance), minimum, maximum);
    }

    float OrbitCameraRig::CalculateFitDistance(
        const OrbitCameraViewBasis& basis) const
    {
        if (!HasFiniteBounds())
        {
            return std::numeric_limits<float>::quiet_NaN();
        }

        const float halfVerticalFov = m_current.verticalFovRadians * 0.5f;
        const float tanVertical = std::tan(halfVerticalFov);
        const float tanHorizontal = tanVertical * m_current.aspectRatio;
        if (!std::isfinite(tanVertical) || !std::isfinite(tanHorizontal) ||
            tanVertical <= 0.0f || tanHorizontal <= 0.0f)
        {
            return std::numeric_limits<float>::quiet_NaN();
        }

        const Vec3 min = m_current.bounds.GetMin();
        const Vec3 max = m_current.bounds.GetMax();
        const Vec3 corners[8] = {
            Vec3(min.x, min.y, min.z), Vec3(max.x, min.y, min.z),
            Vec3(min.x, max.y, min.z), Vec3(max.x, max.y, min.z),
            Vec3(min.x, min.y, max.z), Vec3(max.x, min.y, max.z),
            Vec3(min.x, max.y, max.z), Vec3(max.x, max.y, max.z)};

        float requiredDistance = GetBaseMinimumDistance(basis);
        for (const Vec3& corner : corners)
        {
            const Vec3 offset = corner - m_current.pivot;
            const float alongView = dot(offset, basis.forward);
            const float horizontal = std::abs(dot(offset, basis.right));
            const float vertical = std::abs(dot(offset, basis.up));
            requiredDistance = std::max(requiredDistance,
                                        horizontal / tanHorizontal - alongView);
            requiredDistance = std::max(requiredDistance,
                                        vertical / tanVertical - alongView);
        }

        const float radius = GetBoundsRadius();
        const float fitPadding = std::max(
            radius * (m_current.fitMargin - 1.0f),
            radius * 0.001f);
        const float fittedDistance = requiredDistance + fitPadding;
        return std::isfinite(fittedDistance)
                   ? fittedDistance
                   : std::numeric_limits<float>::quiet_NaN();
    }

    bool OrbitCameraRig::FitInternal()
    {
        const OrbitCameraViewBasis basis = GetViewBasis();
        const float fittedDistance = CalculateFitDistance(basis);
        if (!std::isfinite(fittedDistance))
        {
            return false;
        }
        m_current.maxDistance = std::max(m_current.maxDistance, fittedDistance);
        m_current.distance = ClampDistance(fittedDistance);
        return std::isfinite(m_current.distance) && m_current.distance > 0.0f;
    }

    void OrbitCameraRig::RequestDiscontinuity()
    {
        m_discontinuityPending = true;
    }
} // namespace RVX
