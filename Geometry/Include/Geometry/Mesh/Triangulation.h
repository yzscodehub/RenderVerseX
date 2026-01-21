/**
 * @file Triangulation.h
 * @brief Polygon triangulation algorithms
 */

#pragma once

#include "Core/MathTypes.h"
#include "Geometry/Constants.h"
#include <vector>
#include <span>
#include <cmath>

namespace RVX::Geometry
{

/**
 * @brief Polygon triangulation algorithms
 */
class Triangulator
{
public:
    /**
     * @brief Triangulate a simple polygon using ear clipping
     * 
     * Works for simple (non-self-intersecting) polygons.
     * Time complexity: O(n^2)
     * 
     * @param polygon 2D polygon vertices in CCW order
     * @param outIndices Output triangle indices
     */
    static void EarClipping(
        std::span<const Vec2> polygon,
        std::vector<uint32_t>& outIndices)
    {
        outIndices.clear();
        int n = static_cast<int>(polygon.size());
        if (n < 3) return;

        // Create index list
        std::vector<int> indices(n);
        for (int i = 0; i < n; ++i) indices[i] = i;

        // Determine winding order
        float signedArea = 0;
        for (int i = 0; i < n; ++i)
        {
            int j = (i + 1) % n;
            signedArea += polygon[i].x * polygon[j].y - polygon[j].x * polygon[i].y;
        }
        bool ccw = signedArea > 0;

        // Ear clipping
        int remaining = n;
        int i = 0;
        int maxIterations = n * n;  // Prevent infinite loops

        while (remaining > 3 && maxIterations-- > 0)
        {
            int prev = (i + remaining - 1) % remaining;
            int next = (i + 1) % remaining;

            int iPrev = indices[prev];
            int iCurr = indices[i];
            int iNext = indices[next];

            Vec2 a = polygon[iPrev];
            Vec2 b = polygon[iCurr];
            Vec2 c = polygon[iNext];

            // Check if ear is convex
            bool convex = IsConvex(a, b, c, ccw);

            if (convex)
            {
                // Check if any other vertex is inside this triangle
                bool isEar = true;
                for (int j = 0; j < remaining; ++j)
                {
                    if (j == prev || j == i || j == next) continue;

                    if (PointInTriangle(polygon[indices[j]], a, b, c))
                    {
                        isEar = false;
                        break;
                    }
                }

                if (isEar)
                {
                    // Clip the ear
                    outIndices.push_back(static_cast<uint32_t>(iPrev));
                    outIndices.push_back(static_cast<uint32_t>(iCurr));
                    outIndices.push_back(static_cast<uint32_t>(iNext));

                    indices.erase(indices.begin() + i);
                    --remaining;
                    if (i >= remaining) i = 0;
                    continue;
                }
            }

            i = (i + 1) % remaining;
        }

        // Add last triangle
        if (remaining == 3)
        {
            outIndices.push_back(static_cast<uint32_t>(indices[0]));
            outIndices.push_back(static_cast<uint32_t>(indices[1]));
            outIndices.push_back(static_cast<uint32_t>(indices[2]));
        }
    }

    /**
     * @brief Triangulate a 3D polygon (projects to best-fit plane)
     */
    static void EarClipping3D(
        std::span<const Vec3> polygon,
        std::vector<uint32_t>& outIndices)
    {
        outIndices.clear();
        if (polygon.size() < 3) return;

        // Compute polygon normal
        Vec3 normal(0);
        for (size_t i = 0; i < polygon.size(); ++i)
        {
            size_t j = (i + 1) % polygon.size();
            normal += glm::cross(polygon[i], polygon[j]);
        }
        normal = glm::normalize(normal);

        // Find dominant axis for projection
        Vec3 absNormal = glm::abs(normal);
        int dropAxis = 0;
        if (absNormal.y > absNormal.x) dropAxis = 1;
        if (absNormal.z > absNormal[dropAxis]) dropAxis = 2;

        // Project to 2D
        std::vector<Vec2> projected(polygon.size());
        for (size_t i = 0; i < polygon.size(); ++i)
        {
            switch (dropAxis)
            {
                case 0: projected[i] = Vec2(polygon[i].y, polygon[i].z); break;
                case 1: projected[i] = Vec2(polygon[i].x, polygon[i].z); break;
                case 2: projected[i] = Vec2(polygon[i].x, polygon[i].y); break;
            }
        }

        EarClipping(projected, outIndices);
    }

    /**
     * @brief Fan triangulation (only valid for convex polygons)
     */
    static void FanTriangulation(
        int numVertices,
        std::vector<uint32_t>& outIndices)
    {
        outIndices.clear();
        if (numVertices < 3) return;

        for (int i = 1; i < numVertices - 1; ++i)
        {
            outIndices.push_back(0);
            outIndices.push_back(static_cast<uint32_t>(i));
            outIndices.push_back(static_cast<uint32_t>(i + 1));
        }
    }

private:
    static bool IsConvex(const Vec2& a, const Vec2& b, const Vec2& c, bool ccw)
    {
        float cross = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        return ccw ? (cross > 0) : (cross < 0);
    }

    static bool PointInTriangle(const Vec2& p, const Vec2& a, const Vec2& b, const Vec2& c)
    {
        auto Sign = [](const Vec2& p1, const Vec2& p2, const Vec2& p3)
        {
            return (p1.x - p3.x) * (p2.y - p3.y) - (p2.x - p3.x) * (p1.y - p3.y);
        };

        float d1 = Sign(p, a, b);
        float d2 = Sign(p, b, c);
        float d3 = Sign(p, c, a);

        bool hasNeg = (d1 < 0) || (d2 < 0) || (d3 < 0);
        bool hasPos = (d1 > 0) || (d2 > 0) || (d3 > 0);

        return !(hasNeg && hasPos);
    }
};

} // namespace RVX::Geometry
