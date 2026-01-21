/**
 * @file PickingSystem.cpp
 * @brief Implementation of mouse picking system
 */

#include "Picking/PickingSystem.h"
#include "PickingBVH.h"
#include "Runtime/Camera/Camera.h"
#include "Scene/Mesh.h"
#include "Scene/VertexAttribute.h"
#include <cmath>

namespace RVX
{

struct PickingSystem::Impl
{
    struct PendingMesh
    {
        int nodeIndex;
        int meshIndex;
        std::vector<Vec3> positions;
        std::vector<uint32_t> indices;
        Mat4 worldTransform;
    };

    std::vector<PendingMesh> pendingMeshes;
    std::vector<std::shared_ptr<MeshBVH>> meshBVHs;
    SceneBVH sceneBVH;
};

PickingSystem::PickingSystem()
    : m_impl(std::make_unique<Impl>())
{
}

PickingSystem::~PickingSystem() = default;

Ray PickingSystem::ScreenToRay(
    const Camera& camera,
    float screenX,
    float screenY,
    float screenWidth,
    float screenHeight)
{
    // Convert screen to NDC [-1, 1]
    float ndcX = (2.0f * screenX / screenWidth) - 1.0f;
    float ndcY = 1.0f - (2.0f * screenY / screenHeight);  // Flip Y
    
    return NDCToRay(camera, ndcX, ndcY);
}

Ray PickingSystem::NDCToRay(
    const Camera& camera,
    float ndcX,
    float ndcY)
{
    // Get inverse view-projection matrix
    Mat4 invViewProj = inverse(camera.GetViewProjection());
    
    // Transform NDC points to world space
    Vec4 nearPoint = invViewProj * Vec4(ndcX, ndcY, -1.0f, 1.0f);
    Vec4 farPoint = invViewProj * Vec4(ndcX, ndcY, 1.0f, 1.0f);
    
    // Perspective divide
    Vec3 nearWorld = Vec3(nearPoint) / nearPoint.w;
    Vec3 farWorld = Vec3(farPoint) / farPoint.w;
    
    // Create ray from near to far
    Vec3 direction = normalize(farWorld - nearWorld);
    
    return Ray(nearWorld, direction);
}

void PickingSystem::AddMesh(
    int nodeIndex,
    int meshIndex,
    const std::shared_ptr<Mesh>& mesh,
    const Mat4& worldTransform)
{
    if (!mesh) return;
    
    // Extract position data from mesh
    std::vector<Vec3> positions;
    std::vector<uint32_t> indices;
    
    // Get vertex positions
    const VertexAttribute* posAttr = mesh->GetAttribute(VertexBufferNames::Position);
    if (!posAttr || posAttr->GetVertexCount() == 0)
    {
        return;
    }
    
    size_t vertexCount = mesh->GetVertexCount();
    positions.resize(vertexCount);
    
    const float* posPtr = reinterpret_cast<const float*>(posAttr->GetData());
    
    for (size_t i = 0; i < vertexCount; ++i)
    {
        positions[i] = Vec3(posPtr[i * 3 + 0], posPtr[i * 3 + 1], posPtr[i * 3 + 2]);
    }
    
    // Get indices
    indices = mesh->GetTypedIndices<uint32_t>();
    
    AddMesh(nodeIndex, meshIndex, positions, indices, worldTransform);
}

void PickingSystem::AddMesh(
    int nodeIndex,
    int meshIndex,
    const std::vector<Vec3>& positions,
    const std::vector<uint32_t>& indices,
    const Mat4& worldTransform)
{
    if (positions.empty() || indices.empty()) return;
    
    Impl::PendingMesh pending;
    pending.nodeIndex = nodeIndex;
    pending.meshIndex = meshIndex;
    pending.positions = positions;
    pending.indices = indices;
    pending.worldTransform = worldTransform;
    
    m_impl->pendingMeshes.push_back(std::move(pending));
    m_isBuilt = false;
}

void PickingSystem::Build()
{
    m_impl->meshBVHs.clear();
    m_impl->sceneBVH.Clear();
    
    if (m_impl->pendingMeshes.empty())
    {
        m_isBuilt = true;
        return;
    }
    
    // Build mesh BVHs
    for (const auto& pending : m_impl->pendingMeshes)
    {
        auto meshBVH = std::make_shared<MeshBVH>();
        meshBVH->Build(pending.positions, pending.indices);
        
        // Compute world bounds
        AABB localBounds = meshBVH->GetBounds();
        AABB worldBounds;
        
        // Transform AABB corners to world space
        Vec3 localMin = localBounds.GetMin();
        Vec3 localMax = localBounds.GetMax();
        
        for (int i = 0; i < 8; ++i)
        {
            Vec3 corner(
                (i & 1) ? localMax.x : localMin.x,
                (i & 2) ? localMax.y : localMin.y,
                (i & 4) ? localMax.z : localMin.z
            );
            Vec3 worldCorner = Vec3(pending.worldTransform * Vec4(corner, 1.0f));
            worldBounds.Expand(worldCorner);
        }
        
        // Add to scene BVH
        SceneBVH::ObjectEntry entry;
        entry.nodeIndex = pending.nodeIndex;
        entry.meshIndex = pending.meshIndex;
        entry.worldBounds = worldBounds;
        entry.worldTransform = pending.worldTransform;
        entry.inverseTransform = inverse(pending.worldTransform);
        entry.meshBVH = meshBVH;
        
        m_impl->sceneBVH.AddObject(entry);
        m_impl->meshBVHs.push_back(meshBVH);
    }
    
    // Build scene BVH
    m_impl->sceneBVH.Build();
    m_isBuilt = true;
}

void PickingSystem::Rebuild()
{
    Build();
}

void PickingSystem::Clear()
{
    m_impl->pendingMeshes.clear();
    m_impl->meshBVHs.clear();
    m_impl->sceneBVH.Clear();
    m_isBuilt = false;
}

PickResult PickingSystem::Pick(const Ray& ray, const PickingConfig& config) const
{
    PickResult result;
    result.hit = false;
    result.rayHit.Invalidate();
    
    if (!m_isBuilt) return result;
    
    // Create bounded ray
    Ray boundedRay = ray;
    boundedRay.tMax = config.maxDistance;
    
    // Perform intersection test
    if (m_impl->sceneBVH.Intersect(boundedRay, result.rayHit))
    {
        result.hit = true;
    }
    
    return result;
}

PickResult PickingSystem::PickScreen(
    const Camera& camera,
    float screenX,
    float screenY,
    float screenWidth,
    float screenHeight,
    const PickingConfig& config) const
{
    Ray ray = ScreenToRay(camera, screenX, screenY, screenWidth, screenHeight);
    return Pick(ray, config);
}

bool PickingSystem::IsOccluded(const Ray& ray) const
{
    if (!m_isBuilt) return false;
    return m_impl->sceneBVH.IntersectAny(ray);
}

size_t PickingSystem::GetObjectCount() const
{
    return m_impl->sceneBVH.GetObjectCount();
}

const PickingSystem::BVHStats& PickingSystem::GetStats() const
{
    // Convert from internal BVHStats to PickingSystem::BVHStats
    const auto& internalStats = m_impl->sceneBVH.GetStats();
    m_stats.nodeCount = static_cast<size_t>(internalStats.nodeCount);
    m_stats.leafCount = static_cast<size_t>(internalStats.leafCount);
    m_stats.triangleCount = 0;  // Not tracked in internal stats
    m_stats.maxDepth = internalStats.maxDepth;
    return m_stats;
}

} // namespace RVX
