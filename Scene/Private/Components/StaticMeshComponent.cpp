#include "Scene/Components/StaticMeshComponent.h"

#include "Geometry/Asset/AssetMetadata.h"
#include "RenderContracts/RenderProxy.h"
#include "Scene/Components/SkeletonComponent.h"
#include "Scene/SceneEntity.h"

#include <glm/gtc/matrix_inverse.hpp>

#include <utility>

namespace RVX
{
namespace
{
    RenderMaterialMode ToRenderMaterialMode(
        const IMaterialAssetMetadata* material)
    {
        if (!material)
            return RenderMaterialMode::Opaque;

        switch (material->GetAssetMaterialMode())
        {
            case AssetMaterialMode::Masked:
                return RenderMaterialMode::Masked;
            case AssetMaterialMode::Transparent:
                return RenderMaterialMode::Transparent;
            case AssetMaterialMode::Opaque:
            default:
                return RenderMaterialMode::Opaque;
        }
    }
} // namespace

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

bool StaticMeshComponent::CreateRenderProxy(RenderPrimitiveProxy& outProxy) const
{
    if (!HasRenderData())
        return false;

    const Mat4 worldMatrix = GetWorldTransform();

    outProxy = RenderPrimitiveProxy();
    outProxy.worldMatrix = worldMatrix;
    outProxy.normalMatrix = glm::inverseTranspose(Mat4(Mat3(worldMatrix)));
    outProxy.bounds = GetWorldBounds();
    outProxy.meshAssetId = AssetId{m_mesh.GetId()};
    outProxy.layerMask = GetLayerMask();
    outProxy.castsShadow = m_castsShadow;
    outProxy.receivesShadow = m_receivesShadow;
    outProxy.visible = IsVisible();

    const size_t submeshCount = GetSubmeshCount();
    outProxy.materialAssetIds.resize(submeshCount);
    outProxy.materialModes.resize(submeshCount);
    for (size_t i = 0; i < submeshCount; ++i)
    {
        auto material = GetMaterial(i);
        outProxy.materialAssetIds[i] =
            AssetId{material.IsValid() ? material.GetId() : 0};
        outProxy.materialModes[i] =
            ToRenderMaterialMode(material.As<IMaterialAssetMetadata>());
    }

    outProxy.sortKey = outProxy.materialAssetIds.empty()
                           ? 0
                           : outProxy.materialAssetIds[0].value;

    if (auto* entity = dynamic_cast<SceneEntity*>(GetOwner()))
    {
        if (auto* skeleton = entity->GetComponent<SkeletonComponent>())
        {
            outProxy.skinningMatrices = skeleton->GetSkinningMatrices();
        }
    }

    return true;
}

} // namespace RVX
