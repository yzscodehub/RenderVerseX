#pragma once

/**
 * @file MeshResource.h
 * @brief Mesh resource type
 */

#include "Core/Math/AABB.h"
#include "Geometry/Asset/AssetMetadata.h"
#include "Geometry/Asset/Mesh.h"
#include "Resource/IResource.h"
#include "Resource/ResourceHandle.h"

#include <memory>
#include <vector>

namespace RVX::Resource
{
    /**
     * @brief Mesh resource - encapsulates Mesh data with resource lifecycle
     */
    class MeshResource : public IResource,
                         public IMeshAssetMetadata
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

        // =====================================================================
        // Mesh Data
        // =====================================================================

        std::shared_ptr<Mesh> GetMesh() const { return m_mesh; }
        void SetMesh(std::shared_ptr<Mesh> mesh);

        void SetLODMeshes(std::vector<std::shared_ptr<Mesh>> lodMeshes);
        size_t GetLODCount() const;
        std::shared_ptr<Mesh> GetLODMesh(size_t lodIndex) const;
        const std::vector<std::shared_ptr<Mesh>>& GetLODMeshes() const { return m_lodMeshes; }

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
