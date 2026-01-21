#include "Resource/Types/ModelResource.h"
#include "Scene/SceneManager.h"
#include "Scene/SceneEntity.h"
#include "Scene/Components/MeshRendererComponent.h"
#include "Scene/ComponentFactory.h"
#include "Scene/Node.h"

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

SceneEntity* ModelResource::Instantiate(SceneManager* scene) const
{
    if (!m_rootNode || !scene)
    {
        return nullptr;
    }
    
    return InstantiateNode(m_rootNode.get(), scene, nullptr);
}

SceneEntity* ModelResource::InstantiateNode(const Node* node, SceneManager* scene, SceneEntity* parent) const
{
    if (!node)
        return nullptr;
    
    // Create entity
    SceneEntity::Handle handle = scene->CreateEntity(node->GetName());
    SceneEntity* entity = scene->GetEntity(handle);
    if (!entity)
        return nullptr;
    
    // Set transform from node
    const auto& transform = node->GetLocalTransform();
    entity->SetPosition(transform.GetPosition());
    entity->SetRotation(transform.GetRotation());
    entity->SetScale(transform.GetScale());
    
    // Use ComponentFactory if node uses index mode (preferred new approach)
    if (node->UsesIndexMode())
    {
        // Use ComponentFactory to create components from node indices
        ComponentFactory::CreateComponents(entity, node, this);
    }
    else if (auto* meshComp = node->GetComponent<MeshComponent>())
    {
        // Legacy fallback: use MeshComponent pointer matching
        Mesh::Ptr nodeMesh = meshComp->GetMesh();
        
        // Find matching MeshResource by comparing mesh pointers
        ResourceHandle<MeshResource> meshHandle;
        for (const auto& meshRes : m_meshes)
        {
            if (meshRes.IsValid() && meshRes->GetMesh() == nodeMesh)
            {
                meshHandle = meshRes;
                break;
            }
        }
        
        if (meshHandle.IsValid())
        {
            // Add MeshRendererComponent
            auto* renderer = entity->AddComponent<MeshRendererComponent>();
            renderer->SetMesh(meshHandle);
            
            // Set materials for each submesh
            auto* mesh = nodeMesh.get();
            if (mesh)
            {
                const auto& submeshes = mesh->GetSubMeshes();
                for (size_t i = 0; i < submeshes.size(); ++i)
                {
                    uint32_t matIndex = submeshes[i].materialId;
                    if (matIndex < m_materials.size() && m_materials[matIndex].IsValid())
                    {
                        renderer->SetMaterial(i, m_materials[matIndex]);
                    }
                }
            }
            
            // Copy visibility settings
            renderer->SetVisible(meshComp->IsVisible());
            renderer->SetCastsShadow(meshComp->CastsShadows());
        }
    }
    
    // Set parent relationship
    if (parent)
    {
        parent->AddChild(entity);
    }
    
    // Recursively instantiate children
    for (const auto& child : node->GetChildren())
    {
        InstantiateNode(child.get(), scene, entity);
    }
    
    return entity;
}

} // namespace RVX::Resource
