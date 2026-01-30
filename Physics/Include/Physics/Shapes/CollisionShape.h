/**
 * @file CollisionShape.h
 * @brief Base collision shape interface
 */

#pragma once

#include "Physics/PhysicsTypes.h"
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <memory>
#include <vector>

namespace RVX::Physics
{

/**
 * @brief Shape type enumeration
 */
enum class ShapeType : uint8
{
    Sphere,
    Box,
    Capsule,
    Cylinder,
    ConvexHull,
    TriangleMesh,
    HeightField,
    Compound
};

/**
 * @brief Base class for collision shapes
 */
class CollisionShape
{
public:
    using Ptr = std::shared_ptr<CollisionShape>;

    virtual ~CollisionShape() = default;

    /// Get shape type
    virtual ShapeType GetType() const = 0;

    /// Get shape name for debugging
    virtual const char* GetTypeName() const = 0;

    /// Calculate volume
    virtual float GetVolume() const = 0;

    /// Calculate bounding box
    virtual void GetLocalBounds(Vec3& outMin, Vec3& outMax) const = 0;

    /// Get bounding sphere radius
    virtual float GetBoundingRadius() const = 0;

    /// Calculate mass properties
    virtual MassProperties CalculateMassProperties(float density) const = 0;

    // =========================================================================
    // Material
    // =========================================================================

    const PhysicsMaterial& GetMaterial() const { return m_material; }
    void SetMaterial(const PhysicsMaterial& material) { m_material = material; }

protected:
    PhysicsMaterial m_material;
};

/**
 * @brief Sphere collision shape
 */
class SphereShape : public CollisionShape
{
public:
    explicit SphereShape(float radius = 0.5f) : m_radius(radius) {}

    ShapeType GetType() const override { return ShapeType::Sphere; }
    const char* GetTypeName() const override { return "Sphere"; }

    float GetRadius() const { return m_radius; }
    void SetRadius(float radius) { m_radius = radius; }

    float GetVolume() const override
    {
        return (4.0f / 3.0f) * 3.14159265f * m_radius * m_radius * m_radius;
    }

    void GetLocalBounds(Vec3& outMin, Vec3& outMax) const override
    {
        outMin = Vec3(-m_radius);
        outMax = Vec3(m_radius);
    }

    float GetBoundingRadius() const override { return m_radius; }

    MassProperties CalculateMassProperties(float density) const override
    {
        MassProperties props;
        props.mass = GetVolume() * density;
        float inertia = 0.4f * props.mass * m_radius * m_radius;
        props.inertiaTensor = Mat3(inertia);
        return props;
    }

    static Ptr Create(float radius = 0.5f)
    {
        return std::make_shared<SphereShape>(radius);
    }

private:
    float m_radius;
};

/**
 * @brief Box collision shape
 */
class BoxShape : public CollisionShape
{
public:
    explicit BoxShape(const Vec3& halfExtents = Vec3(0.5f))
        : m_halfExtents(halfExtents) {}

    BoxShape(float hx, float hy, float hz)
        : m_halfExtents(hx, hy, hz) {}

    ShapeType GetType() const override { return ShapeType::Box; }
    const char* GetTypeName() const override { return "Box"; }

    const Vec3& GetHalfExtents() const { return m_halfExtents; }
    void SetHalfExtents(const Vec3& halfExtents) { m_halfExtents = halfExtents; }

    float GetVolume() const override
    {
        return 8.0f * m_halfExtents.x * m_halfExtents.y * m_halfExtents.z;
    }

    void GetLocalBounds(Vec3& outMin, Vec3& outMax) const override
    {
        outMin = -m_halfExtents;
        outMax = m_halfExtents;
    }

    float GetBoundingRadius() const override
    {
        return length(m_halfExtents);
    }

    MassProperties CalculateMassProperties(float density) const override
    {
        MassProperties props;
        props.mass = GetVolume() * density;
        float factor = props.mass / 3.0f;
        float hx2 = m_halfExtents.x * m_halfExtents.x;
        float hy2 = m_halfExtents.y * m_halfExtents.y;
        float hz2 = m_halfExtents.z * m_halfExtents.z;
        props.inertiaTensor = Mat3(0.0f);
        props.inertiaTensor[0][0] = factor * (hy2 + hz2);
        props.inertiaTensor[1][1] = factor * (hx2 + hz2);
        props.inertiaTensor[2][2] = factor * (hx2 + hy2);
        return props;
    }

    static Ptr Create(const Vec3& halfExtents = Vec3(0.5f))
    {
        return std::make_shared<BoxShape>(halfExtents);
    }

private:
    Vec3 m_halfExtents;
};

/**
 * @brief Capsule collision shape (oriented along Y axis)
 */
class CapsuleShape : public CollisionShape
{
public:
    CapsuleShape(float radius = 0.5f, float halfHeight = 0.5f)
        : m_radius(radius), m_halfHeight(halfHeight) {}

    ShapeType GetType() const override { return ShapeType::Capsule; }
    const char* GetTypeName() const override { return "Capsule"; }

    float GetRadius() const { return m_radius; }
    void SetRadius(float radius) { m_radius = radius; }

    float GetHalfHeight() const { return m_halfHeight; }
    void SetHalfHeight(float halfHeight) { m_halfHeight = halfHeight; }

    float GetVolume() const override
    {
        float sphereVolume = (4.0f / 3.0f) * 3.14159265f * m_radius * m_radius * m_radius;
        float cylinderVolume = 3.14159265f * m_radius * m_radius * (2.0f * m_halfHeight);
        return sphereVolume + cylinderVolume;
    }

    void GetLocalBounds(Vec3& outMin, Vec3& outMax) const override
    {
        float totalHeight = m_halfHeight + m_radius;
        outMin = Vec3(-m_radius, -totalHeight, -m_radius);
        outMax = Vec3(m_radius, totalHeight, m_radius);
    }

    float GetBoundingRadius() const override
    {
        return m_halfHeight + m_radius;
    }

    MassProperties CalculateMassProperties(float density) const override
    {
        MassProperties props;
        props.mass = GetVolume() * density;
        // Simplified inertia calculation
        float r2 = m_radius * m_radius;
        float h = m_halfHeight * 2.0f;
        float iyy = props.mass * (0.25f * r2 + (1.0f/12.0f) * h * h);
        float ixx = iyy;
        float izz = 0.5f * props.mass * r2;
        props.inertiaTensor = Mat3(0.0f);
        props.inertiaTensor[0][0] = ixx;
        props.inertiaTensor[1][1] = izz;
        props.inertiaTensor[2][2] = iyy;
        return props;
    }

    static Ptr Create(float radius = 0.5f, float halfHeight = 0.5f)
    {
        return std::make_shared<CapsuleShape>(radius, halfHeight);
    }

private:
    float m_radius;
    float m_halfHeight;
};

/**
 * @brief Cylinder collision shape (oriented along Y axis)
 */
class CylinderShape : public CollisionShape
{
public:
    CylinderShape(float radius = 0.5f, float halfHeight = 0.5f)
        : m_radius(radius), m_halfHeight(halfHeight) {}

    ShapeType GetType() const override { return ShapeType::Cylinder; }
    const char* GetTypeName() const override { return "Cylinder"; }

    float GetRadius() const { return m_radius; }
    void SetRadius(float radius) { m_radius = radius; }

    float GetHalfHeight() const { return m_halfHeight; }
    void SetHalfHeight(float halfHeight) { m_halfHeight = halfHeight; }

    float GetVolume() const override
    {
        return 3.14159265f * m_radius * m_radius * (2.0f * m_halfHeight);
    }

    void GetLocalBounds(Vec3& outMin, Vec3& outMax) const override
    {
        outMin = Vec3(-m_radius, -m_halfHeight, -m_radius);
        outMax = Vec3(m_radius, m_halfHeight, m_radius);
    }

    float GetBoundingRadius() const override
    {
        return std::sqrt(m_radius * m_radius + m_halfHeight * m_halfHeight);
    }

    MassProperties CalculateMassProperties(float density) const override
    {
        MassProperties props;
        props.mass = GetVolume() * density;
        float r2 = m_radius * m_radius;
        float h2 = m_halfHeight * m_halfHeight;
        // I_xx = I_zz = (1/12) * m * (3r^2 + h^2) where h = 2 * halfHeight
        float ixx = props.mass * (3.0f * r2 + 4.0f * h2) / 12.0f;
        // I_yy = (1/2) * m * r^2
        float iyy = 0.5f * props.mass * r2;
        props.inertiaTensor = Mat3(0.0f);
        props.inertiaTensor[0][0] = ixx;
        props.inertiaTensor[1][1] = iyy;
        props.inertiaTensor[2][2] = ixx;
        return props;
    }

    static Ptr Create(float radius = 0.5f, float halfHeight = 0.5f)
    {
        return std::make_shared<CylinderShape>(radius, halfHeight);
    }

private:
    float m_radius;
    float m_halfHeight;
};

/**
 * @brief Convex hull collision shape
 * 
 * Represents a convex polyhedron defined by a set of vertices.
 * The convex hull is computed from the input points.
 */
class ConvexHullShape : public CollisionShape
{
public:
    ConvexHullShape() = default;
    
    explicit ConvexHullShape(const std::vector<Vec3>& points)
    {
        SetPoints(points);
    }

    ShapeType GetType() const override { return ShapeType::ConvexHull; }
    const char* GetTypeName() const override { return "ConvexHull"; }

    /**
     * @brief Set the points that define the convex hull
     */
    void SetPoints(const std::vector<Vec3>& points)
    {
        m_vertices = points;
        ComputeHull();
    }

    const std::vector<Vec3>& GetVertices() const { return m_vertices; }

    /**
     * @brief Get support point in given direction (for GJK)
     */
    Vec3 Support(const Vec3& direction) const
    {
        if (m_vertices.empty())
            return Vec3(0);

        float maxDot = glm::dot(m_vertices[0], direction);
        size_t bestIdx = 0;

        for (size_t i = 1; i < m_vertices.size(); ++i)
        {
            float d = glm::dot(m_vertices[i], direction);
            if (d > maxDot)
            {
                maxDot = d;
                bestIdx = i;
            }
        }

        return m_vertices[bestIdx];
    }

    float GetVolume() const override
    {
        // Approximate volume using bounding box ratio
        Vec3 bmin, bmax;
        GetLocalBounds(bmin, bmax);
        Vec3 size = bmax - bmin;
        return size.x * size.y * size.z * 0.6f;  // Approximate convex fill ratio
    }

    void GetLocalBounds(Vec3& outMin, Vec3& outMax) const override
    {
        if (m_vertices.empty())
        {
            outMin = outMax = Vec3(0);
            return;
        }

        outMin = outMax = m_vertices[0];
        for (const auto& v : m_vertices)
        {
            outMin = glm::min(outMin, v);
            outMax = glm::max(outMax, v);
        }
    }

    float GetBoundingRadius() const override
    {
        float maxDistSq = 0.0f;
        for (const auto& v : m_vertices)
        {
            maxDistSq = std::max(maxDistSq, glm::dot(v, v));
        }
        return std::sqrt(maxDistSq);
    }

    MassProperties CalculateMassProperties(float density) const override
    {
        MassProperties props;
        props.mass = GetVolume() * density;
        
        // Simplified: use bounding box inertia
        Vec3 bmin, bmax;
        GetLocalBounds(bmin, bmax);
        Vec3 size = bmax - bmin;
        
        float factor = props.mass / 12.0f;
        props.inertiaTensor = Mat3(0.0f);
        props.inertiaTensor[0][0] = factor * (size.y * size.y + size.z * size.z);
        props.inertiaTensor[1][1] = factor * (size.x * size.x + size.z * size.z);
        props.inertiaTensor[2][2] = factor * (size.x * size.x + size.y * size.y);
        
        return props;
    }

    static Ptr Create(const std::vector<Vec3>& points)
    {
        return std::make_shared<ConvexHullShape>(points);
    }

private:
    void ComputeHull()
    {
        // Compute center of mass for the hull vertices
        if (m_vertices.empty()) return;
        
        m_center = Vec3(0);
        for (const auto& v : m_vertices)
        {
            m_center += v;
        }
        m_center /= static_cast<float>(m_vertices.size());
    }

    std::vector<Vec3> m_vertices;
    Vec3 m_center{0};
};

/**
 * @brief Triangle mesh collision shape (static geometry only)
 * 
 * Used for static level geometry. Not suitable for dynamic bodies.
 */
class TriangleMeshShape : public CollisionShape
{
public:
    struct Triangle
    {
        Vec3 v0, v1, v2;
        Vec3 normal;
    };

    TriangleMeshShape() = default;

    /**
     * @brief Create from vertex and index buffers
     */
    TriangleMeshShape(const std::vector<Vec3>& vertices, const std::vector<uint32>& indices)
    {
        SetMesh(vertices, indices);
    }

    ShapeType GetType() const override { return ShapeType::TriangleMesh; }
    const char* GetTypeName() const override { return "TriangleMesh"; }

    /**
     * @brief Set mesh data
     */
    void SetMesh(const std::vector<Vec3>& vertices, const std::vector<uint32>& indices)
    {
        m_triangles.clear();
        m_triangles.reserve(indices.size() / 3);

        for (size_t i = 0; i + 2 < indices.size(); i += 3)
        {
            Triangle tri;
            tri.v0 = vertices[indices[i]];
            tri.v1 = vertices[indices[i + 1]];
            tri.v2 = vertices[indices[i + 2]];
            
            Vec3 edge1 = tri.v1 - tri.v0;
            Vec3 edge2 = tri.v2 - tri.v0;
            tri.normal = glm::normalize(glm::cross(edge1, edge2));
            
            m_triangles.push_back(tri);
        }

        // Compute bounds
        if (!vertices.empty())
        {
            m_boundsMin = m_boundsMax = vertices[0];
            for (const auto& v : vertices)
            {
                m_boundsMin = glm::min(m_boundsMin, v);
                m_boundsMax = glm::max(m_boundsMax, v);
            }
        }
    }

    const std::vector<Triangle>& GetTriangles() const { return m_triangles; }
    size_t GetTriangleCount() const { return m_triangles.size(); }

    float GetVolume() const override
    {
        // Mesh volume calculation (closed mesh assumed)
        float volume = 0.0f;
        for (const auto& tri : m_triangles)
        {
            volume += glm::dot(tri.v0, glm::cross(tri.v1, tri.v2)) / 6.0f;
        }
        return std::abs(volume);
    }

    void GetLocalBounds(Vec3& outMin, Vec3& outMax) const override
    {
        outMin = m_boundsMin;
        outMax = m_boundsMax;
    }

    float GetBoundingRadius() const override
    {
        return std::max(length(m_boundsMin), length(m_boundsMax));
    }

    MassProperties CalculateMassProperties(float density) const override
    {
        MassProperties props;
        props.mass = GetVolume() * density;
        
        // Use bounding box for inertia approximation
        Vec3 size = m_boundsMax - m_boundsMin;
        float factor = props.mass / 12.0f;
        props.inertiaTensor = Mat3(0.0f);
        props.inertiaTensor[0][0] = factor * (size.y * size.y + size.z * size.z);
        props.inertiaTensor[1][1] = factor * (size.x * size.x + size.z * size.z);
        props.inertiaTensor[2][2] = factor * (size.x * size.x + size.y * size.y);
        
        return props;
    }

    static Ptr Create(const std::vector<Vec3>& vertices, const std::vector<uint32>& indices)
    {
        return std::make_shared<TriangleMeshShape>(vertices, indices);
    }

private:
    std::vector<Triangle> m_triangles;
    Vec3 m_boundsMin{0};
    Vec3 m_boundsMax{0};
};

/**
 * @brief Height field collision shape for terrain
 */
class HeightFieldShape : public CollisionShape
{
public:
    HeightFieldShape() = default;

    /**
     * @brief Create height field
     * @param heights 2D array of height values (row-major)
     * @param width Number of samples in X direction
     * @param depth Number of samples in Z direction
     * @param scale World-space scale (X, Y height, Z)
     */
    HeightFieldShape(const std::vector<float>& heights, 
                     uint32 width, uint32 depth, 
                     const Vec3& scale = Vec3(1.0f))
        : m_heights(heights)
        , m_width(width)
        , m_depth(depth)
        , m_scale(scale)
    {
        ComputeBounds();
    }

    ShapeType GetType() const override { return ShapeType::HeightField; }
    const char* GetTypeName() const override { return "HeightField"; }

    uint32 GetWidth() const { return m_width; }
    uint32 GetDepth() const { return m_depth; }
    const Vec3& GetScale() const { return m_scale; }

    /**
     * @brief Get height at a world position
     */
    float GetHeightAt(float x, float z) const
    {
        if (m_heights.empty() || m_width < 2 || m_depth < 2)
            return 0.0f;

        // Convert to grid coordinates
        float gx = x / m_scale.x + 0.5f * (m_width - 1);
        float gz = z / m_scale.z + 0.5f * (m_depth - 1);

        // Clamp to grid bounds
        gx = glm::clamp(gx, 0.0f, static_cast<float>(m_width - 1));
        gz = glm::clamp(gz, 0.0f, static_cast<float>(m_depth - 1));

        // Get integer coordinates
        int ix = static_cast<int>(gx);
        int iz = static_cast<int>(gz);
        ix = glm::clamp(ix, 0, static_cast<int>(m_width) - 2);
        iz = glm::clamp(iz, 0, static_cast<int>(m_depth) - 2);

        // Get fractional parts
        float fx = gx - ix;
        float fz = gz - iz;

        // Sample heights at corners
        float h00 = m_heights[iz * m_width + ix];
        float h10 = m_heights[iz * m_width + ix + 1];
        float h01 = m_heights[(iz + 1) * m_width + ix];
        float h11 = m_heights[(iz + 1) * m_width + ix + 1];

        // Bilinear interpolation
        float h0 = h00 * (1.0f - fx) + h10 * fx;
        float h1 = h01 * (1.0f - fx) + h11 * fx;
        float height = h0 * (1.0f - fz) + h1 * fz;

        return height * m_scale.y;
    }

    /**
     * @brief Get normal at a world position
     */
    Vec3 GetNormalAt(float x, float z) const
    {
        const float delta = m_scale.x * 0.1f;
        float hL = GetHeightAt(x - delta, z);
        float hR = GetHeightAt(x + delta, z);
        float hD = GetHeightAt(x, z - delta);
        float hU = GetHeightAt(x, z + delta);

        return glm::normalize(Vec3(hL - hR, 2.0f * delta, hD - hU));
    }

    float GetVolume() const override
    {
        // Not meaningful for height fields
        return 0.0f;
    }

    void GetLocalBounds(Vec3& outMin, Vec3& outMax) const override
    {
        outMin = m_boundsMin;
        outMax = m_boundsMax;
    }

    float GetBoundingRadius() const override
    {
        return length((m_boundsMax - m_boundsMin) * 0.5f);
    }

    MassProperties CalculateMassProperties(float density) const override
    {
        // Height fields are always static, mass not needed
        MassProperties props;
        props.mass = 0.0f;
        return props;
    }

    static Ptr Create(const std::vector<float>& heights, 
                      uint32 width, uint32 depth,
                      const Vec3& scale = Vec3(1.0f))
    {
        return std::make_shared<HeightFieldShape>(heights, width, depth, scale);
    }

private:
    void ComputeBounds()
    {
        if (m_heights.empty()) return;

        float minHeight = m_heights[0];
        float maxHeight = m_heights[0];
        for (float h : m_heights)
        {
            minHeight = std::min(minHeight, h);
            maxHeight = std::max(maxHeight, h);
        }

        float halfWidth = (m_width - 1) * m_scale.x * 0.5f;
        float halfDepth = (m_depth - 1) * m_scale.z * 0.5f;

        m_boundsMin = Vec3(-halfWidth, minHeight * m_scale.y, -halfDepth);
        m_boundsMax = Vec3(halfWidth, maxHeight * m_scale.y, halfDepth);
    }

    std::vector<float> m_heights;
    uint32 m_width = 0;
    uint32 m_depth = 0;
    Vec3 m_scale{1.0f};
    Vec3 m_boundsMin{0};
    Vec3 m_boundsMax{0};
};

/**
 * @brief Compound collision shape (multiple child shapes)
 */
class CompoundShape : public CollisionShape
{
public:
    struct ChildShape
    {
        CollisionShape::Ptr shape;
        Vec3 offset{0};
        Quat rotation{1, 0, 0, 0};
    };

    CompoundShape() = default;

    ShapeType GetType() const override { return ShapeType::Compound; }
    const char* GetTypeName() const override { return "Compound"; }

    /**
     * @brief Add a child shape
     */
    void AddChild(CollisionShape::Ptr shape, const Vec3& offset = Vec3(0),
                  const Quat& rotation = Quat(1, 0, 0, 0))
    {
        m_children.push_back({std::move(shape), offset, rotation});
        UpdateBounds();
    }

    /**
     * @brief Remove a child shape by index
     */
    void RemoveChild(size_t index)
    {
        if (index < m_children.size())
        {
            m_children.erase(m_children.begin() + index);
            UpdateBounds();
        }
    }

    /**
     * @brief Get child shapes
     */
    const std::vector<ChildShape>& GetChildren() const { return m_children; }
    size_t GetChildCount() const { return m_children.size(); }

    float GetVolume() const override
    {
        float volume = 0.0f;
        for (const auto& child : m_children)
        {
            volume += child.shape->GetVolume();
        }
        return volume;
    }

    void GetLocalBounds(Vec3& outMin, Vec3& outMax) const override
    {
        outMin = m_boundsMin;
        outMax = m_boundsMax;
    }

    float GetBoundingRadius() const override
    {
        float radius = 0.0f;
        for (const auto& child : m_children)
        {
            float childRadius = length(child.offset) + child.shape->GetBoundingRadius();
            radius = std::max(radius, childRadius);
        }
        return radius;
    }

    MassProperties CalculateMassProperties(float density) const override
    {
        MassProperties props;
        props.mass = 0.0f;
        props.centerOfMass = Vec3(0);
        props.inertiaTensor = Mat3(0.0f);

        // First pass: compute total mass and center of mass
        for (const auto& child : m_children)
        {
            MassProperties childProps = child.shape->CalculateMassProperties(density);
            props.mass += childProps.mass;
            props.centerOfMass += (child.offset + childProps.centerOfMass) * childProps.mass;
        }

        if (props.mass > 0.0f)
        {
            props.centerOfMass /= props.mass;
        }

        // Second pass: compute combined inertia tensor (parallel axis theorem)
        for (const auto& child : m_children)
        {
            MassProperties childProps = child.shape->CalculateMassProperties(density);
            
            // Transform child inertia to compound space
            Mat3 rotMat = mat3_cast(child.rotation);
            Mat3 rotatedInertia = rotMat * childProps.inertiaTensor * transpose(rotMat);
            
            // Parallel axis theorem: I = I_cm + m * d^2
            Vec3 r = child.offset + childProps.centerOfMass - props.centerOfMass;
            float rDotR = dot(r, r);
            Mat3 parallelAxisTerm = Mat3(0.0f);
            parallelAxisTerm[0][0] = childProps.mass * (rDotR - r.x * r.x);
            parallelAxisTerm[1][1] = childProps.mass * (rDotR - r.y * r.y);
            parallelAxisTerm[2][2] = childProps.mass * (rDotR - r.z * r.z);
            parallelAxisTerm[0][1] = parallelAxisTerm[1][0] = -childProps.mass * r.x * r.y;
            parallelAxisTerm[0][2] = parallelAxisTerm[2][0] = -childProps.mass * r.x * r.z;
            parallelAxisTerm[1][2] = parallelAxisTerm[2][1] = -childProps.mass * r.y * r.z;
            
            props.inertiaTensor += rotatedInertia + parallelAxisTerm;
        }

        return props;
    }

    static Ptr Create()
    {
        return std::make_shared<CompoundShape>();
    }

private:
    void UpdateBounds()
    {
        if (m_children.empty())
        {
            m_boundsMin = m_boundsMax = Vec3(0);
            return;
        }

        m_boundsMin = Vec3(FLT_MAX);
        m_boundsMax = Vec3(-FLT_MAX);

        for (const auto& child : m_children)
        {
            Vec3 childMin, childMax;
            child.shape->GetLocalBounds(childMin, childMax);

            // Transform bounds (approximate: use corners)
            Mat3 rotMat = mat3_cast(child.rotation);
            Vec3 corners[8] = {
                {childMin.x, childMin.y, childMin.z},
                {childMax.x, childMin.y, childMin.z},
                {childMin.x, childMax.y, childMin.z},
                {childMax.x, childMax.y, childMin.z},
                {childMin.x, childMin.y, childMax.z},
                {childMax.x, childMin.y, childMax.z},
                {childMin.x, childMax.y, childMax.z},
                {childMax.x, childMax.y, childMax.z}
            };

            for (const auto& corner : corners)
            {
                Vec3 transformed = rotMat * corner + child.offset;
                m_boundsMin = glm::min(m_boundsMin, transformed);
                m_boundsMax = glm::max(m_boundsMax, transformed);
            }
        }
    }

    std::vector<ChildShape> m_children;
    Vec3 m_boundsMin{0};
    Vec3 m_boundsMax{0};
};

} // namespace RVX::Physics
