#pragma once

#include "Core/MathTypes.h"
#include "Core/Types.h"

#include <cmath>

namespace RVX
{
    struct TransmissionRay
    {
        Vec3 displacement{0.0f};
        float32 pathLength = 0.0f;
        bool valid = false;
    };

    // Plane-parallel slab reference. Thickness is measured along the normal;
    // roughness deliberately does not participate in the geometric ray.
    [[nodiscard]] inline TransmissionRay ResolveTransmissionRay(
        const Vec3& incident, const Vec3& normal, float32 ior,
        float32 thickness) noexcept
    {
        TransmissionRay result;
        const auto finite = [](const Vec3& v)
        { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
        if (!finite(incident) || !finite(normal) || !std::isfinite(ior) ||
            !std::isfinite(thickness) || ior < 1.0f || thickness < 0.0f ||
            dot(incident, incident) < 1.0e-8f || dot(normal, normal) < 1.0e-8f)
            return result;
        const Vec3 direction = normalize(incident);
        Vec3 facingNormal = normalize(normal);
        if (dot(direction, facingNormal) > 0.0f) facingNormal = -facingNormal;
        const Vec3 ray = glm::refract(direction, facingNormal, 1.0f / ior);
        const float32 cosine = std::abs(dot(ray, facingNormal));
        if (!finite(ray) || cosine <= 1.0e-4f) return result;
        result.pathLength = thickness / cosine;
        result.displacement = ray * result.pathLength;
        result.valid = finite(result.displacement) && std::isfinite(result.pathLength);
        return result;
    }

    [[nodiscard]] inline bool IsTransmissionDepthBehindExit(
        float32 sampledDepth, float32 exitDepth, bool reverseZ) noexcept
    {
        if (!std::isfinite(sampledDepth) || !std::isfinite(exitDepth) ||
            sampledDepth < 0.0f || sampledDepth > 1.0f ||
            exitDepth < 0.0f || exitDepth > 1.0f) return false;
        if (sampledDepth == (reverseZ ? 0.0f : 1.0f)) return true;
        return reverseZ ? sampledDepth < exitDepth - 1.0e-5f
                        : sampledDepth > exitDepth + 1.0e-5f;
    }

    // Reject rather than clamp an unavailable screen-space candidate. The caller
    // keeps the unshifted background; this is not an off-screen scene ray trace.
    [[nodiscard]] inline bool ProjectTransmissionCandidate(
        const Mat4& viewProjection, const Vec3& exitPosition,
        const Vec2& pyramidExtent, Vec2& outUV) noexcept
    {
        if (!std::isfinite(pyramidExtent.x) || !std::isfinite(pyramidExtent.y) ||
            pyramidExtent.x < 1.0f || pyramidExtent.y < 1.0f) return false;
        const Vec4 clip = viewProjection * Vec4(exitPosition, 1.0f);
        if (!std::isfinite(clip.x) || !std::isfinite(clip.y) ||
            !std::isfinite(clip.z) || !std::isfinite(clip.w) || clip.w <= 1.0e-4f)
            return false;
        const Vec2 uv = Vec2(clip) / clip.w * Vec2(0.5f, -0.5f) + Vec2(0.5f);
        const Vec2 margin = Vec2(0.5f) / pyramidExtent;
        if (uv.x < margin.x || uv.y < margin.y ||
            uv.x > 1.0f - margin.x || uv.y > 1.0f - margin.y) return false;
        outUV = uv;
        return true;
    }
}
