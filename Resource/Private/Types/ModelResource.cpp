#include "Resource/Types/ModelResource.h"

#include <algorithm>
#include <utility>

namespace RVX::Resource
{

ModelResource::ModelResource() = default;
ModelResource::~ModelResource()
{
    CancelTextureStreaming();
}

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

std::vector<ResourceId> ModelResource::GetOptionalDependencies() const
{
    std::vector<ResourceId> dependencies;
    const std::vector<ResourceHandle<TextureResource>> textures =
        GetStreamingTextures();
    dependencies.reserve(textures.size());
    for (const ResourceHandle<TextureResource>& texture : textures)
    {
        if (texture)
            dependencies.push_back(texture.GetId());
    }
    return dependencies;
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

void ModelResource::SetTextureStreamingSources(
    std::vector<ModelTextureStreamingSource> sources)
{
    std::lock_guard<std::mutex> lock(m_textureStreamingMutex);
    m_textureStreamingSources = std::move(sources);
    m_streamingTextures.clear();
    m_streamingTextures.reserve(m_textureStreamingSources.size());
    for (const ModelTextureStreamingSource& source : m_textureStreamingSources)
    {
        if (source.texture)
            m_streamingTextures.push_back(source.texture);
    }
    m_decodedTextureCount = 0;
    m_residentTextureCount = 0;
    m_decodedTextureBytes = 0;
    m_textureStreamingError.clear();
    m_textureStreamingStage = m_textureStreamingSources.empty()
                                  ? ModelTextureStreamingStage::None
                                  : ModelTextureStreamingStage::AwaitingMinimumResident;
}

std::vector<ModelTextureStreamingSource> ModelResource::BeginTextureStreaming()
{
    std::lock_guard<std::mutex> lock(m_textureStreamingMutex);
    if (m_textureStreamingStage !=
        ModelTextureStreamingStage::AwaitingMinimumResident)
    {
        return {};
    }
    m_textureStreamingStage = ModelTextureStreamingStage::Decoding;
    std::vector<ModelTextureStreamingSource> sources;
    sources.swap(m_textureStreamingSources);
    if (sources.empty() &&
        m_decodedTextureCount >= m_streamingTextures.size())
    {
        m_textureStreamingStage =
            m_residentTextureCount >= m_streamingTextures.size()
                ? ModelTextureStreamingStage::FullyResident
                : ModelTextureStreamingStage::Uploading;
    }
    return sources;
}

void ModelResource::RebindTextureStreamingDependency(
    ResourceId resourceId,
    ResourceHandle<TextureResource> canonical)
{
    if (!canonical)
        return;
    std::lock_guard<std::mutex> lock(m_textureStreamingMutex);
    if (m_textureStreamingStage !=
        ModelTextureStreamingStage::AwaitingMinimumResident)
    {
        return;
    }
    for (ModelTextureStreamingSource& source : m_textureStreamingSources)
    {
        if (source.texture && source.texture.GetId() == resourceId)
            source.texture = canonical;
    }
    for (ResourceHandle<TextureResource>& texture : m_streamingTextures)
    {
        if (texture && texture.GetId() == resourceId)
            texture = canonical;
    }
    if (!canonical->IsStreamingPlaceholder())
    {
        const size_t before = m_textureStreamingSources.size();
        std::erase_if(
            m_textureStreamingSources,
            [resourceId](const ModelTextureStreamingSource& source)
            {
                return source.texture && source.texture.GetId() == resourceId;
            });
        m_decodedTextureCount += static_cast<uint32>(
            before - m_textureStreamingSources.size());
        m_residentTextureCount += static_cast<uint32>(
            before - m_textureStreamingSources.size());
    }
}

void ModelResource::MarkTextureDecodeComplete(uint64 decodedBytes)
{
    std::lock_guard<std::mutex> lock(m_textureStreamingMutex);
    if (m_textureStreamingStage != ModelTextureStreamingStage::Decoding)
        return;
    ++m_decodedTextureCount;
    m_decodedTextureBytes += decodedBytes;
    if (m_decodedTextureCount >= m_streamingTextures.size())
    {
        m_textureStreamingStage =
            m_residentTextureCount >= m_streamingTextures.size()
                ? ModelTextureStreamingStage::FullyResident
                : ModelTextureStreamingStage::Uploading;
    }
}

void ModelResource::MarkTexturePublicationComplete(ResourceId textureId)
{
    if (textureId == InvalidResourceId)
        return;
    std::lock_guard<std::mutex> lock(m_textureStreamingMutex);
    if (m_textureStreamingStage != ModelTextureStreamingStage::Decoding &&
        m_textureStreamingStage != ModelTextureStreamingStage::Uploading)
    {
        return;
    }
    const bool belongsToModel = std::any_of(
        m_streamingTextures.begin(),
        m_streamingTextures.end(),
        [textureId](const ResourceHandle<TextureResource>& texture)
        {
            return texture && texture.GetId() == textureId;
        });
    if (!belongsToModel)
        return;

    ++m_residentTextureCount;
    if (m_decodedTextureCount >= m_streamingTextures.size() &&
        m_residentTextureCount >= m_streamingTextures.size())
    {
        m_textureStreamingStage = ModelTextureStreamingStage::FullyResident;
    }
}

void ModelResource::MarkTextureStreamingFailed(std::string error)
{
    std::lock_guard<std::mutex> lock(m_textureStreamingMutex);
    if (m_textureStreamingStage == ModelTextureStreamingStage::FullyResident ||
        m_textureStreamingStage == ModelTextureStreamingStage::Cancelled)
    {
        return;
    }
    m_textureStreamingError = std::move(error);
    m_textureStreamingSources.clear();
    m_textureStreamingStage = ModelTextureStreamingStage::Failed;
}

void ModelResource::CancelTextureStreaming() noexcept
{
    std::lock_guard<std::mutex> lock(m_textureStreamingMutex);
    if (m_textureStreamingStage == ModelTextureStreamingStage::None ||
        m_textureStreamingStage == ModelTextureStreamingStage::FullyResident ||
        m_textureStreamingStage == ModelTextureStreamingStage::Failed)
    {
        return;
    }
    m_textureStreamingSources.clear();
    m_textureStreamingStage = ModelTextureStreamingStage::Cancelled;
}

ModelTextureStreamingSnapshot ModelResource::GetTextureStreamingSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_textureStreamingMutex);
    return {m_textureStreamingStage,
            static_cast<uint32>(m_streamingTextures.size()),
            m_decodedTextureCount,
            m_residentTextureCount,
            m_decodedTextureBytes,
            m_textureStreamingError};
}

std::vector<ResourceHandle<TextureResource>>
ModelResource::GetStreamingTextures() const
{
    std::lock_guard<std::mutex> lock(m_textureStreamingMutex);
    return m_streamingTextures;
}

} // namespace RVX::Resource
