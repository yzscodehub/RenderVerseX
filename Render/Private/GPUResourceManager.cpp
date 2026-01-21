#include "Render/GPUResourceManager.h"
#include "Resource/Types/MeshResource.h"
#include "Resource/Types/TextureResource.h"
#include "Scene/Mesh.h"
#include "Core/Log.h"
#include <chrono>

namespace RVX
{

GPUResourceManager::GPUResourceManager() = default;

GPUResourceManager::~GPUResourceManager()
{
    Shutdown();
}

void GPUResourceManager::Initialize(IRHIDevice* device)
{
    m_device = device;
    RVX_CORE_INFO("GPUResourceManager initialized with {}MB budget", m_memoryBudget / (1024 * 1024));
}

void GPUResourceManager::Shutdown()
{
    // Clear all GPU resources
    m_meshGPUData.clear();
    m_textureGPUData.clear();
    m_usedMemory = 0;
    
    // Clear pending queue
    while (!m_pendingQueue.empty())
    {
        m_pendingQueue.pop();
    }
    
    m_device = nullptr;
}

void GPUResourceManager::RequestUpload(Resource::MeshResource* mesh, UploadPriority priority)
{
    if (!mesh || !m_device)
        return;

    Resource::ResourceId id = mesh->GetId();
    
    // Skip if already resident
    if (IsResident(id))
        return;

    PendingUpload upload;
    upload.id = id;
    upload.priority = priority;
    upload.resource = mesh;
    
    m_pendingQueue.push(upload);
}

void GPUResourceManager::RequestUpload(Resource::TextureResource* texture, UploadPriority priority)
{
    if (!texture || !m_device)
        return;

    Resource::ResourceId id = texture->GetId();
    
    // Skip if already resident
    if (IsResident(id))
        return;

    PendingUpload upload;
    upload.id = id;
    upload.priority = priority;
    upload.resource = texture;
    
    m_pendingQueue.push(upload);
}

void GPUResourceManager::UploadImmediate(Resource::MeshResource* mesh)
{
    if (!mesh || !m_device)
        return;

    // Skip if already resident
    if (IsResident(mesh->GetId()))
        return;

    UploadMesh(mesh);
}

void GPUResourceManager::UploadImmediate(Resource::TextureResource* texture)
{
    if (!texture || !m_device)
        return;

    // Skip if already resident
    if (IsResident(texture->GetId()))
        return;

    UploadTexture(texture);
}

MeshGPUBuffers GPUResourceManager::GetMeshBuffers(Resource::ResourceId meshId) const
{
    MeshGPUBuffers result;
    
    auto it = m_meshGPUData.find(meshId);
    if (it != m_meshGPUData.end() && it->second.isResident)
    {
        result.positionBuffer = it->second.positionBuffer.Get();
        result.normalBuffer = it->second.normalBuffer.Get();
        result.uvBuffer = it->second.uvBuffer.Get();
        result.tangentBuffer = it->second.tangentBuffer.Get();
        result.indexBuffer = it->second.indexBuffer.Get();
        result.submeshes = it->second.submeshes;
        result.isResident = true;
    }
    
    return result;
}

bool GPUResourceManager::IsResident(Resource::ResourceId id) const
{
    // Check meshes
    auto meshIt = m_meshGPUData.find(id);
    if (meshIt != m_meshGPUData.end() && meshIt->second.isResident)
        return true;

    // Check textures
    auto texIt = m_textureGPUData.find(id);
    if (texIt != m_textureGPUData.end() && texIt->second.isResident)
        return true;

    return false;
}

RHITexture* GPUResourceManager::GetTexture(Resource::ResourceId textureId) const
{
    auto it = m_textureGPUData.find(textureId);
    if (it != m_textureGPUData.end() && it->second.isResident)
    {
        return it->second.texture.Get();
    }
    return nullptr;
}

void GPUResourceManager::ProcessPendingUploads(float timeBudgetMs)
{
    if (!m_device || m_pendingQueue.empty())
        return;

    auto startTime = std::chrono::high_resolution_clock::now();

    while (!m_pendingQueue.empty())
    {
        // Check time budget
        auto elapsed = std::chrono::high_resolution_clock::now() - startTime;
        float elapsedMs = std::chrono::duration<float, std::milli>(elapsed).count();
        if (elapsedMs > timeBudgetMs)
            break;

        PendingUpload upload = m_pendingQueue.top();
        m_pendingQueue.pop();

        // Skip if already resident (may have been uploaded by UploadImmediate)
        if (IsResident(upload.id))
            continue;

        // Determine resource type and upload
        if (auto* mesh = dynamic_cast<Resource::MeshResource*>(upload.resource))
        {
            UploadMesh(mesh);
        }
        else if (auto* texture = dynamic_cast<Resource::TextureResource*>(upload.resource))
        {
            UploadTexture(texture);
        }
    }

    m_currentFrame++;
}

void GPUResourceManager::MarkUsed(Resource::ResourceId id)
{
    auto meshIt = m_meshGPUData.find(id);
    if (meshIt != m_meshGPUData.end())
    {
        meshIt->second.lastUsedFrame = m_currentFrame;
        return;
    }

    auto texIt = m_textureGPUData.find(id);
    if (texIt != m_textureGPUData.end())
    {
        texIt->second.lastUsedFrame = m_currentFrame;
    }
}

void GPUResourceManager::EvictUnused(uint64_t currentFrame, uint64_t frameThreshold)
{
    std::vector<Resource::ResourceId> meshesToEvict;
    std::vector<Resource::ResourceId> texturesToEvict;

    // Find unused meshes
    for (auto& [id, data] : m_meshGPUData)
    {
        if (data.isResident && (currentFrame - data.lastUsedFrame) > frameThreshold)
        {
            meshesToEvict.push_back(id);
        }
    }

    // Find unused textures
    for (auto& [id, data] : m_textureGPUData)
    {
        if (data.isResident && (currentFrame - data.lastUsedFrame) > frameThreshold)
        {
            texturesToEvict.push_back(id);
        }
    }

    // Evict meshes
    for (Resource::ResourceId id : meshesToEvict)
    {
        auto it = m_meshGPUData.find(id);
        if (it != m_meshGPUData.end())
        {
            m_usedMemory -= it->second.gpuMemorySize;
            m_meshGPUData.erase(it);
            RVX_CORE_DEBUG("Evicted mesh GPU data for resource {}", id);
        }
    }

    // Evict textures
    for (Resource::ResourceId id : texturesToEvict)
    {
        auto it = m_textureGPUData.find(id);
        if (it != m_textureGPUData.end())
        {
            m_usedMemory -= it->second.gpuMemorySize;
            m_textureGPUData.erase(it);
            RVX_CORE_DEBUG("Evicted texture GPU data for resource {}", id);
        }
    }
}

void GPUResourceManager::SetMemoryBudget(size_t bytes)
{
    m_memoryBudget = bytes;
}

GPUResourceManager::Stats GPUResourceManager::GetStats() const
{
    Stats stats;
    stats.residentMeshCount = m_meshGPUData.size();
    stats.residentTextureCount = m_textureGPUData.size();
    stats.pendingUploadCount = m_pendingQueue.size();
    stats.usedMemory = m_usedMemory;
    stats.memoryBudget = m_memoryBudget;
    return stats;
}

void GPUResourceManager::UploadMesh(Resource::MeshResource* meshRes)
{
    if (!meshRes || !m_device)
    {
        RVX_CORE_WARN("GPUResourceManager::UploadMesh - invalid meshRes ({}) or device ({})",
                      meshRes ? "valid" : "null", m_device ? "valid" : "null");
        return;
    }

    RVX_CORE_INFO("GPUResourceManager: Uploading mesh '{}' (ID: {})", 
                  meshRes->GetName(), meshRes->GetId());

    auto* mesh = meshRes->GetMesh().get();
    if (!mesh)
    {
        RVX_CORE_WARN("MeshResource has no mesh data: {}", meshRes->GetName());
        return;
    }

    RVX_CORE_INFO("  Vertex count: {}, Index count: {}", 
                  mesh->GetVertexCount(), mesh->GetIndexCount());
    RVX_CORE_INFO("  Attributes: ");
    for (const auto& [name, attr] : mesh->GetAttributes())
    {
        RVX_CORE_INFO("    - {}: stride={}, total={} bytes", 
                      name, attr->GetStride(), attr->GetTotalSize());
    }

    MeshGPUData gpuData;
    size_t totalMemory = 0;

    // Helper lambda to create and upload an attribute buffer
    auto createAttributeBuffer = [this, &totalMemory](const VertexAttribute* attr, const char* name) -> RHIBufferRef
    {
        if (!attr || attr->GetTotalSize() == 0)
            return nullptr;

        RHIBufferDesc desc;
        desc.size = attr->GetTotalSize();
        desc.usage = RHIBufferUsage::Vertex;
        desc.memoryType = RHIMemoryType::Upload;
        desc.stride = static_cast<uint32_t>(attr->GetStride());  // CRITICAL: Set stride for vertex buffer view
        desc.debugName = name;

        auto buffer = m_device->CreateBuffer(desc);
        if (buffer)
        {
            void* mapped = buffer->Map();
            if (mapped)
            {
                std::memcpy(mapped, attr->GetData(), attr->GetTotalSize());
                buffer->Unmap();
                totalMemory += attr->GetTotalSize();
            }
        }
        return buffer;
    };

    // Upload each attribute to its own buffer (no interleaving needed!)
    // Slot 0: Position (required)
    if (auto* posAttr = mesh->GetAttribute("position"))
    {
        gpuData.positionBuffer = createAttributeBuffer(posAttr, "PositionBuffer");
        if (!gpuData.positionBuffer)
        {
            RVX_CORE_ERROR("Failed to create position buffer for mesh: {}", meshRes->GetName());
            return;
        }
    }
    else
    {
        RVX_CORE_ERROR("Mesh has no position attribute: {}", meshRes->GetName());
        return;
    }

    // Slot 1: Normal (optional)
    if (auto* normAttr = mesh->GetAttribute("normal"))
    {
        gpuData.normalBuffer = createAttributeBuffer(normAttr, "NormalBuffer");
        gpuData.hasNormals = (gpuData.normalBuffer != nullptr);
    }

    // Slot 2: UV (optional) - try different names
    const char* uvNames[] = {"uv0", "uv", "texcoord0", "texcoord"};
    for (const char* uvName : uvNames)
    {
        if (auto* uvAttr = mesh->GetAttribute(uvName))
        {
            gpuData.uvBuffer = createAttributeBuffer(uvAttr, "UVBuffer");
            gpuData.hasUVs = (gpuData.uvBuffer != nullptr);
            break;
        }
    }

    // Slot 3: Tangent (optional)
    if (auto* tangentAttr = mesh->GetAttribute("tangent"))
    {
        gpuData.tangentBuffer = createAttributeBuffer(tangentAttr, "TangentBuffer");
        gpuData.hasTangents = (gpuData.tangentBuffer != nullptr);
    }

    // Create index buffer
    const auto& indexData = mesh->GetIndexData();
    if (!indexData.empty())
    {
        RHIBufferDesc ibDesc;
        ibDesc.size = indexData.size();
        ibDesc.usage = RHIBufferUsage::Index;
        ibDesc.memoryType = RHIMemoryType::Upload;
        ibDesc.debugName = "MeshIndexBuffer";
        
        gpuData.indexBuffer = m_device->CreateBuffer(ibDesc);
        if (gpuData.indexBuffer)
        {
            void* mapped = gpuData.indexBuffer->Map();
            if (mapped)
            {
                std::memcpy(mapped, indexData.data(), indexData.size());
                gpuData.indexBuffer->Unmap();
                totalMemory += indexData.size();
            }
        }
    }

    // Collect submesh info
    if (mesh->HasSubMeshes())
    {
        for (const auto& submesh : mesh->GetSubMeshes())
        {
            SubmeshGPUInfo info;
            info.indexOffset = submesh.indexOffset;
            info.indexCount = submesh.indexCount;
            info.baseVertex = submesh.baseVertex;
            gpuData.submeshes.push_back(info);
        }
    }
    else
    {
        // Single submesh covering entire mesh
        SubmeshGPUInfo info;
        info.indexOffset = 0;
        info.indexCount = static_cast<uint32_t>(mesh->GetIndexCount());
        info.baseVertex = 0;
        gpuData.submeshes.push_back(info);
    }

    // Track memory
    gpuData.gpuMemorySize = totalMemory;
    gpuData.lastUsedFrame = m_currentFrame;
    gpuData.isResident = true;

    m_usedMemory += gpuData.gpuMemorySize;
    
    // Log before move
    bool hasPos = gpuData.positionBuffer != nullptr;
    bool hasNorm = gpuData.hasNormals;
    bool hasUV = gpuData.hasUVs;
    bool hasTan = gpuData.hasTangents;
    
    m_meshGPUData[meshRes->GetId()] = std::move(gpuData);

    RVX_CORE_INFO("Uploaded mesh to GPU: {} ({}KB, pos:{} norm:{} uv:{} tan:{})", 
                   meshRes->GetName(), 
                   totalMemory / 1024,
                   hasPos ? "yes" : "no",
                   hasNorm ? "yes" : "no",
                   hasUV ? "yes" : "no",
                   hasTan ? "yes" : "no");
}

void GPUResourceManager::UploadTexture(Resource::TextureResource* textureRes)
{
    if (!textureRes || !m_device)
        return;

    const auto& metadata = textureRes->GetMetadata();
    const auto& data = textureRes->GetData();

    if (data.empty())
    {
        RVX_CORE_WARN("TextureResource has no data: {}", textureRes->GetName());
        return;
    }

    TextureGPUData gpuData;

    // Create texture description
    RHITextureDesc texDesc;
    texDesc.width = metadata.width;
    texDesc.height = metadata.height;
    texDesc.depth = metadata.depth;
    texDesc.mipLevels = metadata.mipLevels;
    texDesc.arraySize = metadata.arrayLayers;
    texDesc.usage = RHITextureUsage::ShaderResource;
    texDesc.debugName = textureRes->GetName().c_str();

    // Map format
    switch (metadata.format)
    {
        case Resource::TextureFormat::RGBA8:
            texDesc.format = metadata.isSRGB ? RHIFormat::RGBA8_UNORM_SRGB : RHIFormat::RGBA8_UNORM;
            break;
        case Resource::TextureFormat::RGB8:
            texDesc.format = metadata.isSRGB ? RHIFormat::RGBA8_UNORM_SRGB : RHIFormat::RGBA8_UNORM;
            break;
        case Resource::TextureFormat::RG8:
            texDesc.format = RHIFormat::RG8_UNORM;
            break;
        case Resource::TextureFormat::R8:
            texDesc.format = RHIFormat::R8_UNORM;
            break;
        case Resource::TextureFormat::RGBA16F:
            texDesc.format = RHIFormat::RGBA16_FLOAT;
            break;
        case Resource::TextureFormat::RGBA32F:
            texDesc.format = RHIFormat::RGBA32_FLOAT;
            break;
        case Resource::TextureFormat::BC1:
            texDesc.format = metadata.isSRGB ? RHIFormat::BC1_UNORM_SRGB : RHIFormat::BC1_UNORM;
            break;
        case Resource::TextureFormat::BC3:
            texDesc.format = metadata.isSRGB ? RHIFormat::BC3_UNORM_SRGB : RHIFormat::BC3_UNORM;
            break;
        case Resource::TextureFormat::BC5:
            texDesc.format = RHIFormat::BC5_UNORM;
            break;
        case Resource::TextureFormat::BC7:
            texDesc.format = metadata.isSRGB ? RHIFormat::BC7_UNORM_SRGB : RHIFormat::BC7_UNORM;
            break;
        default:
            texDesc.format = RHIFormat::RGBA8_UNORM;
            break;
    }

    // Determine texture dimension
    if (metadata.isCubemap)
    {
        texDesc.dimension = RHITextureDimension::TextureCube;
    }
    else if (metadata.depth > 1)
    {
        texDesc.dimension = RHITextureDimension::Texture3D;
    }
    else
    {
        texDesc.dimension = RHITextureDimension::Texture2D;
    }

    // Create the texture
    gpuData.texture = m_device->CreateTexture(texDesc);
    if (!gpuData.texture)
    {
        RVX_CORE_ERROR("Failed to create GPU texture for: {}", textureRes->GetName());
        return;
    }

    // TODO: Upload data using staging buffer and command context
    // For now, texture creation is enough; actual upload requires:
    // 1. Create staging buffer with upload heap
    // 2. Copy data to staging buffer
    // 3. Use command context to copy from staging to texture
    // This will be implemented when RHI has proper upload support

    // Track memory
    gpuData.gpuMemorySize = data.size();
    gpuData.lastUsedFrame = m_currentFrame;
    gpuData.isResident = true;

    m_usedMemory += gpuData.gpuMemorySize;
    m_textureGPUData[textureRes->GetId()] = std::move(gpuData);

    RVX_CORE_DEBUG("Created texture on GPU: {} ({}x{}, {}KB)", 
                   textureRes->GetName(),
                   metadata.width, metadata.height,
                   gpuData.gpuMemorySize / 1024);
}

} // namespace RVX
