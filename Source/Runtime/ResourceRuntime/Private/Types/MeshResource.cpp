#include "Resource/Types/MeshResource.h"

namespace RVX::Resource
{
namespace
{
    size_t GetMeshMemoryUsage(const std::shared_ptr<Mesh>& mesh)
    {
        if (!mesh)
        {
            return 0;
        }

        size_t size = mesh->GetIndexData().size();
        for (const auto& [name, attr] : mesh->GetAttributes())
        {
            (void)name;
            if (attr)
            {
                size += attr->GetTotalSize();
            }
        }
        return size;
    }

} // namespace

MeshResource::MeshResource() = default;
MeshResource::~MeshResource() = default;

void MeshResource::SetMesh(std::shared_ptr<Mesh> mesh)
{
    m_mesh = std::move(mesh);
    m_lodMeshes.clear();
    if (m_mesh)
    {
        m_lodMeshes.push_back(m_mesh);
    }

    if (m_mesh && m_mesh->GetBoundingBox())
    {
        m_bounds = *m_mesh->GetBoundingBox();
    }
}

void MeshResource::SetLODMeshes(std::vector<std::shared_ptr<Mesh>> lodMeshes)
{
    m_lodMeshes = std::move(lodMeshes);
    m_mesh = m_lodMeshes.empty() ? nullptr : m_lodMeshes.front();

    if (m_mesh && m_mesh->GetBoundingBox())
    {
        m_bounds = *m_mesh->GetBoundingBox();
    }
}

size_t MeshResource::GetLODCount() const
{
    if (!m_lodMeshes.empty())
    {
        return m_lodMeshes.size();
    }

    return m_mesh ? 1u : 0u;
}

std::shared_ptr<Mesh> MeshResource::GetLODMesh(size_t lodIndex) const
{
    if (!m_lodMeshes.empty())
    {
        return lodIndex < m_lodMeshes.size() ? m_lodMeshes[lodIndex] : nullptr;
    }

    return lodIndex == 0 ? m_mesh : nullptr;
}

size_t MeshResource::GetAssetMeshSubmeshCount() const
{
    if (!m_mesh)
        return 0;

    return m_mesh->HasSubMeshes() ? m_mesh->GetSubMeshes().size() : 1;
}

size_t MeshResource::GetMemoryUsage() const
{
    size_t size = sizeof(*this);

    if (!m_lodMeshes.empty())
    {
        for (const std::shared_ptr<Mesh>& mesh : m_lodMeshes)
        {
            size += GetMeshMemoryUsage(mesh);
        }
    }
    else
    {
        size += GetMeshMemoryUsage(m_mesh);
    }

    return size;
}

size_t MeshResource::GetGPUMemoryUsage() const
{
    return 0;
}

} // namespace RVX::Resource
