#include "Scene/Components/MeshRendererComponent.h"

#include "Geometry/Asset/AssetMetadata.h"
#include "Scene/SceneEntity.h"

namespace RVX
{

void MeshRendererComponent::OnAttach()
{
    // Notify that bounds may have changed
    NotifyBoundsChanged();
}

void MeshRendererComponent::OnDetach()
{
    // Nothing special needed
}

AABB MeshRendererComponent::GetLocalBounds() const
{
    if (!m_mesh.IsValid() || !m_mesh.IsLoaded())
    {
        return AABB();
    }

    if (auto* meshMetadata = m_mesh.As<IMeshAssetMetadata>())
    {
        return meshMetadata->GetAssetMeshBounds();
    }

    return AABB();
}

void MeshRendererComponent::SetMesh(SceneMeshHandle mesh)
{
    m_mesh = mesh;
    
    // Clear material overrides when mesh changes
    // (they might not be valid for new submesh count)
    m_materialOverrides.clear();
    
    // Notify that bounds have changed
    NotifyBoundsChanged();
}

void MeshRendererComponent::SetMaterial(size_t submeshIndex, SceneMaterialHandle material)
{
    if (submeshIndex >= m_materialOverrides.size())
    {
        m_materialOverrides.resize(submeshIndex + 1);
    }
    m_materialOverrides[submeshIndex] = material;
}

SceneMaterialHandle MeshRendererComponent::GetMaterial(size_t submeshIndex) const
{
    // 1. Check for override
    if (submeshIndex < m_materialOverrides.size() && m_materialOverrides[submeshIndex].IsValid())
    {
        return m_materialOverrides[submeshIndex];
    }

    // 2. Try to get default material from mesh's submesh
    // TODO: When MeshResource supports default materials per submesh, use that
    
    // 3. Return empty handle (caller should use a default material)
    return SceneMaterialHandle();
}

size_t MeshRendererComponent::GetSubmeshCount() const
{
    if (!m_mesh.IsValid() || !m_mesh.IsLoaded())
    {
        return 0;
    }

    auto* meshMetadata = m_mesh.As<IMeshAssetMetadata>();
    if (!meshMetadata)
    {
        return 0;
    }

    return meshMetadata->GetAssetMeshSubmeshCount();
}

} // namespace RVX
