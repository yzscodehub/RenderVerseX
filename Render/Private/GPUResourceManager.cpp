#include "Render/GPUResourceManager.h"
#include "Core/Log.h"
#include "Resource/Types/MeshResource.h"
#include "Resource/Types/TextureResource.h"
#include "RHI/RHICommandContext.h"
#include "Scene/Mesh.h"
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
    m_uploadService = std::make_unique<GPUUploadService>();
    m_uploadService->Initialize(device);
    RVX_CORE_INFO("GPUResourceManager initialized with {}MB budget", m_memoryBudget / (1024 * 1024));
}

void GPUResourceManager::Shutdown()
{
    for (auto& [id, data] : m_textureGPUData)
    {
        (void)id;
        NotifyTextureInvalidated(data.texture.Get());
        AbandonUploadIds(data.pendingUploadIds);
    }
    for (auto& [id, data] : m_meshGPUData)
    {
        (void)id;
        AbandonUploadIds(data.pendingUploadIds);
    }

    // Clear all GPU resources
    m_meshGPUData.clear();
    m_textureGPUData.clear();
    m_resourceStates.clear();
    m_pendingMeshUploadCompletions.clear();
    m_pendingTextureUploadCompletions.clear();
    m_usedMemory = 0;
    
    // Clear pending queue
    while (!m_pendingQueue.empty())
    {
        m_pendingQueue.pop();
    }

    if (m_uploadService)
    {
        m_uploadService->Shutdown();
        m_uploadService.reset();
    }
    
    m_textureInvalidatedCallback = nullptr;
    m_device = nullptr;
}

void GPUResourceManager::RequestUpload(Resource::MeshResource* mesh, UploadPriority priority)
{
    if (!mesh || !m_device)
        return;

    if (mesh->GetRefCount() == 0)
    {
        RVX_CORE_WARN("GPUResourceManager: Ignoring async upload for unmanaged mesh '{}'", mesh->GetName());
        return;
    }

    Resource::ResourceId id = mesh->GetId();
    
    // Skip if already resident
    if (IsResident(id))
        return;

    GPUResourceState currentState = GetResourceState(id);
    if (currentState == GPUResourceState::UploadQueued || currentState == GPUResourceState::Uploading)
        return;

    PendingUpload upload;
    upload.id = id;
    upload.priority = priority;
    upload.retainedResource.Reset(mesh);
    
    SetResourceState(id, GPUResourceState::UploadQueued);
    m_pendingQueue.push(upload);
}

void GPUResourceManager::RequestUpload(Resource::TextureResource* texture, UploadPriority priority)
{
    if (!texture || !m_device)
        return;

    if (texture->GetRefCount() == 0)
    {
        RVX_CORE_WARN("GPUResourceManager: Ignoring async upload for unmanaged texture '{}'", texture->GetName());
        return;
    }

    Resource::ResourceId id = texture->GetId();
    
    // Skip if already resident
    if (IsResident(id))
        return;

    GPUResourceState currentState = GetResourceState(id);
    if (currentState == GPUResourceState::UploadQueued || currentState == GPUResourceState::Uploading)
        return;

    PendingUpload upload;
    upload.id = id;
    upload.priority = priority;
    upload.retainedResource.Reset(texture);
    
    SetResourceState(id, GPUResourceState::UploadQueued);
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
    if (m_uploadService)
    {
        m_uploadService->FlushAndWaitForUploads();
        UpdateCompletedResourceUploads();
    }
}

void GPUResourceManager::UploadImmediate(Resource::TextureResource* texture)
{
    if (!texture || !m_device)
        return;

    // Skip if already resident
    if (IsResident(texture->GetId()))
        return;

    UploadTexture(texture);
    if (m_uploadService)
    {
        m_uploadService->FlushAndWaitForUploads();
        UpdateCompletedResourceUploads();
    }
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

GPUResourceState GPUResourceManager::GetResourceState(Resource::ResourceId id) const
{
    auto stateIt = m_resourceStates.find(id);
    if (stateIt != m_resourceStates.end())
    {
        return stateIt->second;
    }

    if (IsResident(id))
    {
        return GPUResourceState::GPUReady;
    }

    return GPUResourceState::Unloaded;
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

bool GPUResourceManager::TransitionTexture(Resource::ResourceId textureId, RHICommandContext& ctx, RHIResourceState desiredState)
{
    auto it = m_textureGPUData.find(textureId);
    if (it == m_textureGPUData.end() || !it->second.isResident || !it->second.texture)
        return false;

    TextureGPUData& data = it->second;
    if (data.currentState != desiredState)
    {
        ctx.TextureBarrier(data.texture.Get(), data.currentState, desiredState);
        data.currentState = desiredState;
    }

    data.lastUsedFrame = m_currentFrame;
    return true;
}

void GPUResourceManager::ProcessPendingUploads(float timeBudgetMs)
{
    if (!m_device)
        return;

    if (m_uploadService)
    {
        m_uploadService->ProcessCompletedUploads();
        UpdateCompletedResourceUploads();
    }

    if (m_pendingQueue.empty())
    {
        m_currentFrame++;
        return;
    }

    if (timeBudgetMs <= 0.0f)
    {
        m_currentFrame++;
        return;
    }

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
        Resource::IResource* resource = upload.retainedResource.Get();
        if (!resource)
        {
            SetResourceState(upload.id, GPUResourceState::Failed);
            continue;
        }

        if (auto* mesh = dynamic_cast<Resource::MeshResource*>(resource))
        {
            UploadMesh(mesh);
        }
        else if (auto* texture = dynamic_cast<Resource::TextureResource*>(resource))
        {
            UploadTexture(texture);
        }
    }

    if (m_uploadService)
    {
        m_uploadService->FlushBatchUploads();
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
            AbandonUploadIds(it->second.pendingUploadIds);
            m_usedMemory -= it->second.gpuMemorySize;
            m_meshGPUData.erase(it);
            m_pendingMeshUploadCompletions.erase(id);
            m_resourceStates.erase(id);
            RVX_CORE_DEBUG("Evicted mesh GPU data for resource {}", id);
        }
    }

    // Evict textures
    for (Resource::ResourceId id : texturesToEvict)
    {
        auto it = m_textureGPUData.find(id);
        if (it != m_textureGPUData.end())
        {
            NotifyTextureInvalidated(it->second.texture.Get());
            AbandonUploadIds(it->second.pendingUploadIds);
            m_usedMemory -= it->second.gpuMemorySize;
            m_textureGPUData.erase(it);
            m_pendingTextureUploadCompletions.erase(id);
            m_resourceStates.erase(id);
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
    stats.pendingUploadCount = m_pendingQueue.size();
    stats.usedMemory = m_usedMemory;
    stats.memoryBudget = m_memoryBudget;

    for (const auto& [id, data] : m_meshGPUData)
    {
        (void)id;
        if (data.isResident)
        {
            stats.residentMeshCount++;
        }
    }

    for (const auto& [id, data] : m_textureGPUData)
    {
        (void)id;
        if (data.isResident)
        {
            stats.residentTextureCount++;
        }
    }

    for (const auto& [id, state] : m_resourceStates)
    {
        (void)id;
        switch (state)
        {
            case GPUResourceState::UploadQueued:
                stats.queuedUploadCount++;
                break;
            case GPUResourceState::Uploading:
                stats.uploadingCount++;
                break;
            case GPUResourceState::Failed:
                stats.failedUploadCount++;
                break;
            default:
                break;
        }
    }

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

    SetResourceState(meshRes->GetId(), GPUResourceState::Uploading);

    RVX_CORE_DEBUG("GPUResourceManager: Uploading mesh '{}' (ID: {})",
                   meshRes->GetName(), meshRes->GetId());

    auto* mesh = meshRes->GetMesh().get();
    if (!mesh)
    {
        RVX_CORE_WARN("MeshResource has no mesh data: {}", meshRes->GetName());
        SetResourceState(meshRes->GetId(), GPUResourceState::Failed);
        return;
    }

    RVX_CORE_TRACE("  Vertex count: {}, Index count: {}",
                   mesh->GetVertexCount(), mesh->GetIndexCount());
    RVX_CORE_TRACE("  Attributes: ");
    for (const auto& [name, attr] : mesh->GetAttributes())
    {
        RVX_CORE_TRACE("    - {}: stride={}, total={} bytes",
                       name, attr->GetStride(), attr->GetTotalSize());
    }

    MeshGPUData gpuData;
    size_t totalMemory = 0;

    // Helper lambda to create and upload an attribute buffer
    auto createAttributeBuffer = [this, &gpuData, &totalMemory](const VertexAttribute* attr, const char* name) -> RHIBufferRef
    {
        if (!attr || attr->GetTotalSize() == 0)
            return nullptr;

        GPUUploadBufferDesc desc;
        desc.size = attr->GetTotalSize();
        desc.usage = RHIBufferUsage::Vertex;
        desc.stride = static_cast<uint32_t>(attr->GetStride());  // CRITICAL: Set stride for vertex buffer view
        desc.debugName = name;

        auto result = m_uploadService->UploadBufferDataWithResult(desc, attr->GetData(), attr->GetTotalSize());
        if (!result)
        {
            return nullptr;
        }

        if (result.isPending)
        {
            gpuData.pendingUploadIds.push_back(result.uploadId);
        }

        totalMemory += result.bytesUploaded;
        return result.resource;
    };

    // Upload each attribute to its own buffer (no interleaving needed!)
    // Slot 0: Position (required)
    if (auto* posAttr = mesh->GetAttribute("position"))
    {
        gpuData.positionBuffer = createAttributeBuffer(posAttr, "PositionBuffer");
        if (!gpuData.positionBuffer)
        {
            RVX_CORE_ERROR("Failed to create position buffer for mesh: {}", meshRes->GetName());
            AbandonUploadIds(gpuData.pendingUploadIds);
            SetResourceState(meshRes->GetId(), GPUResourceState::Failed);
            return;
        }
    }
    else
    {
        RVX_CORE_ERROR("Mesh has no position attribute: {}", meshRes->GetName());
        SetResourceState(meshRes->GetId(), GPUResourceState::Failed);
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
    if (indexData.empty())
    {
        RVX_CORE_ERROR("Mesh has no index data: {}", meshRes->GetName());
        AbandonUploadIds(gpuData.pendingUploadIds);
        SetResourceState(meshRes->GetId(), GPUResourceState::Failed);
        return;
    }

    GPUUploadBufferDesc ibDesc;
    ibDesc.size = indexData.size();
    ibDesc.usage = RHIBufferUsage::Index;
    ibDesc.debugName = "MeshIndexBuffer";

    auto indexUpload = m_uploadService->UploadBufferDataWithResult(ibDesc, indexData.data(), indexData.size());
    gpuData.indexBuffer = indexUpload.resource;
    if (!indexUpload)
    {
        RVX_CORE_ERROR("Failed to create index buffer for mesh: {}", meshRes->GetName());
        AbandonUploadIds(gpuData.pendingUploadIds);
        SetResourceState(meshRes->GetId(), GPUResourceState::Failed);
        return;
    }

    if (indexUpload.isPending)
    {
        gpuData.pendingUploadIds.push_back(indexUpload.uploadId);
    }

    totalMemory += indexUpload.bytesUploaded;

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
    gpuData.isResident = gpuData.pendingUploadIds.empty();

    if (gpuData.isResident)
    {
        m_usedMemory += gpuData.gpuMemorySize;
    }
    
    // Log before move
    bool hasPos = gpuData.positionBuffer != nullptr;
    bool hasNorm = gpuData.hasNormals;
    bool hasUV = gpuData.hasUVs;
    bool hasTan = gpuData.hasTangents;
    
    m_meshGPUData[meshRes->GetId()] = std::move(gpuData);
    if (!m_meshGPUData[meshRes->GetId()].pendingUploadIds.empty())
    {
        m_pendingMeshUploadCompletions.insert(meshRes->GetId());
    }
    else
    {
        m_pendingMeshUploadCompletions.erase(meshRes->GetId());
    }

    SetResourceState(meshRes->GetId(), hasPos && m_meshGPUData[meshRes->GetId()].isResident ?
                                      GPUResourceState::GPUReady : GPUResourceState::Uploading);

    RVX_CORE_DEBUG("Uploaded mesh to GPU: {} ({}KB, pos:{} norm:{} uv:{} tan:{})",
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

    SetResourceState(textureRes->GetId(), GPUResourceState::Uploading);

    const auto& metadata = textureRes->GetMetadata();
    const auto& data = textureRes->GetData();

    if (data.empty())
    {
        RVX_CORE_WARN("TextureResource has no data: {}", textureRes->GetName());
        SetResourceState(textureRes->GetId(), GPUResourceState::Failed);
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

    GPUUploadTextureDesc uploadDesc;
    uploadDesc.textureDesc = texDesc;
    uploadDesc.dataSize = data.size();

    auto textureUpload = m_uploadService->UploadTextureDataWithResult(uploadDesc, data.data());
    gpuData.texture = textureUpload.resource;
    if (!textureUpload)
    {
        RVX_CORE_ERROR("Failed to create GPU texture for: {}", textureRes->GetName());
        SetResourceState(textureRes->GetId(), GPUResourceState::Failed);
        return;
    }

    if (textureUpload.isPending)
    {
        gpuData.pendingUploadIds.push_back(textureUpload.uploadId);
    }

    // Track memory
    gpuData.gpuMemorySize = static_cast<size_t>(textureUpload.bytesUploaded);
    gpuData.lastUsedFrame = m_currentFrame;
    gpuData.isResident = gpuData.pendingUploadIds.empty();

    if (gpuData.isResident)
    {
        m_usedMemory += gpuData.gpuMemorySize;
    }

    if (auto existingIt = m_textureGPUData.find(textureRes->GetId()); existingIt != m_textureGPUData.end())
    {
        if (existingIt->second.texture.Get() != gpuData.texture.Get())
        {
            NotifyTextureInvalidated(existingIt->second.texture.Get());
        }
        AbandonUploadIds(existingIt->second.pendingUploadIds);
    }

    m_textureGPUData[textureRes->GetId()] = std::move(gpuData);
    if (!m_textureGPUData[textureRes->GetId()].pendingUploadIds.empty())
    {
        m_pendingTextureUploadCompletions.insert(textureRes->GetId());
    }
    else
    {
        m_pendingTextureUploadCompletions.erase(textureRes->GetId());
    }

    SetResourceState(textureRes->GetId(), m_textureGPUData[textureRes->GetId()].isResident ?
                                         GPUResourceState::GPUReady : GPUResourceState::Uploading);

    RVX_CORE_DEBUG("Created texture on GPU: {} ({}x{}, {}KB)", 
                   textureRes->GetName(),
                   metadata.width, metadata.height,
                   gpuData.gpuMemorySize / 1024);
}

void GPUResourceManager::UpdateCompletedResourceUploads()
{
    if (!m_uploadService)
        return;

    std::vector<Resource::ResourceId> completedMeshes;
    for (Resource::ResourceId id : m_pendingMeshUploadCompletions)
    {
        auto it = m_meshGPUData.find(id);
        if (it == m_meshGPUData.end() || it->second.isResident || it->second.pendingUploadIds.empty())
        {
            completedMeshes.push_back(id);
            continue;
        }

        bool allComplete = true;
        auto& data = it->second;
        for (uint64 uploadId : data.pendingUploadIds)
        {
            if (!m_uploadService->IsUploadComplete(uploadId))
            {
                allComplete = false;
                break;
            }
        }

        if (allComplete)
        {
            for (uint64 uploadId : data.pendingUploadIds)
            {
                m_uploadService->ForgetCompletedUpload(uploadId);
            }
            data.pendingUploadIds.clear();
            data.isResident = true;
            data.lastUsedFrame = m_currentFrame;
            m_usedMemory += data.gpuMemorySize;
            SetResourceState(id, GPUResourceState::GPUReady);
            completedMeshes.push_back(id);
        }
    }

    for (Resource::ResourceId id : completedMeshes)
    {
        m_pendingMeshUploadCompletions.erase(id);
    }

    std::vector<Resource::ResourceId> completedTextures;
    for (Resource::ResourceId id : m_pendingTextureUploadCompletions)
    {
        auto it = m_textureGPUData.find(id);
        if (it == m_textureGPUData.end() || it->second.isResident || it->second.pendingUploadIds.empty())
        {
            completedTextures.push_back(id);
            continue;
        }

        bool allComplete = true;
        auto& data = it->second;
        for (uint64 uploadId : data.pendingUploadIds)
        {
            if (!m_uploadService->IsUploadComplete(uploadId))
            {
                allComplete = false;
                break;
            }
        }

        if (allComplete)
        {
            for (uint64 uploadId : data.pendingUploadIds)
            {
                m_uploadService->ForgetCompletedUpload(uploadId);
            }
            data.pendingUploadIds.clear();
            data.isResident = true;
            data.lastUsedFrame = m_currentFrame;
            m_usedMemory += data.gpuMemorySize;
            SetResourceState(id, GPUResourceState::GPUReady);
            completedTextures.push_back(id);
        }
    }

    for (Resource::ResourceId id : completedTextures)
    {
        m_pendingTextureUploadCompletions.erase(id);
    }
}

void GPUResourceManager::AbandonUploadIds(const std::vector<uint64>& uploadIds)
{
    if (!m_uploadService)
        return;

    for (uint64 uploadId : uploadIds)
    {
        m_uploadService->AbandonUpload(uploadId);
    }
}

void GPUResourceManager::NotifyTextureInvalidated(RHITexture* texture)
{
    if (texture && m_textureInvalidatedCallback)
    {
        m_textureInvalidatedCallback(texture);
    }
}

void GPUResourceManager::SetResourceState(Resource::ResourceId id, GPUResourceState state)
{
    if (id == Resource::InvalidResourceId)
        return;

    if (state == GPUResourceState::Failed)
    {
        if (auto meshIt = m_meshGPUData.find(id); meshIt != m_meshGPUData.end())
        {
            AbandonUploadIds(meshIt->second.pendingUploadIds);
            meshIt->second.pendingUploadIds.clear();
        }

        if (auto textureIt = m_textureGPUData.find(id); textureIt != m_textureGPUData.end())
        {
            AbandonUploadIds(textureIt->second.pendingUploadIds);
            textureIt->second.pendingUploadIds.clear();
        }

        m_pendingMeshUploadCompletions.erase(id);
        m_pendingTextureUploadCompletions.erase(id);
    }

    m_resourceStates[id] = state;
}

} // namespace RVX
