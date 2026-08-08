#pragma once

/**
 * @file StaticMeshComponent.h
 * @brief UE-style primitive component for rendering static meshes
 */

#include "Scene/PrimitiveComponent.h"
#include "Scene/SceneAssetHandle.h"

#include <vector>

namespace RVX
{
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

        void SetMesh(SceneMeshHandle mesh);
        SceneMeshHandle GetMesh() const { return m_mesh; }
        bool HasValidMesh() const;

        // =====================================================================
        // Materials
        // =====================================================================

        void SetMaterial(size_t submeshIndex, SceneMaterialHandle material);
        SceneMaterialHandle GetMaterial(size_t submeshIndex) const;
        size_t GetSubmeshCount() const;
        size_t GetMaterialOverrideCount() const { return m_materialOverrides.size(); }
        void ClearMaterialOverrides();

        // =====================================================================
        // Rendering Properties
        // =====================================================================

        bool CastsShadow() const { return m_castsShadow; }
        void SetCastsShadow(bool castsShadow);

        bool ReceivesShadow() const { return m_receivesShadow; }
        void SetReceivesShadow(bool receivesShadow);

        bool HasRenderData() const override;

    private:
        SceneMeshHandle m_mesh;
        std::vector<SceneMaterialHandle> m_materialOverrides;
        bool m_castsShadow = true;
        bool m_receivesShadow = true;
    };

} // namespace RVX
