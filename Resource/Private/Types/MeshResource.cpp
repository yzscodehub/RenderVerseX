#include "Resource/Types/MeshResource.h"

namespace RVX::Resource
{

MeshResource::MeshResource() = default;
MeshResource::~MeshResource() = default;

void MeshResource::SetMesh(std::shared_ptr<Mesh> mesh)
{
    m_mesh = std::move(mesh);
    
    // Compute bounds if mesh has them
    if (m_mesh && m_mesh->GetBoundingBox())
    {
        m_bounds = *m_mesh->GetBoundingBox();
    }
}

size_t MeshResource::GetMemoryUsage() const
{
    size_t size = sizeof(*this);
    
    if (m_mesh)
    {
        // Estimate mesh memory usage
        size += m_mesh->GetIndexData().size();
        
        for (const auto& [name, attr] : m_mesh->GetAttributes())
        {
            size += attr->GetTotalSize();
        }
    }
    
    return size;
}

size_t MeshResource::GetGPUMemoryUsage() const
{
    // TODO: Calculate GPU buffer sizes when implemented
    return 0;
}

} // namespace RVX::Resource
