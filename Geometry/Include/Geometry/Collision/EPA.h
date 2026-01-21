/**
 * @file EPA.h
 * @brief Expanding Polytope Algorithm for penetration depth
 */

#pragma once

#include "Core/MathTypes.h"
#include "Geometry/Collision/IConvexShape.h"
#include "Geometry/Collision/GJK.h"
#include "Geometry/Constants.h"
#include <vector>
#include <algorithm>
#include <cmath>

namespace RVX::Geometry
{

/**
 * @brief Result of EPA query
 */
struct EPAResult
{
    Vec3 normal{0, 1, 0};   ///< Penetration direction (from A to B)
    float depth = 0.0f;     ///< Penetration depth
    Vec3 pointA{0};         ///< Contact point on shape A
    Vec3 pointB{0};         ///< Contact point on shape B
    bool valid = false;     ///< Whether the result is valid
};

/**
 * @brief Expanding Polytope Algorithm
 * 
 * Computes the penetration depth and contact normal for two
 * intersecting convex shapes. Should only be called after GJK
 * confirms intersection.
 */
class EPA
{
public:
    /**
     * @brief Query penetration depth
     * 
     * @param a First shape
     * @param b Second shape
     * @param simplex Initial simplex from GJK (must be a tetrahedron)
     * @param maxIterations Maximum iterations (default: 32)
     * @return EPAResult with penetration info
     */
    static EPAResult Query(
        const IConvexShape& a,
        const IConvexShape& b,
        const Simplex& simplex,
        int maxIterations = 32)
    {
        EPAResult result;

        // Need at least a tetrahedron to start EPA
        if (simplex.size < 4)
        {
            // Try to build a tetrahedron
            return QueryWithSimplexExpansion(a, b, simplex, maxIterations);
        }

        // Initialize polytope with tetrahedron faces
        std::vector<Vec3> vertices;
        std::vector<Vec3> supA, supB;
        std::vector<Face> faces;

        for (int i = 0; i < 4; ++i)
        {
            vertices.push_back(simplex.points[i]);
            supA.push_back(simplex.supportA[i]);
            supB.push_back(simplex.supportB[i]);
        }

        // Create 4 faces of tetrahedron
        faces.push_back({0, 1, 2});
        faces.push_back({0, 3, 1});
        faces.push_back({0, 2, 3});
        faces.push_back({1, 3, 2});

        // Ensure faces point outward
        for (auto& face : faces)
        {
            Vec3 ab = vertices[face.b] - vertices[face.a];
            Vec3 ac = vertices[face.c] - vertices[face.a];
            face.normal = glm::normalize(glm::cross(ab, ac));

            // Find fourth vertex (not on this face)
            int other = -1;
            for (int i = 0; i < 4; ++i)
            {
                if (i != face.a && i != face.b && i != face.c)
                {
                    other = i;
                    break;
                }
            }

            Vec3 toOther = vertices[other] - vertices[face.a];
            if (glm::dot(face.normal, toOther) > 0)
            {
                face.normal = -face.normal;
                std::swap(face.b, face.c);
            }

            face.distance = glm::dot(face.normal, vertices[face.a]);
        }

        // EPA main loop
        for (int iter = 0; iter < maxIterations; ++iter)
        {
            // Find closest face to origin
            int closestIdx = 0;
            float minDist = std::abs(faces[0].distance);

            for (size_t i = 1; i < faces.size(); ++i)
            {
                float dist = std::abs(faces[i].distance);
                if (dist < minDist)
                {
                    minDist = dist;
                    closestIdx = static_cast<int>(i);
                }
            }

            const Face& closest = faces[closestIdx];

            // Get support point in direction of closest face normal
            Vec3 supPointA = a.Support(closest.normal);
            Vec3 supPointB = b.Support(-closest.normal);
            Vec3 newPoint = supPointA - supPointB;

            float supportDist = glm::dot(newPoint, closest.normal);

            // Check for convergence
            if (supportDist - minDist < EPSILON)
            {
                // Compute contact points
                result.normal = closest.normal;
                result.depth = minDist;

                // Barycentric interpolation for contact point
                Vec3 va = vertices[closest.a];
                Vec3 vb = vertices[closest.b];
                Vec3 vc = vertices[closest.c];

                Vec3 projPoint = closest.normal * closest.distance;
                Vec3 bary = ComputeBarycentric(projPoint, va, vb, vc);

                result.pointA = supA[closest.a] * bary.x +
                               supA[closest.b] * bary.y +
                               supA[closest.c] * bary.z;
                result.pointB = supB[closest.a] * bary.x +
                               supB[closest.b] * bary.y +
                               supB[closest.c] * bary.z;
                result.valid = true;
                return result;
            }

            // Add new vertex
            int newIdx = static_cast<int>(vertices.size());
            vertices.push_back(newPoint);
            supA.push_back(supPointA);
            supB.push_back(supPointB);

            // Remove faces visible from new point and collect edges
            std::vector<Edge> horizon;

            for (size_t i = 0; i < faces.size(); )
            {
                if (glm::dot(faces[i].normal, newPoint - vertices[faces[i].a]) > 0)
                {
                    // Face is visible - add edges to horizon
                    AddEdge(horizon, faces[i].a, faces[i].b);
                    AddEdge(horizon, faces[i].b, faces[i].c);
                    AddEdge(horizon, faces[i].c, faces[i].a);

                    // Remove face
                    faces[i] = faces.back();
                    faces.pop_back();
                }
                else
                {
                    ++i;
                }
            }

            // Create new faces from horizon edges
            for (const auto& edge : horizon)
            {
                Face newFace = {edge.a, edge.b, newIdx};
                Vec3 ab = vertices[edge.b] - vertices[edge.a];
                Vec3 ac = vertices[newIdx] - vertices[edge.a];
                newFace.normal = glm::normalize(glm::cross(ab, ac));
                newFace.distance = glm::dot(newFace.normal, vertices[edge.a]);

                // Ensure normal points outward (away from origin)
                if (newFace.distance < 0)
                {
                    std::swap(newFace.a, newFace.b);
                    newFace.normal = -newFace.normal;
                    newFace.distance = -newFace.distance;
                }

                faces.push_back(newFace);
            }

            if (faces.empty())
            {
                break;
            }
        }

        // Did not converge - return best guess
        result.normal = Vec3(0, 1, 0);
        result.depth = 0.0f;
        result.valid = false;
        return result;
    }

private:
    struct Face
    {
        int a, b, c;
        Vec3 normal;
        float distance;
    };

    struct Edge
    {
        int a, b;

        bool operator==(const Edge& other) const
        {
            return (a == other.a && b == other.b) ||
                   (a == other.b && b == other.a);
        }
    };

    static void AddEdge(std::vector<Edge>& edges, int a, int b)
    {
        Edge newEdge = {a, b};

        // Check if reverse edge exists (shared edge)
        for (auto it = edges.begin(); it != edges.end(); ++it)
        {
            if (*it == newEdge)
            {
                edges.erase(it);
                return;
            }
        }

        edges.push_back(newEdge);
    }

    static Vec3 ComputeBarycentric(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c)
    {
        Vec3 v0 = b - a;
        Vec3 v1 = c - a;
        Vec3 v2 = p - a;

        float d00 = glm::dot(v0, v0);
        float d01 = glm::dot(v0, v1);
        float d11 = glm::dot(v1, v1);
        float d20 = glm::dot(v2, v0);
        float d21 = glm::dot(v2, v1);

        float denom = d00 * d11 - d01 * d01;
        if (std::abs(denom) < EPSILON)
            return Vec3(1, 0, 0);

        float v = (d11 * d20 - d01 * d21) / denom;
        float w = (d00 * d21 - d01 * d20) / denom;
        float u = 1.0f - v - w;

        return Vec3(u, v, w);
    }

    static EPAResult QueryWithSimplexExpansion(
        const IConvexShape& a,
        const IConvexShape& b,
        const Simplex& simplex,
        int maxIterations)
    {
        EPAResult result;

        // Expand simplex to tetrahedron
        Simplex expanded = simplex;

        // Add points until we have 4
        Vec3 dirs[] = {
            {1, 0, 0}, {-1, 0, 0},
            {0, 1, 0}, {0, -1, 0},
            {0, 0, 1}, {0, 0, -1}
        };

        for (int i = 0; i < 6 && expanded.size < 4; ++i)
        {
            Vec3 supA = a.Support(dirs[i]);
            Vec3 supB = b.Support(-dirs[i]);
            Vec3 point = supA - supB;

            // Check if point is new
            bool isNew = true;
            for (int j = 0; j < expanded.size; ++j)
            {
                if (glm::length(point - expanded.points[j]) < EPSILON)
                {
                    isNew = false;
                    break;
                }
            }

            if (isNew)
            {
                expanded.Push(point, supA, supB);
            }
        }

        if (expanded.size < 4)
        {
            result.valid = false;
            return result;
        }

        return Query(a, b, expanded, maxIterations);
    }
};

} // namespace RVX::Geometry
