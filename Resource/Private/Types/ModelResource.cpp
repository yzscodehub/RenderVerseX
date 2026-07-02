#include "Resource/Types/ModelResource.h"

#include <utility>

namespace RVX::Resource
{

ModelResource::ModelResource() = default;
ModelResource::~ModelResource() = default;

size_t ModelResource::GetMemoryUsage() const
{
    size_t total = sizeof(*this);
    
    // Add mesh memory
    for (const auto& mesh : m_meshes)
    {
        if (mesh.IsValid())
        {
            total += mesh->GetMemoryUsage();
        }
    }
    
    // Add material memory
    for (const auto& mat : m_materials)
    {
        if (mat.IsValid())
        {
            total += mat->GetMemoryUsage();
        }
    }
    
    return total;
}

size_t ModelResource::GetGPUMemoryUsage() const
{
    size_t total = 0;
    
    for (const auto& mesh : m_meshes)
    {
        if (mesh.IsValid())
        {
            total += mesh->GetGPUMemoryUsage();
        }
    }
    
    return total;
}

std::vector<ResourceId> ModelResource::GetRequiredDependencies() const
{
    std::vector<ResourceId> deps;
    
    for (const auto& mesh : m_meshes)
    {
        if (mesh.IsValid())
        {
            deps.push_back(mesh.GetId());
        }
    }
    
    for (const auto& mat : m_materials)
    {
        if (mat.IsValid())
        {
            deps.push_back(mat.GetId());
        }
    }
    
    return deps;
}

size_t ModelResource::GetNodeCount() const
{
    if (!m_rootNode)
        return 0;
    return CountNodes(m_rootNode.get());
}

size_t ModelResource::CountNodes(const Node* node) const
{
    if (!node)
        return 0;
    
    size_t count = 1;
    for (const auto& child : node->GetChildren())
    {
        count += CountNodes(child.get());
    }
    return count;
}

ResourceHandle<MeshResource> ModelResource::GetMesh(size_t index) const
{
    if (index < m_meshes.size())
    {
        return m_meshes[index];
    }
    return ResourceHandle<MeshResource>();
}

void ModelResource::AddMesh(ResourceHandle<MeshResource> mesh)
{
    m_meshes.push_back(std::move(mesh));
}

void ModelResource::SetMeshes(std::vector<ResourceHandle<MeshResource>> meshes)
{
    m_meshes = std::move(meshes);
}

ResourceHandle<MaterialResource> ModelResource::GetMaterial(size_t index) const
{
    if (index < m_materials.size())
    {
        return m_materials[index];
    }
    return ResourceHandle<MaterialResource>();
}

void ModelResource::AddMaterial(ResourceHandle<MaterialResource> material)
{
    m_materials.push_back(std::move(material));
}

void ModelResource::SetMaterials(std::vector<ResourceHandle<MaterialResource>> materials)
{
    m_materials = std::move(materials);
}

} // namespace RVX::Resource
