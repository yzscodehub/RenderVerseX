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
#include <algorithm>
#include <array>

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

// ============================================================================
// Delaunay Triangulation
// ============================================================================

/**
 * @brief Edge in a triangulation
 */
struct TriangulationEdge
{
    uint32_t v0, v1;

    bool operator==(const TriangulationEdge& other) const
    {
        return (v0 == other.v0 && v1 == other.v1) ||
               (v0 == other.v1 && v1 == other.v0);
    }
};

/**
 * @brief Triangle with circumcircle data
 */
struct DelaunayTriangle
{
    uint32_t v0, v1, v2;
    Vec2 circumcenter;
    float circumradiusSq;

    DelaunayTriangle() = default;

    DelaunayTriangle(uint32_t a, uint32_t b, uint32_t c, const std::vector<Vec2>& points)
        : v0(a), v1(b), v2(c)
    {
        ComputeCircumcircle(points);
    }

    /**
     * @brief Check if a point is inside the circumcircle
     */
    bool ContainsInCircumcircle(const Vec2& point) const
    {
        float dx = point.x - circumcenter.x;
        float dy = point.y - circumcenter.y;
        return (dx * dx + dy * dy) < circumradiusSq;
    }

    /**
     * @brief Get the three edges of the triangle
     */
    std::array<TriangulationEdge, 3> GetEdges() const
    {
        return {{
            {v0, v1},
            {v1, v2},
            {v2, v0}
        }};
    }

private:
    void ComputeCircumcircle(const std::vector<Vec2>& points)
    {
        const Vec2& a = points[v0];
        const Vec2& b = points[v1];
        const Vec2& c = points[v2];

        float ax = a.x, ay = a.y;
        float bx = b.x, by = b.y;
        float cx = c.x, cy = c.y;

        float d = 2.0f * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));

        if (std::abs(d) < EPSILON)
        {
            // Degenerate triangle
            circumcenter = (a + b + c) / 3.0f;
            circumradiusSq = std::numeric_limits<float>::max();
            return;
        }

        float aSq = ax * ax + ay * ay;
        float bSq = bx * bx + by * by;
        float cSq = cx * cx + cy * cy;

        circumcenter.x = (aSq * (by - cy) + bSq * (cy - ay) + cSq * (ay - by)) / d;
        circumcenter.y = (aSq * (cx - bx) + bSq * (ax - cx) + cSq * (bx - ax)) / d;

        float dx = ax - circumcenter.x;
        float dy = ay - circumcenter.y;
        circumradiusSq = dx * dx + dy * dy;
    }
};

/**
 * @brief Delaunay triangulation using Bowyer-Watson algorithm
 * 
 * Creates a Delaunay triangulation of a 2D point set.
 * Time complexity: O(n^2) average case, O(n^2) worst case
 */
class DelaunayTriangulator
{
public:
    /**
     * @brief Triangulate a set of 2D points
     * 
     * @param points Input points
     * @param outTriangles Output triangle indices (triplets of vertex indices)
     */
    static void Triangulate(
        std::span<const Vec2> points,
        std::vector<uint32_t>& outTriangles)
    {
        outTriangles.clear();

        if (points.size() < 3)
            return;

        // Create a working copy of points (including super-triangle)
        std::vector<Vec2> allPoints(points.begin(), points.end());

        // Compute bounding box
        Vec2 minPt = allPoints[0];
        Vec2 maxPt = allPoints[0];
        for (const auto& p : allPoints)
        {
            minPt = glm::min(minPt, p);
            maxPt = glm::max(maxPt, p);
        }

        // Create super-triangle that contains all points
        Vec2 range = maxPt - minPt;
        float maxRange = std::max(range.x, range.y);
        Vec2 center = (minPt + maxPt) * 0.5f;

        // Super-triangle vertices (large enough to contain all points)
        float superSize = maxRange * 10.0f;
        Vec2 st0 = center + Vec2(0, superSize * 2.0f);
        Vec2 st1 = center + Vec2(-superSize * 1.5f, -superSize);
        Vec2 st2 = center + Vec2(superSize * 1.5f, -superSize);

        uint32_t superIdx0 = static_cast<uint32_t>(allPoints.size());
        uint32_t superIdx1 = superIdx0 + 1;
        uint32_t superIdx2 = superIdx0 + 2;

        allPoints.push_back(st0);
        allPoints.push_back(st1);
        allPoints.push_back(st2);

        // Initialize triangulation with super-triangle
        std::vector<DelaunayTriangle> triangles;
        triangles.emplace_back(superIdx0, superIdx1, superIdx2, allPoints);

        // Insert each point
        uint32_t numOriginalPoints = static_cast<uint32_t>(points.size());
        for (uint32_t i = 0; i < numOriginalPoints; ++i)
        {
            InsertPoint(i, allPoints, triangles);
        }

        // Remove triangles connected to super-triangle vertices
        triangles.erase(
            std::remove_if(triangles.begin(), triangles.end(),
                [superIdx0, superIdx1, superIdx2](const DelaunayTriangle& t)
                {
                    return t.v0 >= superIdx0 || t.v1 >= superIdx0 || t.v2 >= superIdx0;
                }),
            triangles.end());

        // Output triangle indices
        for (const auto& tri : triangles)
        {
            outTriangles.push_back(tri.v0);
            outTriangles.push_back(tri.v1);
            outTriangles.push_back(tri.v2);
        }
    }

    /**
     * @brief Triangulate 3D points (projects to best-fit plane first)
     */
    static void Triangulate3D(
        std::span<const Vec3> points,
        std::vector<uint32_t>& outTriangles)
    {
        outTriangles.clear();
        if (points.size() < 3) return;

        // Compute centroid
        Vec3 centroid(0);
        for (const auto& p : points)
        {
            centroid += p;
        }
        centroid /= static_cast<float>(points.size());

        // Compute covariance matrix for best-fit plane
        float cxx = 0, cxy = 0, cxz = 0, cyy = 0, cyz = 0, czz = 0;
        for (const auto& p : points)
        {
            Vec3 r = p - centroid;
            cxx += r.x * r.x;
            cxy += r.x * r.y;
            cxz += r.x * r.z;
            cyy += r.y * r.y;
            cyz += r.y * r.z;
            czz += r.z * r.z;
        }

        // Find the axis with least variance (normal direction)
        // Simple approach: find the smallest eigenvalue axis
        // For simplicity, we'll use the weighted sum of products
        Vec3 normal;
        float detX = cyy * czz - cyz * cyz;
        float detY = cxx * czz - cxz * cxz;
        float detZ = cxx * cyy - cxy * cxy;

        if (detX >= detY && detX >= detZ)
        {
            normal = Vec3(detX, cxz * cyz - cxy * czz, cxy * cyz - cxz * cyy);
        }
        else if (detY >= detZ)
        {
            normal = Vec3(cxz * cyz - cxy * czz, detY, cxy * cxz - cyz * cxx);
        }
        else
        {
            normal = Vec3(cxy * cyz - cxz * cyy, cxy * cxz - cyz * cxx, detZ);
        }

        float normalLen = glm::length(normal);
        if (normalLen > EPSILON)
        {
            normal /= normalLen;
        }
        else
        {
            normal = Vec3(0, 0, 1);  // Fallback
        }

        // Create basis vectors for projection
        Vec3 up = std::abs(normal.y) < 0.9f ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
        Vec3 right = glm::normalize(glm::cross(up, normal));
        up = glm::cross(normal, right);

        // Project to 2D
        std::vector<Vec2> projected(points.size());
        for (size_t i = 0; i < points.size(); ++i)
        {
            Vec3 r = points[i] - centroid;
            projected[i] = Vec2(glm::dot(r, right), glm::dot(r, up));
        }

        Triangulate(projected, outTriangles);
    }

private:
    static void InsertPoint(
        uint32_t pointIdx,
        const std::vector<Vec2>& points,
        std::vector<DelaunayTriangle>& triangles)
    {
        const Vec2& point = points[pointIdx];

        // Find all triangles whose circumcircle contains the point
        std::vector<DelaunayTriangle> badTriangles;
        std::vector<TriangulationEdge> polygon;

        for (auto it = triangles.begin(); it != triangles.end(); )
        {
            if (it->ContainsInCircumcircle(point))
            {
                badTriangles.push_back(*it);
                it = triangles.erase(it);
            }
            else
            {
                ++it;
            }
        }

        // Find the boundary of the polygonal hole (non-shared edges)
        for (const auto& tri : badTriangles)
        {
            auto edges = tri.GetEdges();
            for (const auto& edge : edges)
            {
                // Check if this edge is shared with another bad triangle
                bool shared = false;
                for (const auto& other : badTriangles)
                {
                    if (&other == &tri) continue;
                    auto otherEdges = other.GetEdges();
                    for (const auto& otherEdge : otherEdges)
                    {
                        if (edge == otherEdge)
                        {
                            shared = true;
                            break;
                        }
                    }
                    if (shared) break;
                }

                if (!shared)
                {
                    polygon.push_back(edge);
                }
            }
        }

        // Create new triangles from the polygon edges to the new point
        for (const auto& edge : polygon)
        {
            triangles.emplace_back(edge.v0, edge.v1, pointIdx, points);
        }
    }
};

/**
 * @brief Voronoi diagram from Delaunay triangulation
 */
class VoronoiDiagram
{
public:
    struct VoronoiCell
    {
        uint32_t siteIndex;           ///< Index of the site point
        std::vector<Vec2> vertices;   ///< Vertices of the cell (CCW order)
        std::vector<uint32_t> neighbors; ///< Indices of neighboring cells
    };

    /**
     * @brief Compute Voronoi diagram from Delaunay triangulation
     * 
     * Note: This is a simplified implementation that computes circumcenters.
     * A complete implementation would need to handle infinite edges at the boundary.
     * 
     * @param points Input site points
     * @param delaunayTriangles Delaunay triangulation indices
     * @param outCells Output Voronoi cells
     */
    static void FromDelaunay(
        std::span<const Vec2> points,
        std::span<const uint32_t> delaunayTriangles,
        std::vector<VoronoiCell>& outCells)
    {
        outCells.clear();
        if (points.empty() || delaunayTriangles.size() < 3)
            return;

        uint32_t numPoints = static_cast<uint32_t>(points.size());
        outCells.resize(numPoints);

        for (uint32_t i = 0; i < numPoints; ++i)
        {
            outCells[i].siteIndex = i;
        }

        // Compute circumcenters of all triangles
        std::vector<Vec2> circumcenters;
        uint32_t numTris = static_cast<uint32_t>(delaunayTriangles.size() / 3);

        std::vector<Vec2> allPoints(points.begin(), points.end());

        for (uint32_t t = 0; t < numTris; ++t)
        {
            uint32_t v0 = delaunayTriangles[t * 3];
            uint32_t v1 = delaunayTriangles[t * 3 + 1];
            uint32_t v2 = delaunayTriangles[t * 3 + 2];

            DelaunayTriangle tri(v0, v1, v2, allPoints);
            circumcenters.push_back(tri.circumcenter);

            // Add circumcenter to each vertex's cell
            if (v0 < numPoints) outCells[v0].vertices.push_back(tri.circumcenter);
            if (v1 < numPoints) outCells[v1].vertices.push_back(tri.circumcenter);
            if (v2 < numPoints) outCells[v2].vertices.push_back(tri.circumcenter);

            // Record neighbors
            auto AddNeighborPair = [&outCells, numPoints](uint32_t a, uint32_t b)
            {
                if (a < numPoints && b < numPoints)
                {
                    auto& neighborsA = outCells[a].neighbors;
                    if (std::find(neighborsA.begin(), neighborsA.end(), b) == neighborsA.end())
                    {
                        neighborsA.push_back(b);
                    }
                    auto& neighborsB = outCells[b].neighbors;
                    if (std::find(neighborsB.begin(), neighborsB.end(), a) == neighborsB.end())
                    {
                        neighborsB.push_back(a);
                    }
                }
            };

            AddNeighborPair(v0, v1);
            AddNeighborPair(v1, v2);
            AddNeighborPair(v2, v0);
        }

        // Sort cell vertices in CCW order around the site
        for (auto& cell : outCells)
        {
            if (cell.vertices.size() < 3) continue;

            Vec2 site = points[cell.siteIndex];
            std::sort(cell.vertices.begin(), cell.vertices.end(),
                [&site](const Vec2& a, const Vec2& b)
                {
                    return std::atan2(a.y - site.y, a.x - site.x) <
                           std::atan2(b.y - site.y, b.x - site.x);
                });
        }
    }
};

} // namespace RVX::Geometry
