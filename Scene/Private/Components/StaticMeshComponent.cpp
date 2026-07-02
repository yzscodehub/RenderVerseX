#include "Scene/Components/StaticMeshComponent.h"

#include "Render/Renderer/RenderProxy.h"
#include "Render/Renderer/RenderScene.h"
#include "Scene/Components/SkeletonComponent.h"
#include "Scene/Mesh.h"
#include "Scene/SceneEntity.h"

#include <glm/gtc/matrix_inverse.hpp>

#include <utility>

namespace RVX
{

void StaticMeshComponent::SetMesh(Resource::ResourceHandle<Resource::MeshResource> mesh)
{
    m_mesh = std::move(mesh);
    m_materialOverrides.clear();

    if (!m_mesh.IsValid())
    {
        SetLocalBounds(AABB());
        return;
    }

    auto* meshData = m_mesh->GetMesh().get();
    if (meshData && meshData->GetBoundingBox().has_value())
    {
        SetLocalBounds(meshData->GetBoundingBox().value());
        return;
    }

    SetLocalBounds(m_mesh->GetBounds());
}

bool StaticMeshComponent::HasValidMesh() const
{
    return m_mesh.IsValid() && m_mesh.IsLoaded();
}

void StaticMeshComponent::SetMaterial(size_t submeshIndex,
                                      Resource::ResourceHandle<Resource::MaterialResource> material)
{
    if (submeshIndex >= m_materialOverrides.size())
    {
        m_materialOverrides.resize(submeshIndex + 1);
    }
    m_materialOverrides[submeshIndex] = std::move(material);
}

Resource::ResourceHandle<Resource::MaterialResource> StaticMeshComponent::GetMaterial(size_t submeshIndex) const
{
    if (submeshIndex < m_materialOverrides.size() && m_materialOverrides[submeshIndex].IsValid())
    {
        return m_materialOverrides[submeshIndex];
    }

    return Resource::ResourceHandle<Resource::MaterialResource>();
}

size_t StaticMeshComponent::GetSubmeshCount() const
{
    if (!HasValidMesh())
    {
        return 0;
    }

    auto* meshData = m_mesh->GetMesh().get();
    if (!meshData)
    {
        return 0;
    }

    if (meshData->HasSubMeshes())
    {
        return meshData->GetSubMeshes().size();
    }

    return 1;
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
    outProxy.meshResource = m_mesh.Get();
    outProxy.meshId = m_mesh.GetId();
    outProxy.layerMask = GetLayerMask();
    outProxy.castsShadow = m_castsShadow;
    outProxy.receivesShadow = m_receivesShadow;
    outProxy.visible = IsVisible();

    const size_t submeshCount = GetSubmeshCount();
    outProxy.materialIds.resize(submeshCount);
    outProxy.materialResources.resize(submeshCount);
    for (size_t i = 0; i < submeshCount; ++i)
    {
        auto material = GetMaterial(i);
        outProxy.materialIds[i] = material.IsValid() ? material.GetId() : 0;
        outProxy.materialResources[i] = material.Get();
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

void StaticMeshComponent::CollectRenderData(RenderScene& scene) const
{
    RenderPrimitiveProxy proxy;
    if (!CreateRenderProxy(proxy))
        return;

    RenderObject object;
    object.worldMatrix = proxy.worldMatrix;
    object.normalMatrix = proxy.normalMatrix;
    object.bounds = proxy.bounds;
    object.meshResource = proxy.meshResource;
    object.meshId = proxy.meshId;
    object.materialIds = proxy.materialIds;
    object.materialResources = proxy.materialResources;
    object.skinningMatrices = proxy.skinningMatrices;
    object.sortKey = proxy.sortKey;
    object.layerMask = proxy.layerMask;
    object.visible = proxy.visible;
    object.castsShadow = proxy.castsShadow;
    object.receivesShadow = proxy.receivesShadow;

    if (auto* entity = dynamic_cast<SceneEntity*>(GetOwner()))
    {
        object.entityId = entity->GetHandle();
    }

    scene.AddObject(object);
}

} // namespace RVX
