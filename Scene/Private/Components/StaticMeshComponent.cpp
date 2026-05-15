#include "Scene/Components/StaticMeshComponent.h"

#include "Render/Renderer/RenderScene.h"
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

void StaticMeshComponent::CollectRenderData(RenderScene& scene) const
{
    if (!HasRenderData())
        return;

    const Mat4 worldMatrix = GetWorldTransform();

    RenderObject object;
    object.worldMatrix = worldMatrix;
    object.normalMatrix = glm::inverseTranspose(Mat4(Mat3(worldMatrix)));
    object.bounds = GetWorldBounds();
    object.meshResource = m_mesh.Get();
    object.meshId = m_mesh.GetId();
    object.castsShadow = m_castsShadow;
    object.receivesShadow = m_receivesShadow;
    object.visible = IsVisible();

    if (auto* entity = dynamic_cast<SceneEntity*>(GetOwner()))
    {
        object.entityId = entity->GetHandle();
    }

    const size_t submeshCount = GetSubmeshCount();
    object.materialIds.resize(submeshCount);
    object.materialResources.resize(submeshCount);
    for (size_t i = 0; i < submeshCount; ++i)
    {
        auto material = GetMaterial(i);
        object.materialIds[i] = material.IsValid() ? material.GetId() : 0;
        object.materialResources[i] = material.Get();
    }

    object.sortKey = object.materialIds.empty() ? 0 : object.materialIds[0];
    scene.AddObject(object);
}

} // namespace RVX
