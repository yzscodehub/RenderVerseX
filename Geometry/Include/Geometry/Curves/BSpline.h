/**
 * @file BSpline.h
 * @brief B-Spline curve implementation
 */

#pragma once

#include "Geometry/Curves/ICurve.h"
#include "Geometry/Constants.h"
#include <vector>

namespace RVX::Geometry
{

/**
 * @brief Uniform B-Spline curve
 * 
 * A smooth curve that approximates (but doesn't necessarily pass through)
 * the control points. Provides local control - moving one control point
 * only affects a limited portion of the curve.
 * 
 * @tparam T Point type (Vec2 or Vec3)
 */
template<typename T>
class BSpline : public ICurve<T>
{
public:
    std::vector<T> controlPoints;
    int degree = 3;  ///< Degree of the B-spline (3 = cubic)

    BSpline() = default;

    BSpline(const std::vector<T>& points, int deg = 3)
        : controlPoints(points), degree(deg)
    {
        GenerateUniformKnots();
    }

    /**
     * @brief Set control points and regenerate knots
     */
    void SetControlPoints(const std::vector<T>& points)
    {
        controlPoints = points;
        GenerateUniformKnots();
    }

    /**
     * @brief Add a control point
     */
    void AddControlPoint(const T& point)
    {
        controlPoints.push_back(point);
        GenerateUniformKnots();
    }

    /**
     * @brief Get the valid parameter range
     */
    void GetParameterRange(float& tMin, float& tMax) const
    {
        if (knots.empty())
        {
            tMin = 0.0f;
            tMax = 1.0f;
            return;
        }

        int n = static_cast<int>(controlPoints.size()) - 1;
        tMin = knots[degree];
        tMax = knots[n + 1];
    }

    /**
     * @brief Evaluate the B-spline at parameter t
     */
    T Evaluate(float t) const override
    {
        if (controlPoints.size() < static_cast<size_t>(degree + 1))
        {
            return controlPoints.empty() ? T{} : controlPoints[0];
        }

        // Map t from [0, 1] to valid knot range
        float tMin, tMax;
        GetParameterRange(tMin, tMax);
        float u = tMin + t * (tMax - tMin);

        // Find knot span
        int span = FindKnotSpan(u);

        // Compute basis functions
        std::vector<float> N(degree + 1);
        ComputeBasisFunctions(span, u, N);

        // Evaluate point
        T result{};
        for (int i = 0; i <= degree; ++i)
        {
            result = result + controlPoints[span - degree + i] * N[i];
        }

        return result;
    }

    /**
     * @brief Evaluate tangent at parameter t
     */
    T EvaluateTangent(float t) const override
    {
        if (controlPoints.size() < static_cast<size_t>(degree + 1))
        {
            return T{};
        }

        float tMin, tMax;
        GetParameterRange(tMin, tMax);
        float u = tMin + t * (tMax - tMin);

        int span = FindKnotSpan(u);

        // Compute derivative basis functions
        std::vector<std::vector<float>> ders(2, std::vector<float>(degree + 1));
        ComputeBasisFunctionDerivatives(span, u, 1, ders);

        T result{};
        for (int i = 0; i <= degree; ++i)
        {
            result = result + controlPoints[span - degree + i] * ders[1][i];
        }

        return result * (tMax - tMin);  // Chain rule for parameter mapping
    }

    /**
     * @brief Insert a knot at parameter u
     */
    void InsertKnot(float u, int times = 1)
    {
        for (int r = 0; r < times; ++r)
        {
            InsertKnotOnce(u);
        }
    }

private:
    std::vector<float> knots;

    void GenerateUniformKnots()
    {
        int n = static_cast<int>(controlPoints.size()) - 1;
        int m = n + degree + 1;

        knots.resize(m + 1);

        // Clamped uniform knot vector
        for (int i = 0; i <= degree; ++i)
        {
            knots[i] = 0.0f;
        }

        int numInternalKnots = m - 2 * degree - 1;
        for (int i = 1; i <= numInternalKnots; ++i)
        {
            knots[degree + i] = static_cast<float>(i) / (numInternalKnots + 1);
        }

        for (int i = m - degree; i <= m; ++i)
        {
            knots[i] = 1.0f;
        }
    }

    int FindKnotSpan(float u) const
    {
        int n = static_cast<int>(controlPoints.size()) - 1;

        // Special case: u at end
        if (u >= knots[n + 1])
        {
            return n;
        }

        // Binary search
        int low = degree;
        int high = n + 1;

        while (low < high)
        {
            int mid = (low + high) / 2;
            if (u < knots[mid])
            {
                high = mid;
            }
            else
            {
                low = mid + 1;
            }
        }

        return low - 1;
    }

    void ComputeBasisFunctions(int span, float u, std::vector<float>& N) const
    {
        std::vector<float> left(degree + 1);
        std::vector<float> right(degree + 1);

        N[0] = 1.0f;

        for (int j = 1; j <= degree; ++j)
        {
            left[j] = u - knots[span + 1 - j];
            right[j] = knots[span + j] - u;

            float saved = 0.0f;
            for (int r = 0; r < j; ++r)
            {
                float temp = N[r] / (right[r + 1] + left[j - r]);
                N[r] = saved + right[r + 1] * temp;
                saved = left[j - r] * temp;
            }
            N[j] = saved;
        }
    }

    void ComputeBasisFunctionDerivatives(
        int span, float u, int numDerivatives,
        std::vector<std::vector<float>>& ders) const
    {
        std::vector<std::vector<float>> ndu(degree + 1, std::vector<float>(degree + 1));
        std::vector<float> left(degree + 1);
        std::vector<float> right(degree + 1);

        ndu[0][0] = 1.0f;

        for (int j = 1; j <= degree; ++j)
        {
            left[j] = u - knots[span + 1 - j];
            right[j] = knots[span + j] - u;

            float saved = 0.0f;
            for (int r = 0; r < j; ++r)
            {
                ndu[j][r] = right[r + 1] + left[j - r];
                float temp = ndu[r][j - 1] / ndu[j][r];
                ndu[r][j] = saved + right[r + 1] * temp;
                saved = left[j - r] * temp;
            }
            ndu[j][j] = saved;
        }

        // Load basis functions
        for (int j = 0; j <= degree; ++j)
        {
            ders[0][j] = ndu[j][degree];
        }

        // Compute derivatives
        std::vector<std::vector<float>> a(2, std::vector<float>(degree + 1));

        for (int r = 0; r <= degree; ++r)
        {
            int s1 = 0, s2 = 1;
            a[0][0] = 1.0f;

            for (int k = 1; k <= numDerivatives; ++k)
            {
                float d = 0.0f;
                int rk = r - k;
                int pk = degree - k;

                if (r >= k)
                {
                    a[s2][0] = a[s1][0] / ndu[pk + 1][rk];
                    d = a[s2][0] * ndu[rk][pk];
                }

                int j1 = (rk >= -1) ? 1 : -rk;
                int j2 = (r - 1 <= pk) ? k - 1 : degree - r;

                for (int j = j1; j <= j2; ++j)
                {
                    a[s2][j] = (a[s1][j] - a[s1][j - 1]) / ndu[pk + 1][rk + j];
                    d += a[s2][j] * ndu[rk + j][pk];
                }

                if (r <= pk)
                {
                    a[s2][k] = -a[s1][k - 1] / ndu[pk + 1][r];
                    d += a[s2][k] * ndu[r][pk];
                }

                ders[k][r] = d;
                std::swap(s1, s2);
            }
        }

        // Multiply by correct factors
        float r = static_cast<float>(degree);
        for (int k = 1; k <= numDerivatives; ++k)
        {
            for (int j = 0; j <= degree; ++j)
            {
                ders[k][j] *= r;
            }
            r *= (degree - k);
        }
    }

    void InsertKnotOnce(float u)
    {
        int span = FindKnotSpan(u);
        int n = static_cast<int>(controlPoints.size()) - 1;

        // New control points
        std::vector<T> newPoints(n + 2);

        for (int i = 0; i <= span - degree; ++i)
        {
            newPoints[i] = controlPoints[i];
        }

        for (int i = span - degree + 1; i <= span; ++i)
        {
            float alpha = (u - knots[i]) / (knots[i + degree] - knots[i]);
            newPoints[i] = (1.0f - alpha) * controlPoints[i - 1] + alpha * controlPoints[i];
        }

        for (int i = span + 1; i <= n + 1; ++i)
        {
            newPoints[i] = controlPoints[i - 1];
        }

        controlPoints = newPoints;

        // Insert knot
        knots.insert(knots.begin() + span + 1, u);
    }
};

// Convenience typedefs
using BSpline2D = BSpline<Vec2>;
using BSpline3D = BSpline<Vec3>;

} // namespace RVX::Geometry
