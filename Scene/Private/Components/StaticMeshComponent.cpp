#include "Scene/Components/StaticMeshComponent.h"

#include "Scene/Components/SkeletonComponent.h"
#include "RenderContracts/RenderProxy.h"
#include "Scene/SceneEntity.h"

#include <glm/gtc/matrix_inverse.hpp>

#include <utility>

namespace RVX
{
namespace
{
    RenderMaterialMode ToRenderMaterialMode(const IRenderMaterialSource* material)
    {
        if (!material)
            return RenderMaterialMode::Opaque;

        switch (material->GetRenderMaterialSourceData().alphaMode)
        {
            case MaterialSourceAlphaMode::Mask:
                return RenderMaterialMode::Masked;
            case MaterialSourceAlphaMode::Blend:
                return RenderMaterialMode::Transparent;
            case MaterialSourceAlphaMode::Opaque:
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

    if (auto* meshSource = m_mesh.As<IRenderMeshUploadSource>())
    {
        SetLocalBounds(meshSource->GetRenderMeshBounds());
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

    auto* meshSource = m_mesh.As<IRenderMeshUploadSource>();
    if (!meshSource)
    {
        return 0;
    }

    return meshSource->GetRenderMeshSubmeshCount();
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
    outProxy.meshResource = m_mesh.As<IRenderMeshUploadSource>();
    outProxy.meshId = m_mesh.GetId();
    outProxy.layerMask = GetLayerMask();
    outProxy.castsShadow = m_castsShadow;
    outProxy.receivesShadow = m_receivesShadow;
    outProxy.visible = IsVisible();

    const size_t submeshCount = GetSubmeshCount();
    outProxy.materialIds.resize(submeshCount);
    outProxy.materialModes.resize(submeshCount);
    outProxy.materialResources.resize(submeshCount);
    for (size_t i = 0; i < submeshCount; ++i)
    {
        auto material = GetMaterial(i);
        outProxy.materialIds[i] = material.IsValid() ? material.GetId() : 0;
        outProxy.materialModes[i] = ToRenderMaterialMode(material.As<IRenderMaterialSource>());
        outProxy.materialResources[i] = material.As<IRenderMaterialSource>();
    }

    outProxy.sortKey = outProxy.materialIds.empty() ? 0 : outProxy.materialIds[0];

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
