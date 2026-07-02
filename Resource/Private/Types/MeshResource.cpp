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

    MeshAttributeUploadView MakeAttributeView(const Mesh* mesh, const char* name)
    {
        if (!mesh)
            return {};

        const VertexAttribute* attribute = mesh->GetAttribute(name);
        if (!attribute || attribute->GetTotalSize() == 0)
            return {};

        MeshAttributeUploadView view;
        view.data = attribute->GetData();
        view.size = attribute->GetTotalSize();
        view.stride = attribute->GetStride();
        return view;
    }

    RenderMeshIndexType ToRenderMeshIndexType(IndexType indexType)
    {
        switch (indexType)
        {
            case IndexType::UInt8:
                return RenderMeshIndexType::UInt8;
            case IndexType::UInt16:
                return RenderMeshIndexType::UInt16;
            case IndexType::UInt32:
            default:
                return RenderMeshIndexType::UInt32;
        }
    }

    RenderPrimitiveTopology ToRenderPrimitiveTopology(PrimitiveType primitive)
    {
        switch (primitive)
        {
            case PrimitiveType::TriangleStrip:
                return RenderPrimitiveTopology::TriangleStrip;
            case PrimitiveType::TriangleFan:
                return RenderPrimitiveTopology::TriangleFan;
            case PrimitiveType::Lines:
                return RenderPrimitiveTopology::Lines;
            case PrimitiveType::LineStrip:
                return RenderPrimitiveTopology::LineStrip;
            case PrimitiveType::LineLoop:
                return RenderPrimitiveTopology::LineLoop;
            case PrimitiveType::Points:
                return RenderPrimitiveTopology::Points;
            case PrimitiveType::Triangles:
            default:
                return RenderPrimitiveTopology::Triangles;
        }
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

MeshUploadData MeshResource::GetUploadData() const
{
    MeshUploadData uploadData;
    if (!m_mesh)
        return uploadData;

    uploadData.vertexCount = m_mesh->GetVertexCount();
    uploadData.indexCount = m_mesh->GetIndexCount();
    uploadData.indexType = ToRenderMeshIndexType(m_mesh->GetIndexType());
    uploadData.primitive = ToRenderPrimitiveTopology(m_mesh->GetPrimitiveType());

    const auto& indexData = m_mesh->GetIndexData();
    uploadData.indexData = indexData.empty() ? nullptr : indexData.data();
    uploadData.indexDataSize = indexData.size();

    uploadData.position = MakeAttributeView(m_mesh.get(), VertexBufferNames::Position);
    uploadData.normal = MakeAttributeView(m_mesh.get(), VertexBufferNames::Normal);
    uploadData.tangent = MakeAttributeView(m_mesh.get(), VertexBufferNames::Tangent);
    uploadData.boneIndices = MakeAttributeView(m_mesh.get(), VertexBufferNames::BoneIndices);
    uploadData.boneWeights = MakeAttributeView(m_mesh.get(), VertexBufferNames::BoneWeights);

    const char* uvNames[] = {VertexBufferNames::UV, "uv", "texcoord0", "texcoord"};
    for (const char* uvName : uvNames)
    {
        uploadData.uv = MakeAttributeView(m_mesh.get(), uvName);
        if (uploadData.uv.IsValid())
            break;
    }

    if (m_mesh->HasSubMeshes())
    {
        uploadData.submeshes.reserve(m_mesh->GetSubMeshes().size());
        for (const auto& submesh : m_mesh->GetSubMeshes())
        {
            MeshSubmeshUploadInfo info;
            info.indexOffset = submesh.indexOffset;
            info.indexCount = submesh.indexCount;
            info.baseVertex = submesh.baseVertex;
            info.primitive = ToRenderPrimitiveTopology(
                submesh.primitive.value_or(m_mesh->GetPrimitiveType()));
            uploadData.submeshes.push_back(info);
        }
    }
    else
    {
        MeshSubmeshUploadInfo info;
        info.indexOffset = 0;
        info.indexCount = static_cast<uint32>(m_mesh->GetIndexCount());
        info.baseVertex = 0;
        info.primitive = uploadData.primitive;
        uploadData.submeshes.push_back(info);
    }

    return uploadData;
}

size_t MeshResource::GetRenderMeshSubmeshCount() const
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
