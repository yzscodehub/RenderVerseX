#pragma once

/**
 * @file StaticMeshComponent.h
 * @brief UE-style primitive component for rendering static meshes
 */

#include "Resource/ResourceHandle.h"
#include "Resource/Types/MaterialResource.h"
#include "Resource/Types/MeshResource.h"
#include "Scene/PrimitiveComponent.h"

#include <vector>

namespace RVX
{
    class RenderScene;

    /**
     * @brief Renderable primitive component for static mesh instances.
     */
    class StaticMeshComponent : public PrimitiveComponent
    {
    public:
        // =====================================================================
        // Type Information
        // =====================================================================

        const char* GetClassName() const override { return "StaticMeshComponent"; }

        // =====================================================================
        // Mesh
        // =====================================================================

        void SetMesh(Resource::ResourceHandle<Resource::MeshResource> mesh);
        Resource::ResourceHandle<Resource::MeshResource> GetMesh() const { return m_mesh; }
        bool HasValidMesh() const;

        // =====================================================================
        // Materials
        // =====================================================================

        void SetMaterial(size_t submeshIndex, Resource::ResourceHandle<Resource::MaterialResource> material);
        Resource::ResourceHandle<Resource::MaterialResource> GetMaterial(size_t submeshIndex) const;
        size_t GetSubmeshCount() const;
        size_t GetMaterialOverrideCount() const { return m_materialOverrides.size(); }
        void ClearMaterialOverrides() { m_materialOverrides.clear(); }

        // =====================================================================
        // Rendering Properties
        // =====================================================================

        bool CastsShadow() const { return m_castsShadow; }
        void SetCastsShadow(bool castsShadow) { m_castsShadow = castsShadow; }

        bool ReceivesShadow() const { return m_receivesShadow; }
        void SetReceivesShadow(bool receivesShadow) { m_receivesShadow = receivesShadow; }

        // =====================================================================
        // Render Extraction
        // =====================================================================

        bool HasRenderData() const override;
        void CollectRenderData(RenderScene& scene) const override;

    private:
        Resource::ResourceHandle<Resource::MeshResource> m_mesh;
        std::vector<Resource::ResourceHandle<Resource::MaterialResource>> m_materialOverrides;
        bool m_castsShadow = true;
        bool m_receivesShadow = true;
    };

} // namespace RVX
