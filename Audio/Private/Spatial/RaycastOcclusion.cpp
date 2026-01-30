/**
 * @file RaycastOcclusion.cpp
 * @brief RaycastOcclusionProvider implementation
 */

#include "Audio/Spatial/IOcclusionProvider.h"
#include <cmath>

namespace RVX::Audio
{

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
        
        // Reduce cutoff based on occlusion
        result.lowPassCutoff = 20000.0f - (result.occlusion * (20000.0f - 500.0f));
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
    // This is a stub implementation
    // In a real implementation, this would integrate with the Physics module:
    //
    // PhysicsWorld* physics = ...;
    // RaycastResult result;
    // if (physics->Raycast(start, end, result))
    // {
    //     hitCount = result.hitCount;
    //     return true;
    // }
    //
    // For now, return no hits

    (void)start;
    (void)end;
    hitCount = 0;
    return false;
}

} // namespace RVX::Audio
