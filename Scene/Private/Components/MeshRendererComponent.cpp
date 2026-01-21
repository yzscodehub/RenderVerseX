#include "Scene/Components/MeshRendererComponent.h"
#include "Scene/SceneEntity.h"
#include "Scene/Mesh.h"

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

    auto* mesh = m_mesh->GetMesh().get();
    if (mesh && mesh->GetBoundingBox().has_value())
    {
        // BoundingBox is an alias for AABB, just return it
        return mesh->GetBoundingBox().value();
    }

    return m_mesh->GetBounds();
}

void MeshRendererComponent::SetMesh(Resource::ResourceHandle<Resource::MeshResource> mesh)
{
    m_mesh = mesh;
    
    // Clear material overrides when mesh changes
    // (they might not be valid for new submesh count)
    m_materialOverrides.clear();
    
    // Notify that bounds have changed
    NotifyBoundsChanged();
}

void MeshRendererComponent::SetMaterial(size_t submeshIndex, Resource::ResourceHandle<Resource::MaterialResource> material)
{
    if (submeshIndex >= m_materialOverrides.size())
    {
        m_materialOverrides.resize(submeshIndex + 1);
    }
    m_materialOverrides[submeshIndex] = material;
}

Resource::ResourceHandle<Resource::MaterialResource> MeshRendererComponent::GetMaterial(size_t submeshIndex) const
{
    // 1. Check for override
    if (submeshIndex < m_materialOverrides.size() && m_materialOverrides[submeshIndex].IsValid())
    {
        return m_materialOverrides[submeshIndex];
    }

    // 2. Try to get default material from mesh's submesh
    // TODO: When MeshResource supports default materials per submesh, use that
    
    // 3. Return empty handle (caller should use a default material)
    return Resource::ResourceHandle<Resource::MaterialResource>();
}

size_t MeshRendererComponent::GetSubmeshCount() const
{
    if (!m_mesh.IsValid() || !m_mesh.IsLoaded())
    {
        return 0;
    }

    auto* mesh = m_mesh->GetMesh().get();
    if (!mesh)
    {
        return 0;
    }

    // If mesh has submeshes, return that count
    if (mesh->HasSubMeshes())
    {
        return mesh->GetSubMeshes().size();
    }

    // Otherwise, treat whole mesh as one submesh
    return 1;
}

void MeshRendererComponent::CollectRenderData(RenderScene& scene, const Mat4& worldMatrix) const
{
    // This will be implemented in Phase 5 when RenderScene is updated
    // For now, this is a placeholder
    
    (void)scene;
    (void)worldMatrix;
    
    // Future implementation:
    // RenderObject obj;
    // obj.meshId = m_mesh.GetId();
    // obj.worldMatrix = worldMatrix;
    // obj.normalMatrix = glm::transpose(glm::inverse(Mat3(worldMatrix)));
    // obj.bounds = GetLocalBounds().Transformed(worldMatrix);
    // obj.entityId = GetOwner()->GetHandle();
    // obj.castsShadow = m_castsShadow;
    // obj.receivesShadow = m_receivesShadow;
    // 
    // // Fill materials for each submesh
    // size_t submeshCount = GetSubmeshCount();
    // obj.materials.resize(submeshCount);
    // for (size_t i = 0; i < submeshCount; ++i)
    // {
    //     auto mat = GetMaterial(i);
    //     obj.materials[i] = mat.IsValid() ? mat.GetId() : 0;
    // }
    // 
    // scene.AddObject(std::move(obj));
}

} // namespace RVX
