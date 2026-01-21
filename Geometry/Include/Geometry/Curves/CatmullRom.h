/**
 * @file CatmullRom.h
 * @brief Catmull-Rom spline implementation
 */

#pragma once

#include "Geometry/Curves/ICurve.h"
#include "Geometry/Constants.h"
#include <vector>

namespace RVX::Geometry
{

/**
 * @brief Catmull-Rom spline
 * 
 * A smooth interpolating spline that passes through all control points.
 * Commonly used for camera paths and smooth motion.
 * 
 * @tparam T Point type (Vec2 or Vec3)
 */
template<typename T>
class CatmullRomSpline : public ICurve<T>
{
public:
    std::vector<T> points;
    float tension = 0.5f;  ///< Controls the tightness of the curve (0.5 = standard)
    bool closed = false;   ///< Whether the spline forms a closed loop

    CatmullRomSpline() = default;

    explicit CatmullRomSpline(const std::vector<T>& pts, float t = 0.5f, bool loop = false)
        : points(pts), tension(t), closed(loop) {}

    /**
     * @brief Add a control point
     */
    void AddPoint(const T& point)
    {
        points.push_back(point);
    }

    /**
     * @brief Get number of segments
     */
    int GetSegmentCount() const
    {
        if (points.size() < 2) return 0;
        return closed
            ? static_cast<int>(points.size())
            : static_cast<int>(points.size()) - 1;
    }

    /**
     * @brief Evaluate the spline at parameter t in [0, 1]
     */
    T Evaluate(float t) const override
    {
        int numSegments = GetSegmentCount();
        if (numSegments == 0)
        {
            return points.empty() ? T{} : points[0];
        }

        // Map t to segment
        float scaledT = t * numSegments;
        int segIdx = static_cast<int>(scaledT);
        segIdx = std::clamp(segIdx, 0, numSegments - 1);
        float localT = scaledT - segIdx;

        return EvaluateSegment(segIdx, localT);
    }

    /**
     * @brief Evaluate tangent at parameter t
     */
    T EvaluateTangent(float t) const override
    {
        int numSegments = GetSegmentCount();
        if (numSegments == 0) return T{};

        float scaledT = t * numSegments;
        int segIdx = static_cast<int>(scaledT);
        segIdx = std::clamp(segIdx, 0, numSegments - 1);
        float localT = scaledT - segIdx;

        return EvaluateSegmentTangent(segIdx, localT);
    }

    /**
     * @brief Evaluate a specific segment
     * @param segIdx Segment index
     * @param t Local parameter in [0, 1]
     */
    T EvaluateSegment(int segIdx, float t) const
    {
        int n = static_cast<int>(points.size());
        if (n < 2) return points.empty() ? T{} : points[0];

        // Get the 4 control points for this segment
        int i0, i1, i2, i3;
        GetSegmentIndices(segIdx, i0, i1, i2, i3);

        T p0 = points[i0];
        T p1 = points[i1];
        T p2 = points[i2];
        T p3 = points[i3];

        // Catmull-Rom basis matrix with tension
        float t2 = t * t;
        float t3 = t2 * t;

        float s = tension;
        
        // Basis functions
        float h1 = -s * t3 + 2.0f * s * t2 - s * t;
        float h2 = (2.0f - s) * t3 + (s - 3.0f) * t2 + 1.0f;
        float h3 = (s - 2.0f) * t3 + (3.0f - 2.0f * s) * t2 + s * t;
        float h4 = s * t3 - s * t2;

        return h1 * p0 + h2 * p1 + h3 * p2 + h4 * p3;
    }

    /**
     * @brief Evaluate segment tangent
     */
    T EvaluateSegmentTangent(int segIdx, float t) const
    {
        int n = static_cast<int>(points.size());
        if (n < 2) return T{};

        int i0, i1, i2, i3;
        GetSegmentIndices(segIdx, i0, i1, i2, i3);

        T p0 = points[i0];
        T p1 = points[i1];
        T p2 = points[i2];
        T p3 = points[i3];

        float t2 = t * t;
        float s = tension;

        // Derivative of basis functions
        float h1 = -3.0f * s * t2 + 4.0f * s * t - s;
        float h2 = 3.0f * (2.0f - s) * t2 + 2.0f * (s - 3.0f) * t;
        float h3 = 3.0f * (s - 2.0f) * t2 + 2.0f * (3.0f - 2.0f * s) * t + s;
        float h4 = 3.0f * s * t2 - 2.0f * s * t;

        return h1 * p0 + h2 * p1 + h3 * p2 + h4 * p3;
    }

    /**
     * @brief Get total arc length
     */
    float GetLength(int samplesPerSegment = 16) const
    {
        int numSegments = GetSegmentCount();
        if (numSegments == 0) return 0.0f;

        float length = 0.0f;
        T prev = EvaluateSegment(0, 0.0f);

        for (int seg = 0; seg < numSegments; ++seg)
        {
            for (int i = 1; i <= samplesPerSegment; ++i)
            {
                float t = static_cast<float>(i) / samplesPerSegment;
                T curr = EvaluateSegment(seg, t);
                length += glm::length(curr - prev);
                prev = curr;
            }
        }

        return length;
    }

    /**
     * @brief Sample the spline uniformly by arc length
     */
    void SampleByArcLength(float spacing, std::vector<T>& outPoints) const
    {
        outPoints.clear();
        float totalLength = GetLength();
        int numPoints = static_cast<int>(totalLength / spacing) + 1;

        for (int i = 0; i < numPoints; ++i)
        {
            float targetDist = i * spacing;
            float t = GetParameterAtDistance(targetDist);
            outPoints.push_back(Evaluate(t));
        }
    }

    /**
     * @brief Get the point closest to a given position
     */
    T GetClosestPoint(const T& position, float* outT = nullptr, int samples = 64) const
    {
        float minDistSq = FLT_MAX;
        float bestT = 0.0f;

        for (int i = 0; i <= samples; ++i)
        {
            float t = static_cast<float>(i) / samples;
            T p = Evaluate(t);
            T d = p - position;
            float distSq = glm::dot(d, d);

            if (distSq < minDistSq)
            {
                minDistSq = distSq;
                bestT = t;
            }
        }

        if (outT) *outT = bestT;
        return Evaluate(bestT);
    }

private:
    void GetSegmentIndices(int segIdx, int& i0, int& i1, int& i2, int& i3) const
    {
        int n = static_cast<int>(points.size());

        if (closed)
        {
            i0 = (segIdx - 1 + n) % n;
            i1 = segIdx % n;
            i2 = (segIdx + 1) % n;
            i3 = (segIdx + 2) % n;
        }
        else
        {
            i1 = segIdx;
            i2 = segIdx + 1;
            i0 = i1 > 0 ? i1 - 1 : i1;
            i3 = i2 < n - 1 ? i2 + 1 : i2;
        }
    }

    float GetParameterAtDistance(float distance, int samplesPerSegment = 32) const
    {
        int numSegments = GetSegmentCount();
        if (numSegments == 0) return 0.0f;

        float totalLength = 0.0f;
        T prev = EvaluateSegment(0, 0.0f);

        for (int seg = 0; seg < numSegments; ++seg)
        {
            for (int i = 1; i <= samplesPerSegment; ++i)
            {
                float localT = static_cast<float>(i) / samplesPerSegment;
                T curr = EvaluateSegment(seg, localT);
                float segmentLength = glm::length(curr - prev);

                if (totalLength + segmentLength >= distance)
                {
                    float remaining = distance - totalLength;
                    float frac = remaining / segmentLength;
                    float prevLocalT = static_cast<float>(i - 1) / samplesPerSegment;
                    float globalT = (seg + prevLocalT + frac / samplesPerSegment) / numSegments;
                    return globalT;
                }

                totalLength += segmentLength;
                prev = curr;
            }
        }

        return 1.0f;
    }
};

// Convenience typedefs
using CatmullRomSpline2D = CatmullRomSpline<Vec2>;
using CatmullRomSpline3D = CatmullRomSpline<Vec3>;

} // namespace RVX::Geometry
