#pragma once

/**
 * @file RenderScene.h
 * @brief Render scene - snapshot of scene data for rendering
 */

#include "Core/Types.h"
#include "Core/MathTypes.h"
#include "Core/Math/AABB.h"
#include <vector>

namespace RVX
{
    class World;
    class Camera;

    /**
     * @brief Renderable object data
     */
    struct RenderObject
    {
        /// World transform matrix
        Mat4 worldMatrix = Mat4Identity();
        
        /// Inverse transpose for normal transformation
        Mat4 normalMatrix = Mat4Identity();
        
        /// World-space bounding box
        AABB bounds;
        
        /// Mesh resource ID (0 = invalid)
        uint64_t meshId = 0;
        
        /// Material resource IDs (one per submesh)
        std::vector<uint64_t> materialIds;
        
        /// Entity handle for picking/identification
        uint64_t entityId = 0;
        
        /// Sort key for batching (usually first material ID)
        uint64_t sortKey = 0;
        
        /// Visibility flags
        bool visible = true;
        bool castsShadow = true;
        bool receivesShadow = true;
    };

    /**
     * @brief Light data for rendering
     */
    struct RenderLight
    {
        enum class Type : uint8_t
        {
            Directional,
            Point,
            Spot
        };

        Type type = Type::Directional;
        Vec3 position{0.0f, 0.0f, 0.0f};
        Vec3 direction{0.0f, 0.0f, -1.0f};
        Vec3 color{1.0f, 1.0f, 1.0f};
        float intensity = 1.0f;
        float range = 10.0f;           // For point/spot
        float innerConeAngle = 0.0f;   // For spot
        float outerConeAngle = 0.7854f; // For spot (~45 degrees)
        bool castsShadow = false;
    };

    /**
     * @brief Render scene - contains all renderable objects for a frame
     * 
     * RenderScene is a snapshot of the game world's renderable state.
     * It is collected from the World/Scene each frame and passed to
     * the SceneRenderer for rendering.
     * 
     * This separation allows:
     * - Thread-safe rendering (scene snapshot is immutable)
     * - Multiple views of the same scene
     * - Efficient culling and sorting
     */
    class RenderScene
    {
    public:
        RenderScene() = default;

        /**
         * @brief Clear the scene for the next frame
         */
        void Clear();

        /**
         * @brief Collect renderable objects from a World
         * @param world The world to collect from
         */
        void CollectFromWorld(World* world);

        /**
         * @brief Perform view frustum culling
         * @param camera The camera to cull against
         * @param outVisibleIndices Output: indices of visible objects
         */
        void CullAgainstCamera(const Camera& camera, std::vector<uint32_t>& outVisibleIndices) const;

        /**
         * @brief Sort visible objects for optimal rendering
         * @param visibleIndices The indices to sort
         * @param cameraPosition Camera position for depth sorting
         */
        void SortVisibleObjects(std::vector<uint32_t>& visibleIndices, const Vec3& cameraPosition) const;

        // =====================================================================
        // Accessors
        // =====================================================================

        const std::vector<RenderObject>& GetObjects() const { return m_objects; }
        const std::vector<RenderLight>& GetLights() const { return m_lights; }

        void AddObject(const RenderObject& obj) { m_objects.push_back(obj); }
        void AddLight(const RenderLight& light) { m_lights.push_back(light); }

        size_t GetObjectCount() const { return m_objects.size(); }
        size_t GetLightCount() const { return m_lights.size(); }

        const RenderObject& GetObject(size_t index) const { return m_objects[index]; }
        const RenderLight& GetLight(size_t index) const { return m_lights[index]; }

    private:
        /// Recursive helper for collecting from entity hierarchy
        void CollectFromEntity(class SceneEntity* entity, const Mat4& parentMatrix);

        std::vector<RenderObject> m_objects;
        std::vector<RenderLight> m_lights;
    };

} // namespace RVX
