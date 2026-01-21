/**
 * @file PickingSystem.h
 * @brief Mouse picking system for selecting scene objects
 * 
 * Provides ray-based object selection using BVH acceleration.
 */

#pragma once

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
class MeshBVH;
class SceneBVH;
struct BVHStats;

/**
 * @brief Picking result with detailed hit information
 */
struct PickResult
{
    bool hit{false};
    RayHit rayHit;

    // Convenience accessors
    bool HasHit() const { return hit && rayHit.IsValid(); }
    float GetDistance() const { return rayHit.t; }
    Vec3 GetPosition() const { return rayHit.position; }
    Vec3 GetNormal() const { return rayHit.normal; }
    int GetNodeIndex() const { return rayHit.nodeIndex; }
    int GetMeshIndex() const { return rayHit.meshIndex; }
    int GetTriangleIndex() const { return rayHit.primitiveIndex; }
};

/**
 * @brief Picking configuration
 */
struct PickingConfig
{
    bool pickClosest{true};         // Find closest hit or first hit
    bool cullBackfaces{false};      // Cull backfacing triangles
    float maxDistance{10000.0f};    // Maximum picking distance
};

/**
 * @brief Mouse picking system
 * 
 * Usage:
 * @code
 * PickingSystem picker;
 * 
 * // Build acceleration structure from scene
 * picker.BuildFromScene(rootNode);
 * 
 * // Pick on mouse click
 * Ray ray = picker.ScreenToRay(camera, mouseX, mouseY, screenWidth, screenHeight);
 * PickResult result = picker.Pick(ray);
 * 
 * if (result.HasHit()) {
 *     int nodeIndex = result.GetNodeIndex();
 *     // Handle selection...
 * }
 * @endcode
 */
class PickingSystem
{
public:
    PickingSystem();
    ~PickingSystem();

    /**
     * @brief Convert screen coordinates to world-space ray
     */
    static Ray ScreenToRay(
        const Camera& camera,
        float screenX,
        float screenY,
        float screenWidth,
        float screenHeight);

    /**
     * @brief Convert normalized device coordinates to ray
     */
    static Ray NDCToRay(
        const Camera& camera,
        float ndcX,
        float ndcY);

    /**
     * @brief Add a pickable mesh with its transform
     */
    void AddMesh(
        int nodeIndex,
        int meshIndex,
        const std::shared_ptr<Mesh>& mesh,
        const Mat4& worldTransform);

    /**
     * @brief Add mesh with position data directly
     */
    void AddMesh(
        int nodeIndex,
        int meshIndex,
        const std::vector<Vec3>& positions,
        const std::vector<uint32_t>& indices,
        const Mat4& worldTransform);

    /**
     * @brief Build acceleration structure
     */
    void Build();

    /**
     * @brief Rebuild acceleration structure
     */
    void Rebuild();

    /**
     * @brief Clear all pickable objects
     */
    void Clear();

    /**
     * @brief Pick with a ray
     */
    PickResult Pick(const Ray& ray, const PickingConfig& config = {}) const;

    /**
     * @brief Pick from screen coordinates
     */
    PickResult PickScreen(
        const Camera& camera,
        float screenX,
        float screenY,
        float screenWidth,
        float screenHeight,
        const PickingConfig& config = {}) const;

    /**
     * @brief Check if any object is hit (shadow-ray style)
     */
    bool IsOccluded(const Ray& ray) const;

    /**
     * @brief Get number of pickable objects
     */
    size_t GetObjectCount() const;

    /**
     * @brief Check if built
     */
    bool IsBuilt() const { return m_isBuilt; }

    /**
     * @brief Get build statistics
     */
    const BVHStats& GetStats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    bool m_isBuilt{false};
};

} // namespace RVX
