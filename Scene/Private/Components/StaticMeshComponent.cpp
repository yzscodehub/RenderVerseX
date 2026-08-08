#include "Scene/Components/StaticMeshComponent.h"

#include "Geometry/Asset/AssetMetadata.h"

#include <utility>

namespace RVX
{
void StaticMeshComponent::SetMesh(SceneMeshHandle mesh)
{
    m_mesh = std::move(mesh);
    m_materialOverrides.clear();

    if (!m_mesh.IsValid())
    {
        SetLocalBounds(AABB());
        return;
    }

    if (auto* meshMetadata = m_mesh.As<IMeshAssetMetadata>())
    {
        SetLocalBounds(meshMetadata->GetAssetMeshBounds());
        return;
    }

    SetLocalBounds(AABB());
}

bool StaticMeshComponent::HasValidMesh() const
{
    return m_mesh.IsValid() && m_mesh.IsLoaded();
}

void StaticMeshComponent::SetMaterial(size_t submeshIndex,
                                      SceneMaterialHandle material)
{
    if (submeshIndex >= m_materialOverrides.size())
    {
        m_materialOverrides.resize(submeshIndex + 1);
    }
    m_materialOverrides[submeshIndex] = std::move(material);
    NotifySceneStateChanged();
}

void StaticMeshComponent::ClearMaterialOverrides()
{
    if (m_materialOverrides.empty())
        return;
    m_materialOverrides.clear();
    NotifySceneStateChanged();
}

void StaticMeshComponent::SetCastsShadow(bool castsShadow)
{
    if (m_castsShadow == castsShadow)
        return;
    m_castsShadow = castsShadow;
    NotifySceneStateChanged();
}

void StaticMeshComponent::SetReceivesShadow(bool receivesShadow)
{
    if (m_receivesShadow == receivesShadow)
        return;
    m_receivesShadow = receivesShadow;
    NotifySceneStateChanged();
}

SceneMaterialHandle StaticMeshComponent::GetMaterial(size_t submeshIndex) const
{
    if (submeshIndex < m_materialOverrides.size() && m_materialOverrides[submeshIndex].IsValid())
    {
        return m_materialOverrides[submeshIndex];
    }

    return SceneMaterialHandle();
}

size_t StaticMeshComponent::GetSubmeshCount() const
{
    if (!HasValidMesh())
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

bool StaticMeshComponent::HasRenderData() const
{
    return HasValidMesh() && GetSubmeshCount() > 0;
}

} // namespace RVX
