/**
 * @file Interpolation.h
 * @brief Interpolation algorithms for animation keyframes
 * 
 * Migrated from found::animation::interpolation
 */

#pragma once

#include "Animation/Core/Types.h"
#include "Animation/Core/Keyframe.h"
#include "Animation/Core/TransformSample.h"
#include "Core/MathTypes.h"
#include <cmath>

namespace RVX::Animation
{

// ============================================================================
// Basic Interpolation Functions
// ============================================================================

template<typename T>
inline T InterpolateStep(const T& a, const T& /*b*/, float /*t*/)
{
    return a;
}

template<typename T>
inline T InterpolateLinear(const T& a, const T& b, float t)
{
    return mix(a, b, t);
}

template<>
inline float InterpolateLinear(const float& a, const float& b, float t)
{
    return a + (b - a) * t;
}

template<>
inline bool InterpolateLinear(const bool& a, const bool& b, float t)
{
    return t < 0.5f ? a : b;
}

inline Quat InterpolateSlerp(const Quat& a, const Quat& b, float t)
{
    float d = dot(a, b);
    Quat bAdjusted = d < 0.0f ? -b : b;
    return slerp(a, bAdjusted, t);
}

// ============================================================================
// Cubic Spline Interpolation
// ============================================================================

template<typename T>
inline T InterpolateCubicHermite(const T& v0, const T& m0, const T& v1, const T& m1, float t)
{
    float t2 = t * t;
    float t3 = t2 * t;
    
    float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
    float h10 = t3 - 2.0f * t2 + t;
    float h01 = -2.0f * t3 + 3.0f * t2;
    float h11 = t3 - t2;
    
    return v0 * h00 + m0 * h10 + v1 * h01 + m1 * h11;
}

// ============================================================================
// Bezier Curve Interpolation
// ============================================================================

template<typename T>
inline T EvaluateBezierCubic(const T& p0, const T& p1, const T& p2, const T& p3, float t)
{
    float u = 1.0f - t;
    float u2 = u * u;
    float u3 = u2 * u;
    float t2 = t * t;
    float t3 = t2 * t;
    
    return p0 * u3 + p1 * (3.0f * u2 * t) + p2 * (3.0f * u * t2) + p3 * t3;
}

inline Vec3 InterpolateBezier(
    const Vec3& v0, const TangentData& tangent0,
    const Vec3& v1, const TangentData& tangent1,
    float t, float timeDelta)
{
    Vec3 p1 = v0 + tangent0.outTangent * (timeDelta * tangent0.outWeight / 3.0f);
    Vec3 p2 = v1 - tangent1.inTangent * (timeDelta * tangent1.inWeight / 3.0f);
    return EvaluateBezierCubic(v0, p1, p2, v1, t);
}

inline float InterpolateBezierScalar(
    float v0, const ScalarTangentData& tangent0,
    float v1, const ScalarTangentData& tangent1,
    float t, float timeDelta)
{
    float p1 = v0 + tangent0.outTangent * (timeDelta * tangent0.outWeight / 3.0f);
    float p2 = v1 - tangent1.inTangent * (timeDelta * tangent1.inWeight / 3.0f);
    return EvaluateBezierCubic(v0, p1, p2, v1, t);
}

// ============================================================================
// Matrix Interpolation
// ============================================================================

inline Mat4 InterpolateMatrix(const Mat4& a, const Mat4& b, float t)
{
    TransformSample sampleA = TransformSample::FromMatrix(a);
    TransformSample sampleB = TransformSample::FromMatrix(b);
    return TransformSample::Lerp(sampleA, sampleB, t).ToMatrix();
}

// ============================================================================
// Unified Interpolation Interface
// ============================================================================

inline float Interpolate(const KeyframeFloat& kf0, const KeyframeFloat& kf1, float t)
{
    switch (kf0.interpolation)
    {
        case InterpolationMode::Step:
            return kf0.value;
        case InterpolationMode::Linear:
            return InterpolateLinear(kf0.value, kf1.value, t);
        case InterpolationMode::CubicSpline:
        case InterpolationMode::Bezier:
            if (kf0.tangent && kf1.tangent)
            {
                float timeDelta = static_cast<float>(TimeUsToSeconds(kf1.time - kf0.time));
                return InterpolateBezierScalar(kf0.value, *kf0.tangent, kf1.value, *kf1.tangent, t, timeDelta);
            }
            return InterpolateLinear(kf0.value, kf1.value, t);
        default:
            return InterpolateLinear(kf0.value, kf1.value, t);
    }
}

inline Vec3 Interpolate(const KeyframeVec3& kf0, const KeyframeVec3& kf1, float t)
{
    switch (kf0.interpolation)
    {
        case InterpolationMode::Step:
            return kf0.value;
        case InterpolationMode::Linear:
            return InterpolateLinear(kf0.value, kf1.value, t);
        case InterpolationMode::CubicSpline:
        case InterpolationMode::Bezier:
            if (kf0.tangent && kf1.tangent)
            {
                float timeDelta = static_cast<float>(TimeUsToSeconds(kf1.time - kf0.time));
                return InterpolateBezier(kf0.value, *kf0.tangent, kf1.value, *kf1.tangent, t, timeDelta);
            }
            return InterpolateLinear(kf0.value, kf1.value, t);
        default:
            return InterpolateLinear(kf0.value, kf1.value, t);
    }
}

inline Quat Interpolate(const KeyframeQuat& kf0, const KeyframeQuat& kf1, float t)
{
    if (kf0.interpolation == InterpolationMode::Step) return kf0.value;
    return InterpolateSlerp(kf0.value, kf1.value, t);
}

inline Mat4 Interpolate(const KeyframeMat4& kf0, const KeyframeMat4& kf1, float t)
{
    if (kf0.interpolation == InterpolationMode::Step) return kf0.value;
    return InterpolateMatrix(kf0.value, kf1.value, t);
}

// ============================================================================
// Time Wrapping Utilities
// ============================================================================

inline TimeUs ApplyWrapMode(TimeUs time, TimeUs duration, WrapMode mode)
{
    if (duration <= 0) return 0;
    
    switch (mode)
    {
        case WrapMode::Once:
            if (time < 0) return 0;
            if (time >= duration) return duration;
            return time;
            
        case WrapMode::Loop:
            time = time % duration;
            if (time < 0) time += duration;
            return time;
            
        case WrapMode::PingPong:
        {
            TimeUs cycle = duration * 2;
            time = time % cycle;
            if (time < 0) time += cycle;
            if (time > duration) time = cycle - time;
            return time;
        }
            
        case WrapMode::ClampForever:
            if (time < 0) return 0;
            if (time >= duration) return duration;
            return time;
            
        default:
            return time;
    }
}

inline bool IsAnimationFinished(TimeUs time, TimeUs duration, WrapMode mode)
{
    if (duration <= 0) return true;
    
    switch (mode)
    {
        case WrapMode::Once:
            return time >= duration;
        case WrapMode::Loop:
        case WrapMode::PingPong:
        case WrapMode::ClampForever:
            return false;
        default:
            return time >= duration;
    }
}

// ============================================================================
// Easing Functions
// ============================================================================

namespace Easing
{
    inline float Linear(float t) { return t; }
    inline float EaseInQuad(float t) { return t * t; }
    inline float EaseOutQuad(float t) { return t * (2.0f - t); }
    inline float EaseInOutQuad(float t)
    {
        return t < 0.5f ? 2.0f * t * t : -1.0f + (4.0f - 2.0f * t) * t;
    }
    inline float EaseInCubic(float t) { return t * t * t; }
    inline float EaseOutCubic(float t)
    {
        float u = t - 1.0f;
        return u * u * u + 1.0f;
    }
}

} // namespace RVX::Animation
