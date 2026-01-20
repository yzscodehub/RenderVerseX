/**
 * @file AnimationTrack.h
 * @brief Animation track definitions
 * 
 * Migrated from found::animation::animation_track
 */

#pragma once

#include "Core/MathTypes.h"
#include "Animation/Core/Types.h"
#include "Animation/Core/Keyframe.h"
#include "Animation/Core/Interpolation.h"
#include "Animation/Core/TransformSample.h"
#include <string>
#include <vector>
#include <limits>

namespace RVX::Animation
{

// ============================================================================
// Transform Track
// ============================================================================

/**
 * @brief Cursor for optimizing transform track sampling
 */
struct TransformTrackCursor
{
    int translationIdx{-1};
    int rotationIdx{-1};
    int scaleIdx{-1};
    int matrixIdx{-1};

    void Reset()
    {
        translationIdx = rotationIdx = scaleIdx = matrixIdx = -1;
    }
};

/**
 * @brief Transform animation mode
 */
enum class TransformMode : uint8_t
{
    TRS,    ///< Separate Translation, Rotation, Scale channels
    Matrix  ///< Matrix-based animation
};

/**
 * @brief Transform animation track for bones or nodes
 */
struct TransformTrack
{
    /// Target bone/node name
    std::string targetName;

    /// Target type (Bone or Node)
    TrackTargetType targetType{TrackTargetType::Bone};

    /// Transform mode
    TransformMode mode{TransformMode::TRS};

    // TRS mode channels
    std::vector<KeyframeVec3> translationKeyframes;
    std::vector<KeyframeQuat> rotationKeyframes;
    std::vector<KeyframeVec3> scaleKeyframes;

    // Matrix mode keyframes
    std::vector<KeyframeMat4> matrixKeyframes;

    // ========================================================================
    // Query
    // ========================================================================

    std::pair<TimeUs, TimeUs> GetTimeRange() const
    {
        TimeUs start = std::numeric_limits<TimeUs>::max();
        TimeUs end = std::numeric_limits<TimeUs>::min();

        auto updateRange = [&](const auto& keyframes) {
            if (!keyframes.empty())
            {
                start = std::min(start, keyframes.front().time);
                end = std::max(end, keyframes.back().time);
            }
        };

        if (mode == TransformMode::TRS)
        {
            updateRange(translationKeyframes);
            updateRange(rotationKeyframes);
            updateRange(scaleKeyframes);
        }
        else
        {
            updateRange(matrixKeyframes);
        }

        if (start > end) return {0, 0};
        return {start, end};
    }

    size_t GetKeyframeCount() const
    {
        if (mode == TransformMode::TRS)
            return translationKeyframes.size() + rotationKeyframes.size() + scaleKeyframes.size();
        return matrixKeyframes.size();
    }

    bool IsEmpty() const
    {
        if (mode == TransformMode::TRS)
            return translationKeyframes.empty() && rotationKeyframes.empty() && scaleKeyframes.empty();
        return matrixKeyframes.empty();
    }

    // ========================================================================
    // Sampling
    // ========================================================================

    TransformSample Sample(TimeUs time, TransformTrackCursor* cursor = nullptr) const
    {
        if (mode == TransformMode::TRS)
            return SampleTRS(time, cursor);
        else
            return SampleMatrix(time, cursor);
    }

private:
    TransformSample SampleTRS(TimeUs time, TransformTrackCursor* cursor) const
    {
        TransformSample result = TransformSample::Identity();

        // Sample translation
        if (!translationKeyframes.empty())
        {
            int idxA, idxB;
            float t;
            int hint = cursor ? cursor->translationIdx : -1;

            if (FindKeyframePair(translationKeyframes, time, idxA, idxB, t, hint))
            {
                result.translation = Interpolate(translationKeyframes[idxA], translationKeyframes[idxB], t);
                if (cursor) cursor->translationIdx = idxA;
            }
        }

        // Sample rotation
        if (!rotationKeyframes.empty())
        {
            int idxA, idxB;
            float t;
            int hint = cursor ? cursor->rotationIdx : -1;

            if (FindKeyframePair(rotationKeyframes, time, idxA, idxB, t, hint))
            {
                result.rotation = Interpolate(rotationKeyframes[idxA], rotationKeyframes[idxB], t);
                if (cursor) cursor->rotationIdx = idxA;
            }
        }

        // Sample scale
        if (!scaleKeyframes.empty())
        {
            int idxA, idxB;
            float t;
            int hint = cursor ? cursor->scaleIdx : -1;

            if (FindKeyframePair(scaleKeyframes, time, idxA, idxB, t, hint))
            {
                result.scale = Interpolate(scaleKeyframes[idxA], scaleKeyframes[idxB], t);
                if (cursor) cursor->scaleIdx = idxA;
            }
        }

        return result;
    }

    TransformSample SampleMatrix(TimeUs time, TransformTrackCursor* cursor) const
    {
        if (matrixKeyframes.empty())
            return TransformSample::Identity();

        int idxA, idxB;
        float t;
        int hint = cursor ? cursor->matrixIdx : -1;

        if (FindKeyframePair(matrixKeyframes, time, idxA, idxB, t, hint))
        {
            Mat4 result = Interpolate(matrixKeyframes[idxA], matrixKeyframes[idxB], t);
            if (cursor) cursor->matrixIdx = idxA;
            return TransformSample::FromMatrix(result);
        }

        return TransformSample::Identity();
    }
};

// ============================================================================
// BlendShape Track
// ============================================================================

/**
 * @brief BlendShape/Morph target animation track
 */
struct BlendShapeTrack
{
    std::string targetName;  // Mesh name
    std::vector<std::string> channelNames;  // BlendShape channel names
    std::vector<std::vector<KeyframeFloat>> weightsKeyframes;  // One per channel

    std::pair<TimeUs, TimeUs> GetTimeRange() const
    {
        TimeUs start = std::numeric_limits<TimeUs>::max();
        TimeUs end = std::numeric_limits<TimeUs>::min();

        for (const auto& kfs : weightsKeyframes)
        {
            if (!kfs.empty())
            {
                start = std::min(start, kfs.front().time);
                end = std::max(end, kfs.back().time);
            }
        }

        if (start > end) return {0, 0};
        return {start, end};
    }

    bool IsEmpty() const { return weightsKeyframes.empty(); }
};

// ============================================================================
// Visibility Track
// ============================================================================

/**
 * @brief Visibility toggle animation track
 */
struct VisibilityTrack
{
    std::string targetName;
    TrackTargetType targetType{TrackTargetType::Node};
    std::vector<KeyframeBool> keyframes;

    std::pair<TimeUs, TimeUs> GetTimeRange() const
    {
        if (keyframes.empty()) return {0, 0};
        return {keyframes.front().time, keyframes.back().time};
    }

    bool IsEmpty() const { return keyframes.empty(); }

    bool Sample(TimeUs time) const
    {
        if (keyframes.empty()) return true;

        int idx = FindKeyframeIndex(keyframes, time);
        if (idx < 0) return keyframes.front().value;
        return keyframes[idx].value;
    }
};

// ============================================================================
// Property Track
// ============================================================================

/**
 * @brief Generic property animation track
 */
struct PropertyTrack
{
    std::string targetName;
    std::string propertyName;
    TrackTargetType targetType{TrackTargetType::Material};
    PropertyValueType valueType{PropertyValueType::Float};

    std::vector<KeyframeFloat> floatKeyframes;
    std::vector<KeyframeVec3> vec3Keyframes;
    std::vector<KeyframeVec4> vec4Keyframes;
    std::vector<Keyframe<int>> intKeyframes;
    std::vector<KeyframeBool> boolKeyframes;

    std::pair<TimeUs, TimeUs> GetTimeRange() const
    {
        TimeUs start = std::numeric_limits<TimeUs>::max();
        TimeUs end = std::numeric_limits<TimeUs>::min();

        auto updateRange = [&](const auto& kfs) {
            if (!kfs.empty())
            {
                start = std::min(start, kfs.front().time);
                end = std::max(end, kfs.back().time);
            }
        };

        updateRange(floatKeyframes);
        updateRange(vec3Keyframes);
        updateRange(vec4Keyframes);
        updateRange(intKeyframes);
        updateRange(boolKeyframes);

        if (start > end) return {0, 0};
        return {start, end};
    }

    bool IsEmpty() const
    {
        return floatKeyframes.empty() && vec3Keyframes.empty() &&
               vec4Keyframes.empty() && intKeyframes.empty() && boolKeyframes.empty();
    }
};

} // namespace RVX::Animation
