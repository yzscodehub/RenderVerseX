/**
 * @file Polygon.h
 * @brief 3D polygon for CSG operations
 */

#pragma once

#include "Core/MathTypes.h"
#include "Core/Math/Plane.h"
#include "Geometry/Constants.h"
#include <vector>
#include <memory>

namespace RVX::Geometry
{

/**
 * @brief 3D polygon vertex with position and optional attributes
 */
struct CSGVertex
{
    Vec3 position{0};
    Vec3 normal{0, 1, 0};
    Vec2 uv{0};

    CSGVertex() = default;
    CSGVertex(const Vec3& pos) : position(pos) {}
    CSGVertex(const Vec3& pos, const Vec3& n) : position(pos), normal(n) {}
    CSGVertex(const Vec3& pos, const Vec3& n, const Vec2& texcoord)
        : position(pos), normal(n), uv(texcoord) {}

    /// Linearly interpolate between two vertices
    static CSGVertex Lerp(const CSGVertex& a, const CSGVertex& b, float t)
    {
        CSGVertex result;
        result.position = glm::mix(a.position, b.position, t);
        result.normal = glm::normalize(glm::mix(a.normal, b.normal, t));
        result.uv = glm::mix(a.uv, b.uv, t);
        return result;
    }
};

/**
 * @brief 3D convex polygon for CSG operations
 */
class CSGPolygon
{
public:
    std::vector<CSGVertex> vertices;
    Plane plane;
    int material = 0;  ///< Optional material ID

    CSGPolygon() = default;

    explicit CSGPolygon(const std::vector<CSGVertex>& verts)
        : vertices(verts)
    {
        ComputePlane();
    }

    explicit CSGPolygon(std::vector<CSGVertex>&& verts)
        : vertices(std::move(verts))
    {
        ComputePlane();
    }

    /// Create from positions only
    static CSGPolygon FromPositions(const std::vector<Vec3>& positions)
    {
        std::vector<CSGVertex> verts;
        verts.reserve(positions.size());
        for (const auto& p : positions)
        {
            verts.emplace_back(p);
        }

        CSGPolygon poly(std::move(verts));

        // Set vertex normals to face normal
        for (auto& v : poly.vertices)
        {
            v.normal = poly.plane.normal;
        }

        return poly;
    }

    /// Flip the polygon (reverse winding and plane)
    void Flip()
    {
        std::reverse(vertices.begin(), vertices.end());
        for (auto& v : vertices)
        {
            v.normal = -v.normal;
        }
        plane = Plane(-plane.normal, -plane.distance);
    }

    /// Get a flipped copy
    CSGPolygon Flipped() const
    {
        CSGPolygon result = *this;
        result.Flip();
        return result;
    }

    /// Check if polygon is degenerate
    bool IsDegenerate() const
    {
        return vertices.size() < 3 || GetArea() < DEGENERATE_TOLERANCE;
    }

    /// Compute the area of the polygon
    float GetArea() const
    {
        if (vertices.size() < 3) return 0.0f;

        float area = 0.0f;
        Vec3 v0 = vertices[0].position;

        for (size_t i = 1; i + 1 < vertices.size(); ++i)
        {
            Vec3 e1 = vertices[i].position - v0;
            Vec3 e2 = vertices[i + 1].position - v0;
            area += 0.5f * glm::length(glm::cross(e1, e2));
        }

        return area;
    }

    /// Get the centroid of the polygon
    Vec3 GetCentroid() const
    {
        if (vertices.empty()) return Vec3(0);

        Vec3 sum(0);
        for (const auto& v : vertices)
        {
            sum += v.position;
        }
        return sum / static_cast<float>(vertices.size());
    }

    /// Classify a point relative to this polygon's plane
    int ClassifyPoint(const Vec3& point) const
    {
        float dist = plane.SignedDistance(point);
        if (dist > PLANE_THICKNESS) return 1;   // Front
        if (dist < -PLANE_THICKNESS) return -1; // Back
        return 0;  // Coplanar
    }

    /// Split polygon by a plane
    /// Returns: -1 = back, 0 = coplanar, 1 = front, 2 = spanning
    int SplitByPlane(
        const Plane& splitPlane,
        std::vector<CSGPolygon>& coplanarFront,
        std::vector<CSGPolygon>& coplanarBack,
        std::vector<CSGPolygon>& front,
        std::vector<CSGPolygon>& back) const
    {
        enum { COPLANAR = 0, FRONT = 1, BACK = 2, SPANNING = 3 };

        int polygonType = 0;
        std::vector<int> types(vertices.size());

        for (size_t i = 0; i < vertices.size(); ++i)
        {
            float dist = splitPlane.SignedDistance(vertices[i].position);
            int type = (dist > PLANE_THICKNESS) ? FRONT :
                       (dist < -PLANE_THICKNESS) ? BACK : COPLANAR;
            types[i] = type;
            polygonType |= type;
        }

        switch (polygonType)
        {
            case COPLANAR:
                if (glm::dot(plane.normal, splitPlane.normal) > 0)
                    coplanarFront.push_back(*this);
                else
                    coplanarBack.push_back(*this);
                return 0;

            case FRONT:
                front.push_back(*this);
                return 1;

            case BACK:
                back.push_back(*this);
                return -1;

            case SPANNING:
            {
                std::vector<CSGVertex> f, b;
                size_t n = vertices.size();

                for (size_t i = 0; i < n; ++i)
                {
                    size_t j = (i + 1) % n;
                    int ti = types[i];
                    int tj = types[j];
                    const CSGVertex& vi = vertices[i];
                    const CSGVertex& vj = vertices[j];

                    if (ti != BACK) f.push_back(vi);
                    if (ti != FRONT) b.push_back(vi);

                    if ((ti | tj) == SPANNING)
                    {
                        // Compute intersection point
                        float distI = splitPlane.SignedDistance(vi.position);
                        float distJ = splitPlane.SignedDistance(vj.position);
                        float t = distI / (distI - distJ);

                        CSGVertex intersection = CSGVertex::Lerp(vi, vj, t);
                        f.push_back(intersection);
                        b.push_back(intersection);
                    }
                }

                if (f.size() >= 3)
                    front.emplace_back(std::move(f));
                if (b.size() >= 3)
                    back.emplace_back(std::move(b));

                return 2;
            }
        }

        return 0;
    }

private:
    void ComputePlane()
    {
        if (vertices.size() < 3)
        {
            plane = Plane(Vec3(0, 1, 0), 0);
            return;
        }

        Vec3 normal(0);
        for (size_t i = 0; i < vertices.size(); ++i)
        {
            size_t j = (i + 1) % vertices.size();
            const Vec3& vi = vertices[i].position;
            const Vec3& vj = vertices[j].position;
            normal.x += (vi.y - vj.y) * (vi.z + vj.z);
            normal.y += (vi.z - vj.z) * (vi.x + vj.x);
            normal.z += (vi.x - vj.x) * (vi.y + vj.y);
        }

        float len = glm::length(normal);
        if (len > EPSILON)
        {
            normal /= len;
        }
        else
        {
            normal = Vec3(0, 1, 0);
        }

        float d = -glm::dot(normal, vertices[0].position);
        plane = Plane(normal, d);
    }
};

} // namespace RVX::Geometry
