#pragma once

/**
 * @file MeshResource.h
 * @brief Mesh resource type
 */

#include "Core/Math/AABB.h"
#include "Geometry/Asset/AssetMetadata.h"
#include "Geometry/Asset/Mesh.h"
#include "RenderContracts/RenderResource.h"
#include "Resource/IResource.h"
#include "Resource/ResourceHandle.h"

#include <memory>
#include <vector>

namespace RVX::Resource
{
    using MeshAttributeUploadView = RenderMeshAttributeUploadView;
    using MeshSubmeshUploadInfo = RenderMeshSubmeshUploadInfo;
    using MeshUploadData = RenderMeshUploadData;

    /**
     * @brief Mesh resource - encapsulates Mesh data with resource lifecycle
     */
    class MeshResource : public IResource,
                         public IMeshAssetMetadata,
                         public IRenderMeshUploadSource
    {
    public:
        MeshResource();
        ~MeshResource() override;

        // =====================================================================
        // Resource Interface
        // =====================================================================

        ResourceType GetType() const override { return ResourceType::Mesh; }
        const char* GetTypeName() const override { return "Mesh"; }
        size_t GetMemoryUsage() const override;
        size_t GetGPUMemoryUsage() const override;

        AABB GetAssetMeshBounds() const override { return GetBounds(); }
        size_t GetAssetMeshSubmeshCount() const override;

        uint64 GetRenderResourceId() const override { return GetId(); }
        std::string_view GetRenderResourceName() const override { return GetName(); }
        uint32 GetRenderResourceRefCount() const override { return GetRefCount(); }
        RefCounted* GetRenderResourceRefCounted() override { return this; }
        RenderMeshUploadData GetRenderMeshUploadData() const override { return GetUploadData(); }
        AABB GetRenderMeshBounds() const override { return GetBounds(); }
        size_t GetRenderMeshSubmeshCount() const override;

        // =====================================================================
        // Mesh Data
        // =====================================================================

        std::shared_ptr<Mesh> GetMesh() const { return m_mesh; }
        void SetMesh(std::shared_ptr<Mesh> mesh);

        void SetLODMeshes(std::vector<std::shared_ptr<Mesh>> lodMeshes);
        size_t GetLODCount() const;
        std::shared_ptr<Mesh> GetLODMesh(size_t lodIndex) const;
        const std::vector<std::shared_ptr<Mesh>>& GetLODMeshes() const { return m_lodMeshes; }

        MeshUploadData GetUploadData() const;

        // =====================================================================
        // Bounds
        // =====================================================================

        const AABB& GetBounds() const { return m_bounds; }
        void SetBounds(const AABB& bounds) { m_bounds = bounds; }

    private:
        std::shared_ptr<Mesh> m_mesh;
        std::vector<std::shared_ptr<Mesh>> m_lodMeshes;
        AABB m_bounds;
    };

} // namespace RVX::Resource
