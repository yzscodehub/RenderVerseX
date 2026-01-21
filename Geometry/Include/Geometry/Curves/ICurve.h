/**
 * @file ICurve.h
 * @brief Base interface for parametric curves
 */

#pragma once

#include "Core/MathTypes.h"
#include "Core/Math/AABB.h"
#include <vector>

namespace RVX::Geometry
{

/**
 * @brief Base interface for parametric curves
 * 
 * All curves are parameterized by t in [0, 1].
 * 
 * @tparam T The point type (Vec2 or Vec3)
 */
template<typename T>
class ICurve
{
public:
    virtual ~ICurve() = default;

    // =========================================================================
    // Core Evaluation
    // =========================================================================

    /**
     * @brief Evaluate the curve position at parameter t
     * @param t Parameter in [0, 1]
     * @return Position on the curve
     */
    virtual T Evaluate(float t) const = 0;

    /**
     * @brief Evaluate the curve tangent at parameter t
     * @param t Parameter in [0, 1]
     * @return Tangent vector (not normalized)
     */
    virtual T EvaluateTangent(float t) const = 0;

    /**
     * @brief Evaluate the curve normal at parameter t
     * @param t Parameter in [0, 1]
     * @return Normal vector (perpendicular to tangent)
     */
    virtual T EvaluateNormal(float t) const
    {
        T tangent = EvaluateTangent(t);
        // Default implementation for 3D: use Frenet frame
        return GetPerpendicularVector(tangent);
    }

    // =========================================================================
    // Length Queries
    // =========================================================================

    /**
     * @brief Get the approximate arc length of the curve
     * @param samples Number of samples for approximation
     */
    virtual float GetLength(int samples = 32) const
    {
        float length = 0.0f;
        T prev = Evaluate(0.0f);

        for (int i = 1; i <= samples; ++i)
        {
            float t = static_cast<float>(i) / samples;
            T curr = Evaluate(t);
            length += glm::length(curr - prev);
            prev = curr;
        }

        return length;
    }

    /**
     * @brief Get parameter t for a given arc length distance
     * @param distance Arc length from start
     * @param samples Number of samples for approximation
     */
    virtual float GetParameterAtDistance(float distance, int samples = 32) const
    {
        float totalLength = 0.0f;
        T prev = Evaluate(0.0f);

        for (int i = 1; i <= samples; ++i)
        {
            float t = static_cast<float>(i) / samples;
            T curr = Evaluate(t);
            float segmentLength = glm::length(curr - prev);

            if (totalLength + segmentLength >= distance)
            {
                // Interpolate within this segment
                float remaining = distance - totalLength;
                float segmentT = remaining / segmentLength;
                float prevT = static_cast<float>(i - 1) / samples;
                return prevT + segmentT / samples;
            }

            totalLength += segmentLength;
            prev = curr;
        }

        return 1.0f;
    }

    // =========================================================================
    // Sampling
    // =========================================================================

    /**
     * @brief Sample the curve at uniform parameter intervals
     * @param numSamples Number of samples
     * @param outPoints Output points
     */
    virtual void SampleUniform(int numSamples, std::vector<T>& outPoints) const
    {
        outPoints.clear();
        outPoints.reserve(numSamples);

        for (int i = 0; i < numSamples; ++i)
        {
            float t = static_cast<float>(i) / (numSamples - 1);
            outPoints.push_back(Evaluate(t));
        }
    }

    /**
     * @brief Sample the curve adaptively based on curvature
     * @param tolerance Maximum deviation from curve
     * @param outPoints Output points
     */
    virtual void SampleAdaptive(float tolerance, std::vector<T>& outPoints) const
    {
        outPoints.clear();
        outPoints.push_back(Evaluate(0.0f));
        SampleAdaptiveRecursive(0.0f, 1.0f, tolerance, outPoints);
    }

protected:
    void SampleAdaptiveRecursive(float t0, float t1, float tolerance, std::vector<T>& outPoints) const
    {
        float tMid = (t0 + t1) * 0.5f;

        T p0 = Evaluate(t0);
        T p1 = Evaluate(t1);
        T pMid = Evaluate(tMid);

        T linear = (p0 + p1) * 0.5f;
        float error = glm::length(pMid - linear);

        if (error > tolerance && (t1 - t0) > 0.001f)
        {
            SampleAdaptiveRecursive(t0, tMid, tolerance, outPoints);
            SampleAdaptiveRecursive(tMid, t1, tolerance, outPoints);
        }
        else
        {
            outPoints.push_back(p1);
        }
    }

    // Helper for 3D normal calculation
    static Vec3 GetPerpendicularVector(const Vec3& v)
    {
        Vec3 n = glm::normalize(v);
        Vec3 arbitrary = std::abs(n.x) < 0.9f ? Vec3(1, 0, 0) : Vec3(0, 1, 0);
        return glm::normalize(glm::cross(n, arbitrary));
    }

    static Vec2 GetPerpendicularVector(const Vec2& v)
    {
        return Vec2(-v.y, v.x);
    }
};

} // namespace RVX::Geometry
