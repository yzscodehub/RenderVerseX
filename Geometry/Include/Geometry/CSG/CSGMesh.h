/**
 * @file CSGMesh.h
 * @brief CSG mesh with boolean operations
 */

#pragma once

#include "Geometry/CSG/Polygon.h"
#include "Geometry/CSG/BSPTree.h"
#include <vector>
#include <cmath>

namespace RVX::Geometry
{

/**
 * @brief Mesh representation for CSG operations
 * 
 * Supports boolean operations: union, intersection, subtraction.
 */
class CSGMesh
{
public:
    std::vector<CSGPolygon> polygons;

    CSGMesh() = default;

    explicit CSGMesh(const std::vector<CSGPolygon>& polys)
        : polygons(polys) {}

    explicit CSGMesh(std::vector<CSGPolygon>&& polys)
        : polygons(std::move(polys)) {}

    // =========================================================================
    // Boolean Operations
    // =========================================================================

    /**
     * @brief Compute union of two meshes (A ∪ B)
     */
    CSGMesh Union(const CSGMesh& other) const
    {
        BSPTree a(polygons);
        BSPTree b(other.polygons);

        a.ClipTo(b);
        b.ClipTo(a);
        b.Invert();
        b.ClipTo(a);
        b.Invert();

        auto result = a.GetAllPolygons();
        auto bPolys = b.GetAllPolygons();
        result.insert(result.end(), bPolys.begin(), bPolys.end());

        return CSGMesh(std::move(result));
    }

    /**
     * @brief Compute intersection of two meshes (A ∩ B)
     */
    CSGMesh Intersect(const CSGMesh& other) const
    {
        BSPTree a(polygons);
        BSPTree b(other.polygons);

        a.Invert();
        b.ClipTo(a);
        b.Invert();
        a.ClipTo(b);
        b.ClipTo(a);

        auto result = a.GetAllPolygons();
        auto bPolys = b.GetAllPolygons();
        result.insert(result.end(), bPolys.begin(), bPolys.end());

        BSPTree final(result);
        final.Invert();

        return CSGMesh(final.GetAllPolygons());
    }

    /**
     * @brief Compute subtraction of two meshes (A - B)
     */
    CSGMesh Subtract(const CSGMesh& other) const
    {
        BSPTree a(polygons);
        BSPTree b(other.polygons);

        a.Invert();
        a.ClipTo(b);
        b.ClipTo(a);
        b.Invert();
        b.ClipTo(a);
        b.Invert();

        auto result = a.GetAllPolygons();
        auto bPolys = b.GetAllPolygons();
        result.insert(result.end(), bPolys.begin(), bPolys.end());

        BSPTree final(result);
        final.Invert();

        return CSGMesh(final.GetAllPolygons());
    }

    // =========================================================================
    // Primitive Factories
    // =========================================================================

    /**
     * @brief Create a box primitive
     */
    static CSGMesh Box(const Vec3& center, const Vec3& halfSize)
    {
        Vec3 c = center;
        Vec3 r = halfSize;

        // 6 faces, each with 4 vertices
        std::vector<CSGPolygon> polys;

        // Helper to create a quad
        auto Quad = [](const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d) {
            return CSGPolygon::FromPositions({a, b, c, d});
        };

        // Front (+Z)
        polys.push_back(Quad(
            c + Vec3(-r.x, -r.y, r.z),
            c + Vec3(r.x, -r.y, r.z),
            c + Vec3(r.x, r.y, r.z),
            c + Vec3(-r.x, r.y, r.z)));

        // Back (-Z)
        polys.push_back(Quad(
            c + Vec3(r.x, -r.y, -r.z),
            c + Vec3(-r.x, -r.y, -r.z),
            c + Vec3(-r.x, r.y, -r.z),
            c + Vec3(r.x, r.y, -r.z)));

        // Right (+X)
        polys.push_back(Quad(
            c + Vec3(r.x, -r.y, r.z),
            c + Vec3(r.x, -r.y, -r.z),
            c + Vec3(r.x, r.y, -r.z),
            c + Vec3(r.x, r.y, r.z)));

        // Left (-X)
        polys.push_back(Quad(
            c + Vec3(-r.x, -r.y, -r.z),
            c + Vec3(-r.x, -r.y, r.z),
            c + Vec3(-r.x, r.y, r.z),
            c + Vec3(-r.x, r.y, -r.z)));

        // Top (+Y)
        polys.push_back(Quad(
            c + Vec3(-r.x, r.y, r.z),
            c + Vec3(r.x, r.y, r.z),
            c + Vec3(r.x, r.y, -r.z),
            c + Vec3(-r.x, r.y, -r.z)));

        // Bottom (-Y)
        polys.push_back(Quad(
            c + Vec3(-r.x, -r.y, -r.z),
            c + Vec3(r.x, -r.y, -r.z),
            c + Vec3(r.x, -r.y, r.z),
            c + Vec3(-r.x, -r.y, r.z)));

        return CSGMesh(std::move(polys));
    }

    /**
     * @brief Create a sphere primitive
     */
    static CSGMesh Sphere(const Vec3& center, float radius, int slices = 16, int stacks = 8)
    {
        std::vector<CSGPolygon> polys;

        for (int i = 0; i < stacks; ++i)
        {
            float v0 = static_cast<float>(i) / stacks;
            float v1 = static_cast<float>(i + 1) / stacks;
            float theta0 = v0 * PI;
            float theta1 = v1 * PI;

            for (int j = 0; j < slices; ++j)
            {
                float u0 = static_cast<float>(j) / slices;
                float u1 = static_cast<float>(j + 1) / slices;
                float phi0 = u0 * 2.0f * PI;
                float phi1 = u1 * 2.0f * PI;

                // Compute 4 corners of this cell
                auto GetPoint = [&](float theta, float phi) {
                    float sinTheta = std::sin(theta);
                    float cosTheta = std::cos(theta);
                    float sinPhi = std::sin(phi);
                    float cosPhi = std::cos(phi);

                    Vec3 n(sinTheta * cosPhi, cosTheta, sinTheta * sinPhi);
                    return center + n * radius;
                };

                Vec3 p00 = GetPoint(theta0, phi0);
                Vec3 p10 = GetPoint(theta1, phi0);
                Vec3 p11 = GetPoint(theta1, phi1);
                Vec3 p01 = GetPoint(theta0, phi1);

                // Create triangles (handle poles)
                if (i == 0)
                {
                    // Top cap - triangle
                    polys.push_back(CSGPolygon::FromPositions({p00, p10, p11}));
                }
                else if (i == stacks - 1)
                {
                    // Bottom cap - triangle
                    polys.push_back(CSGPolygon::FromPositions({p00, p10, p01}));
                }
                else
                {
                    // Quad
                    polys.push_back(CSGPolygon::FromPositions({p00, p10, p11, p01}));
                }
            }
        }

        return CSGMesh(std::move(polys));
    }

    /**
     * @brief Create a cylinder primitive
     */
    static CSGMesh Cylinder(const Vec3& start, const Vec3& end, float radius, int slices = 16)
    {
        std::vector<CSGPolygon> polys;

        Vec3 axis = end - start;
        float height = glm::length(axis);
        if (height < EPSILON) return CSGMesh();

        Vec3 axisNorm = axis / height;

        // Find perpendicular vectors
        Vec3 arbitrary = std::abs(axisNorm.x) < 0.9f ? Vec3(1, 0, 0) : Vec3(0, 1, 0);
        Vec3 tangent = glm::normalize(glm::cross(axisNorm, arbitrary));
        Vec3 bitangent = glm::cross(axisNorm, tangent);

        // Generate vertices around the cylinder
        std::vector<Vec3> topRing(slices);
        std::vector<Vec3> bottomRing(slices);

        for (int i = 0; i < slices; ++i)
        {
            float angle = static_cast<float>(i) / slices * 2.0f * PI;
            Vec3 offset = (tangent * std::cos(angle) + bitangent * std::sin(angle)) * radius;
            bottomRing[i] = start + offset;
            topRing[i] = end + offset;
        }

        // Side quads
        for (int i = 0; i < slices; ++i)
        {
            int j = (i + 1) % slices;
            polys.push_back(CSGPolygon::FromPositions({
                bottomRing[i], bottomRing[j], topRing[j], topRing[i]
            }));
        }

        // Bottom cap
        std::vector<Vec3> bottomCap;
        for (int i = slices - 1; i >= 0; --i)
        {
            bottomCap.push_back(bottomRing[i]);
        }
        polys.push_back(CSGPolygon::FromPositions(bottomCap));

        // Top cap
        polys.push_back(CSGPolygon::FromPositions(
            std::vector<Vec3>(topRing.begin(), topRing.end())));

        return CSGMesh(std::move(polys));
    }

    // =========================================================================
    // Conversion
    // =========================================================================

    /**
     * @brief Convert to indexed triangle mesh
     */
    void ToTriangleMesh(
        std::vector<Vec3>& outVertices,
        std::vector<uint32_t>& outIndices) const
    {
        outVertices.clear();
        outIndices.clear();

        for (const auto& poly : polygons)
        {
            if (poly.vertices.size() < 3) continue;

            uint32_t baseIdx = static_cast<uint32_t>(outVertices.size());

            // Add vertices
            for (const auto& v : poly.vertices)
            {
                outVertices.push_back(v.position);
            }

            // Triangulate (fan)
            for (size_t i = 1; i + 1 < poly.vertices.size(); ++i)
            {
                outIndices.push_back(baseIdx);
                outIndices.push_back(baseIdx + static_cast<uint32_t>(i));
                outIndices.push_back(baseIdx + static_cast<uint32_t>(i + 1));
            }
        }
    }

    /**
     * @brief Convert to indexed triangle mesh with normals
     */
    void ToTriangleMeshWithNormals(
        std::vector<Vec3>& outVertices,
        std::vector<Vec3>& outNormals,
        std::vector<uint32_t>& outIndices) const
    {
        outVertices.clear();
        outNormals.clear();
        outIndices.clear();

        for (const auto& poly : polygons)
        {
            if (poly.vertices.size() < 3) continue;

            uint32_t baseIdx = static_cast<uint32_t>(outVertices.size());

            for (const auto& v : poly.vertices)
            {
                outVertices.push_back(v.position);
                outNormals.push_back(v.normal);
            }

            for (size_t i = 1; i + 1 < poly.vertices.size(); ++i)
            {
                outIndices.push_back(baseIdx);
                outIndices.push_back(baseIdx + static_cast<uint32_t>(i));
                outIndices.push_back(baseIdx + static_cast<uint32_t>(i + 1));
            }
        }
    }

    /**
     * @brief Get polygon count
     */
    size_t GetPolygonCount() const { return polygons.size(); }

    /**
     * @brief Check if mesh is empty
     */
    bool IsEmpty() const { return polygons.empty(); }
};

} // namespace RVX::Geometry
