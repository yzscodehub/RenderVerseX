#pragma once

/**
 * @file RenderScene.h
 * @brief Render scene - snapshot of scene data for rendering
 */

#include "Core/Math/AABB.h"
#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "RenderContracts/RenderMaterial.h"
#include "RenderContracts/RenderProxy.h"
#include "RenderContracts/RenderResource.h"

#include <vector>

namespace RVX
{
    class Camera;
    class SceneManager;
    class World;

    /**
     * @brief Renderable object data
     */
    struct RenderObject
    {
        Mat4 worldMatrix = Mat4Identity();
        Mat4 previousWorldMatrix = Mat4Identity();
        uint8 previousWorldMatrixValid = 0;
        Mat4 normalMatrix = Mat4Identity();
        AABB bounds;

        uint64_t meshId = 0;
        IRenderMeshUploadSource* meshResource = nullptr;

        std::vector<uint64_t> materialIds;
        std::vector<RenderMaterialMode> materialModes;
        std::vector<IRenderMaterialSource*> materialResources;
        std::vector<Mat4> skinningMatrices;

        uint64_t entityId = 0;
        uint64_t sortKey = 0;
        uint32_t layerMask = ~0u;

        bool visible = true;
        bool castsShadow = true;
        bool receivesShadow = true;

        bool HasSkinningData() const { return !skinningMatrices.empty(); }
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
        float range = 10.0f;
        float innerConeAngle = 0.0f;
        float outerConeAngle = 0.7854f;
        bool castsShadow = false;
    };

    /**
     * @brief Render scene - contains all renderable objects for a frame
     */
    class RenderScene
    {
    public:
        RenderScene() = default;

        void Clear();
        void CollectFromWorld(World* world);
        void CollectFromSceneManager(SceneManager* sceneManager);
        void ApplyProxySnapshot(const RenderProxySnapshot& snapshot);

        void CullAgainstCamera(const Camera& camera, std::vector<uint32_t>& outVisibleIndices) const;
        void SortVisibleObjects(std::vector<uint32_t>& visibleIndices, const Vec3& cameraPosition) const;

        const std::vector<RenderObject>& GetObjects() const { return m_objects; }
        const std::vector<RenderLight>& GetLights() const { return m_lights; }
        const RenderProxySnapshotMetadata& GetSourceSnapshotMetadata() const { return m_sourceSnapshotMetadata; }

        void AddObject(const RenderObject& obj) { m_objects.push_back(obj); }
        void AddLight(const RenderLight& light) { m_lights.push_back(light); }

        size_t GetObjectCount() const { return m_objects.size(); }
        size_t GetLightCount() const { return m_lights.size(); }

        const RenderObject& GetObject(size_t index) const { return m_objects[index]; }
        RenderObject& GetMutableObject(size_t index) { return m_objects[index]; }
        const RenderLight& GetLight(size_t index) const { return m_lights[index]; }

    private:
        std::vector<RenderObject> m_objects;
        std::vector<RenderLight> m_lights;
        RenderProxySnapshotMetadata m_sourceSnapshotMetadata;
    };

} // namespace RVX
