/**
 * @file Keyframe.h
 * @brief Keyframe template and tangent data structures
 * 
 * Migrated from found::animation::keyframe
 */

#pragma once

#include "Animation/Core/Types.h"
#include "Core/MathTypes.h"
#include <optional>
#include <vector>
#include <algorithm>

namespace RVX::Animation
{

// ============================================================================
// Tangent Data
// ============================================================================

/**
 * @brief Tangent data for curve-based interpolation
 */
struct TangentData
{
    Vec3 inTangent{0.0f};
    Vec3 outTangent{0.0f};
    float inWeight{1.0f};
    float outWeight{1.0f};
    
    TangentData() = default;
    TangentData(const Vec3& inTan, const Vec3& outTan)
        : inTangent(inTan), outTangent(outTan) {}
};

/**
 * @brief Scalar tangent data (for float keyframes)
 */
struct ScalarTangentData
{
    float inTangent{0.0f};
    float outTangent{0.0f};
    float inWeight{1.0f};
    float outWeight{1.0f};
    
    ScalarTangentData() = default;
    ScalarTangentData(float inTan, float outTan)
        : inTangent(inTan), outTangent(outTan) {}
};

// ============================================================================
// Keyframe Template
// ============================================================================

/**
 * @brief Generic keyframe template
 */
template<typename T>
struct Keyframe
{
    TimeUs time{0};
    T value{};
    InterpolationMode interpolation{InterpolationMode::Linear};
    
    Keyframe() = default;
    Keyframe(TimeUs t, const T& v, InterpolationMode interp = InterpolationMode::Linear)
        : time(t), value(v), interpolation(interp) {}
    
    bool operator<(const Keyframe& other) const { return time < other.time; }
    bool operator==(const Keyframe& other) const { return time == other.time && value == other.value; }
};

/**
 * @brief Float keyframe with scalar tangent support
 */
struct KeyframeFloat : Keyframe<float>
{
    std::optional<ScalarTangentData> tangent;
    
    KeyframeFloat() = default;
    KeyframeFloat(TimeUs t, float v, InterpolationMode interp = InterpolationMode::Linear)
        : Keyframe<float>(t, v, interp) {}
    
    void SetTangent(float inTan, float outTan)
    {
        tangent = ScalarTangentData(inTan, outTan);
    }
};

/**
 * @brief Vec3 keyframe with vector tangent support
 */
struct KeyframeVec3 : Keyframe<Vec3>
{
    std::optional<TangentData> tangent;
    
    KeyframeVec3() = default;
    KeyframeVec3(TimeUs t, const Vec3& v, InterpolationMode interp = InterpolationMode::Linear)
        : Keyframe<Vec3>(t, v, interp) {}
    
    void SetTangent(const Vec3& inTan, const Vec3& outTan)
    {
        tangent = TangentData(inTan, outTan);
    }
};

/**
 * @brief Quaternion keyframe for rotation
 */
struct KeyframeQuat : Keyframe<Quat>
{
    KeyframeQuat() = default;
    KeyframeQuat(TimeUs t, const Quat& v, InterpolationMode interp = InterpolationMode::Linear)
        : Keyframe<Quat>(t, v, interp) {}
};

/**
 * @brief Mat4 keyframe for matrix-based transforms
 */
struct KeyframeMat4 : Keyframe<Mat4>
{
    KeyframeMat4() = default;
    KeyframeMat4(TimeUs t, const Mat4& v, InterpolationMode interp = InterpolationMode::Linear)
        : Keyframe<Mat4>(t, v, interp) {}
};

/**
 * @brief Bool keyframe for visibility/toggle animation
 */
struct KeyframeBool : Keyframe<bool>
{
    KeyframeBool() : Keyframe<bool>() { interpolation = InterpolationMode::Step; }
    KeyframeBool(TimeUs t, bool v) : Keyframe<bool>(t, v, InterpolationMode::Step) {}
};

// ============================================================================
// Keyframe Utilities
// ============================================================================

/**
 * @brief Find the index of the keyframe at or before the given time
 */
template<typename KeyframeType>
inline int FindKeyframeIndex(const std::vector<KeyframeType>& keyframes, TimeUs time)
{
    if (keyframes.empty()) return -1;
    
    auto it = std::upper_bound(keyframes.begin(), keyframes.end(), time,
        [](TimeUs t, const KeyframeType& kf) { return t < kf.time; });
    
    if (it == keyframes.begin()) return -1;
    return static_cast<int>(std::distance(keyframes.begin(), it) - 1);
}

/**
 * @brief Find the two keyframes surrounding the given time
 */
template<typename KeyframeType>
inline bool FindKeyframePair(
    const std::vector<KeyframeType>& keyframes,
    TimeUs time,
    int& outIndexA,
    int& outIndexB,
    float& outT,
    int hintIndex = -1)
{
    if (keyframes.empty()) return false;
    
    if (keyframes.size() == 1)
    {
        outIndexA = outIndexB = 0;
        outT = 0.0f;
        return true;
    }
    
    int index = FindKeyframeIndex(keyframes, time);
    
    if (index < 0)
    {
        outIndexA = outIndexB = 0;
        outT = 0.0f;
        return true;
    }
    
    if (index >= static_cast<int>(keyframes.size()) - 1)
    {
        outIndexA = outIndexB = static_cast<int>(keyframes.size()) - 1;
        outT = 0.0f;
        return true;
    }
    
    outIndexA = index;
    outIndexB = index + 1;
    
    TimeUs timeA = keyframes[outIndexA].time;
    TimeUs timeB = keyframes[outIndexB].time;
    TimeUs duration = timeB - timeA;
    
    outT = (duration <= 0) ? 0.0f : static_cast<float>(time - timeA) / static_cast<float>(duration);
    return true;
}

/**
 * @brief Sort keyframes by time
 */
template<typename KeyframeType>
inline void SortKeyframes(std::vector<KeyframeType>& keyframes)
{
    std::sort(keyframes.begin(), keyframes.end(),
        [](const KeyframeType& a, const KeyframeType& b) { return a.time < b.time; });
}

} // namespace RVX::Animation
