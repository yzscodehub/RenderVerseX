#include "Asset/Types/MeshAsset.h"

namespace RVX::Asset
{

MeshAsset::MeshAsset() = default;
MeshAsset::~MeshAsset() = default;

void MeshAsset::SetMesh(std::shared_ptr<Mesh> mesh)
{
    m_mesh = std::move(mesh);
    
    // Compute bounds if mesh has them
    if (m_mesh && m_mesh->GetBoundingBox())
    {
        m_bounds = *m_mesh->GetBoundingBox();
    }
}

size_t MeshAsset::GetMemoryUsage() const
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

size_t MeshAsset::GetGPUMemoryUsage() const
{
    // TODO: Calculate GPU buffer sizes when implemented
    return 0;
}

} // namespace RVX::Asset
