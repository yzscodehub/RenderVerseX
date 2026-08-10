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
#include "Core/Diagnostics/Trace.h"
#include "Geometry/Asset/Node.h"
#include <memory>
#include <mutex>
#include <vector>
#include <string>

namespace RVX
{
    // Forward declarations
    class Actor;
    class Scene;
    class SceneManager;
    class SceneEntity;
    class Skeleton;
}

namespace RVX::Resource
{
    enum class ModelTextureStreamingStage : uint8
    {
        None = 0,
        AwaitingMinimumResident,
        Decoding,
        Uploading,
        FullyResident,
        Failed,
        Cancelled
    };

    struct ModelTextureStreamingSource
    {
        ResourceHandle<TextureResource> texture;
        TextureReference reference;
        std::string sourceModelPath;
        std::string resourceIdentityBase;
        Diagnostics::TraceContext traceContext;
        uint64 estimatedDecodedBytes = 0;
        std::string preflightError;
    };

    struct ModelTextureStreamingSnapshot
    {
        ModelTextureStreamingStage stage = ModelTextureStreamingStage::None;
        uint32 textureCount = 0;
        uint32 decodedTextureCount = 0;
        uint32 residentTextureCount = 0;
        uint64 decodedBytes = 0;
        std::string error;

        [[nodiscard]] bool HasStreamingTextures() const noexcept
        {
            return textureCount != 0;
        }
    };

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
     * auto* entity = model->Instantiate(world->GetScene());
     * entity->SetPosition(Vec3(0, 0, 0));
     * @endcode
     */
    class ModelResource : public IResource
    {
    public:
        static constexpr ResourceType StaticResourceType = ResourceType::Model;

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
        std::vector<ResourceId> GetOptionalDependencies() const override;

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
        // Progressive Texture Residency
        // =====================================================================

        void SetTextureStreamingSources(
            std::vector<ModelTextureStreamingSource> sources);
        [[nodiscard]] std::vector<ModelTextureStreamingSource>
            BeginTextureStreaming();
        void RebindTextureStreamingDependency(
            ResourceId resourceId,
            ResourceHandle<TextureResource> canonical);
        void MarkTextureDecodeComplete(uint64 decodedBytes);
        void MarkTexturePublicationComplete(ResourceId textureId);
        void MarkTextureStreamingFailed(std::string error);
        void CancelTextureStreaming() noexcept;
        [[nodiscard]] ModelTextureStreamingSnapshot
            GetTextureStreamingSnapshot() const;
        [[nodiscard]] std::vector<ResourceHandle<TextureResource>>
            GetStreamingTextures() const;

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
        /// Creates a SceneEntity tree with StaticMeshComponents attached
        SceneEntity* Instantiate(Scene* scene) const;

        /// Compatibility overload for the legacy SceneManager facade.
        SceneEntity* Instantiate(SceneManager* scene) const;

        /// Instantiate the model into the scene and return the actor view.
        /// Compatibility implementation currently creates SceneEntity instances.
        Actor* InstantiateActor(Scene* scene) const;

        /// Compatibility overload for the legacy SceneManager facade.
        Actor* InstantiateActor(SceneManager* scene) const;

    private:
        /// Recursive helper for instantiation
        SceneEntity* InstantiateActorNode(const Node* node,
                                          Scene* scene,
                                          SceneEntity* parent) const;
        SceneEntity* InstantiateActorNode(const Node* node, SceneManager* scene, SceneEntity* parent) const;

        /// Count nodes recursively
        size_t CountNodes(const Node* node) const;

        Node::Ptr m_rootNode;
        std::vector<ResourceHandle<MeshResource>> m_meshes;
        std::vector<ResourceHandle<MaterialResource>> m_materials;
        std::shared_ptr<Skeleton> m_skeleton;
        mutable std::mutex m_textureStreamingMutex;
        std::vector<ModelTextureStreamingSource> m_textureStreamingSources;
        std::vector<ResourceHandle<TextureResource>> m_streamingTextures;
        ModelTextureStreamingStage m_textureStreamingStage =
            ModelTextureStreamingStage::None;
        uint32 m_decodedTextureCount = 0;
        uint32 m_residentTextureCount = 0;
        uint64 m_decodedTextureBytes = 0;
        std::string m_textureStreamingError;
    };

} // namespace RVX::Resource

namespace RVX
{
    using Resource::ModelResource;
}
