/**
 * @file BVH.h
 * @brief Bounding Volume Hierarchy for ray-scene intersection
 * 
 * Implements a BVH with Surface Area Heuristic (SAH) for optimal construction.
 * Supports both scene-level and mesh-level acceleration.
 */

#pragma once

#include "Acceleration/Ray.h"
#include "Acceleration/Intersection.h"
#include "Scene/BoundingBox.h"
#include "Core/MathTypes.h"
#include <vector>
#include <memory>
#include <functional>
#include <algorithm>
#include <span>

namespace RVX
{

// Forward declarations
class Mesh;
class Node;

/**
 * @brief BVH node structure
 */
struct BVHNode
{
    BoundingBox bounds;
    int leftChild{-1};      // Index of left child (-1 if leaf)
    int rightChild{-1};     // Index of right child (-1 if leaf)
    int primitiveStart{0};  // Start index in primitive array (for leaves)
    int primitiveCount{0};  // Number of primitives (0 for internal nodes)

    bool IsLeaf() const { return primitiveCount > 0; }
};

/**
 * @brief Primitive info for BVH construction
 */
struct BVHPrimitive
{
    int index;              // Original primitive index
    BoundingBox bounds;
    Vec3 centroid;

    BVHPrimitive() = default;
    BVHPrimitive(int idx, const BoundingBox& b)
        : index(idx), bounds(b), centroid(b.GetCenter()) {}
};

/**
 * @brief SAH bucket for cost evaluation
 */
struct SAHBucket
{
    int count{0};
    BoundingBox bounds;
};

/**
 * @brief BVH build statistics
 */
struct BVHStats
{
    int nodeCount{0};
    int leafCount{0};
    int maxDepth{0};
    int maxPrimitivesPerLeaf{0};
    float buildTimeMs{0.0f};
};

/**
 * @brief BVH construction parameters
 */
struct BVHBuildParams
{
    int maxPrimitivesPerLeaf{4};    // Maximum primitives in a leaf
    int sahBucketCount{12};         // Number of buckets for SAH evaluation
    float traversalCost{1.0f};      // Cost of node traversal
    float intersectionCost{1.0f};   // Cost of primitive intersection
};

/**
 * @brief Bounding Volume Hierarchy for mesh triangles
 */
class MeshBVH
{
public:
    MeshBVH() = default;

    /**
     * @brief Build BVH from triangle positions
     * 
     * @param positions Vertex positions (3 floats per vertex)
     * @param indices Triangle indices (3 indices per triangle)
     * @param params Build parameters
     */
    void Build(
        const std::vector<Vec3>& positions,
        const std::vector<uint32_t>& indices,
        const BVHBuildParams& params = {});

    /**
     * @brief Ray intersection test
     * 
     * @param ray The ray to test
     * @param hit Output hit information
     * @return true if ray hits any triangle
     */
    bool Intersect(const Ray& ray, RayHit& hit) const;

    /**
     * @brief Check if any intersection exists (shadow ray)
     */
    bool IntersectAny(const Ray& ray) const;

    /**
     * @brief Get build statistics
     */
    const BVHStats& GetStats() const { return m_stats; }

    /**
     * @brief Get root bounding box
     */
    const BoundingBox& GetBounds() const
    {
        return m_nodes.empty() ? m_emptyBox : m_nodes[0].bounds;
    }

    bool IsBuilt() const { return !m_nodes.empty(); }

private:
    int BuildRecursive(
        std::vector<BVHPrimitive>& primitives,
        int start,
        int end,
        int depth,
        const BVHBuildParams& params);

    void IntersectNode(
        int nodeIndex,
        const Ray& ray,
        RayHit& hit) const;

    bool IntersectNodeAny(
        int nodeIndex,
        const Ray& ray) const;

private:
    std::vector<BVHNode> m_nodes;
    std::vector<int> m_primitiveIndices;  // Reordered primitive indices
    std::vector<Vec3> m_positions;
    std::vector<uint32_t> m_indices;
    BVHStats m_stats;
    BoundingBox m_emptyBox;
};

/**
 * @brief Scene-level BVH for multiple meshes/nodes
 */
class SceneBVH
{
public:
    /**
     * @brief Object entry in scene BVH
     */
    struct ObjectEntry
    {
        int nodeIndex{-1};
        int meshIndex{-1};
        BoundingBox worldBounds;
        Mat4 worldTransform{1.0f};
        Mat4 inverseTransform{1.0f};
        std::shared_ptr<MeshBVH> meshBVH;
    };

    SceneBVH() = default;

    /**
     * @brief Add an object to the scene
     */
    void AddObject(const ObjectEntry& entry);

    /**
     * @brief Build scene BVH from added objects
     */
    void Build(const BVHBuildParams& params = {});

    /**
     * @brief Clear all objects
     */
    void Clear();

    /**
     * @brief Ray intersection test against entire scene
     */
    bool Intersect(const Ray& ray, RayHit& hit) const;

    /**
     * @brief Check if any intersection exists
     */
    bool IntersectAny(const Ray& ray) const;

    /**
     * @brief Get build statistics
     */
    const BVHStats& GetStats() const { return m_stats; }

    size_t GetObjectCount() const { return m_objects.size(); }

private:
    int BuildRecursive(
        std::vector<BVHPrimitive>& primitives,
        int start,
        int end,
        int depth,
        const BVHBuildParams& params);

    void IntersectNode(
        int nodeIndex,
        const Ray& ray,
        RayHit& hit) const;

    bool IntersectNodeAny(
        int nodeIndex,
        const Ray& ray) const;

private:
    std::vector<ObjectEntry> m_objects;
    std::vector<BVHNode> m_nodes;
    std::vector<int> m_objectIndices;  // Reordered object indices
    BVHStats m_stats;
};

// ============================================================================
// Inline Implementations
// ============================================================================

inline void MeshBVH::Build(
    const std::vector<Vec3>& positions,
    const std::vector<uint32_t>& indices,
    const BVHBuildParams& params)
{
    m_positions = positions;
    m_indices = indices;
    m_nodes.clear();
    m_primitiveIndices.clear();
    m_stats = {};

    if (indices.empty() || positions.empty())
        return;

    int triangleCount = static_cast<int>(indices.size() / 3);
    std::vector<BVHPrimitive> primitives;
    primitives.reserve(triangleCount);

    // Build primitive list
    for (int i = 0; i < triangleCount; ++i)
    {
        uint32_t i0 = indices[i * 3 + 0];
        uint32_t i1 = indices[i * 3 + 1];
        uint32_t i2 = indices[i * 3 + 2];

        BoundingBox bounds;
        bounds.Expand(positions[i0]);
        bounds.Expand(positions[i1]);
        bounds.Expand(positions[i2]);

        primitives.emplace_back(i, bounds);
    }

    m_nodes.reserve(triangleCount * 2);
    BuildRecursive(primitives, 0, triangleCount, 0, params);

    // Build reordered primitive indices
    m_primitiveIndices.resize(triangleCount);
    for (int i = 0; i < triangleCount; ++i)
    {
        m_primitiveIndices[i] = primitives[i].index;
    }

    m_stats.nodeCount = static_cast<int>(m_nodes.size());
}

inline int MeshBVH::BuildRecursive(
    std::vector<BVHPrimitive>& primitives,
    int start,
    int end,
    int depth,
    const BVHBuildParams& params)
{
    int nodeIndex = static_cast<int>(m_nodes.size());
    m_nodes.emplace_back();

    // Compute bounds
    BoundingBox bounds;
    BoundingBox centroidBounds;
    for (int i = start; i < end; ++i)
    {
        bounds = bounds.Union(primitives[i].bounds);
        centroidBounds.Expand(primitives[i].centroid);
    }

    m_nodes[nodeIndex].bounds = bounds;
    int primitiveCount = end - start;

    m_stats.maxDepth = std::max(m_stats.maxDepth, depth);

    // Create leaf if few primitives
    if (primitiveCount <= params.maxPrimitivesPerLeaf)
    {
        m_nodes[nodeIndex].primitiveStart = start;
        m_nodes[nodeIndex].primitiveCount = primitiveCount;
        m_stats.leafCount++;
        m_stats.maxPrimitivesPerLeaf = std::max(m_stats.maxPrimitivesPerLeaf, primitiveCount);
        return nodeIndex;
    }

    // Find best split using SAH
    int bestAxis = 0;
    int bestBucket = 0;
    float bestCost = std::numeric_limits<float>::max();

    Vec3 extent = centroidBounds.GetExtent();

    for (int axis = 0; axis < 3; ++axis)
    {
        if (extent[axis] < 1e-6f) continue;

        // Initialize buckets
        std::vector<SAHBucket> buckets(params.sahBucketCount);

        for (int i = start; i < end; ++i)
        {
            float offset = (primitives[i].centroid[axis] - centroidBounds.GetMin()[axis]) / extent[axis];
            int bucketIdx = std::min(
                static_cast<int>(offset * params.sahBucketCount),
                params.sahBucketCount - 1);
            buckets[bucketIdx].count++;
            buckets[bucketIdx].bounds = buckets[bucketIdx].bounds.Union(primitives[i].bounds);
        }

        // Evaluate SAH cost for each split
        for (int split = 1; split < params.sahBucketCount; ++split)
        {
            BoundingBox leftBounds, rightBounds;
            int leftCount = 0, rightCount = 0;

            for (int i = 0; i < split; ++i)
            {
                leftBounds = leftBounds.Union(buckets[i].bounds);
                leftCount += buckets[i].count;
            }

            for (int i = split; i < params.sahBucketCount; ++i)
            {
                rightBounds = rightBounds.Union(buckets[i].bounds);
                rightCount += buckets[i].count;
            }

            if (leftCount == 0 || rightCount == 0) continue;

            float cost = params.traversalCost +
                (leftBounds.SurfaceArea() * leftCount +
                 rightBounds.SurfaceArea() * rightCount) *
                params.intersectionCost / bounds.SurfaceArea();

            if (cost < bestCost)
            {
                bestCost = cost;
                bestAxis = axis;
                bestBucket = split;
            }
        }
    }

    // Check if split is beneficial
    float leafCost = primitiveCount * params.intersectionCost;
    if (bestCost >= leafCost || extent[bestAxis] < 1e-6f)
    {
        m_nodes[nodeIndex].primitiveStart = start;
        m_nodes[nodeIndex].primitiveCount = primitiveCount;
        m_stats.leafCount++;
        m_stats.maxPrimitivesPerLeaf = std::max(m_stats.maxPrimitivesPerLeaf, primitiveCount);
        return nodeIndex;
    }

    // Partition primitives
    float splitPos = centroidBounds.GetMin()[bestAxis] +
        (extent[bestAxis] * bestBucket) / params.sahBucketCount;

    auto midIter = std::partition(
        primitives.begin() + start,
        primitives.begin() + end,
        [bestAxis, splitPos](const BVHPrimitive& p) {
            return p.centroid[bestAxis] < splitPos;
        });

    int mid = static_cast<int>(midIter - primitives.begin());

    // Ensure valid split
    if (mid == start || mid == end)
    {
        mid = (start + end) / 2;
        std::nth_element(
            primitives.begin() + start,
            primitives.begin() + mid,
            primitives.begin() + end,
            [bestAxis](const BVHPrimitive& a, const BVHPrimitive& b) {
                return a.centroid[bestAxis] < b.centroid[bestAxis];
            });
    }

    // Build children
    m_nodes[nodeIndex].leftChild = BuildRecursive(primitives, start, mid, depth + 1, params);
    m_nodes[nodeIndex].rightChild = BuildRecursive(primitives, mid, end, depth + 1, params);

    return nodeIndex;
}

inline bool MeshBVH::Intersect(const Ray& ray, RayHit& hit) const
{
    if (m_nodes.empty()) return false;

    float tMin, tMax;
    if (!RayAABBIntersect(ray, m_nodes[0].bounds, tMin, tMax))
        return false;

    RayHit localHit;
    localHit.Invalidate();
    IntersectNode(0, ray, localHit);

    if (localHit.IsValid())
    {
        hit = localHit;
        return true;
    }
    return false;
}

inline void MeshBVH::IntersectNode(int nodeIndex, const Ray& ray, RayHit& hit) const
{
    const BVHNode& node = m_nodes[nodeIndex];

    if (node.IsLeaf())
    {
        // Test all primitives in leaf
        for (int i = 0; i < node.primitiveCount; ++i)
        {
            int primIdx = m_primitiveIndices[node.primitiveStart + i];
            uint32_t i0 = m_indices[primIdx * 3 + 0];
            uint32_t i1 = m_indices[primIdx * 3 + 1];
            uint32_t i2 = m_indices[primIdx * 3 + 2];

            RayHit tempHit = hit;
            if (RayTriangleIntersect(ray, m_positions[i0], m_positions[i1], m_positions[i2], tempHit))
            {
                if (tempHit.t < hit.t)
                {
                    hit = tempHit;
                    hit.primitiveIndex = primIdx;
                }
            }
        }
        return;
    }

    // Check children
    float tMinL, tMaxL, tMinR, tMaxR;
    bool hitL = RayAABBIntersect(ray, m_nodes[node.leftChild].bounds, tMinL, tMaxL);
    bool hitR = RayAABBIntersect(ray, m_nodes[node.rightChild].bounds, tMinR, tMaxR);

    if (hitL && hitR)
    {
        // Visit closer child first
        if (tMinL < tMinR)
        {
            IntersectNode(node.leftChild, ray, hit);
            if (tMinR < hit.t)
                IntersectNode(node.rightChild, ray, hit);
        }
        else
        {
            IntersectNode(node.rightChild, ray, hit);
            if (tMinL < hit.t)
                IntersectNode(node.leftChild, ray, hit);
        }
    }
    else if (hitL)
    {
        IntersectNode(node.leftChild, ray, hit);
    }
    else if (hitR)
    {
        IntersectNode(node.rightChild, ray, hit);
    }
}

inline bool MeshBVH::IntersectAny(const Ray& ray) const
{
    if (m_nodes.empty()) return false;

    float tMin, tMax;
    if (!RayAABBIntersect(ray, m_nodes[0].bounds, tMin, tMax))
        return false;

    return IntersectNodeAny(0, ray);
}

inline bool MeshBVH::IntersectNodeAny(int nodeIndex, const Ray& ray) const
{
    const BVHNode& node = m_nodes[nodeIndex];

    if (node.IsLeaf())
    {
        for (int i = 0; i < node.primitiveCount; ++i)
        {
            int primIdx = m_primitiveIndices[node.primitiveStart + i];
            uint32_t i0 = m_indices[primIdx * 3 + 0];
            uint32_t i1 = m_indices[primIdx * 3 + 1];
            uint32_t i2 = m_indices[primIdx * 3 + 2];

            float t, u, v;
            if (RayTriangleIntersect(ray, m_positions[i0], m_positions[i1], m_positions[i2], t, u, v))
                return true;
        }
        return false;
    }

    float tMin, tMax;
    if (RayAABBIntersect(ray, m_nodes[node.leftChild].bounds, tMin, tMax))
    {
        if (IntersectNodeAny(node.leftChild, ray))
            return true;
    }

    if (RayAABBIntersect(ray, m_nodes[node.rightChild].bounds, tMin, tMax))
    {
        if (IntersectNodeAny(node.rightChild, ray))
            return true;
    }

    return false;
}

// ============================================================================
// SceneBVH Implementation
// ============================================================================

inline void SceneBVH::AddObject(const ObjectEntry& entry)
{
    m_objects.push_back(entry);
}

inline void SceneBVH::Clear()
{
    m_objects.clear();
    m_nodes.clear();
    m_objectIndices.clear();
    m_stats = {};
}

inline void SceneBVH::Build(const BVHBuildParams& params)
{
    m_nodes.clear();
    m_objectIndices.clear();
    m_stats = {};

    if (m_objects.empty()) return;

    int objectCount = static_cast<int>(m_objects.size());
    std::vector<BVHPrimitive> primitives;
    primitives.reserve(objectCount);

    for (int i = 0; i < objectCount; ++i)
    {
        primitives.emplace_back(i, m_objects[i].worldBounds);
    }

    m_nodes.reserve(objectCount * 2);
    BuildRecursive(primitives, 0, objectCount, 0, params);

    m_objectIndices.resize(objectCount);
    for (int i = 0; i < objectCount; ++i)
    {
        m_objectIndices[i] = primitives[i].index;
    }

    m_stats.nodeCount = static_cast<int>(m_nodes.size());
}

inline int SceneBVH::BuildRecursive(
    std::vector<BVHPrimitive>& primitives,
    int start,
    int end,
    int depth,
    const BVHBuildParams& params)
{
    int nodeIndex = static_cast<int>(m_nodes.size());
    m_nodes.emplace_back();

    BoundingBox bounds;
    BoundingBox centroidBounds;
    for (int i = start; i < end; ++i)
    {
        bounds = bounds.Union(primitives[i].bounds);
        centroidBounds.Expand(primitives[i].centroid);
    }

    m_nodes[nodeIndex].bounds = bounds;
    int objectCount = end - start;

    m_stats.maxDepth = std::max(m_stats.maxDepth, depth);

    if (objectCount <= params.maxPrimitivesPerLeaf)
    {
        m_nodes[nodeIndex].primitiveStart = start;
        m_nodes[nodeIndex].primitiveCount = objectCount;
        m_stats.leafCount++;
        return nodeIndex;
    }

    // Find best axis using simple median split
    Vec3 extent = centroidBounds.GetExtent();
    int axis = 0;
    if (extent.y > extent.x) axis = 1;
    if (extent.z > extent[axis]) axis = 2;

    int mid = (start + end) / 2;
    std::nth_element(
        primitives.begin() + start,
        primitives.begin() + mid,
        primitives.begin() + end,
        [axis](const BVHPrimitive& a, const BVHPrimitive& b) {
            return a.centroid[axis] < b.centroid[axis];
        });

    m_nodes[nodeIndex].leftChild = BuildRecursive(primitives, start, mid, depth + 1, params);
    m_nodes[nodeIndex].rightChild = BuildRecursive(primitives, mid, end, depth + 1, params);

    return nodeIndex;
}

inline bool SceneBVH::Intersect(const Ray& ray, RayHit& hit) const
{
    if (m_nodes.empty()) return false;

    float tMin, tMax;
    if (!RayAABBIntersect(ray, m_nodes[0].bounds, tMin, tMax))
        return false;

    hit.Invalidate();
    IntersectNode(0, ray, hit);
    return hit.IsValid();
}

inline void SceneBVH::IntersectNode(int nodeIndex, const Ray& ray, RayHit& hit) const
{
    const BVHNode& node = m_nodes[nodeIndex];

    if (node.IsLeaf())
    {
        for (int i = 0; i < node.primitiveCount; ++i)
        {
            int objIdx = m_objectIndices[node.primitiveStart + i];
            const ObjectEntry& obj = m_objects[objIdx];

            if (!obj.meshBVH) continue;

            // Transform ray to object space
            Ray localRay = ray.Transform(obj.inverseTransform);

            RayHit localHit;
            localHit.Invalidate();

            if (obj.meshBVH->Intersect(localRay, localHit))
            {
                // Transform hit back to world space
                Vec3 worldPos = Vec3(obj.worldTransform * Vec4(localHit.position, 1.0f));
                float worldT = length(worldPos - ray.origin);

                if (worldT < hit.t)
                {
                    hit = localHit;
                    hit.t = worldT;
                    hit.position = worldPos;
                    hit.normal = normalize(Vec3(
                        transpose(obj.inverseTransform) * Vec4(localHit.normal, 0.0f)));
                    hit.nodeIndex = obj.nodeIndex;
                    hit.meshIndex = obj.meshIndex;
                }
            }
        }
        return;
    }

    float tMinL, tMaxL, tMinR, tMaxR;
    bool hitL = RayAABBIntersect(ray, m_nodes[node.leftChild].bounds, tMinL, tMaxL);
    bool hitR = RayAABBIntersect(ray, m_nodes[node.rightChild].bounds, tMinR, tMaxR);

    if (hitL && hitR)
    {
        if (tMinL < tMinR)
        {
            IntersectNode(node.leftChild, ray, hit);
            if (tMinR < hit.t)
                IntersectNode(node.rightChild, ray, hit);
        }
        else
        {
            IntersectNode(node.rightChild, ray, hit);
            if (tMinL < hit.t)
                IntersectNode(node.leftChild, ray, hit);
        }
    }
    else if (hitL)
    {
        IntersectNode(node.leftChild, ray, hit);
    }
    else if (hitR)
    {
        IntersectNode(node.rightChild, ray, hit);
    }
}

inline bool SceneBVH::IntersectAny(const Ray& ray) const
{
    if (m_nodes.empty()) return false;

    float tMin, tMax;
    if (!RayAABBIntersect(ray, m_nodes[0].bounds, tMin, tMax))
        return false;

    return IntersectNodeAny(0, ray);
}

inline bool SceneBVH::IntersectNodeAny(int nodeIndex, const Ray& ray) const
{
    const BVHNode& node = m_nodes[nodeIndex];

    if (node.IsLeaf())
    {
        for (int i = 0; i < node.primitiveCount; ++i)
        {
            int objIdx = m_objectIndices[node.primitiveStart + i];
            const ObjectEntry& obj = m_objects[objIdx];

            if (!obj.meshBVH) continue;

            Ray localRay = ray.Transform(obj.inverseTransform);
            if (obj.meshBVH->IntersectAny(localRay))
                return true;
        }
        return false;
    }

    float tMin, tMax;
    if (RayAABBIntersect(ray, m_nodes[node.leftChild].bounds, tMin, tMax))
    {
        if (IntersectNodeAny(node.leftChild, ray))
            return true;
    }

    if (RayAABBIntersect(ray, m_nodes[node.rightChild].bounds, tMin, tMax))
    {
        if (IntersectNodeAny(node.rightChild, ray))
            return true;
    }

    return false;
}

} // namespace RVX
