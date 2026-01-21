/**
 * @file PickingSystem.h
 * @brief DEPRECATED: Use World/PickingService.h instead
 * 
 * This header provides backward compatibility.
 * New code should use World/PickingService.h and World/SpatialSubsystem.h
 */

#pragma once

#pragma message("Warning: Picking/PickingSystem.h is deprecated, use World/PickingService.h")

#include "World/PickingService.h"
#include "World/SpatialSubsystem.h"
#include "Core/Math/Geometry.h"
#include <memory>
#include <vector>
#include <functional>

namespace RVX
{

// Forward declarations
class Camera;
class Node;
class Mesh;

/**
 * @brief Picking result with detailed hit information
 * @deprecated Use RaycastHit from Scene/SceneManager.h
 */
struct PickResult
{
    bool hit{false};
    RayHit rayHit;

    bool HasHit() const { return hit && rayHit.IsValid(); }
    float GetDistance() const { return rayHit.t; }
    Vec3 GetPosition() const { return rayHit.position; }
    Vec3 GetNormal() const { return rayHit.normal; }
    int GetNodeIndex() const { return rayHit.nodeIndex; }
    int GetMeshIndex() const { return rayHit.meshIndex; }
    int GetTriangleIndex() const { return rayHit.primitiveIndex; }
};

/**
 * @brief Mouse picking system
 * @deprecated Use World::Pick() or SpatialSubsystem::Raycast()
 */
class PickingSystem
{
public:
    PickingSystem();
    ~PickingSystem();

    static Ray ScreenToRay(
        const Camera& camera,
        float screenX,
        float screenY,
        float screenWidth,
        float screenHeight);

    static Ray NDCToRay(
        const Camera& camera,
        float ndcX,
        float ndcY);

    void AddMesh(
        int nodeIndex,
        int meshIndex,
        const std::shared_ptr<Mesh>& mesh,
        const Mat4& worldTransform);

    void AddMesh(
        int nodeIndex,
        int meshIndex,
        const std::vector<Vec3>& positions,
        const std::vector<uint32_t>& indices,
        const Mat4& worldTransform);

    void Build();
    void Rebuild();
    void Clear();

    PickResult Pick(const Ray& ray, const PickingConfig& config = {}) const;

    PickResult PickScreen(
        const Camera& camera,
        float screenX,
        float screenY,
        float screenWidth,
        float screenHeight,
        const PickingConfig& config = {}) const;

    bool IsOccluded(const Ray& ray) const;
    size_t GetObjectCount() const;
    bool IsBuilt() const { return m_isBuilt; }

    struct BVHStats
    {
        size_t nodeCount = 0;
        size_t leafCount = 0;
        size_t triangleCount = 0;
        int maxDepth = 0;
    };

    const BVHStats& GetStats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    bool m_isBuilt{false};
    mutable BVHStats m_stats;
};

} // namespace RVX
