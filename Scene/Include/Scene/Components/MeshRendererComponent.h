#pragma once

/**
 * @file MeshRendererComponent.h
 * @brief Component for rendering meshes
 * 
 * MeshRendererComponent holds a reference to a MeshResource and
 * optional material overrides for each submesh.
 */

#include "Scene/Component.h"
#include "Resource/ResourceHandle.h"
#include "Resource/Types/MeshResource.h"
#include "Resource/Types/MaterialResource.h"
#include <vector>

namespace RVX
{
    // Forward declarations
    class RenderScene;

    /**
     * @brief Component for rendering meshes
     * 
     * Features:
     * - References a MeshResource
     * - Supports per-submesh material overrides
     * - Provides bounds for spatial indexing
     * - Collects render data for the rendering pipeline
     * 
     * Usage:
     * @code
     * auto* entity = scene->CreateEntity("Helmet");
     * auto* renderer = entity->AddComponent<MeshRendererComponent>();
     * renderer->SetMesh(meshHandle);
     * renderer->SetMaterial(0, glassMaterial);  // Override submesh 0
     * @endcode
     */
    class MeshRendererComponent : public Component
    {
    public:
        MeshRendererComponent() = default;
        ~MeshRendererComponent() override = default;

        // =====================================================================
        // Component Interface
        // =====================================================================

        const char* GetTypeName() const override { return "MeshRenderer"; }

        void OnAttach() override;
        void OnDetach() override;

        // =====================================================================
        // Spatial Bounds
        // =====================================================================

        bool ProvidesBounds() const override { return true; }
        AABB GetLocalBounds() const override;

        // =====================================================================
        // Mesh
        // =====================================================================

        /// Set the mesh to render
        void SetMesh(Resource::ResourceHandle<Resource::MeshResource> mesh);

        /// Get the current mesh
        Resource::ResourceHandle<Resource::MeshResource> GetMesh() const { return m_mesh; }

        /// Check if mesh is valid and loaded
        bool HasValidMesh() const { return m_mesh.IsValid() && m_mesh.IsLoaded(); }

        // =====================================================================
        // Materials (per-submesh overrides)
        // =====================================================================

        /// Set material override for a specific submesh
        void SetMaterial(size_t submeshIndex, Resource::ResourceHandle<Resource::MaterialResource> material);

        /// Get material for a submesh (returns override or default)
        Resource::ResourceHandle<Resource::MaterialResource> GetMaterial(size_t submeshIndex) const;

        /// Get number of submeshes (from mesh)
        size_t GetSubmeshCount() const;

        /// Get number of material overrides set
        size_t GetMaterialOverrideCount() const { return m_materialOverrides.size(); }

        /// Clear all material overrides
        void ClearMaterialOverrides() { m_materialOverrides.clear(); }

        // =====================================================================
        // Rendering Properties
        // =====================================================================

        bool IsVisible() const { return m_visible; }
        void SetVisible(bool visible) { m_visible = visible; }

        bool CastsShadow() const { return m_castsShadow; }
        void SetCastsShadow(bool casts) { m_castsShadow = casts; }

        bool ReceivesShadow() const { return m_receivesShadow; }
        void SetReceivesShadow(bool receives) { m_receivesShadow = receives; }

        // =====================================================================
        // Render Data Collection
        // =====================================================================

        /// Collect render data for this component (called by RenderScene)
        void CollectRenderData(RenderScene& scene, const Mat4& worldMatrix) const;

    private:
        Resource::ResourceHandle<Resource::MeshResource> m_mesh;
        std::vector<Resource::ResourceHandle<Resource::MaterialResource>> m_materialOverrides;

        bool m_visible = true;
        bool m_castsShadow = true;
        bool m_receivesShadow = true;
    };

} // namespace RVX
