#pragma once

/**
 * @file ModelResource.h
 * @brief Model resource type - contains mesh hierarchy, materials, and skeleton
 * 
 * ModelResource represents a complete 3D model loaded from formats like
 * glTF, FBX, OBJ, etc. It stores the original node hierarchy as a template
 * that can be instantiated into the scene.
 */

#include "Resource/IResource.h"
#include "Resource/ResourceHandle.h"
#include "Resource/Types/MeshResource.h"
#include "Resource/Types/MaterialResource.h"
#include "Scene/Node.h"
#include <memory>
#include <vector>
#include <string>

namespace RVX
{
    // Forward declarations
    class SceneManager;
    class SceneEntity;
    class Skeleton;
}

namespace RVX::Resource
{
    /**
     * @brief Model resource - complete 3D model with hierarchy
     * 
     * ModelResource contains:
     * - Node tree (hierarchy template)
     * - Referenced MeshResource list
     * - Referenced MaterialResource list
     * - Optional Skeleton for skeletal animation
     * 
     * Usage:
     * @code
     * auto model = resourceManager.Load<ModelResource>("models/helmet.gltf");
     * 
     * // Instantiate to scene (creates SceneEntity tree)
     * auto* entity = model->Instantiate(world->GetSceneManager());
     * entity->SetPosition(Vec3(0, 0, 0));
     * @endcode
     */
    class ModelResource : public IResource
    {
    public:
        ModelResource();
        ~ModelResource() override;

        // =====================================================================
        // Resource Interface
        // =====================================================================

        ResourceType GetType() const override { return ResourceType::Model; }
        const char* GetTypeName() const override { return "Model"; }
        size_t GetMemoryUsage() const override;
        size_t GetGPUMemoryUsage() const override;

        std::vector<ResourceId> GetRequiredDependencies() const override;

        // =====================================================================
        // Node Tree (Hierarchy Template)
        // =====================================================================

        /// Get the root node of the model hierarchy
        Node::Ptr GetRootNode() const { return m_rootNode; }

        /// Set the root node
        void SetRootNode(Node::Ptr root) { m_rootNode = std::move(root); }

        /// Get node count (total in hierarchy)
        size_t GetNodeCount() const;

        // =====================================================================
        // Mesh Resources
        // =====================================================================

        /// Get all mesh resources
        const std::vector<ResourceHandle<MeshResource>>& GetMeshes() const { return m_meshes; }

        /// Get mesh by index
        ResourceHandle<MeshResource> GetMesh(size_t index) const;

        /// Get mesh count
        size_t GetMeshCount() const { return m_meshes.size(); }

        /// Add a mesh resource
        void AddMesh(ResourceHandle<MeshResource> mesh);

        /// Set all mesh resources
        void SetMeshes(std::vector<ResourceHandle<MeshResource>> meshes);

        // =====================================================================
        // Material Resources
        // =====================================================================

        /// Get all material resources
        const std::vector<ResourceHandle<MaterialResource>>& GetMaterials() const { return m_materials; }

        /// Get material by index
        ResourceHandle<MaterialResource> GetMaterial(size_t index) const;

        /// Get material count
        size_t GetMaterialCount() const { return m_materials.size(); }

        /// Add a material resource
        void AddMaterial(ResourceHandle<MaterialResource> material);

        /// Set all material resources
        void SetMaterials(std::vector<ResourceHandle<MaterialResource>> materials);

        // =====================================================================
        // Skeleton (Optional)
        // =====================================================================

        /// Get skeleton (may be null)
        std::shared_ptr<Skeleton> GetSkeleton() const { return m_skeleton; }

        /// Set skeleton
        void SetSkeleton(std::shared_ptr<Skeleton> skeleton) { m_skeleton = std::move(skeleton); }

        /// Check if model has skeleton
        bool HasSkeleton() const { return m_skeleton != nullptr; }

        // =====================================================================
        // Instantiation
        // =====================================================================

        /// Instantiate the model into the scene
        /// Creates a SceneEntity tree with MeshRendererComponents attached
        SceneEntity* Instantiate(SceneManager* scene) const;

    private:
        /// Recursive helper for instantiation
        SceneEntity* InstantiateNode(const Node* node, SceneManager* scene, SceneEntity* parent) const;

        /// Count nodes recursively
        size_t CountNodes(const Node* node) const;

        Node::Ptr m_rootNode;
        std::vector<ResourceHandle<MeshResource>> m_meshes;
        std::vector<ResourceHandle<MaterialResource>> m_materials;
        std::shared_ptr<Skeleton> m_skeleton;
    };

} // namespace RVX::Resource

namespace RVX
{
    using Resource::ModelResource;
}
