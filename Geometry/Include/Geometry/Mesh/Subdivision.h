/**
 * @file Subdivision.h
 * @brief Subdivision surface algorithms
 */

#pragma once

#include "Core/MathTypes.h"
#include "Geometry/Constants.h"
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace RVX::Geometry
{

/**
 * @brief Subdivision surface algorithms
 */
class Subdivision
{
public:
    /**
     * @brief Loop subdivision for triangle meshes
     * 
     * Subdivides each triangle into 4 smaller triangles and smooths vertices.
     * 
     * @param vertices Input/output vertex positions
     * @param indices Input/output triangle indices
     * @param iterations Number of subdivision iterations
     */
    static void Loop(
        std::vector<Vec3>& vertices,
        std::vector<uint32_t>& indices,
        int iterations = 1)
    {
        for (int iter = 0; iter < iterations; ++iter)
        {
            LoopOnce(vertices, indices);
        }
    }

    /**
     * @brief Catmull-Clark subdivision for quad meshes
     * 
     * Generalizes to arbitrary polygon meshes.
     * 
     * @param vertices Input/output vertex positions
     * @param indices Input/output quad indices (4 per face)
     * @param iterations Number of subdivision iterations
     */
    static void CatmullClark(
        std::vector<Vec3>& vertices,
        std::vector<uint32_t>& indices,
        int iterations = 1)
    {
        for (int iter = 0; iter < iterations; ++iter)
        {
            CatmullClarkOnce(vertices, indices);
        }
    }

    /**
     * @brief Simple midpoint subdivision (no smoothing)
     * 
     * Just splits each triangle into 4 without moving vertices.
     */
    static void Midpoint(
        std::vector<Vec3>& vertices,
        std::vector<uint32_t>& indices,
        int iterations = 1)
    {
        for (int iter = 0; iter < iterations; ++iter)
        {
            MidpointOnce(vertices, indices);
        }
    }

private:
    struct EdgeKey
    {
        uint32_t v0, v1;

        EdgeKey(uint32_t a, uint32_t b)
            : v0(std::min(a, b)), v1(std::max(a, b)) {}

        bool operator==(const EdgeKey& other) const
        {
            return v0 == other.v0 && v1 == other.v1;
        }
    };

    struct EdgeKeyHash
    {
        size_t operator()(const EdgeKey& k) const
        {
            return std::hash<uint64_t>{}(
                (static_cast<uint64_t>(k.v0) << 32) | k.v1);
        }
    };

    static void LoopOnce(
        std::vector<Vec3>& vertices,
        std::vector<uint32_t>& indices)
    {
        size_t origVertCount = vertices.size();
        size_t numTriangles = indices.size() / 3;

        // Build edge adjacency
        std::unordered_map<EdgeKey, std::vector<uint32_t>, EdgeKeyHash> edgeFaces;
        std::unordered_map<EdgeKey, uint32_t, EdgeKeyHash> edgeVertices;
        std::vector<std::vector<uint32_t>> vertexFaces(origVertCount);

        for (size_t f = 0; f < numTriangles; ++f)
        {
            uint32_t idx[3] = {
                indices[f * 3 + 0],
                indices[f * 3 + 1],
                indices[f * 3 + 2]
            };

            for (int i = 0; i < 3; ++i)
            {
                vertexFaces[idx[i]].push_back(static_cast<uint32_t>(f));

                EdgeKey edge(idx[i], idx[(i + 1) % 3]);
                edgeFaces[edge].push_back(static_cast<uint32_t>(f));
            }
        }

        // Create edge vertices
        for (auto& [edge, faces] : edgeFaces)
        {
            Vec3 edgePoint;

            if (faces.size() == 2)
            {
                // Interior edge: weighted average
                Vec3 v0 = vertices[edge.v0];
                Vec3 v1 = vertices[edge.v1];

                // Find opposite vertices
                Vec3 v2(0), v3(0);
                for (int fi = 0; fi < 2; ++fi)
                {
                    uint32_t f = faces[fi];
                    for (int i = 0; i < 3; ++i)
                    {
                        uint32_t v = indices[f * 3 + i];
                        if (v != edge.v0 && v != edge.v1)
                        {
                            if (fi == 0) v2 = vertices[v];
                            else v3 = vertices[v];
                            break;
                        }
                    }
                }

                edgePoint = (v0 + v1) * 0.375f + (v2 + v3) * 0.125f;
            }
            else
            {
                // Boundary edge: simple midpoint
                edgePoint = (vertices[edge.v0] + vertices[edge.v1]) * 0.5f;
            }

            edgeVertices[edge] = static_cast<uint32_t>(vertices.size());
            vertices.push_back(edgePoint);
        }

        // Update original vertices
        std::vector<Vec3> newPositions(origVertCount);

        for (size_t v = 0; v < origVertCount; ++v)
        {
            int n = static_cast<int>(vertexFaces[v].size());

            if (IsBoundaryVertex(v, edgeFaces, vertices.size() - origVertCount))
            {
                // Boundary vertex: average with boundary neighbors
                Vec3 boundarySum(0);
                int boundaryCount = 0;

                for (const auto& [edge, faces] : edgeFaces)
                {
                    if (faces.size() == 1)
                    {
                        if (edge.v0 == v)
                        {
                            boundarySum += vertices[edge.v1];
                            ++boundaryCount;
                        }
                        else if (edge.v1 == v)
                        {
                            boundarySum += vertices[edge.v0];
                            ++boundaryCount;
                        }
                    }
                }

                if (boundaryCount == 2)
                {
                    newPositions[v] = vertices[v] * 0.75f + boundarySum * 0.125f;
                }
                else
                {
                    newPositions[v] = vertices[v];
                }
            }
            else
            {
                // Interior vertex
                float beta = (n > 3)
                    ? 3.0f / (8.0f * n)
                    : 3.0f / 16.0f;

                Vec3 neighborSum(0);
                for (uint32_t f : vertexFaces[v])
                {
                    for (int i = 0; i < 3; ++i)
                    {
                        uint32_t nv = indices[f * 3 + i];
                        if (nv != v)
                        {
                            neighborSum += vertices[nv];
                        }
                    }
                }
                // Each neighbor counted twice (once per adjacent face)
                neighborSum /= 2.0f;

                newPositions[v] = vertices[v] * (1.0f - static_cast<float>(n) * beta) + neighborSum * (beta / static_cast<float>(n));
            }
        }

        for (size_t v = 0; v < origVertCount; ++v)
        {
            vertices[v] = newPositions[v];
        }

        // Generate new triangles
        std::vector<uint32_t> newIndices;
        newIndices.reserve(numTriangles * 4 * 3);

        for (size_t f = 0; f < numTriangles; ++f)
        {
            uint32_t v0 = indices[f * 3 + 0];
            uint32_t v1 = indices[f * 3 + 1];
            uint32_t v2 = indices[f * 3 + 2];

            uint32_t e01 = edgeVertices[EdgeKey(v0, v1)];
            uint32_t e12 = edgeVertices[EdgeKey(v1, v2)];
            uint32_t e20 = edgeVertices[EdgeKey(v2, v0)];

            // 4 new triangles
            newIndices.push_back(v0);  newIndices.push_back(e01); newIndices.push_back(e20);
            newIndices.push_back(e01); newIndices.push_back(v1);  newIndices.push_back(e12);
            newIndices.push_back(e20); newIndices.push_back(e12); newIndices.push_back(v2);
            newIndices.push_back(e01); newIndices.push_back(e12); newIndices.push_back(e20);
        }

        indices = std::move(newIndices);
    }

    static void CatmullClarkOnce(
        std::vector<Vec3>& vertices,
        std::vector<uint32_t>& indices)
    {
        // Catmull-Clark subdivision for quad meshes
        size_t numQuads = indices.size() / 4;
        if (numQuads == 0) return;

        size_t origVertCount = vertices.size();

        // Build adjacency data for original vertices
        std::vector<std::vector<uint32_t>> vertexFaces(origVertCount);
        std::vector<std::vector<EdgeKey>> vertexEdges(origVertCount);

        for (size_t f = 0; f < numQuads; ++f)
        {
            for (int i = 0; i < 4; ++i)
            {
                uint32_t v = indices[f * 4 + i];
                vertexFaces[v].push_back(static_cast<uint32_t>(f));

                uint32_t vNext = indices[f * 4 + (i + 1) % 4];
                vertexEdges[v].push_back(EdgeKey(v, vNext));
            }
        }

        // Create face points (average of face vertices)
        std::vector<Vec3> facePointPos(numQuads);
        std::vector<uint32_t> facePoints(numQuads);
        for (size_t f = 0; f < numQuads; ++f)
        {
            Vec3 sum(0);
            for (int i = 0; i < 4; ++i)
            {
                sum += vertices[indices[f * 4 + i]];
            }
            facePointPos[f] = sum * 0.25f;
            facePoints[f] = static_cast<uint32_t>(vertices.size());
            vertices.push_back(facePointPos[f]);
        }

        // Create edge points (average of edge endpoints and adjacent face points)
        std::unordered_map<EdgeKey, uint32_t, EdgeKeyHash> edgePoints;
        std::unordered_map<EdgeKey, std::vector<uint32_t>, EdgeKeyHash> edgeFaces;

        // Build edge-to-face mapping
        for (size_t f = 0; f < numQuads; ++f)
        {
            for (int i = 0; i < 4; ++i)
            {
                uint32_t v0 = indices[f * 4 + i];
                uint32_t v1 = indices[f * 4 + (i + 1) % 4];
                EdgeKey edge(v0, v1);
                edgeFaces[edge].push_back(static_cast<uint32_t>(f));
            }
        }

        // Create edge points
        for (const auto& [edge, faces] : edgeFaces)
        {
            if (edgePoints.find(edge) != edgePoints.end()) continue;

            Vec3 edgePoint;
            if (faces.size() == 2)
            {
                // Interior edge: average of endpoints and adjacent face points
                edgePoint = (vertices[edge.v0] + vertices[edge.v1] +
                            facePointPos[faces[0]] + facePointPos[faces[1]]) * 0.25f;
            }
            else
            {
                // Boundary edge: midpoint
                edgePoint = (vertices[edge.v0] + vertices[edge.v1]) * 0.5f;
            }

            edgePoints[edge] = static_cast<uint32_t>(vertices.size());
            vertices.push_back(edgePoint);
        }

        // Update original vertices using Catmull-Clark formula:
        // P' = (F + 2R + (n-3)P) / n
        // where F = average of face points, R = average of edge midpoints, n = valence
        std::vector<Vec3> newPositions(origVertCount);

        for (size_t v = 0; v < origVertCount; ++v)
        {
            const auto& faces = vertexFaces[v];
            int n = static_cast<int>(faces.size());

            if (n == 0)
            {
                newPositions[v] = vertices[v];
                continue;
            }

            // Check if boundary vertex
            bool isBoundary = false;
            std::vector<uint32_t> boundaryNeighbors;

            for (const auto& edge : vertexEdges[v])
            {
                if (edgeFaces[edge].size() == 1)
                {
                    isBoundary = true;
                    uint32_t neighbor = (edge.v0 == v) ? edge.v1 : edge.v0;
                    boundaryNeighbors.push_back(neighbor);
                }
            }

            if (isBoundary && boundaryNeighbors.size() >= 2)
            {
                // Boundary vertex: average with boundary neighbors
                Vec3 sum = vertices[v] * 6.0f;
                for (uint32_t neighbor : boundaryNeighbors)
                {
                    sum += vertices[neighbor];
                }
                newPositions[v] = sum / (6.0f + static_cast<float>(boundaryNeighbors.size()));
            }
            else
            {
                // Interior vertex: full Catmull-Clark rule
                // F = average of adjacent face points
                Vec3 F(0);
                for (uint32_t f : faces)
                {
                    F += facePointPos[f];
                }
                F /= static_cast<float>(n);

                // R = average of edge midpoints
                Vec3 R(0);
                int edgeCount = 0;
                for (const auto& edge : vertexEdges[v])
                {
                    R += (vertices[edge.v0] + vertices[edge.v1]) * 0.5f;
                    ++edgeCount;
                }
                if (edgeCount > 0) R /= static_cast<float>(edgeCount);

                // P' = (F + 2R + (n-3)P) / n
                Vec3 P = vertices[v];
                newPositions[v] = (F + R * 2.0f + P * static_cast<float>(n - 3)) / static_cast<float>(n);
            }
        }

        // Apply new positions to original vertices
        for (size_t v = 0; v < origVertCount; ++v)
        {
            vertices[v] = newPositions[v];
        }

        // Generate new quads (same as before)
        std::vector<uint32_t> newIndices;

        for (size_t f = 0; f < numQuads; ++f)
        {
            uint32_t v[4] = {
                indices[f * 4 + 0],
                indices[f * 4 + 1],
                indices[f * 4 + 2],
                indices[f * 4 + 3]
            };

            uint32_t fp = facePoints[f];

            for (int i = 0; i < 4; ++i)
            {
                uint32_t v0 = v[i];
                uint32_t e0 = edgePoints[EdgeKey(v[(i + 3) % 4], v[i])];
                uint32_t e1 = edgePoints[EdgeKey(v[i], v[(i + 1) % 4])];

                newIndices.push_back(v0);
                newIndices.push_back(e1);
                newIndices.push_back(fp);
                newIndices.push_back(e0);
            }
        }

        indices = std::move(newIndices);
    }

    static void MidpointOnce(
        std::vector<Vec3>& vertices,
        std::vector<uint32_t>& indices)
    {
        std::unordered_map<EdgeKey, uint32_t, EdgeKeyHash> edgeVertices;
        size_t numTriangles = indices.size() / 3;

        // Create edge midpoints
        for (size_t f = 0; f < numTriangles; ++f)
        {
            for (int i = 0; i < 3; ++i)
            {
                EdgeKey edge(indices[f * 3 + i], indices[f * 3 + (i + 1) % 3]);
                if (edgeVertices.find(edge) == edgeVertices.end())
                {
                    Vec3 mid = (vertices[edge.v0] + vertices[edge.v1]) * 0.5f;
                    edgeVertices[edge] = static_cast<uint32_t>(vertices.size());
                    vertices.push_back(mid);
                }
            }
        }

        // Generate new triangles
        std::vector<uint32_t> newIndices;

        for (size_t f = 0; f < numTriangles; ++f)
        {
            uint32_t v0 = indices[f * 3 + 0];
            uint32_t v1 = indices[f * 3 + 1];
            uint32_t v2 = indices[f * 3 + 2];

            uint32_t e01 = edgeVertices[EdgeKey(v0, v1)];
            uint32_t e12 = edgeVertices[EdgeKey(v1, v2)];
            uint32_t e20 = edgeVertices[EdgeKey(v2, v0)];

            newIndices.push_back(v0);  newIndices.push_back(e01); newIndices.push_back(e20);
            newIndices.push_back(e01); newIndices.push_back(v1);  newIndices.push_back(e12);
            newIndices.push_back(e20); newIndices.push_back(e12); newIndices.push_back(v2);
            newIndices.push_back(e01); newIndices.push_back(e12); newIndices.push_back(e20);
        }

        indices = std::move(newIndices);
    }

    static bool IsBoundaryVertex(
        size_t v,
        const std::unordered_map<EdgeKey, std::vector<uint32_t>, EdgeKeyHash>& edgeFaces,
        size_t /*numEdgeVerts*/)
    {
        for (const auto& [edge, faces] : edgeFaces)
        {
            if (faces.size() == 1 && (edge.v0 == v || edge.v1 == v))
            {
                return true;
            }
        }
        return false;
    }
};

} // namespace RVX::Geometry
