/**
 * @file RaycastOcclusion.cpp
 * @brief RaycastOcclusionProvider implementation
 */

#include "Audio/Spatial/IOcclusionProvider.h"
#include "Physics/PhysicsWorld.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace RVX::Audio
{
namespace
{
    bool IsFiniteVec3(const Vec3& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }
} // namespace

OcclusionResult RaycastOcclusionProvider::CalculateOcclusion(
    const Vec3& sourcePosition,
    const Vec3& listenerPosition)
{
    if (!m_enabled)
    {
        return OcclusionResult{};
    }

    int hitCount = 0;
    bool hit = Raycast(sourcePosition, listenerPosition, hitCount);

    OcclusionResult result;
    
    if (hit && hitCount > 0)
    {
        result.occlusion = std::min(1.0f, hitCount * m_occlusionPerHit);
        result.obstruction = result.occlusion;
        result.transmission = 1.0f - result.occlusion;

        result.lowPassCutoff = std::max(500.0f, 20000.0f - hitCount * m_lowPassReduction);
        result.volumeScale = 1.0f - (result.occlusion * 0.5f);  // Max 50% volume reduction
    }

    return result;
}

OcclusionResult RaycastOcclusionProvider::CalculateOcclusionMultiSample(
    const Vec3& sourcePosition,
    const Vec3& listenerPosition,
    float sourceRadius,
    int sampleCount)
{
    if (!m_enabled || sampleCount <= 0)
    {
        return OcclusionResult{};
    }

    // Simple multi-sample: cast rays to multiple points around the source
    // For a more accurate result, we'd sample in a sphere pattern

    OcclusionResult accumulated;
    float totalWeight = 0.0f;

    // Center ray
    auto centerResult = CalculateOcclusion(sourcePosition, listenerPosition);
    accumulated.occlusion += centerResult.occlusion;
    accumulated.obstruction += centerResult.obstruction;
    accumulated.transmission += centerResult.transmission;
    accumulated.lowPassCutoff += centerResult.lowPassCutoff;
    accumulated.volumeScale += centerResult.volumeScale;
    totalWeight += 1.0f;

    if (sampleCount > 1 && sourceRadius > 0.0f)
    {
        // Sample points around the source
        Vec3 offsets[] = {
            Vec3(sourceRadius, 0.0f, 0.0f),
            Vec3(-sourceRadius, 0.0f, 0.0f),
            Vec3(0.0f, sourceRadius, 0.0f),
            Vec3(0.0f, -sourceRadius, 0.0f),
            Vec3(0.0f, 0.0f, sourceRadius),
            Vec3(0.0f, 0.0f, -sourceRadius)
        };

        int samples = std::min(sampleCount - 1, 6);
        for (int i = 0; i < samples; ++i)
        {
            Vec3 samplePos = Vec3(
                sourcePosition.x + offsets[i].x,
                sourcePosition.y + offsets[i].y,
                sourcePosition.z + offsets[i].z
            );

            auto result = CalculateOcclusion(samplePos, listenerPosition);
            accumulated.occlusion += result.occlusion;
            accumulated.obstruction += result.obstruction;
            accumulated.transmission += result.transmission;
            accumulated.lowPassCutoff += result.lowPassCutoff;
            accumulated.volumeScale += result.volumeScale;
            totalWeight += 1.0f;
        }
    }

    // Average the results
    if (totalWeight > 0.0f)
    {
        accumulated.occlusion /= totalWeight;
        accumulated.obstruction /= totalWeight;
        accumulated.transmission /= totalWeight;
        accumulated.lowPassCutoff /= totalWeight;
        accumulated.volumeScale /= totalWeight;
    }

    return accumulated;
}

bool RaycastOcclusionProvider::Raycast(const Vec3& start, const Vec3& end, int& hitCount)
{
    hitCount = 0;
    if (!m_physicsWorld || !IsFiniteVec3(start) || !IsFiniteVec3(end))
    {
        return false;
    }

    const Vec3 delta = end - start;
    const float distanceSq = dot(delta, delta);
    if (!std::isfinite(distanceSq) || distanceSq <= 0.000001f)
    {
        return false;
    }

    const float distance = std::sqrt(distanceSq);
    if (!std::isfinite(distance) || distance > m_maxDistance)
    {
        return false;
    }

    std::vector<Physics::RaycastHit> hits;
    hitCount = static_cast<int>(m_physicsWorld->RaycastAll(start,
                                                           delta / distance,
                                                           distance,
                                                           hits,
                                                           m_layerMask));
    return hitCount > 0;
}

} // namespace RVX::Audio
