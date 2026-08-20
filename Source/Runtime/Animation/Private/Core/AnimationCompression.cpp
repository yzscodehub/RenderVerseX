/**
 * @file AnimationCompression.cpp
 * @brief Animation compression implementation
 */

#include "Animation/Core/AnimationCompression.h"
#include "Animation/Core/Interpolation.h"
#include <algorithm>
#include <cmath>

namespace RVX::Animation
{

// ============================================================================
// AnimationCompressor Implementation
// ============================================================================

CompressionStats AnimationCompressor::Compress(AnimationClip& clip)
{
    m_lastStats = CompressionStats();
    m_lastStats.originalSize = CalculateOriginalSize(clip);

    // Phase 1: Remove static tracks
    if (m_config.removeStaticTracks)
    {
        RemoveStaticTracks(clip);
    }

    // Phase 2: Reduce keyframes
    if (m_config.keyframeReduction.enabled)
    {
        for (auto& track : clip.transformTracks)
        {
            ReduceTransformTrack(track);
        }

        for (auto& track : clip.blendShapeTracks)
        {
            ReduceBlendShapeTrack(track);
        }

        for (auto& track : clip.propertyTracks)
        {
            ReducePropertyTrack(track);
        }
    }

    // Calculate final stats
    m_lastStats.compressedSize = CalculateOriginalSize(clip);  // After reduction
    m_lastStats.Calculate();

    return m_lastStats;
}

AnimationClip::Ptr AnimationCompressor::CompressCopy(const AnimationClip& clip)
{
    auto copy = clip.Clone();
    Compress(*copy);
    return copy;
}

size_t AnimationCompressor::EstimateCompressedSize(const AnimationClip& clip) const
{
    size_t size = 0;

    // Basic clip metadata
    size += sizeof(TimeUs);  // duration
    size += clip.name.size();

    // Transform tracks
    for (const auto& track : clip.transformTracks)
    {
        size += track.targetName.size();
        
        // Estimate with quantization
        if (!track.translationKeyframes.empty())
        {
            // Quantized: time (4 bytes) + 3 x int16 = 10 bytes per keyframe
            size += track.translationKeyframes.size() * 10;
        }
        if (!track.rotationKeyframes.empty())
        {
            // Smallest-three: time (4 bytes) + index (1 byte) + 3 x int16 = 11 bytes
            size += track.rotationKeyframes.size() * 11;
        }
        if (!track.scaleKeyframes.empty())
        {
            size += track.scaleKeyframes.size() * 10;
        }
    }

    // BlendShape tracks
    for (const auto& track : clip.blendShapeTracks)
    {
        size += track.targetName.size();
        for (const auto& channel : track.weightsKeyframes)
        {
            // time + quantized float = 6 bytes
            size += channel.size() * 6;
        }
    }

    // Property tracks
    for (const auto& track : clip.propertyTracks)
    {
        size += track.targetName.size() + track.propertyName.size();
        size += track.floatKeyframes.size() * 6;
        size += track.vec3Keyframes.size() * 10;
        size += track.vec4Keyframes.size() * 12;
    }

    return size;
}

size_t AnimationCompressor::CalculateOriginalSize(const AnimationClip& clip) const
{
    size_t size = 0;

    // Basic clip metadata
    size += sizeof(AnimationClip);
    size += clip.name.size();
    size += clip.description.size();

    // Transform tracks
    for (const auto& track : clip.transformTracks)
    {
        size += sizeof(TransformTrack);
        size += track.targetName.size();
        
        // Full precision keyframes
        size += track.translationKeyframes.size() * sizeof(KeyframeVec3);
        size += track.rotationKeyframes.size() * sizeof(KeyframeQuat);
        size += track.scaleKeyframes.size() * sizeof(KeyframeVec3);
        size += track.matrixKeyframes.size() * sizeof(KeyframeMat4);
    }

    // BlendShape tracks
    for (const auto& track : clip.blendShapeTracks)
    {
        size += sizeof(BlendShapeTrack);
        size += track.targetName.size();
        for (const auto& name : track.channelNames)
        {
            size += name.size();
        }
        for (const auto& channel : track.weightsKeyframes)
        {
            size += channel.size() * sizeof(KeyframeFloat);
        }
    }

    // Property tracks
    for (const auto& track : clip.propertyTracks)
    {
        size += sizeof(PropertyTrack);
        size += track.targetName.size();
        size += track.propertyName.size();
        size += track.floatKeyframes.size() * sizeof(KeyframeFloat);
        size += track.vec3Keyframes.size() * sizeof(KeyframeVec3);
        size += track.vec4Keyframes.size() * sizeof(KeyframeVec4);
        size += track.intKeyframes.size() * sizeof(Keyframe<int>);
        size += track.boolKeyframes.size() * sizeof(KeyframeBool);
    }

    // Visibility tracks
    for (const auto& track : clip.visibilityTracks)
    {
        size += sizeof(VisibilityTrack);
        size += track.targetName.size();
        size += track.keyframes.size() * sizeof(KeyframeBool);
    }

    return size;
}

void AnimationCompressor::ReduceTransformTrack(TransformTrack& track)
{
    const auto& settings = m_config.keyframeReduction;

    // Reduce translation keyframes
    if (!track.translationKeyframes.empty())
    {
        size_t removed = ReduceKeyframes(track.translationKeyframes, 
                                          settings.maxTranslationError);
        m_lastStats.keyframesRemoved += removed;
    }

    // Reduce rotation keyframes
    if (!track.rotationKeyframes.empty())
    {
        // Special handling for quaternions
        auto& keyframes = track.rotationKeyframes;
        if (keyframes.size() > settings.minKeyframes)
        {
            std::vector<bool> keep(keyframes.size(), true);
            size_t removed = 0;

            for (size_t i = 1; i < keyframes.size() - 1; ++i)
            {
                const auto& prev = keyframes[i - 1];
                const auto& curr = keyframes[i];
                const auto& next = keyframes[i + 1];

                float t = 0.0f;
                TimeUs duration = next.time - prev.time;
                if (duration > 0)
                {
                    t = static_cast<float>(curr.time - prev.time) / 
                        static_cast<float>(duration);
                }

                Quat interpolated = InterpolateSlerp(prev.value, next.value, t);
                float error = CalculateQuatError(curr.value, interpolated);

                if (error <= settings.maxRotationError)
                {
                    keep[i] = false;
                    removed++;
                }
            }

            if (removed > 0)
            {
                std::vector<KeyframeQuat> filtered;
                filtered.reserve(keyframes.size() - removed);
                
                for (size_t i = 0; i < keyframes.size(); ++i)
                {
                    if (keep[i])
                    {
                        filtered.push_back(std::move(keyframes[i]));
                    }
                }
                
                keyframes = std::move(filtered);
                m_lastStats.keyframesRemoved += removed;
            }
        }
    }

    // Reduce scale keyframes
    if (!track.scaleKeyframes.empty())
    {
        size_t removed = ReduceKeyframes(track.scaleKeyframes, 
                                          settings.maxScaleError);
        m_lastStats.keyframesRemoved += removed;
    }
}

void AnimationCompressor::ReduceBlendShapeTrack(BlendShapeTrack& track)
{
    const auto& settings = m_config.keyframeReduction;

    for (auto& channel : track.weightsKeyframes)
    {
        if (!channel.empty())
        {
            size_t removed = ReduceKeyframes(channel, settings.maxPropertyError);
            m_lastStats.keyframesRemoved += removed;
        }
    }
}

void AnimationCompressor::ReducePropertyTrack(PropertyTrack& track)
{
    const auto& settings = m_config.keyframeReduction;

    if (!track.floatKeyframes.empty())
    {
        size_t removed = ReduceKeyframes(track.floatKeyframes, 
                                          settings.maxPropertyError);
        m_lastStats.keyframesRemoved += removed;
    }

    if (!track.vec3Keyframes.empty())
    {
        size_t removed = ReduceKeyframes(track.vec3Keyframes, 
                                          settings.maxPropertyError);
        m_lastStats.keyframesRemoved += removed;
    }
}

void AnimationCompressor::RemoveStaticTracks(AnimationClip& clip)
{
    // Remove static transform tracks
    auto removeIt = std::remove_if(
        clip.transformTracks.begin(),
        clip.transformTracks.end(),
        [this](const TransformTrack& track) {
            return IsTrackStatic(track);
        });

    size_t removed = std::distance(removeIt, clip.transformTracks.end());
    clip.transformTracks.erase(removeIt, clip.transformTracks.end());
    m_lastStats.tracksRemoved += removed;

    // Remove static property tracks
    auto propRemoveIt = std::remove_if(
        clip.propertyTracks.begin(),
        clip.propertyTracks.end(),
        [this](const PropertyTrack& track) {
            return IsTrackStatic(track.floatKeyframes);
        });

    removed = std::distance(propRemoveIt, clip.propertyTracks.end());
    clip.propertyTracks.erase(propRemoveIt, clip.propertyTracks.end());
    m_lastStats.tracksRemoved += removed;
}

bool AnimationCompressor::IsTrackStatic(const TransformTrack& track) const
{
    const float threshold = m_config.staticThreshold;

    // Check translation
    if (!track.translationKeyframes.empty())
    {
        const Vec3& firstVal = track.translationKeyframes[0].value;
        for (const auto& kf : track.translationKeyframes)
        {
            if (length(kf.value - firstVal) > threshold)
                return false;
        }
    }

    // Check rotation
    if (!track.rotationKeyframes.empty())
    {
        const Quat& firstVal = track.rotationKeyframes[0].value;
        for (const auto& kf : track.rotationKeyframes)
        {
            float d = std::abs(dot(kf.value, firstVal));
            if (1.0f - d > threshold)
                return false;
        }
    }

    // Check scale
    if (!track.scaleKeyframes.empty())
    {
        const Vec3& firstVal = track.scaleKeyframes[0].value;
        for (const auto& kf : track.scaleKeyframes)
        {
            if (length(kf.value - firstVal) > threshold)
                return false;
        }
    }

    return true;
}

bool AnimationCompressor::IsTrackStatic(const std::vector<KeyframeFloat>& keyframes) const
{
    if (keyframes.empty() || keyframes.size() == 1)
        return true;

    const float threshold = m_config.staticThreshold;
    float firstVal = keyframes[0].value;

    for (const auto& kf : keyframes)
    {
        if (std::abs(kf.value - firstVal) > threshold)
            return false;
    }

    return true;
}

float AnimationCompressor::CalculateVec3Error(const Vec3& a, const Vec3& b) const
{
    return length(a - b);
}

float AnimationCompressor::CalculateQuatError(const Quat& a, const Quat& b) const
{
    // Use 1 - |dot| as error metric
    float d = std::abs(dot(a, b));
    return 1.0f - d;
}

} // namespace RVX::Animation
