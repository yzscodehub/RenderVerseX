/**
 * @file Bezier.h
 * @brief Bezier curve implementations
 */

#pragma once

#include "Geometry/Curves/ICurve.h"
#include "Geometry/Constants.h"
#include <array>

namespace RVX::Geometry
{

/**
 * @brief Quadratic Bezier curve (3 control points)
 */
template<typename T>
class QuadraticBezier : public ICurve<T>
{
public:
    T p0, p1, p2;

    QuadraticBezier() = default;
    QuadraticBezier(const T& a, const T& b, const T& c)
        : p0(a), p1(b), p2(c) {}

    T Evaluate(float t) const override
    {
        float u = 1.0f - t;
        return u * u * p0 + 2.0f * u * t * p1 + t * t * p2;
    }

    T EvaluateTangent(float t) const override
    {
        float u = 1.0f - t;
        return 2.0f * u * (p1 - p0) + 2.0f * t * (p2 - p1);
    }

    /**
     * @brief Split the curve at parameter t
     */
    void Split(float t, QuadraticBezier& left, QuadraticBezier& right) const
    {
        T q0 = glm::mix(p0, p1, t);
        T q1 = glm::mix(p1, p2, t);
        T r0 = glm::mix(q0, q1, t);

        left = QuadraticBezier(p0, q0, r0);
        right = QuadraticBezier(r0, q1, p2);
    }

    /**
     * @brief Get approximate bounding box
     */
    AABB GetBoundingBox() const
    {
        AABB box;
        box.Expand(Vec3(p0));
        box.Expand(Vec3(p1));
        box.Expand(Vec3(p2));
        return box;
    }
};

/**
 * @brief Cubic Bezier curve (4 control points)
 * 
 * The most commonly used Bezier curve for animation paths,
 * vector graphics, and smooth interpolation.
 */
template<typename T>
class CubicBezier : public ICurve<T>
{
public:
    T p0, p1, p2, p3;

    CubicBezier() = default;
    CubicBezier(const T& a, const T& b, const T& c, const T& d)
        : p0(a), p1(b), p2(c), p3(d) {}

    /**
     * @brief Evaluate using De Casteljau's algorithm
     */
    T Evaluate(float t) const override
    {
        float u = 1.0f - t;
        float uu = u * u;
        float uuu = uu * u;
        float tt = t * t;
        float ttt = tt * t;

        return uuu * p0 + 3.0f * uu * t * p1 + 3.0f * u * tt * p2 + ttt * p3;
    }

    /**
     * @brief Evaluate tangent (first derivative)
     */
    T EvaluateTangent(float t) const override
    {
        float u = 1.0f - t;
        float uu = u * u;
        float tt = t * t;

        return 3.0f * uu * (p1 - p0) + 6.0f * u * t * (p2 - p1) + 3.0f * tt * (p3 - p2);
    }

    /**
     * @brief Evaluate second derivative
     */
    T EvaluateSecondDerivative(float t) const
    {
        float u = 1.0f - t;
        return 6.0f * u * (p2 - 2.0f * p1 + p0) + 6.0f * t * (p3 - 2.0f * p2 + p1);
    }

    /**
     * @brief Evaluate curvature at parameter t
     */
    float EvaluateCurvature(float t) const
    {
        T d1 = EvaluateTangent(t);
        T d2 = EvaluateSecondDerivative(t);

        float d1Len = glm::length(d1);
        if (d1Len < EPSILON) return 0.0f;

        // For 3D: |d1 x d2| / |d1|^3
        // For 2D: (d1.x * d2.y - d1.y * d2.x) / |d1|^3
        return GetCurvatureImpl(d1, d2, d1Len);
    }

    /**
     * @brief Split the curve at parameter t using De Casteljau
     */
    void Split(float t, CubicBezier& left, CubicBezier& right) const
    {
        T q0 = glm::mix(p0, p1, t);
        T q1 = glm::mix(p1, p2, t);
        T q2 = glm::mix(p2, p3, t);

        T r0 = glm::mix(q0, q1, t);
        T r1 = glm::mix(q1, q2, t);

        T s0 = glm::mix(r0, r1, t);

        left = CubicBezier(p0, q0, r0, s0);
        right = CubicBezier(s0, r1, q2, p3);
    }

    /**
     * @brief Get approximate bounding box
     */
    AABB GetBoundingBox() const
    {
        AABB box;
        box.Expand(ToVec3(p0));
        box.Expand(ToVec3(p1));
        box.Expand(ToVec3(p2));
        box.Expand(ToVec3(p3));
        return box;
    }

    /**
     * @brief Elevate to higher degree (add control point)
     */
    void ElevateDegree(std::array<T, 5>& outPoints) const
    {
        outPoints[0] = p0;
        outPoints[1] = 0.25f * p0 + 0.75f * p1;
        outPoints[2] = 0.5f * p1 + 0.5f * p2;
        outPoints[3] = 0.75f * p2 + 0.25f * p3;
        outPoints[4] = p3;
    }

    /**
     * @brief Create a line as a cubic Bezier
     */
    static CubicBezier FromLine(const T& start, const T& end)
    {
        T third = (end - start) / 3.0f;
        return CubicBezier(start, start + third, end - third, end);
    }

    /**
     * @brief Create a smooth curve through two endpoints with specified tangents
     */
    static CubicBezier FromEndpointTangents(
        const T& start, const T& startTangent,
        const T& end, const T& endTangent,
        float tangentScale = 1.0f / 3.0f)
    {
        return CubicBezier(
            start,
            start + startTangent * tangentScale,
            end - endTangent * tangentScale,
            end);
    }

private:
    static Vec3 ToVec3(const Vec3& v) { return v; }
    static Vec3 ToVec3(const Vec2& v) { return Vec3(v.x, v.y, 0.0f); }

    static float GetCurvatureImpl(const Vec3& d1, const Vec3& d2, float d1Len)
    {
        Vec3 cross = glm::cross(d1, d2);
        return glm::length(cross) / (d1Len * d1Len * d1Len);
    }

    static float GetCurvatureImpl(const Vec2& d1, const Vec2& d2, float d1Len)
    {
        float cross = d1.x * d2.y - d1.y * d2.x;
        return std::abs(cross) / (d1Len * d1Len * d1Len);
    }
};

/**
 * @brief Bezier spline - multiple connected cubic Bezier segments
 */
template<typename T>
class BezierSpline
{
public:
    std::vector<CubicBezier<T>> segments;

    BezierSpline() = default;

    /**
     * @brief Add a segment to the spline
     */
    void AddSegment(const CubicBezier<T>& segment)
    {
        segments.push_back(segment);
    }

    /**
     * @brief Evaluate the spline at global parameter t in [0, 1]
     */
    T Evaluate(float t) const
    {
        if (segments.empty()) return T{};

        int numSegments = static_cast<int>(segments.size());
        float scaledT = t * numSegments;
        int segIdx = static_cast<int>(scaledT);
        segIdx = std::clamp(segIdx, 0, numSegments - 1);

        float localT = scaledT - segIdx;
        localT = std::clamp(localT, 0.0f, 1.0f);

        return segments[segIdx].Evaluate(localT);
    }

    /**
     * @brief Get total length of the spline
     */
    float GetLength(int samplesPerSegment = 16) const
    {
        float length = 0.0f;
        for (const auto& seg : segments)
        {
            length += seg.GetLength(samplesPerSegment);
        }
        return length;
    }

    /**
     * @brief Build from a list of points (C1 continuous)
     */
    static BezierSpline FromPoints(const std::vector<T>& points)
    {
        BezierSpline spline;
        if (points.size() < 2) return spline;

        for (size_t i = 0; i < points.size() - 1; ++i)
        {
            T p0 = points[i];
            T p3 = points[i + 1];

            // Simple tangent estimation
            T tangent0 = (i > 0)
                ? (points[i + 1] - points[i - 1]) * 0.25f
                : (p3 - p0) * 0.5f;
            T tangent1 = (i + 2 < points.size())
                ? (points[i + 2] - points[i]) * 0.25f
                : (p3 - p0) * 0.5f;

            T p1 = p0 + tangent0;
            T p2 = p3 - tangent1;

            spline.AddSegment(CubicBezier<T>(p0, p1, p2, p3));
        }

        return spline;
    }
};

// Convenience typedefs
using CubicBezier2D = CubicBezier<Vec2>;
using CubicBezier3D = CubicBezier<Vec3>;
using QuadraticBezier2D = QuadraticBezier<Vec2>;
using QuadraticBezier3D = QuadraticBezier<Vec3>;
using BezierSpline2D = BezierSpline<Vec2>;
using BezierSpline3D = BezierSpline<Vec3>;

} // namespace RVX::Geometry
