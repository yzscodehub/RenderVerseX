/**
 * @file GJK.h
 * @brief Gilbert-Johnson-Keerthi collision detection algorithm
 */

#pragma once

#include "Core/MathTypes.h"
#include "Geometry/Collision/IConvexShape.h"
#include "Geometry/Constants.h"
#include <array>
#include <cmath>

namespace RVX::Geometry
{

/**
 * @brief Result of GJK collision query
 */
struct GJKResult
{
    bool intersecting = false;      ///< True if shapes overlap
    Vec3 closestA{0};               ///< Closest point on shape A (if not intersecting)
    Vec3 closestB{0};               ///< Closest point on shape B (if not intersecting)
    float distance = 0.0f;          ///< Distance between shapes (0 if intersecting)
    int iterations = 0;             ///< Number of iterations used
};

/**
 * @brief GJK Simplex for 3D
 */
struct Simplex
{
    std::array<Vec3, 4> points;     ///< Simplex vertices (in Minkowski difference)
    std::array<Vec3, 4> supportA;   ///< Support points from shape A
    std::array<Vec3, 4> supportB;   ///< Support points from shape B
    int size = 0;                   ///< Number of points (1-4)

    void Push(const Vec3& point, const Vec3& supA, const Vec3& supB)
    {
        points[size] = point;
        supportA[size] = supA;
        supportB[size] = supB;
        ++size;
    }

    void Clear() { size = 0; }

    Vec3 Last() const { return points[size - 1]; }
};

/**
 * @brief GJK collision detection algorithm
 * 
 * Tests if two convex shapes intersect. If they do not, returns the
 * closest points on each shape.
 */
class GJK
{
public:
    /**
     * @brief Query collision between two convex shapes
     * 
     * @param a First shape
     * @param b Second shape
     * @param maxIterations Maximum iterations (default: 32)
     * @return GJKResult containing collision info
     */
    static GJKResult Query(
        const IConvexShape& a,
        const IConvexShape& b,
        int maxIterations = 32)
    {
        GJKResult result;
        Simplex simplex;

        // Initial direction: from center of A to center of B
        Vec3 direction = b.GetCenter() - a.GetCenter();
        if (glm::dot(direction, direction) < EPSILON)
        {
            direction = Vec3(1, 0, 0);
        }

        // First support point
        Vec3 supA = a.Support(direction);
        Vec3 supB = b.Support(-direction);
        Vec3 point = supA - supB;
        simplex.Push(point, supA, supB);

        direction = -point;

        for (int i = 0; i < maxIterations; ++i)
        {
            result.iterations = i + 1;

            // Get new support point
            supA = a.Support(direction);
            supB = b.Support(-direction);
            point = supA - supB;

            // Check if we've passed the origin
            if (glm::dot(point, direction) < 0)
            {
                // No intersection - compute closest points
                result.intersecting = false;
                ComputeClosestPoints(simplex, result);
                return result;
            }

            simplex.Push(point, supA, supB);

            // Check if origin is inside the simplex
            if (DoSimplex(simplex, direction))
            {
                result.intersecting = true;
                result.distance = 0.0f;
                return result;
            }
        }

        // Did not converge - assume intersection
        result.intersecting = true;
        return result;
    }

private:
    /**
     * @brief Process simplex and update direction
     * @return true if origin is inside simplex
     */
    static bool DoSimplex(Simplex& simplex, Vec3& direction)
    {
        switch (simplex.size)
        {
            case 2: return Line(simplex, direction);
            case 3: return Triangle(simplex, direction);
            case 4: return Tetrahedron(simplex, direction);
        }
        return false;
    }

    static bool Line(Simplex& simplex, Vec3& direction)
    {
        Vec3 a = simplex.points[1];
        Vec3 b = simplex.points[0];

        Vec3 ab = b - a;
        Vec3 ao = -a;

        if (glm::dot(ab, ao) > 0)
        {
            direction = glm::cross(glm::cross(ab, ao), ab);
            if (glm::dot(direction, direction) < EPSILON)
            {
                // Degenerate case - pick perpendicular
                direction = glm::cross(ab, Vec3(1, 0, 0));
                if (glm::dot(direction, direction) < EPSILON)
                    direction = glm::cross(ab, Vec3(0, 1, 0));
            }
        }
        else
        {
            // Remove point b, keep only a
            simplex.points[0] = simplex.points[1];
            simplex.supportA[0] = simplex.supportA[1];
            simplex.supportB[0] = simplex.supportB[1];
            simplex.size = 1;
            direction = ao;
        }

        return false;
    }

    static bool Triangle(Simplex& simplex, Vec3& direction)
    {
        Vec3 a = simplex.points[2];
        Vec3 b = simplex.points[1];
        Vec3 c = simplex.points[0];

        Vec3 ab = b - a;
        Vec3 ac = c - a;
        Vec3 ao = -a;

        Vec3 abc = glm::cross(ab, ac);

        if (glm::dot(glm::cross(abc, ac), ao) > 0)
        {
            if (glm::dot(ac, ao) > 0)
            {
                // Keep a, c
                simplex.points[0] = simplex.points[0];
                simplex.points[1] = simplex.points[2];
                simplex.supportA[1] = simplex.supportA[2];
                simplex.supportB[1] = simplex.supportB[2];
                simplex.size = 2;
                direction = glm::cross(glm::cross(ac, ao), ac);
            }
            else
            {
                return Line(SetLine(simplex, a, b), direction);
            }
        }
        else
        {
            if (glm::dot(glm::cross(ab, abc), ao) > 0)
            {
                return Line(SetLine(simplex, a, b), direction);
            }
            else
            {
                if (glm::dot(abc, ao) > 0)
                {
                    direction = abc;
                }
                else
                {
                    // Flip winding
                    std::swap(simplex.points[0], simplex.points[1]);
                    std::swap(simplex.supportA[0], simplex.supportA[1]);
                    std::swap(simplex.supportB[0], simplex.supportB[1]);
                    direction = -abc;
                }
            }
        }

        return false;
    }

    static Simplex& SetLine(Simplex& simplex, const Vec3& a, const Vec3& b)
    {
        // Find indices
        int ai = -1, bi = -1;
        for (int i = 0; i < simplex.size; ++i)
        {
            if (glm::length(simplex.points[i] - a) < EPSILON) ai = i;
            if (glm::length(simplex.points[i] - b) < EPSILON) bi = i;
        }

        if (ai >= 0 && bi >= 0)
        {
            simplex.points[0] = simplex.points[bi];
            simplex.points[1] = simplex.points[ai];
            simplex.supportA[0] = simplex.supportA[bi];
            simplex.supportA[1] = simplex.supportA[ai];
            simplex.supportB[0] = simplex.supportB[bi];
            simplex.supportB[1] = simplex.supportB[ai];
        }
        simplex.size = 2;
        return simplex;
    }

    static bool Tetrahedron(Simplex& simplex, Vec3& direction)
    {
        Vec3 a = simplex.points[3];
        Vec3 b = simplex.points[2];
        Vec3 c = simplex.points[1];
        Vec3 d = simplex.points[0];

        Vec3 ab = b - a;
        Vec3 ac = c - a;
        Vec3 ad = d - a;
        Vec3 ao = -a;

        Vec3 abc = glm::cross(ab, ac);
        Vec3 acd = glm::cross(ac, ad);
        Vec3 adb = glm::cross(ad, ab);

        if (glm::dot(abc, ao) > 0)
        {
            // Remove d
            simplex.points[0] = simplex.points[1];
            simplex.points[1] = simplex.points[2];
            simplex.points[2] = simplex.points[3];
            simplex.supportA[0] = simplex.supportA[1];
            simplex.supportA[1] = simplex.supportA[2];
            simplex.supportA[2] = simplex.supportA[3];
            simplex.supportB[0] = simplex.supportB[1];
            simplex.supportB[1] = simplex.supportB[2];
            simplex.supportB[2] = simplex.supportB[3];
            simplex.size = 3;
            return Triangle(simplex, direction);
        }

        if (glm::dot(acd, ao) > 0)
        {
            // Remove b: keep a, c, d
            simplex.points[1] = simplex.points[2];
            simplex.points[2] = simplex.points[3];
            simplex.supportA[1] = simplex.supportA[2];
            simplex.supportA[2] = simplex.supportA[3];
            simplex.supportB[1] = simplex.supportB[2];
            simplex.supportB[2] = simplex.supportB[3];
            simplex.size = 3;
            return Triangle(simplex, direction);
        }

        if (glm::dot(adb, ao) > 0)
        {
            // Remove c: keep a, d, b
            simplex.points[1] = simplex.points[0];
            simplex.points[0] = simplex.points[2];
            simplex.points[2] = simplex.points[3];
            simplex.supportA[1] = simplex.supportA[0];
            simplex.supportA[0] = simplex.supportA[2];
            simplex.supportA[2] = simplex.supportA[3];
            simplex.supportB[1] = simplex.supportB[0];
            simplex.supportB[0] = simplex.supportB[2];
            simplex.supportB[2] = simplex.supportB[3];
            simplex.size = 3;
            return Triangle(simplex, direction);
        }

        // Origin is inside tetrahedron
        return true;
    }

    static void ComputeClosestPoints(const Simplex& simplex, GJKResult& result)
    {
        // Compute barycentric coordinates of origin's projection onto simplex
        // Then interpolate support points

        switch (simplex.size)
        {
            case 1:
                result.closestA = simplex.supportA[0];
                result.closestB = simplex.supportB[0];
                result.distance = glm::length(simplex.points[0]);
                break;

            case 2:
            {
                Vec3 a = simplex.points[0];
                Vec3 b = simplex.points[1];
                Vec3 ab = b - a;
                float t = -glm::dot(a, ab) / glm::dot(ab, ab);
                t = std::clamp(t, 0.0f, 1.0f);

                result.closestA = simplex.supportA[0] * (1 - t) + simplex.supportA[1] * t;
                result.closestB = simplex.supportB[0] * (1 - t) + simplex.supportB[1] * t;
                Vec3 closest = a + ab * t;
                result.distance = glm::length(closest);
                break;
            }

            case 3:
            {
                // Project origin onto triangle plane
                Vec3 a = simplex.points[0];
                Vec3 b = simplex.points[1];
                Vec3 c = simplex.points[2];

                Vec3 ab = b - a;
                Vec3 ac = c - a;
                Vec3 ao = -a;

                float d00 = glm::dot(ab, ab);
                float d01 = glm::dot(ab, ac);
                float d11 = glm::dot(ac, ac);
                float d20 = glm::dot(ao, ab);
                float d21 = glm::dot(ao, ac);

                float denom = d00 * d11 - d01 * d01;
                if (std::abs(denom) < EPSILON)
                {
                    result.closestA = simplex.supportA[0];
                    result.closestB = simplex.supportB[0];
                    result.distance = glm::length(a);
                    break;
                }

                float v = (d11 * d20 - d01 * d21) / denom;
                float w = (d00 * d21 - d01 * d20) / denom;
                float u = 1.0f - v - w;

                result.closestA = simplex.supportA[0] * u + simplex.supportA[1] * v + simplex.supportA[2] * w;
                result.closestB = simplex.supportB[0] * u + simplex.supportB[1] * v + simplex.supportB[2] * w;
                Vec3 closest = a * u + b * v + c * w;
                result.distance = glm::length(closest);
                break;
            }

            default:
                result.distance = 0.0f;
                break;
        }
    }
};

} // namespace RVX::Geometry
