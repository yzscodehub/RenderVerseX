/**
 * @file NormalGeneration.h
 * @brief Normal vector generation for meshes
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
 * @brief Normal generation algorithms
 */
class NormalGenerator
{
public:
    /**
     * @brief Compute face normals for a triangle mesh
     * 
     * @param vertices Vertex positions
     * @param indices Triangle indices
     * @param outNormals Output: one normal per triangle
     */
    static void ComputeFaceNormals(
        std::span<const Vec3> vertices,
        std::span<const uint32_t> indices,
        std::vector<Vec3>& outNormals)
    {
        outNormals.clear();
        size_t numTriangles = indices.size() / 3;
        outNormals.reserve(numTriangles);

        for (size_t i = 0; i < numTriangles; ++i)
        {
            Vec3 v0 = vertices[indices[i * 3 + 0]];
            Vec3 v1 = vertices[indices[i * 3 + 1]];
            Vec3 v2 = vertices[indices[i * 3 + 2]];

            Vec3 edge1 = v1 - v0;
            Vec3 edge2 = v2 - v0;
            Vec3 normal = glm::cross(edge1, edge2);

            float len = glm::length(normal);
            outNormals.push_back(len > EPSILON ? normal / len : Vec3(0, 1, 0));
        }
    }

    /**
     * @brief Compute vertex normals using area-weighted average of adjacent faces
     * 
     * @param vertices Vertex positions
     * @param indices Triangle indices
     * @param outNormals Output: one normal per vertex
     */
    static void ComputeVertexNormals(
        std::span<const Vec3> vertices,
        std::span<const uint32_t> indices,
        std::vector<Vec3>& outNormals)
    {
        outNormals.clear();
        outNormals.resize(vertices.size(), Vec3(0));

        size_t numTriangles = indices.size() / 3;

        // Accumulate weighted normals
        for (size_t i = 0; i < numTriangles; ++i)
        {
            uint32_t i0 = indices[i * 3 + 0];
            uint32_t i1 = indices[i * 3 + 1];
            uint32_t i2 = indices[i * 3 + 2];

            Vec3 v0 = vertices[i0];
            Vec3 v1 = vertices[i1];
            Vec3 v2 = vertices[i2];

            Vec3 edge1 = v1 - v0;
            Vec3 edge2 = v2 - v0;
            Vec3 faceNormal = glm::cross(edge1, edge2);  // Not normalized - area-weighted

            outNormals[i0] += faceNormal;
            outNormals[i1] += faceNormal;
            outNormals[i2] += faceNormal;
        }

        // Normalize
        for (auto& n : outNormals)
        {
            float len = glm::length(n);
            n = len > EPSILON ? n / len : Vec3(0, 1, 0);
        }
    }

    /**
     * @brief Compute vertex normals with angle weighting
     * 
     * Weights each face contribution by the angle at the vertex.
     * Produces smoother results for non-uniform meshes.
     */
    static void ComputeVertexNormalsAngleWeighted(
        std::span<const Vec3> vertices,
        std::span<const uint32_t> indices,
        std::vector<Vec3>& outNormals)
    {
        outNormals.clear();
        outNormals.resize(vertices.size(), Vec3(0));

        size_t numTriangles = indices.size() / 3;

        for (size_t i = 0; i < numTriangles; ++i)
        {
            uint32_t idx[3] = {
                indices[i * 3 + 0],
                indices[i * 3 + 1],
                indices[i * 3 + 2]
            };

            Vec3 v[3] = {vertices[idx[0]], vertices[idx[1]], vertices[idx[2]]};

            Vec3 edge01 = v[1] - v[0];
            Vec3 edge12 = v[2] - v[1];
            Vec3 edge20 = v[0] - v[2];

            Vec3 faceNormal = glm::normalize(glm::cross(edge01, -edge20));

            // Compute angle at each vertex
            float angle0 = AngleBetween(-edge20, edge01);
            float angle1 = AngleBetween(-edge01, edge12);
            float angle2 = AngleBetween(-edge12, edge20);

            outNormals[idx[0]] += faceNormal * angle0;
            outNormals[idx[1]] += faceNormal * angle1;
            outNormals[idx[2]] += faceNormal * angle2;
        }

        // Normalize
        for (auto& n : outNormals)
        {
            float len = glm::length(n);
            n = len > EPSILON ? n / len : Vec3(0, 1, 0);
        }
    }

    /**
     * @brief Compute smooth normals with hard edge threshold
     * 
     * Faces with dihedral angle greater than threshold create hard edges.
     * 
     * @param vertices Vertex positions
     * @param indices Triangle indices
     * @param angleThreshold Angle in radians above which edges are considered hard
     * @param outNormals Output normals (one per vertex per face - may have duplicates)
     */
    static void ComputeSmoothNormals(
        std::span<const Vec3> vertices,
        std::span<const uint32_t> indices,
        float angleThreshold,
        std::vector<Vec3>& outNormals)
    {
        // Simplified implementation: use angle-weighted normals with threshold check
        // Full implementation would split vertices at hard edges
        
        std::vector<Vec3> faceNormals;
        ComputeFaceNormals(vertices, indices, faceNormals);

        float cosThreshold = std::cos(angleThreshold);

        outNormals.clear();
        outNormals.resize(vertices.size(), Vec3(0));

        size_t numTriangles = indices.size() / 3;

        // For each vertex, average normals of faces with similar orientation
        for (size_t i = 0; i < numTriangles; ++i)
        {
            for (int j = 0; j < 3; ++j)
            {
                uint32_t vertIdx = indices[i * 3 + j];
                Vec3 faceN = faceNormals[i];

                // Check all adjacent faces
                Vec3 accumNormal = faceN;
                for (size_t k = 0; k < numTriangles; ++k)
                {
                    if (k == i) continue;

                    // Check if face k shares this vertex
                    bool sharesVertex = false;
                    for (int l = 0; l < 3; ++l)
                    {
                        if (indices[k * 3 + l] == vertIdx)
                        {
                            sharesVertex = true;
                            break;
                        }
                    }

                    if (sharesVertex)
                    {
                        float dot = glm::dot(faceN, faceNormals[k]);
                        if (dot >= cosThreshold)
                        {
                            accumNormal += faceNormals[k];
                        }
                    }
                }

                float len = glm::length(accumNormal);
                outNormals[vertIdx] = len > EPSILON ? accumNormal / len : Vec3(0, 1, 0);
            }
        }
    }

    /**
     * @brief Compute tangent vectors for normal mapping
     * 
     * @param vertices Vertex positions
     * @param normals Vertex normals
     * @param uvs Texture coordinates
     * @param indices Triangle indices
     * @param outTangents Output tangent vectors
     * @param outBitangents Output bitangent vectors
     */
    static void ComputeTangentSpace(
        std::span<const Vec3> vertices,
        std::span<const Vec3> normals,
        std::span<const Vec2> uvs,
        std::span<const uint32_t> indices,
        std::vector<Vec3>& outTangents,
        std::vector<Vec3>& outBitangents)
    {
        outTangents.clear();
        outTangents.resize(vertices.size(), Vec3(0));
        outBitangents.clear();
        outBitangents.resize(vertices.size(), Vec3(0));

        size_t numTriangles = indices.size() / 3;

        for (size_t i = 0; i < numTriangles; ++i)
        {
            uint32_t i0 = indices[i * 3 + 0];
            uint32_t i1 = indices[i * 3 + 1];
            uint32_t i2 = indices[i * 3 + 2];

            Vec3 v0 = vertices[i0], v1 = vertices[i1], v2 = vertices[i2];
            Vec2 uv0 = uvs[i0], uv1 = uvs[i1], uv2 = uvs[i2];

            Vec3 deltaPos1 = v1 - v0;
            Vec3 deltaPos2 = v2 - v0;
            Vec2 deltaUV1 = uv1 - uv0;
            Vec2 deltaUV2 = uv2 - uv0;

            float r = deltaUV1.x * deltaUV2.y - deltaUV1.y * deltaUV2.x;
            if (std::abs(r) < EPSILON) r = 1.0f;
            r = 1.0f / r;

            Vec3 tangent = (deltaPos1 * deltaUV2.y - deltaPos2 * deltaUV1.y) * r;
            Vec3 bitangent = (deltaPos2 * deltaUV1.x - deltaPos1 * deltaUV2.x) * r;

            outTangents[i0] += tangent;
            outTangents[i1] += tangent;
            outTangents[i2] += tangent;

            outBitangents[i0] += bitangent;
            outBitangents[i1] += bitangent;
            outBitangents[i2] += bitangent;
        }

        // Orthonormalize
        for (size_t i = 0; i < vertices.size(); ++i)
        {
            Vec3 n = normals[i];
            Vec3 t = outTangents[i];
            Vec3 b = outBitangents[i];

            // Gram-Schmidt orthonormalize
            t = glm::normalize(t - n * glm::dot(n, t));

            // Fix handedness
            if (glm::dot(glm::cross(n, t), b) < 0.0f)
            {
                t = -t;
            }

            outTangents[i] = t;
            outBitangents[i] = glm::normalize(glm::cross(n, t));
        }
    }

private:
    static float AngleBetween(const Vec3& a, const Vec3& b)
    {
        float lenA = glm::length(a);
        float lenB = glm::length(b);
        if (lenA < EPSILON || lenB < EPSILON) return 0.0f;

        float cosAngle = glm::dot(a, b) / (lenA * lenB);
        cosAngle = std::clamp(cosAngle, -1.0f, 1.0f);
        return std::acos(cosAngle);
    }
};

} // namespace RVX::Geometry
