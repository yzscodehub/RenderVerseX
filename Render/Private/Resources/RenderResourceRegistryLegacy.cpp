#include "Resources/RenderResourceRegistry.h"
#include "Core/Log.h"
#include "Render/GPUResourceManager.h"
#include "Render/GPUUploadService.h"
#include "RHI/RHICommandContext.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace RVX
{
namespace
{
    struct TextureUploadFormatInfo
    {
        RHIFormat format = RHIFormat::Unknown;
        uint32 blockWidth = 1;
        uint32 blockHeight = 1;
        uint32 bytesPerBlock = 0;
        bool compressed = false;
    };

    TextureUploadFormatInfo ResolveTextureUploadFormat(const RenderTextureUploadMetadata& metadata)
    {
        TextureUploadFormatInfo info;
        switch (metadata.format)
        {
            case RenderTextureUploadFormat::RGBA8:
                info.format = metadata.isSRGB ? RHIFormat::RGBA8_UNORM_SRGB : RHIFormat::RGBA8_UNORM;
                info.bytesPerBlock = 4;
                break;
            case RenderTextureUploadFormat::RGB8:
                info.format = metadata.isSRGB ? RHIFormat::RGBA8_UNORM_SRGB : RHIFormat::RGBA8_UNORM;
                info.bytesPerBlock = 3;
                break;
            case RenderTextureUploadFormat::RG8:
                info.format = RHIFormat::RG8_UNORM;
                info.bytesPerBlock = 2;
                break;
            case RenderTextureUploadFormat::R8:
                info.format = RHIFormat::R8_UNORM;
                info.bytesPerBlock = 1;
                break;
            case RenderTextureUploadFormat::RGBA16F:
                info.format = RHIFormat::RGBA16_FLOAT;
                info.bytesPerBlock = 8;
                break;
            case RenderTextureUploadFormat::RGBA32F:
                info.format = RHIFormat::RGBA32_FLOAT;
                info.bytesPerBlock = 16;
                break;
            case RenderTextureUploadFormat::BC1:
                info.format = metadata.isSRGB ? RHIFormat::BC1_UNORM_SRGB : RHIFormat::BC1_UNORM;
                info.blockWidth = 4;
                info.blockHeight = 4;
                info.bytesPerBlock = 8;
                info.compressed = true;
                break;
            case RenderTextureUploadFormat::BC3:
                info.format = metadata.isSRGB ? RHIFormat::BC3_UNORM_SRGB : RHIFormat::BC3_UNORM;
                info.blockWidth = 4;
                info.blockHeight = 4;
                info.bytesPerBlock = 16;
                info.compressed = true;
                break;
            case RenderTextureUploadFormat::BC5:
                info.format = RHIFormat::BC5_UNORM;
                info.blockWidth = 4;
                info.blockHeight = 4;
                info.bytesPerBlock = 16;
                info.compressed = true;
                break;
            case RenderTextureUploadFormat::BC7:
                info.format = metadata.isSRGB ? RHIFormat::BC7_UNORM_SRGB : RHIFormat::BC7_UNORM;
                info.blockWidth = 4;
                info.blockHeight = 4;
                info.bytesPerBlock = 16;
                info.compressed = true;
                break;
            default:
                break;
        }

        return info;
    }
} // namespace

struct RenderResourceRegistry::LegacyResourceState
{
    using Stats = LegacyGPUResourceStats;

    struct MeshGPUData
    {
        RHIBufferRef positionBuffer;
        RHIBufferRef normalBuffer;
        RHIBufferRef uvBuffer;
        RHIBufferRef tangentBuffer;
        RHIBufferRef boneIndicesBuffer;
        RHIBufferRef boneWeightsBuffer;
        RHIBufferRef indexBuffer;
        std::vector<SubmeshGPUInfo> submeshes;
        std::vector<uint64> pendingUploadIds;
        uint64 lastUsedFrame = 0;
        size_t gpuMemorySize = 0;
        bool isResident = false;
        bool hasNormals = false;
        bool hasUVs = false;
        bool hasTangents = false;
        bool hasBoneIndices = false;
        bool hasBoneWeights = false;
    };

    struct TextureGPUData
    {
        RHITextureRef texture;
        std::vector<uint64> pendingUploadIds;
        uint64 lastUsedFrame = 0;
        size_t gpuMemorySize = 0;
        RHIResourceState currentState = RHIResourceState::Common;
        bool isResident = false;
    };

    struct PendingUpload
    {
        uint64 id = 0;
        UploadPriority priority = UploadPriority::Normal;
        Ref<RefCounted> retainedResource;

        bool operator<(const PendingUpload& other) const
        {
            return static_cast<int>(priority) <
                   static_cast<int>(other.priority);
        }
    };

    struct PreparedTextureUpload
    {
        RHITextureDesc textureDesc;
        std::vector<uint8> data;
        std::string debugName;
        bool valid = false;
    };

    LegacyResourceState() = default;
    ~LegacyResourceState();

    void Initialize(IRHIDevice* device);
    void Shutdown();
    void RequestUpload(IRenderMeshUploadSource* mesh, UploadPriority priority);
    void RequestUpload(IRenderTextureUploadSource* texture,
                       UploadPriority priority);
    void UploadImmediate(IRenderMeshUploadSource* mesh);
    void UploadImmediate(IRenderTextureUploadSource* texture);
    MeshGPUBuffers GetMeshBuffers(uint64 meshId) const;
    bool IsResident(uint64 id) const;
    bool IsResident(IRenderTextureUploadSource* texture) const;
    GPUResourceState GetResourceState(uint64 id) const;
    RHITexture* GetTexture(uint64 textureId) const;
    RHITexture* GetTexture(IRenderTextureUploadSource* texture) const;
    bool IsGPUReady(IRenderTextureUploadSource* texture) const;
    bool TransitionTexture(uint64 textureId,
                           RHICommandContext& context,
                           RHIResourceState desiredState);
    void ProcessPendingUploads(float timeBudgetMs);
    void MarkUsed(uint64 id);
    void MarkUsed(IRenderTextureUploadSource* texture);
    void EvictUnused(uint64 currentFrame, uint64 frameThreshold);
    void SetMemoryBudget(size_t bytes);
    Stats GetStats() const;
    void UploadMesh(IRenderMeshUploadSource* mesh);
    void UploadTexture(IRenderTextureUploadSource* texture);
    PreparedTextureUpload PrepareTextureUpload(
        const IRenderTextureUploadSource& texture) const;
    void ReleaseTextureGPUData(uint64 id);
    size_t RemoveQueuedUploadRequests(uint64 id);
    void UpdateCompletedResourceUploads();
    void AbandonUploadIds(const std::vector<uint64>& uploadIds);
    void NotifyTextureInvalidated(RHITexture* texture);
    void SetResourceState(uint64 id, GPUResourceState state);

    IRHIDevice* m_device = nullptr;
    std::priority_queue<PendingUpload> m_pendingQueue;
    std::unordered_map<uint64, MeshGPUData> m_meshGPUData;
    std::unordered_map<uint64, TextureGPUData> m_textureGPUData;
    std::unordered_map<uint64, GPUResourceState> m_resourceStates;
    std::unordered_set<uint64> m_pendingMeshUploadCompletions;
    std::unordered_set<uint64> m_pendingTextureUploadCompletions;
    std::unique_ptr<GPUUploadService> m_uploadService;
    std::function<void(RHITexture*)> m_textureInvalidatedCallback;
    size_t m_usedMemory = 0;
    size_t m_memoryBudget = 512 * 1024 * 1024;
    uint64 m_currentFrame = 0;
};

RenderResourceRegistry::LegacyResourceState::~LegacyResourceState()
{
    Shutdown();
}

void RenderResourceRegistry::LegacyResourceState::Initialize(IRHIDevice* device)
{
    m_device = device;
    m_uploadService = std::make_unique<GPUUploadService>();
    m_uploadService->Initialize(device);
    RVX_CORE_INFO("GPUResourceManager initialized with {}MB budget", m_memoryBudget / (1024 * 1024));
}

void RenderResourceRegistry::LegacyResourceState::Shutdown()
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

void RenderResourceRegistry::LegacyResourceState::RequestUpload(IRenderMeshUploadSource* mesh, UploadPriority priority)
{
    if (!mesh || !m_device)
        return;

    if (mesh->GetRenderResourceRefCount() == 0)
    {
        RVX_CORE_WARN("GPUResourceManager: Ignoring async upload for unmanaged mesh '{}'",
                      mesh->GetRenderResourceName());
        return;
    }

    const uint64 id = mesh->GetRenderResourceId();

    // Skip if already resident
    if (IsResident(id))
        return;

    GPUResourceState currentState = GetResourceState(id);
    if (currentState == GPUResourceState::UploadQueued || currentState == GPUResourceState::Uploading)
        return;

    PendingUpload upload;
    upload.id = id;
    upload.priority = priority;
    upload.retainedResource.Reset(mesh->GetRenderResourceRefCounted());

    SetResourceState(id, GPUResourceState::UploadQueued);
    m_pendingQueue.push(upload);
}

void RenderResourceRegistry::LegacyResourceState::RequestUpload(IRenderTextureUploadSource* texture, UploadPriority priority)
{
    if (!texture || !m_device)
        return;

    if (texture->GetRenderResourceRefCount() == 0)
    {
        RVX_CORE_WARN("GPUResourceManager: Ignoring async upload for unmanaged texture '{}'",
                      texture->GetRenderResourceName());
        return;
    }

    const uint64 id = texture->GetRenderResourceId();

    // Skip if already resident
    if (IsResident(id))
        return;

    GPUResourceState currentState = GetResourceState(id);
    if (currentState == GPUResourceState::UploadQueued || currentState == GPUResourceState::Uploading)
        return;

    PendingUpload upload;
    upload.id = id;
    upload.priority = priority;
    upload.retainedResource.Reset(texture->GetRenderResourceRefCounted());

    SetResourceState(id, GPUResourceState::UploadQueued);
    m_pendingQueue.push(upload);
}

void RenderResourceRegistry::LegacyResourceState::UploadImmediate(IRenderMeshUploadSource* mesh)
{
    if (!mesh || !m_device)
        return;

    // Skip if already resident
    if (IsResident(mesh->GetRenderResourceId()))
        return;

    UploadMesh(mesh);
    if (m_uploadService)
    {
        m_uploadService->FlushAndWaitForUploads();
        UpdateCompletedResourceUploads();
    }
}

void RenderResourceRegistry::LegacyResourceState::UploadImmediate(IRenderTextureUploadSource* texture)
{
    if (!texture || !m_device)
        return;

    const size_t removedQueuedUploads = RemoveQueuedUploadRequests(texture->GetRenderResourceId());
    if (removedQueuedUploads > 0)
    {
        RVX_CORE_DEBUG("GPUResourceManager: Removed {} queued texture upload(s) before immediate upload of resource {}",
                       removedQueuedUploads,
                       texture->GetRenderResourceId());
    }

    // Match mesh semantics: UploadImmediate is a blocking residency request, not
    // an implicit same-id refresh path. Explicit refresh can be added as a
    // separate API when callers need to replace resident GPU data.
    if (IsResident(texture->GetRenderResourceId()))
        return;

    UploadTexture(texture);
    if (m_uploadService)
    {
        m_uploadService->FlushAndWaitForUploads();
        UpdateCompletedResourceUploads();
    }
}

MeshGPUBuffers RenderResourceRegistry::LegacyResourceState::GetMeshBuffers(uint64 meshId) const
{
    MeshGPUBuffers result;

    auto it = m_meshGPUData.find(meshId);
    if (it != m_meshGPUData.end() && it->second.isResident)
    {
        result.positionBuffer = it->second.positionBuffer.Get();
        result.normalBuffer = it->second.normalBuffer.Get();
        result.uvBuffer = it->second.uvBuffer.Get();
        result.tangentBuffer = it->second.tangentBuffer.Get();
        result.boneIndicesBuffer = it->second.boneIndicesBuffer.Get();
        result.boneWeightsBuffer = it->second.boneWeightsBuffer.Get();
        result.indexBuffer = it->second.indexBuffer.Get();
        result.submeshes = it->second.submeshes;
        result.isResident = true;
        result.hasNormals = it->second.hasNormals;
        result.hasUVs = it->second.hasUVs;
        result.hasTangents = it->second.hasTangents;
        result.hasBoneIndices = it->second.hasBoneIndices;
        result.hasBoneWeights = it->second.hasBoneWeights;
    }

    return result;
}

bool RenderResourceRegistry::LegacyResourceState::IsResident(uint64 id) const
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

bool RenderResourceRegistry::LegacyResourceState::IsResident(IRenderTextureUploadSource* texture) const
{
    return texture && IsResident(texture->GetRenderResourceId());
}

GPUResourceState RenderResourceRegistry::LegacyResourceState::GetResourceState(uint64 id) const
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

RHITexture* RenderResourceRegistry::LegacyResourceState::GetTexture(uint64 textureId) const
{
    auto it = m_textureGPUData.find(textureId);
    if (it != m_textureGPUData.end() && it->second.isResident)
    {
        return it->second.texture.Get();
    }
    return nullptr;
}

RHITexture* RenderResourceRegistry::LegacyResourceState::GetTexture(IRenderTextureUploadSource* texture) const
{
    return texture ? GetTexture(texture->GetRenderResourceId()) : nullptr;
}

bool RenderResourceRegistry::LegacyResourceState::IsGPUReady(IRenderTextureUploadSource* texture) const
{
    return texture &&
           GetResourceState(texture->GetRenderResourceId()) ==
               GPUResourceState::GPUReady;
}

bool RenderResourceRegistry::LegacyResourceState::TransitionTexture(uint64 textureId, RHICommandContext& ctx, RHIResourceState desiredState)
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

void RenderResourceRegistry::LegacyResourceState::ProcessPendingUploads(float timeBudgetMs)
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
        RefCounted* resource = upload.retainedResource.Get();
        if (!resource)
        {
            SetResourceState(upload.id, GPUResourceState::Failed);
            continue;
        }

        if (auto* mesh = dynamic_cast<IRenderMeshUploadSource*>(resource))
        {
            UploadMesh(mesh);
        }
        else if (auto* texture = dynamic_cast<IRenderTextureUploadSource*>(resource))
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

void RenderResourceRegistry::LegacyResourceState::MarkUsed(uint64 id)
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

void RenderResourceRegistry::LegacyResourceState::MarkUsed(IRenderTextureUploadSource* texture)
{
    if (texture)
    {
        MarkUsed(texture->GetRenderResourceId());
    }
}

void RenderResourceRegistry::LegacyResourceState::EvictUnused(uint64_t currentFrame, uint64_t frameThreshold)
{
    std::vector<uint64> meshesToEvict;
    std::vector<uint64> texturesToEvict;

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
    for (uint64 id : meshesToEvict)
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
    for (uint64 id : texturesToEvict)
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

void RenderResourceRegistry::LegacyResourceState::SetMemoryBudget(size_t bytes)
{
    m_memoryBudget = bytes;
}

RenderResourceRegistry::LegacyResourceState::Stats RenderResourceRegistry::LegacyResourceState::GetStats() const
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

void RenderResourceRegistry::LegacyResourceState::UploadMesh(IRenderMeshUploadSource* meshRes)
{
    if (!meshRes || !m_device)
    {
        RVX_CORE_WARN("RenderResourceRegistry::LegacyResourceState::UploadMesh - invalid meshRes ({}) or device ({})",
                      meshRes ? "valid" : "null", m_device ? "valid" : "null");
        return;
    }

    const uint64 meshId = meshRes->GetRenderResourceId();
    const std::string_view meshName = meshRes->GetRenderResourceName();

    SetResourceState(meshId, GPUResourceState::Uploading);

    RVX_CORE_DEBUG("GPUResourceManager: Uploading mesh '{}' (ID: {})",
                   meshName, meshId);

    const RenderMeshUploadData uploadData = meshRes->GetRenderMeshUploadData();
    if (uploadData.vertexCount == 0 || !uploadData.position.IsValid())
    {
        RVX_CORE_WARN("MeshResource has no uploadable mesh data: {}", meshName);
        SetResourceState(meshId, GPUResourceState::Failed);
        return;
    }

    RVX_CORE_TRACE("  Vertex count: {}, Index count: {}",
                   uploadData.vertexCount, uploadData.indexCount);

    MeshGPUData gpuData;
    size_t totalMemory = 0;

    const bool rayTracingGeometryInputEnabled = m_device->GetCapabilities().supportsRaytracing;
    const RHIBufferUsage rayTracingInputUsage =
        RHIBufferUsage::AccelerationStructureInput |
        RHIBufferUsage::DeviceAddress |
        RHIBufferUsage::ShaderResource;

    // Helper lambda to create and upload an attribute buffer
    auto createAttributeBuffer = [this, &gpuData, &totalMemory, rayTracingGeometryInputEnabled, rayTracingInputUsage](
        const RenderMeshAttributeUploadView& attribute,
        const char* name) -> RHIBufferRef
    {
        if (!attribute.IsValid())
            return nullptr;

        GPUUploadBufferDesc desc;
        desc.size = attribute.size;
        desc.usage = RHIBufferUsage::Vertex;
        if (rayTracingGeometryInputEnabled)
        {
            desc.usage = desc.usage | rayTracingInputUsage;
        }
        desc.stride = static_cast<uint32_t>(attribute.stride);  // CRITICAL: Set stride for vertex buffer view
        desc.debugName = name;

        auto result = m_uploadService->UploadBufferDataWithResult(desc, attribute.data, attribute.size);
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
    gpuData.positionBuffer = createAttributeBuffer(uploadData.position, "PositionBuffer");
    if (!gpuData.positionBuffer)
    {
        RVX_CORE_ERROR("Failed to create position buffer for mesh: {}", meshName);
        AbandonUploadIds(gpuData.pendingUploadIds);
        SetResourceState(meshId, GPUResourceState::Failed);
        return;
    }
    // Slot 1: Normal (optional)
    if (uploadData.normal.IsValid())
    {
        gpuData.normalBuffer = createAttributeBuffer(uploadData.normal, "NormalBuffer");
        gpuData.hasNormals = (gpuData.normalBuffer != nullptr);
    }

    // Slot 2: UV (optional)
    if (uploadData.uv.IsValid())
    {
        gpuData.uvBuffer = createAttributeBuffer(uploadData.uv, "UVBuffer");
        gpuData.hasUVs = (gpuData.uvBuffer != nullptr);
    }

    // Slot 3: Tangent (optional)
    if (uploadData.tangent.IsValid())
    {
        gpuData.tangentBuffer = createAttributeBuffer(uploadData.tangent, "TangentBuffer");
        gpuData.hasTangents = (gpuData.tangentBuffer != nullptr);
    }

    // Slot 4: Bone indices (optional)
    if (uploadData.boneIndices.IsValid())
    {
        gpuData.boneIndicesBuffer = createAttributeBuffer(uploadData.boneIndices, "BoneIndicesBuffer");
        gpuData.hasBoneIndices = (gpuData.boneIndicesBuffer != nullptr);
    }

    // Slot 5: Bone weights (optional)
    if (uploadData.boneWeights.IsValid())
    {
        gpuData.boneWeightsBuffer = createAttributeBuffer(uploadData.boneWeights, "BoneWeightsBuffer");
        gpuData.hasBoneWeights = (gpuData.boneWeightsBuffer != nullptr);
    }

    // Create index buffer
    if (!uploadData.HasIndexData())
    {
        RVX_CORE_ERROR("Mesh has no index data: {}", meshName);
        AbandonUploadIds(gpuData.pendingUploadIds);
        SetResourceState(meshId, GPUResourceState::Failed);
        return;
    }

    GPUUploadBufferDesc ibDesc;
    ibDesc.size = uploadData.indexDataSize;
    ibDesc.usage = RHIBufferUsage::Index;
    if (rayTracingGeometryInputEnabled)
    {
        ibDesc.usage = ibDesc.usage | rayTracingInputUsage;
    }
    ibDesc.debugName = "MeshIndexBuffer";

    auto indexUpload = m_uploadService->UploadBufferDataWithResult(
        ibDesc,
        uploadData.indexData,
        uploadData.indexDataSize);
    gpuData.indexBuffer = indexUpload.resource;
    if (!indexUpload)
    {
        RVX_CORE_ERROR("Failed to create index buffer for mesh: {}", meshName);
        AbandonUploadIds(gpuData.pendingUploadIds);
        SetResourceState(meshId, GPUResourceState::Failed);
        return;
    }

    if (indexUpload.isPending)
    {
        gpuData.pendingUploadIds.push_back(indexUpload.uploadId);
    }

    totalMemory += indexUpload.bytesUploaded;

    // Collect submesh info
    for (const auto& submesh : uploadData.submeshes)
    {
        SubmeshGPUInfo info;
        info.indexOffset = submesh.indexOffset;
        info.indexCount = submesh.indexCount;
        info.baseVertex = submesh.baseVertex;
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
    bool hasSkin = gpuData.hasBoneIndices && gpuData.hasBoneWeights;

    m_meshGPUData[meshId] = std::move(gpuData);
    if (!m_meshGPUData[meshId].pendingUploadIds.empty())
    {
        m_pendingMeshUploadCompletions.insert(meshId);
    }
    else
    {
        m_pendingMeshUploadCompletions.erase(meshId);
    }

    SetResourceState(meshId, hasPos && m_meshGPUData[meshId].isResident ?
                             GPUResourceState::GPUReady : GPUResourceState::Uploading);

    RVX_CORE_DEBUG("Uploaded mesh to GPU: {} ({}KB, pos:{} norm:{} uv:{} tan:{} skin:{})",
                   meshName,
                   totalMemory / 1024,
                   hasPos ? "yes" : "no",
                   hasNorm ? "yes" : "no",
                   hasUV ? "yes" : "no",
                   hasTan ? "yes" : "no",
                   hasSkin ? "yes" : "no");
}

RenderResourceRegistry::LegacyResourceState::PreparedTextureUpload RenderResourceRegistry::LegacyResourceState::PrepareTextureUpload(
    const IRenderTextureUploadSource& texture) const
{
    PreparedTextureUpload prepared;

    const RenderTextureUploadData uploadData = texture.GetRenderTextureUploadData();
    const RenderTextureUploadMetadata& metadata = uploadData.metadata;
    const uint8* sourceData = uploadData.data;
    if (metadata.width == 0 || metadata.height == 0 || metadata.depth == 0 ||
        metadata.mipLevels == 0 || metadata.arrayLayers == 0 || !uploadData.HasData())
    {
        return prepared;
    }

    if (metadata.depth != 1)
    {
        return prepared;
    }

    TextureUploadFormatInfo uploadFormat = ResolveTextureUploadFormat(metadata);
    if (uploadFormat.format == RHIFormat::Unknown || uploadFormat.bytesPerBlock == 0)
    {
        return prepared;
    }

    if (metadata.format == RenderTextureUploadFormat::RGB8 &&
        (metadata.isCubemap || metadata.isArray || metadata.mipLevels != 1 || metadata.arrayLayers != 1))
    {
        return prepared;
    }

    RHITextureDimension dimension = RHITextureDimension::Texture2D;
    uint32 logicalArraySize = std::max(1u, metadata.arrayLayers);
    if (metadata.isCubemap)
    {
        if (metadata.arrayLayers == 0 || metadata.arrayLayers % 6 != 0)
        {
            return prepared;
        }

        dimension = RHITextureDimension::TextureCube;
        logicalArraySize = metadata.arrayLayers / 6;
    }
    else if (!metadata.isArray && metadata.arrayLayers != 1)
    {
        return prepared;
    }

    const uint32 mipLevels = std::max(1u, metadata.mipLevels);
    const uint32 physicalLayerCount = metadata.isCubemap ? metadata.arrayLayers : logicalArraySize;

    std::vector<uint64> mipSizes;
    std::vector<uint64> mipOffsets;
    mipSizes.reserve(mipLevels);
    mipOffsets.reserve(mipLevels);

    uint64 expectedSourceSize = 0;
    for (uint32 mipLevel = 0; mipLevel < mipLevels; ++mipLevel)
    {
        const uint32 mipWidth = std::max(1u, metadata.width >> mipLevel);
        const uint32 mipHeight = std::max(1u, metadata.height >> mipLevel);
        const uint32 rowBlockCount = (mipWidth + uploadFormat.blockWidth - 1u) / uploadFormat.blockWidth;
        const uint32 rowCount = (mipHeight + uploadFormat.blockHeight - 1u) / uploadFormat.blockHeight;
        const uint64 blockCount = static_cast<uint64>(rowBlockCount) * rowCount;
        if (blockCount > (std::numeric_limits<uint64>::max() / uploadFormat.bytesPerBlock))
        {
            return prepared;
        }

        const uint64 mipSize = blockCount * uploadFormat.bytesPerBlock;
        mipOffsets.push_back(expectedSourceSize);
        mipSizes.push_back(mipSize);

        if (physicalLayerCount != 0 &&
            mipSize > (std::numeric_limits<uint64>::max() - expectedSourceSize) / physicalLayerCount)
        {
            return prepared;
        }

        expectedSourceSize += mipSize * physicalLayerCount;
    }

    if (uploadData.dataSize != expectedSourceSize)
    {
        return prepared;
    }

    prepared.textureDesc.width = metadata.width;
    prepared.textureDesc.height = metadata.height;
    prepared.textureDesc.depth = 1;
    prepared.textureDesc.mipLevels = mipLevels;
    prepared.textureDesc.arraySize = logicalArraySize;
    prepared.textureDesc.usage = RHITextureUsage::ShaderResource;
    prepared.textureDesc.format = uploadFormat.format;
    prepared.textureDesc.dimension = dimension;
    prepared.debugName = std::string(texture.GetRenderResourceName());

    if (metadata.format == RenderTextureUploadFormat::RGB8)
    {
        const uint64 pixelCount = static_cast<uint64>(metadata.width) * metadata.height;
        prepared.data.resize(static_cast<size_t>(pixelCount) * 4);
        const uint8* src = sourceData;
        auto* dst = prepared.data.data();
        for (uint64 i = 0; i < pixelCount; ++i)
        {
            dst[i * 4 + 0] = src[i * 3 + 0];
            dst[i * 4 + 1] = src[i * 3 + 1];
            dst[i * 4 + 2] = src[i * 3 + 2];
            dst[i * 4 + 3] = 255;
        }
    }
    else if (metadata.isCubemap && mipLevels > 1)
    {
        prepared.data.resize(static_cast<size_t>(expectedSourceSize));

        uint64 dstOffset = 0;
        for (uint32 physicalLayer = 0; physicalLayer < physicalLayerCount; ++physicalLayer)
        {
            for (uint32 mipLevel = 0; mipLevel < mipLevels; ++mipLevel)
            {
                const uint64 mipSize = mipSizes[mipLevel];
                const uint64 srcOffset = mipOffsets[mipLevel] + static_cast<uint64>(physicalLayer) * mipSize;
                std::memcpy(prepared.data.data() + dstOffset,
                            sourceData + srcOffset,
                            static_cast<size_t>(mipSize));
                dstOffset += mipSize;
            }
        }
    }
    else
    {
        prepared.data.assign(sourceData, sourceData + uploadData.dataSize);
    }

    prepared.valid = true;
    return prepared;
}

void RenderResourceRegistry::LegacyResourceState::UploadTexture(IRenderTextureUploadSource* textureRes)
{
    if (!textureRes || !m_device)
        return;

    const uint64 textureId = textureRes->GetRenderResourceId();
    const std::string_view textureName = textureRes->GetRenderResourceName();

    SetResourceState(textureId, GPUResourceState::Uploading);

    TextureGPUData gpuData;
    PreparedTextureUpload prepared = PrepareTextureUpload(*textureRes);
    if (!prepared.valid)
    {
        RVX_CORE_WARN("TextureResource has unsupported or inconsistent upload data: {}", textureName);
        ReleaseTextureGPUData(textureId);
        SetResourceState(textureId, GPUResourceState::Failed);
        return;
    }
    prepared.textureDesc.debugName = prepared.debugName.c_str();

    GPUUploadTextureDesc uploadDesc;
    uploadDesc.textureDesc = prepared.textureDesc;
    uploadDesc.dataSize = prepared.data.size();

    auto textureUpload = m_uploadService->UploadTextureDataWithResult(uploadDesc, prepared.data.data());
    gpuData.texture = textureUpload.resource;
    if (!textureUpload)
    {
        RVX_CORE_ERROR("Failed to create GPU texture for: {}", textureName);
        ReleaseTextureGPUData(textureId);
        SetResourceState(textureId, GPUResourceState::Failed);
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

    const size_t uploadedMemory = gpuData.gpuMemorySize;
    const uint32 uploadedWidth = prepared.textureDesc.width;
    const uint32 uploadedHeight = prepared.textureDesc.height;

    ReleaseTextureGPUData(textureId);
    m_textureGPUData[textureId] = std::move(gpuData);
    if (!m_textureGPUData[textureId].pendingUploadIds.empty())
    {
        m_pendingTextureUploadCompletions.insert(textureId);
    }
    else
    {
        m_pendingTextureUploadCompletions.erase(textureId);
    }

    SetResourceState(textureId, m_textureGPUData[textureId].isResident ?
                                GPUResourceState::GPUReady : GPUResourceState::Uploading);

    RVX_CORE_DEBUG("Created texture on GPU: {} ({}x{}, {}KB)",
                   textureName,
                   uploadedWidth, uploadedHeight,
                   uploadedMemory / 1024);
}

void RenderResourceRegistry::LegacyResourceState::ReleaseTextureGPUData(uint64 id)
{
    auto it = m_textureGPUData.find(id);
    if (it == m_textureGPUData.end())
        return;

    NotifyTextureInvalidated(it->second.texture.Get());
    AbandonUploadIds(it->second.pendingUploadIds);
    if (it->second.isResident && it->second.gpuMemorySize <= m_usedMemory)
    {
        m_usedMemory -= it->second.gpuMemorySize;
    }

    m_pendingTextureUploadCompletions.erase(id);
    m_textureGPUData.erase(it);
}

size_t RenderResourceRegistry::LegacyResourceState::RemoveQueuedUploadRequests(uint64 id)
{
    if (id == 0 || m_pendingQueue.empty())
        return 0;

    std::vector<PendingUpload> retainedUploads;
    retainedUploads.reserve(m_pendingQueue.size());
    size_t removedCount = 0;

    while (!m_pendingQueue.empty())
    {
        PendingUpload upload = m_pendingQueue.top();
        m_pendingQueue.pop();

        if (upload.id == id)
        {
            ++removedCount;
            continue;
        }

        retainedUploads.push_back(std::move(upload));
    }

    for (PendingUpload& upload : retainedUploads)
    {
        m_pendingQueue.push(std::move(upload));
    }

    return removedCount;
}

void RenderResourceRegistry::LegacyResourceState::UpdateCompletedResourceUploads()
{
    if (!m_uploadService)
        return;

    std::vector<uint64> completedMeshes;
    for (uint64 id : m_pendingMeshUploadCompletions)
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

    for (uint64 id : completedMeshes)
    {
        m_pendingMeshUploadCompletions.erase(id);
    }

    std::vector<uint64> completedTextures;
    for (uint64 id : m_pendingTextureUploadCompletions)
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

    for (uint64 id : completedTextures)
    {
        m_pendingTextureUploadCompletions.erase(id);
    }
}

void RenderResourceRegistry::LegacyResourceState::AbandonUploadIds(const std::vector<uint64>& uploadIds)
{
    if (!m_uploadService)
        return;

    for (uint64 uploadId : uploadIds)
    {
        m_uploadService->AbandonUpload(uploadId);
    }
}

void RenderResourceRegistry::LegacyResourceState::NotifyTextureInvalidated(RHITexture* texture)
{
    if (texture && m_textureInvalidatedCallback)
    {
        m_textureInvalidatedCallback(texture);
    }
}

void RenderResourceRegistry::LegacyResourceState::SetResourceState(uint64 id, GPUResourceState state)
{
    if (id == 0)
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

void RenderResourceRegistry::InitializeLegacy(IRHIDevice* device)
{
    ShutdownLegacy();
    if (device == nullptr)
    {
        return;
    }
    m_legacy = new LegacyResourceState();
    m_legacy->Initialize(device);
    if (m_legacy->m_device == nullptr || !m_legacy->m_uploadService ||
        !m_legacy->m_uploadService->IsInitialized())
    {
        delete m_legacy;
        m_legacy = nullptr;
    }
}

void RenderResourceRegistry::ShutdownLegacy()
{
    if (m_legacy != nullptr)
    {
        m_legacy->Shutdown();
        delete m_legacy;
        m_legacy = nullptr;
    }
}

bool RenderResourceRegistry::IsLegacyInitialized() const
{
    return m_legacy != nullptr && m_legacy->m_device != nullptr;
}

void RenderResourceRegistry::RequestLegacyUpload(
    IRenderMeshUploadSource* mesh,
    UploadPriority priority)
{
    if (m_legacy)
    {
        m_legacy->RequestUpload(mesh, priority);
    }
}

void RenderResourceRegistry::RequestLegacyUpload(
    IRenderTextureUploadSource* texture,
    UploadPriority priority)
{
    if (m_legacy)
    {
        m_legacy->RequestUpload(texture, priority);
    }
}

void RenderResourceRegistry::UploadLegacyImmediate(
    IRenderMeshUploadSource* mesh)
{
    if (m_legacy)
    {
        m_legacy->UploadImmediate(mesh);
    }
}

void RenderResourceRegistry::UploadLegacyImmediate(
    IRenderTextureUploadSource* texture)
{
    if (m_legacy)
    {
        m_legacy->UploadImmediate(texture);
    }
}

void RenderResourceRegistry::SetLegacyTextureInvalidatedCallback(
    std::function<void(RHITexture*)> callback)
{
    if (m_legacy)
    {
        m_legacy->m_textureInvalidatedCallback = std::move(callback);
    }
}

MeshGPUBuffers RenderResourceRegistry::GetLegacyMeshBuffers(
    uint64 meshId) const
{
    return m_legacy ? m_legacy->GetMeshBuffers(meshId) : MeshGPUBuffers{};
}

RHITexture* RenderResourceRegistry::GetLegacyTexture(uint64 textureId) const
{
    return m_legacy ? m_legacy->GetTexture(textureId) : nullptr;
}

RHITexture* RenderResourceRegistry::GetLegacyTexture(
    IRenderTextureUploadSource* texture) const
{
    return m_legacy ? m_legacy->GetTexture(texture) : nullptr;
}

bool RenderResourceRegistry::TransitionLegacyTexture(
    uint64 textureId,
    RHICommandContext& context,
    RHIResourceState desiredState)
{
    return m_legacy &&
           m_legacy->TransitionTexture(textureId, context, desiredState);
}

bool RenderResourceRegistry::IsLegacyResident(uint64 id) const
{
    return m_legacy && m_legacy->IsResident(id);
}

bool RenderResourceRegistry::IsLegacyResident(
    IRenderTextureUploadSource* texture) const
{
    return m_legacy && m_legacy->IsResident(texture);
}

GPUResourceState RenderResourceRegistry::GetLegacyResourceState(
    uint64 id) const
{
    return m_legacy ? m_legacy->GetResourceState(id)
                    : GPUResourceState::Unloaded;
}

bool RenderResourceRegistry::IsLegacyGPUReady(
    IRenderTextureUploadSource* texture) const
{
    return m_legacy && m_legacy->IsGPUReady(texture);
}

void RenderResourceRegistry::ProcessLegacyPendingUploads(float timeBudgetMs)
{
    if (m_legacy)
    {
        m_legacy->ProcessPendingUploads(timeBudgetMs);
    }
}

void RenderResourceRegistry::MarkLegacyUsed(uint64 id)
{
    if (m_legacy)
    {
        m_legacy->MarkUsed(id);
    }
}

void RenderResourceRegistry::MarkLegacyUsed(
    IRenderTextureUploadSource* texture)
{
    if (m_legacy)
    {
        m_legacy->MarkUsed(texture);
    }
}

void RenderResourceRegistry::EvictLegacyUnused(
    uint64 currentFrame,
    uint64 frameThreshold)
{
    if (m_legacy)
    {
        m_legacy->EvictUnused(currentFrame, frameThreshold);
    }
}

void RenderResourceRegistry::SetLegacyMemoryBudget(size_t bytes)
{
    if (m_legacy)
    {
        m_legacy->SetMemoryBudget(bytes);
    }
}

size_t RenderResourceRegistry::GetLegacyUsedMemory() const
{
    return m_legacy ? m_legacy->m_usedMemory : 0;
}

size_t RenderResourceRegistry::GetLegacyMemoryBudget() const
{
    return m_legacy ? m_legacy->m_memoryBudget : 0;
}

LegacyGPUResourceStats RenderResourceRegistry::GetLegacyStats() const
{
    return m_legacy ? m_legacy->GetStats() : LegacyGPUResourceStats{};
}

} // namespace RVX
