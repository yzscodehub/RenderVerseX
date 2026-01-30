/**
 * @file TerrainCollider.cpp
 * @brief Implementation of terrain physics collider
 */

#include "Terrain/TerrainCollider.h"
#include "Terrain/Heightmap.h"
#include "Core/Log.h"

#include <cmath>
#include <algorithm>

namespace RVX
{

bool TerrainCollider::Initialize(const Heightmap* heightmap, const Vec3& terrainSize,
                                  const Vec3& terrainPosition)
{
    if (!heightmap || !heightmap->IsValid())
    {
        RVX_CORE_ERROR("TerrainCollider: Invalid heightmap");
        return false;
    }

    m_heightmap = heightmap;
    m_terrainSize = terrainSize;
    m_terrainPosition = terrainPosition;
    m_terrainRotation = Quat(1, 0, 0, 0);

    // Calculate transforms
    m_localToWorld = MakeTranslation(terrainPosition);
    m_worldToLocal = inverse(m_localToWorld);

    // Calculate world bounds
    Vec3 halfSize = terrainSize * 0.5f;
    m_worldBounds = AABB(
        Vec3(terrainPosition.x - halfSize.x, terrainPosition.y, terrainPosition.z - halfSize.z),
        Vec3(terrainPosition.x + halfSize.x, terrainPosition.y + terrainSize.y, 
             terrainPosition.z + halfSize.z)
    );

    BuildQuadTree();

    RVX_CORE_INFO("TerrainCollider: Initialized with {} quadtree nodes", m_quadTree.size());
    return true;
}

void TerrainCollider::UpdateHeightmap(const Heightmap* heightmap)
{
    m_heightmap = heightmap;
    BuildQuadTree();
}

void TerrainCollider::SetTransform(const Vec3& position, const Quat& rotation)
{
    m_terrainPosition = position;
    m_terrainRotation = rotation;

    // Update transforms
    m_localToWorld = MakeTranslation(position) * QuatToMat4(rotation);
    m_worldToLocal = inverse(m_localToWorld);

    // Update bounds
    Vec3 halfSize = m_terrainSize * 0.5f;
    AABB localBounds(
        Vec3(-halfSize.x, 0, -halfSize.z),
        Vec3(halfSize.x, m_terrainSize.y, halfSize.z)
    );
    m_worldBounds = localBounds.Transformed(m_localToWorld);
}

bool TerrainCollider::Raycast(const Vec3& origin, const Vec3& direction, float maxDistance,
                               TerrainRaycastHit& outHit) const
{
    if (!m_heightmap) return false;

    // Quick AABB check
    Vec3 invDir(
        std::abs(direction.x) > 1e-6f ? 1.0f / direction.x : 1e30f,
        std::abs(direction.y) > 1e-6f ? 1.0f / direction.y : 1e30f,
        std::abs(direction.z) > 1e-6f ? 1.0f / direction.z : 1e30f
    );

    float tMin = 0.0f;
    float tMax = maxDistance;

    // AABB intersection
    Vec3 t1 = (m_worldBounds.GetMin() - origin) * invDir;
    Vec3 t2 = (m_worldBounds.GetMax() - origin) * invDir;

    for (int i = 0; i < 3; ++i)
    {
        float tNear = std::min(t1[i], t2[i]);
        float tFar = std::max(t1[i], t2[i]);
        tMin = std::max(tMin, tNear);
        tMax = std::min(tMax, tFar);
    }

    if (tMin > tMax) return false;

    // Traverse quadtree
    if (m_quadTree.empty()) return false;

    return RaycastNode(0, origin, invDir, tMin, tMax, outHit);
}

uint32 TerrainCollider::RaycastBatch(const Vec3* origins, const Vec3* directions, uint32 count,
                                      float maxDistance, TerrainRaycastHit* outHits) const
{
    uint32 hitCount = 0;
    for (uint32 i = 0; i < count; ++i)
    {
        if (Raycast(origins[i], directions[i], maxDistance, outHits[hitCount]))
        {
            ++hitCount;
        }
    }
    return hitCount;
}

bool TerrainCollider::SphereOverlap(const Vec3& center, float radius) const
{
    if (!m_heightmap) return false;

    // Quick bounds check
    AABB sphereBounds(center - Vec3(radius), center + Vec3(radius));
    if (!m_worldBounds.Overlaps(sphereBounds)) return false;

    // Check height at sphere center
    auto height = GetHeightAt(center.x, center.z);
    if (!height) return false;

    // Simple sphere-plane intersection
    return center.y - radius < *height;
}

bool TerrainCollider::CapsuleOverlap(const Vec3& start, const Vec3& end, float radius) const
{
    if (!m_heightmap) return false;

    // Sample multiple points along the capsule
    const int numSamples = 8;
    for (int i = 0; i <= numSamples; ++i)
    {
        float t = static_cast<float>(i) / numSamples;
        Vec3 point = mix(start, end, t);

        if (SphereOverlap(point, radius))
            return true;
    }

    return false;
}

bool TerrainCollider::AABBOverlap(const AABB& aabb) const
{
    if (!m_heightmap) return false;

    // Quick bounds check
    if (!m_worldBounds.Overlaps(aabb)) return false;

    // Sample heights at AABB corners
    Vec3 corners[4] = {
        Vec3(aabb.GetMin().x, 0, aabb.GetMin().z),
        Vec3(aabb.GetMax().x, 0, aabb.GetMin().z),
        Vec3(aabb.GetMin().x, 0, aabb.GetMax().z),
        Vec3(aabb.GetMax().x, 0, aabb.GetMax().z)
    };

    float minY = aabb.GetMin().y;

    for (const auto& corner : corners)
    {
        auto height = GetHeightAt(corner.x, corner.z);
        if (height && minY < *height)
            return true;
    }

    return false;
}

uint32 TerrainCollider::GenerateSphereContacts(const Vec3& center, float radius,
                                                ContactPoint* outContacts, uint32 maxContacts) const
{
    if (!m_heightmap || maxContacts == 0) return 0;

    auto height = GetHeightAt(center.x, center.z);
    if (!height) return 0;

    float penetration = *height - (center.y - radius);
    if (penetration <= 0) return 0;

    auto normal = GetNormalAt(center.x, center.z);

    outContacts[0].position = Vec3(center.x, *height, center.z);
    outContacts[0].normal = normal.value_or(Vec3(0, 1, 0));
    outContacts[0].penetration = penetration;

    return 1;
}

uint32 TerrainCollider::GenerateCapsuleContacts(const Vec3& start, const Vec3& end, float radius,
                                                 ContactPoint* outContacts, uint32 maxContacts) const
{
    if (!m_heightmap || maxContacts == 0) return 0;

    uint32 contactCount = 0;
    const int numSamples = std::min(static_cast<int>(maxContacts), 4);

    for (int i = 0; i < numSamples && contactCount < maxContacts; ++i)
    {
        float t = static_cast<float>(i) / (numSamples - 1);
        Vec3 point = mix(start, end, t);

        auto height = GetHeightAt(point.x, point.z);
        if (!height) continue;

        float penetration = *height - (point.y - radius);
        if (penetration <= 0) continue;

        auto normal = GetNormalAt(point.x, point.z);

        outContacts[contactCount].position = Vec3(point.x, *height, point.z);
        outContacts[contactCount].normal = normal.value_or(Vec3(0, 1, 0));
        outContacts[contactCount].penetration = penetration;
        ++contactCount;
    }

    return contactCount;
}

std::optional<float> TerrainCollider::GetHeightAt(float worldX, float worldZ) const
{
    if (!m_heightmap) return std::nullopt;

    Vec2 uv = WorldToUV(worldX, worldZ);
    if (uv.x < 0 || uv.x > 1 || uv.y < 0 || uv.y > 1)
        return std::nullopt;

    float normalizedHeight = m_heightmap->SampleHeight(uv.x, uv.y);
    return m_terrainPosition.y + normalizedHeight * m_terrainSize.y;
}

std::optional<Vec3> TerrainCollider::GetNormalAt(float worldX, float worldZ) const
{
    if (!m_heightmap) return std::nullopt;

    Vec2 uv = WorldToUV(worldX, worldZ);
    if (uv.x < 0 || uv.x > 1 || uv.y < 0 || uv.y > 1)
        return std::nullopt;

    return m_heightmap->SampleNormal(uv.x, uv.y, m_terrainSize);
}

bool TerrainCollider::IsWithinBounds(float worldX, float worldZ) const
{
    Vec2 uv = WorldToUV(worldX, worldZ);
    return uv.x >= 0 && uv.x <= 1 && uv.y >= 0 && uv.y <= 1;
}

void TerrainCollider::BuildQuadTree()
{
    m_quadTree.clear();

    if (!m_heightmap) return;

    // Create root node
    QuadNode root;
    root.bounds = m_worldBounds;
    root.isLeaf = true;

    m_quadTree.push_back(root);

    // For now, keep it simple - just use root node
    // A full implementation would subdivide based on heightmap resolution
}

bool TerrainCollider::RaycastNode(uint32 nodeIndex, const Vec3& origin, const Vec3& invDir,
                                   float tMin, float tMax, TerrainRaycastHit& outHit) const
{
    if (nodeIndex >= m_quadTree.size()) return false;

    const QuadNode& node = m_quadTree[nodeIndex];

    // AABB intersection for this node
    Vec3 t1 = (node.bounds.GetMin() - origin) * invDir;
    Vec3 t2 = (node.bounds.GetMax() - origin) * invDir;

    float tNodeMin = tMin;
    float tNodeMax = tMax;

    for (int i = 0; i < 3; ++i)
    {
        float tNear = std::min(t1[i], t2[i]);
        float tFar = std::max(t1[i], t2[i]);
        tNodeMin = std::max(tNodeMin, tNear);
        tNodeMax = std::min(tNodeMax, tFar);
    }

    if (tNodeMin > tNodeMax) return false;

    if (node.isLeaf)
    {
        // Ray march through the terrain
        Vec3 direction = Vec3(1.0f / invDir.x, 1.0f / invDir.y, 1.0f / invDir.z);
        
        const int maxSteps = 128;
        float stepSize = (tNodeMax - tNodeMin) / maxSteps;

        for (int step = 0; step < maxSteps; ++step)
        {
            float t = tNodeMin + step * stepSize;
            Vec3 pos = origin + direction * t;

            auto height = GetHeightAt(pos.x, pos.z);
            if (height && pos.y < *height)
            {
                // Found intersection - refine with binary search
                float tLow = t - stepSize;
                float tHigh = t;

                for (int refine = 0; refine < 8; ++refine)
                {
                    float tMid = (tLow + tHigh) * 0.5f;
                    Vec3 midPos = origin + direction * tMid;
                    auto midHeight = GetHeightAt(midPos.x, midPos.z);

                    if (midHeight && midPos.y < *midHeight)
                        tHigh = tMid;
                    else
                        tLow = tMid;
                }

                outHit.distance = tHigh;
                outHit.position = origin + direction * tHigh;
                
                auto normal = GetNormalAt(outHit.position.x, outHit.position.z);
                outHit.normal = normal.value_or(Vec3(0, 1, 0));
                outHit.uv = WorldToUV(outHit.position.x, outHit.position.z);

                return true;
            }
        }
    }
    else
    {
        // Recurse into children
        for (int i = 0; i < 4; ++i)
        {
            if (node.children[i] != 0)
            {
                if (RaycastNode(node.children[i], origin, invDir, tNodeMin, tNodeMax, outHit))
                    return true;
            }
        }
    }

    return false;
}

bool TerrainCollider::RaycastTriangle(const Vec3& v0, const Vec3& v1, const Vec3& v2,
                                       const Vec3& origin, const Vec3& direction,
                                       float& outT, Vec3& outNormal) const
{
    // Möller–Trumbore intersection algorithm
    Vec3 edge1 = v1 - v0;
    Vec3 edge2 = v2 - v0;
    Vec3 h = cross(direction, edge2);
    float a = dot(edge1, h);

    if (std::abs(a) < 1e-6f) return false;

    float f = 1.0f / a;
    Vec3 s = origin - v0;
    float u = f * dot(s, h);

    if (u < 0.0f || u > 1.0f) return false;

    Vec3 q = cross(s, edge1);
    float v = f * dot(direction, q);

    if (v < 0.0f || u + v > 1.0f) return false;

    float t = f * dot(edge2, q);

    if (t > 1e-6f)
    {
        outT = t;
        outNormal = normalize(cross(edge1, edge2));
        return true;
    }

    return false;
}

Vec3 TerrainCollider::WorldToLocal(const Vec3& worldPos) const
{
    Vec4 local = m_worldToLocal * Vec4(worldPos, 1.0f);
    return Vec3(local);
}

Vec3 TerrainCollider::LocalToWorld(const Vec3& localPos) const
{
    Vec4 world = m_localToWorld * Vec4(localPos, 1.0f);
    return Vec3(world);
}

Vec2 TerrainCollider::WorldToUV(float worldX, float worldZ) const
{
    float halfWidth = m_terrainSize.x * 0.5f;
    float halfDepth = m_terrainSize.z * 0.5f;

    float u = (worldX - m_terrainPosition.x + halfWidth) / m_terrainSize.x;
    float v = (worldZ - m_terrainPosition.z + halfDepth) / m_terrainSize.z;

    return Vec2(u, v);
}

} // namespace RVX
