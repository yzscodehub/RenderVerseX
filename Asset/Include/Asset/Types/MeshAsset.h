#pragma once

/**
 * @file MeshAsset.h
 * @brief Mesh asset type
 */

#include "Asset/Asset.h"
#include "Asset/AssetHandle.h"
#include "Scene/Mesh.h"
#include "Core/Math/AABB.h"
#include <memory>

namespace RVX::Asset
{
    /**
     * @brief Mesh asset - encapsulates Scene::Mesh with resource lifecycle
     */
    class MeshAsset : public Asset
    {
    public:
        MeshAsset();
        ~MeshAsset() override;

        // =====================================================================
        // Asset Interface
        // =====================================================================

        AssetType GetType() const override { return AssetType::Mesh; }
        const char* GetTypeName() const override { return "Mesh"; }
        size_t GetMemoryUsage() const override;
        size_t GetGPUMemoryUsage() const override;

        // =====================================================================
        // Mesh Data
        // =====================================================================

        std::shared_ptr<Mesh> GetMesh() const { return m_mesh; }
        void SetMesh(std::shared_ptr<Mesh> mesh);

        // =====================================================================
        // Bounds
        // =====================================================================

        const AABB& GetBounds() const { return m_bounds; }
        void SetBounds(const AABB& bounds) { m_bounds = bounds; }

        // =====================================================================
        // GPU Resources (future)
        // =====================================================================

        // RHI::BufferHandle GetVertexBuffer();
        // RHI::BufferHandle GetIndexBuffer();
        // void UploadToGPU(RHI::Device* device);
        // void ReleaseGPUResources();
        // bool IsGPUResident() const;

    private:
        std::shared_ptr<Mesh> m_mesh;
        AABB m_bounds;

        // GPU resources (future)
        // RHI::BufferHandle m_vertexBuffer;
        // RHI::BufferHandle m_indexBuffer;
        // bool m_gpuResident = false;
    };

} // namespace RVX::Asset
