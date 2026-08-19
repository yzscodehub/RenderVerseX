#include "Render/RayTracing/RayTracingSceneManager.h"

#include "RHI/RHICommandContext.h"
#include "Resources/RenderOwnerSnapshotRetirement.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <utility>

namespace RVX
{
namespace
{
    constexpr uint64 RVX_RAY_TRACING_INSTANCE_BUFFER_ALIGNMENT = 256;

    bool TryMultiplyUint64(uint64 lhs, uint64 rhs, uint64& result)
    {
        if (lhs != 0 && rhs > std::numeric_limits<uint64>::max() / lhs)
        {
            return false;
        }

        result = lhs * rhs;
        return true;
    }

    bool TryAlignUpUint64(uint64 value, uint64 alignment, uint64& result)
    {
        if (alignment == 0)
        {
            result = value;
            return true;
        }

        if (value > std::numeric_limits<uint64>::max() - (alignment - 1))
        {
            return false;
        }

        const uint64 biasedValue = value + alignment - 1;
        result = (biasedValue / alignment) * alignment;
        return true;
    }

    uint64 AddSaturatingUint64(uint64 lhs, uint64 rhs, bool& overflowed)
    {
        if (lhs > std::numeric_limits<uint64>::max() - rhs)
        {
            overflowed = true;
            return std::numeric_limits<uint64>::max();
        }

        return lhs + rhs;
    }

    bool AreGeometriesEquivalent(const RHIRayTracingGeometryDesc& lhs,
                                 const RHIRayTracingGeometryDesc& rhs)
    {
        if (lhs.type != rhs.type || lhs.flags != rhs.flags)
            return false;

        if (lhs.type == RHIRayTracingGeometryType::Triangles)
        {
            return lhs.triangles.vertexBuffer == rhs.triangles.vertexBuffer &&
                   lhs.triangles.vertexOffset == rhs.triangles.vertexOffset &&
                   lhs.triangles.vertexStride == rhs.triangles.vertexStride &&
                   lhs.triangles.vertexFormat == rhs.triangles.vertexFormat &&
                   lhs.triangles.vertexCount == rhs.triangles.vertexCount &&
                   lhs.triangles.indexBuffer == rhs.triangles.indexBuffer &&
                   lhs.triangles.indexOffset == rhs.triangles.indexOffset &&
                   lhs.triangles.indexFormat == rhs.triangles.indexFormat &&
                   lhs.triangles.indexCount == rhs.triangles.indexCount &&
                   lhs.triangles.transformBuffer == rhs.triangles.transformBuffer &&
                   lhs.triangles.transformOffset == rhs.triangles.transformOffset;
        }

        return lhs.aabbs.aabbBuffer == rhs.aabbs.aabbBuffer &&
               lhs.aabbs.offset == rhs.aabbs.offset &&
               lhs.aabbs.stride == rhs.aabbs.stride &&
               lhs.aabbs.count == rhs.aabbs.count;
    }

    uint32 FindOrAddAlphaTextureIndex(
        std::vector<uint64>& textureIds,
        std::unordered_map<uint64, uint32>& textureIndexLookup,
        uint64 textureId)
    {
        if (textureId == 0)
        {
            return RVX_INVALID_INDEX;
        }

        const auto it = textureIndexLookup.find(textureId);
        if (it != textureIndexLookup.end())
        {
            return it->second;
        }

        const uint32 index = static_cast<uint32>(textureIds.size());
        textureIds.push_back(textureId);
        textureIndexLookup.emplace(textureId, index);
        return index;
    }

    uint32 FindOrAddMaterialTextureIndex(
        std::vector<uint64>& textureIds,
        std::unordered_map<uint64, uint32>& textureIndexLookup,
        uint64 textureId)
    {
        if (textureId == 0)
        {
            return RVX_INVALID_INDEX;
        }

        const auto it = textureIndexLookup.find(textureId);
        if (it != textureIndexLookup.end())
        {
            return it->second;
        }

        const uint32 index = static_cast<uint32>(textureIds.size());
        textureIds.push_back(textureId);
        textureIndexLookup.emplace(textureId, index);
        return index;
    }

    uint32 FindOrAddAlphaBufferIndex(std::vector<RHIBuffer*>& buffers,
                                     std::unordered_map<RHIBuffer*, uint32>& bufferIndexLookup,
                                     RHIBuffer* buffer)
    {
        if (!buffer)
        {
            return RVX_INVALID_INDEX;
        }

        const auto it = bufferIndexLookup.find(buffer);
        if (it != bufferIndexLookup.end())
        {
            return it->second;
        }

        const uint32 index = static_cast<uint32>(buffers.size());
        buffers.push_back(buffer);
        bufferIndexLookup.emplace(buffer, index);
        return index;
    }

    RayTracingInstanceAlphaMetadata MakeInstanceAlphaMetadata(
        const RayTracingAlphaTestMetadata& source,
        uint32 baseColorTextureTableIndex,
        uint32 indexBufferTableIndex,
        uint32 uvBufferTableIndex,
        uint32 normalBufferTableIndex,
        uint32 tangentBufferTableIndex)
    {
        RayTracingInstanceAlphaMetadata metadata;
        RayTracingInstanceAlphaMetadataFlags flags = RayTracingInstanceAlphaMetadataFlags::None;
        if (source.enabled)
        {
            flags = flags | RayTracingInstanceAlphaMetadataFlags::AlphaTestEnabled;
        }
        if (source.hasBaseColorTexture)
        {
            flags = flags | RayTracingInstanceAlphaMetadataFlags::HasBaseColorTexture;
        }
        if (source.hasResolvedBaseColorTexture)
        {
            flags = flags | RayTracingInstanceAlphaMetadataFlags::HasResolvedBaseColorTexture;
        }
        if (source.hasUVBuffer)
        {
            flags = flags | RayTracingInstanceAlphaMetadataFlags::HasUVBuffer;
        }
        if (source.hasIndexBuffer)
        {
            flags = flags | RayTracingInstanceAlphaMetadataFlags::HasIndexBuffer;
        }
        if (source.hasNormalBuffer)
        {
            flags = flags | RayTracingInstanceAlphaMetadataFlags::HasNormalBuffer;
        }
        if (source.hasTangentBuffer)
        {
            flags = flags | RayTracingInstanceAlphaMetadataFlags::HasTangentBuffer;
        }
        if (source.indexFormat == RHIFormat::R32_UINT)
        {
            flags = flags | RayTracingInstanceAlphaMetadataFlags::IndexFormatUInt32;
        }
        else if (source.indexFormat == RHIFormat::R16_UINT)
        {
            flags = flags | RayTracingInstanceAlphaMetadataFlags::IndexFormatUInt16;
        }

        metadata.flags = static_cast<uint32>(flags);
        metadata.baseColorUVSet = source.baseColorUVSet;
        metadata.alphaCutoff = source.alphaCutoff;
        metadata.baseColorAlpha = source.baseColorAlpha;
        metadata.baseColorTextureIdLow = static_cast<uint32>(source.baseColorTextureId & 0xFFFF'FFFFull);
        metadata.baseColorTextureIdHigh = static_cast<uint32>((source.baseColorTextureId >> 32) & 0xFFFF'FFFFull);
        metadata.baseColorTextureTableIndex = baseColorTextureTableIndex;
        metadata.indexBufferTableIndex = indexBufferTableIndex;
        metadata.uvBufferTableIndex = uvBufferTableIndex;
        metadata.indexElementOffset = source.indexElementOffset;
        metadata.baseVertex = source.baseVertex;
        metadata.baseColorSamplerFlags = source.baseColorSamplerFlags;
        metadata.normalBufferTableIndex = normalBufferTableIndex;
        metadata.tangentBufferTableIndex = tangentBufferTableIndex;
        metadata.baseColorUVOffset = source.baseColorUVOffset;
        metadata.baseColorUVScale = source.baseColorUVScale;
        metadata.baseColorUVRotation = source.baseColorUVRotation;
        return metadata;
    }

    RayTracingInstanceMaterialMetadata MakeInstanceMaterialMetadata(
        const RayTracingMaterialMetadata& source,
        uint32 baseColorTextureTableIndex,
        uint32 metallicRoughnessTextureTableIndex,
        uint32 normalTextureTableIndex,
        uint32 emissiveTextureTableIndex)
    {
        RayTracingInstanceMaterialMetadata metadata;
        metadata.baseColorFactor = source.baseColorFactor;
        metadata.emissiveFactor = Vec4(source.emissiveColor, source.emissiveStrength);
        metadata.materialFactors = Vec4(source.metallicFactor,
                                        source.roughnessFactor,
                                        source.alphaCutoff,
                                        source.normalScale);
        metadata.flags = source.flags;
        metadata.materialIdLow = static_cast<uint32>(source.materialId & 0xFFFF'FFFFull);
        metadata.materialIdHigh = static_cast<uint32>((source.materialId >> 32) & 0xFFFF'FFFFull);
        metadata.workflow = source.workflow;
        metadata.baseColorTextureTableIndex = baseColorTextureTableIndex;
        metadata.metallicRoughnessTextureTableIndex = metallicRoughnessTextureTableIndex;
        metadata.normalTextureTableIndex = normalTextureTableIndex;
        metadata.emissiveTextureTableIndex = emissiveTextureTableIndex;
        metadata.baseColorTextureSampling = RayTracingInstanceTextureSamplingMetadata{
            source.baseColorTextureSampling.uvOffset,
            source.baseColorTextureSampling.uvScale,
            source.baseColorTextureSampling.uvRotation,
            source.baseColorTextureSampling.uvSet,
            source.baseColorTextureSampling.samplerFlags,
            0u
        };
        metadata.metallicRoughnessTextureSampling = RayTracingInstanceTextureSamplingMetadata{
            source.metallicRoughnessTextureSampling.uvOffset,
            source.metallicRoughnessTextureSampling.uvScale,
            source.metallicRoughnessTextureSampling.uvRotation,
            source.metallicRoughnessTextureSampling.uvSet,
            source.metallicRoughnessTextureSampling.samplerFlags,
            0u
        };
        metadata.normalTextureSampling = RayTracingInstanceTextureSamplingMetadata{
            source.normalTextureSampling.uvOffset,
            source.normalTextureSampling.uvScale,
            source.normalTextureSampling.uvRotation,
            source.normalTextureSampling.uvSet,
            source.normalTextureSampling.samplerFlags,
            0u
        };
        metadata.emissiveTextureSampling = RayTracingInstanceTextureSamplingMetadata{
            source.emissiveTextureSampling.uvOffset,
            source.emissiveTextureSampling.uvScale,
            source.emissiveTextureSampling.uvRotation,
            source.emissiveTextureSampling.uvSet,
            source.emissiveTextureSampling.samplerFlags,
            0u
        };
        return metadata;
    }
} // namespace

RayTracingSceneManager::~RayTracingSceneManager()
{
    Shutdown();
}

void RayTracingSceneManager::Initialize(IRHIDevice* device)
{
    Shutdown();
    m_device = device;
    m_stats.supported = IsSupported();
}

void RayTracingSceneManager::Shutdown()
{
    m_blasCache.clear();
    m_frameBLAS.clear();
    m_pendingBLASBuilds.clear();
    m_instanceRecords.clear();
    m_instanceMaterialMetadataRecords.clear();
    m_instanceMaterialTextureIds.clear();
    m_instanceAlphaMetadataRecords.clear();
    m_instanceAlphaTextureIds.clear();
    m_instanceAlphaIndexBuffers.clear();
    m_instanceAlphaUVBuffers.clear();
    m_instanceAlphaNormalBuffers.clear();
    m_instanceAlphaTangentBuffers.clear();
    m_instanceBuffer.Reset();
    m_instanceBufferSize = 0;
    m_instanceMaterialMetadataBuffer.Reset();
    m_instanceMaterialMetadataBufferSize = 0;
    m_instanceAlphaMetadataBuffer.Reset();
    m_instanceAlphaMetadataBufferSize = 0;
    m_topLevelAS.Reset();
    m_topLevelSizes = {};
    m_topLevelScratchBuffer.Reset();
    m_topLevelBuildDesc = {};
    m_pendingOwnerRetirements.clear();
    m_stats = {};
    m_frameCounter = 0;
    m_trackedResourceBudget = 0;
    m_device = nullptr;
}

void RayTracingSceneManager::RetireOwnerSnapshots(
    const GPUCompletionToken& completion,
    RenderRetirementQueue& retirement)
{
    FlushRenderOwnerRetirements(
        m_pendingOwnerRetirements, completion, retirement);
}

bool RayTracingSceneManager::IsSupported() const
{
    return m_device && m_device->GetCapabilities().supportsRaytracing;
}

bool RayTracingSceneManager::Prepare(const RayTracingSceneBuildPlan& plan)
{
    ResetFrameState();
    ++m_frameCounter;
    ReleaseRetiredBLASScratchBuffers();

    m_stats.supported = IsSupported();
    m_stats.requestedBLASCount = plan.blasBuilds.size();
    m_stats.instanceCount = plan.instances.size();
    m_stats.alphaTestedInstanceCount = plan.stats.alphaTestedInstanceCount;
    m_stats.skippedCount = plan.skips.size();

    if (!m_device)
    {
        SetFallback(RayTracingSceneFallbackCode::MissingDevice,
                    "ray tracing scene manager has no RHI device");
        return false;
    }

    if (!m_stats.supported)
    {
        SetFallback(RayTracingSceneFallbackCode::RayTracingUnsupported,
                    "RHI device does not support ray tracing");
        return false;
    }

    if (!plan.HasWork())
    {
        SetFallback(RayTracingSceneFallbackCode::EmptyBuildPlan,
                    "ray tracing scene plan has no buildable work");
        return false;
    }

    EvictUnusedBLAS(plan.blasBuilds);
    EvictUnusedBLASForResourceBudget(plan.blasBuilds);

    for (const RayTracingBLASBuild& build : plan.blasBuilds)
    {
        BLASCacheEntry* entry = GetOrCreateBLAS(build);
        if (!entry)
        {
            SetFallback(RayTracingSceneFallbackCode::BottomLevelASCreationFailed,
                        "failed to create bottom-level acceleration structure");
            return false;
        }

        entry->lastUsedFrame = m_frameCounter;
    }

    EvictUnusedBLASForResourceBudget(plan.blasBuilds);

    auto collectFrameBLAS = [this, &plan](std::vector<RHIAccelerationStructure*>* outBLASResources) -> bool
    {
        m_frameBLAS.clear();
        m_pendingBLASBuilds.clear();
        m_frameBLAS.reserve(plan.blasBuilds.size());
        m_pendingBLASBuilds.reserve(plan.blasBuilds.size());
        if (outBLASResources)
        {
            outBLASResources->clear();
            outBLASResources->reserve(plan.blasBuilds.size());
        }

        for (const RayTracingBLASBuild& build : plan.blasBuilds)
        {
            BLASCacheEntry* entry = FindBLAS(build.key);
            if (!entry || !entry->accelerationStructure)
                return false;

            entry->lastUsedFrame = m_frameCounter;
            m_frameBLAS.push_back(entry);
            if (entry->needsBuild)
            {
                m_pendingBLASBuilds.push_back(entry);
            }
            if (outBLASResources)
            {
                outBLASResources->push_back(entry->accelerationStructure.Get());
            }
        }

        return true;
    };

    std::vector<RHIAccelerationStructure*> blasResources;
    if (!collectFrameBLAS(&blasResources))
    {
        SetFallback(RayTracingSceneFallbackCode::BottomLevelASCreationFailed,
                    "failed to create bottom-level acceleration structure");
        return false;
    }

    RHITopLevelASDesc cpuTopLevelDesc = BuildRayTracingTopLevelDesc(
        plan,
        std::span<RHIAccelerationStructure* const>(blasResources.data(), blasResources.size()));
    if (cpuTopLevelDesc.instances.size() != plan.instances.size())
    {
        SetFallback(RayTracingSceneFallbackCode::TopLevelInstanceMappingFailed,
                    "failed to map all ray tracing TLAS instances");
        return false;
    }

    if (!ValidateRHITopLevelASDesc(cpuTopLevelDesc))
    {
        SetFallback(RayTracingSceneFallbackCode::InvalidTopLevelDescription,
                    "failed to build a valid CPU TLAS description");
        return false;
    }

    m_instanceRecords.reserve(cpuTopLevelDesc.instances.size());
    m_instanceMaterialMetadataRecords.reserve(cpuTopLevelDesc.instances.size());
    m_instanceAlphaMetadataRecords.reserve(cpuTopLevelDesc.instances.size());
    std::unordered_map<uint64, uint32> alphaTextureIndexLookup;
    std::unordered_map<uint64, uint32> materialTextureIndexLookup;
    std::unordered_map<RHIBuffer*, uint32> alphaIndexBufferLookup;
    std::unordered_map<RHIBuffer*, uint32> alphaUVBufferLookup;
    std::unordered_map<RHIBuffer*, uint32> alphaNormalBufferLookup;
    std::unordered_map<RHIBuffer*, uint32> alphaTangentBufferLookup;
    for (const RHIRayTracingInstanceDesc& instance : cpuTopLevelDesc.instances)
    {
        const uint32 expectedInstanceId = static_cast<uint32>(m_instanceRecords.size());
        if (instance.instanceId != expectedInstanceId)
        {
            SetFallback(RayTracingSceneFallbackCode::InstanceMetadataOrderMismatch,
                        "ray tracing TLAS instance IDs must match metadata order");
            return false;
        }

        const uint64 address = instance.bottomLevel ? instance.bottomLevel->GetGPUVirtualAddress() : 0;
        if (address == 0)
        {
            SetFallback(RayTracingSceneFallbackCode::MissingBottomLevelASAddress,
                        "BLAS GPU address is unavailable");
            return false;
        }

        m_instanceRecords.push_back(PackRHIRayTracingInstanceRecord(instance, address));
        const uint32 instanceIndex = instance.instanceId;
        const RayTracingMaterialMetadata* material =
            instanceIndex < plan.instances.size() ? &plan.instances[instanceIndex].material : nullptr;
        const RayTracingAlphaTestMetadata* alphaTest =
            instanceIndex < plan.instances.size() ? &plan.instances[instanceIndex].alphaTest : nullptr;
        uint32 materialBaseColorTextureTableIndex = RVX_INVALID_INDEX;
        uint32 materialMetallicRoughnessTextureTableIndex = RVX_INVALID_INDEX;
        uint32 materialNormalTextureTableIndex = RVX_INVALID_INDEX;
        uint32 materialEmissiveTextureTableIndex = RVX_INVALID_INDEX;
        if (material)
        {
            materialBaseColorTextureTableIndex = FindOrAddMaterialTextureIndex(
                m_instanceMaterialTextureIds,
                materialTextureIndexLookup,
                material->baseColorTextureId);
            materialMetallicRoughnessTextureTableIndex = FindOrAddMaterialTextureIndex(
                m_instanceMaterialTextureIds,
                materialTextureIndexLookup,
                material->metallicRoughnessTextureId);
            materialNormalTextureTableIndex = FindOrAddMaterialTextureIndex(
                m_instanceMaterialTextureIds,
                materialTextureIndexLookup,
                material->normalTextureId);
            materialEmissiveTextureTableIndex = FindOrAddMaterialTextureIndex(
                m_instanceMaterialTextureIds,
                materialTextureIndexLookup,
                material->emissiveTextureId);
        }
        uint32 baseColorTextureTableIndex = RVX_INVALID_INDEX;
        uint32 indexBufferTableIndex = RVX_INVALID_INDEX;
        uint32 uvBufferTableIndex = RVX_INVALID_INDEX;
        uint32 normalBufferTableIndex = RVX_INVALID_INDEX;
        uint32 tangentBufferTableIndex = RVX_INVALID_INDEX;
        if (alphaTest &&
            alphaTest->enabled &&
            alphaTest->hasResolvedBaseColorTexture)
        {
            baseColorTextureTableIndex = FindOrAddAlphaTextureIndex(
                m_instanceAlphaTextureIds,
                alphaTextureIndexLookup,
                alphaTest->baseColorTextureId);
        }
        if (alphaTest && alphaTest->hasIndexBuffer)
        {
            indexBufferTableIndex = FindOrAddAlphaBufferIndex(
                m_instanceAlphaIndexBuffers,
                alphaIndexBufferLookup,
                alphaTest->indexBuffer);
        }
        if (alphaTest && alphaTest->hasUVBuffer)
        {
            uvBufferTableIndex = FindOrAddAlphaBufferIndex(
                m_instanceAlphaUVBuffers,
                alphaUVBufferLookup,
                alphaTest->uvBuffer);
        }
        if (alphaTest && alphaTest->hasNormalBuffer)
        {
            normalBufferTableIndex = FindOrAddAlphaBufferIndex(
                m_instanceAlphaNormalBuffers,
                alphaNormalBufferLookup,
                alphaTest->normalBuffer);
        }
        if (alphaTest && alphaTest->hasTangentBuffer)
        {
            tangentBufferTableIndex = FindOrAddAlphaBufferIndex(
                m_instanceAlphaTangentBuffers,
                alphaTangentBufferLookup,
                alphaTest->tangentBuffer);
        }

        m_instanceMaterialMetadataRecords.push_back(
            MakeInstanceMaterialMetadata(
                material ? *material : RayTracingMaterialMetadata{},
                materialBaseColorTextureTableIndex,
                materialMetallicRoughnessTextureTableIndex,
                materialNormalTextureTableIndex,
                materialEmissiveTextureTableIndex));
        m_instanceAlphaMetadataRecords.push_back(
            MakeInstanceAlphaMetadata(
                alphaTest ? *alphaTest : RayTracingAlphaTestMetadata{},
                baseColorTextureTableIndex,
                indexBufferTableIndex,
                uvBufferTableIndex,
                normalBufferTableIndex,
                tangentBufferTableIndex));
    }

    if (!EnsureInstanceBuffer(m_instanceRecords))
    {
        SetFallback(RayTracingSceneFallbackCode::InstanceBufferUpdateFailed,
                    "failed to update TLAS instance buffer");
        return false;
    }

    if (!EnsureInstanceMaterialMetadataBuffer(m_instanceMaterialMetadataRecords))
    {
        SetFallback(RayTracingSceneFallbackCode::MaterialMetadataBufferUpdateFailed,
                    "failed to update ray tracing instance material metadata buffer");
        return false;
    }

    if (!EnsureInstanceAlphaMetadataBuffer(m_instanceAlphaMetadataRecords))
    {
        SetFallback(RayTracingSceneFallbackCode::AlphaMetadataBufferUpdateFailed,
                    "failed to update ray tracing instance alpha metadata buffer");
        return false;
    }

    m_topLevelBuildDesc = cpuTopLevelDesc;
    m_topLevelBuildDesc.instances.clear();
    m_topLevelBuildDesc.instanceBuffer = m_instanceBuffer.Get();
    m_topLevelBuildDesc.instanceOffset = 0;
    m_topLevelBuildDesc.instanceCount = static_cast<uint32>(m_instanceRecords.size());

    if (!EnsureTopLevelAS(m_topLevelBuildDesc))
    {
        SetFallback(RayTracingSceneFallbackCode::TopLevelASCreationFailed,
                    "failed to create top-level acceleration structure");
        return false;
    }

    m_stats.cachedBLASCount = m_blasCache.size();
    m_stats.pendingBLASBuildCount = m_pendingBLASBuilds.size();
    m_stats.materialTextureCount = m_instanceMaterialTextureIds.size();
    m_stats.alphaTextureCount = m_instanceAlphaTextureIds.size();
    m_stats.alphaIndexBufferCount = m_instanceAlphaIndexBuffers.size();
    m_stats.alphaUVBufferCount = m_instanceAlphaUVBuffers.size();
    m_stats.alphaNormalBufferCount = m_instanceAlphaNormalBuffers.size();
    m_stats.alphaTangentBufferCount = m_instanceAlphaTangentBuffers.size();
    m_stats.hasTopLevelAS = m_topLevelAS != nullptr;
    m_stats.hasInstanceBuffer = m_instanceBuffer != nullptr;
    m_stats.hasMaterialMetadataBuffer = m_instanceMaterialMetadataBuffer != nullptr;
    m_stats.hasAlphaMetadataBuffer = m_instanceAlphaMetadataBuffer != nullptr;
    m_stats.prepared = m_stats.hasTopLevelAS &&
                       m_stats.hasInstanceBuffer &&
                       m_stats.hasMaterialMetadataBuffer &&
                       m_stats.hasAlphaMetadataBuffer;
    UpdateResourceStats();
    EvictUnusedBLASForResourceBudget(plan.blasBuilds);
    if (!collectFrameBLAS(nullptr))
    {
        SetFallback(RayTracingSceneFallbackCode::BottomLevelASCreationFailed,
                    "failed to create bottom-level acceleration structure");
        return false;
    }
    m_stats.pendingBLASBuildCount = m_pendingBLASBuilds.size();
    m_stats.fallbackCode = RayTracingSceneFallbackCode::None;
    m_stats.fallbackReason = "";
    return m_stats.prepared;
}

void RayTracingSceneManager::RecordBuildCommands(RHICommandContext& ctx)
{
    m_stats.recordedBLASBuildCount = 0;
    m_stats.recordedTLASBuild = false;

    if (!m_stats.prepared || !m_topLevelAS || !m_topLevelScratchBuffer)
        return;

    for (BLASCacheEntry* entry : m_pendingBLASBuilds)
    {
        if (!entry || !entry->accelerationStructure || !entry->scratchBuffer)
            continue;

        ctx.BuildBottomLevelAccelerationStructure(
            entry->accelerationStructure.Get(),
            entry->desc,
            entry->scratchBuffer.Get());
        entry->needsBuild = false;
        entry->scratchReleasePending = true;
        entry->scratchLastUsedFrame = m_frameCounter;
        ++m_stats.recordedBLASBuildCount;
    }

    ctx.BuildTopLevelAccelerationStructure(
        m_topLevelAS.Get(),
        m_topLevelBuildDesc,
        m_topLevelScratchBuffer.Get());
    m_stats.recordedTLASBuild = true;
    m_pendingBLASBuilds.clear();
    m_stats.pendingBLASBuildCount = 0;
    UpdateResourceStats();
}

void RayTracingSceneManager::GatherPendingBuildScratchBuffers(std::vector<RHIBuffer*>& outScratchBuffers) const
{
    for (const BLASCacheEntry* entry : m_pendingBLASBuilds)
    {
        RHIBuffer* scratchBuffer = entry && entry->scratchBuffer ? entry->scratchBuffer.Get() : nullptr;
        if (!scratchBuffer)
            continue;

        if (std::find(outScratchBuffers.begin(), outScratchBuffers.end(), scratchBuffer) ==
            outScratchBuffers.end())
        {
            outScratchBuffers.push_back(scratchBuffer);
        }
    }
}

RayTracingSceneManager::BLASCacheEntry* RayTracingSceneManager::FindBLAS(const RayTracingBLASKey& key)
{
    const auto it = std::find_if(
        m_blasCache.begin(),
        m_blasCache.end(),
        [&key](const BLASCacheEntry& entry)
        {
            return entry.key == key;
        });

    return it != m_blasCache.end() ? &(*it) : nullptr;
}

RayTracingSceneManager::BLASCacheEntry* RayTracingSceneManager::GetOrCreateBLAS(
    const RayTracingBLASBuild& build)
{
    BLASCacheEntry* entry = FindBLAS(build.key);
    const bool existing = entry != nullptr;
    if (!entry)
    {
        m_blasCache.push_back({});
        entry = &m_blasCache.back();
        entry->key = build.key;
    }

    entry->lastUsedFrame = m_frameCounter;

    const bool descChanged = existing && !AreBLASDescriptionsEquivalent(entry->desc, build.desc);
    const bool sizesMissing = !entry->sizes.IsValid();
    const bool accelerationStructureMissing =
        !entry->accelerationStructure ||
        (!sizesMissing && entry->accelerationStructure->GetSize() < entry->sizes.accelerationStructureSize);
    const bool buildRequired = !existing || descChanged || sizesMissing || accelerationStructureMissing || entry->needsBuild;
    if (buildRequired)
    {
        const RHIAccelerationStructureBuildSizes sizes = m_device->GetBottomLevelASBuildSizes(build.desc);
        if (!sizes.IsValid())
            return nullptr;

        const bool needsAS =
            !entry->accelerationStructure ||
            entry->accelerationStructure->GetSize() < sizes.accelerationStructureSize;

        entry->desc = build.desc;
        entry->sizes = sizes;
        entry->scratchReleasePending = false;
        entry->needsBuild = true;
        if (needsAS)
        {
            RHIAccelerationStructureDesc asDesc;
            asDesc.type = RHIAccelerationStructureType::BottomLevel;
            asDesc.size = sizes.accelerationStructureSize;
            asDesc.debugName = "RayTracingSceneBLAS";
            RHIAccelerationStructureRef replacement =
                m_device->CreateAccelerationStructure(asDesc);
            if (!replacement)
                return nullptr;

            QueueRenderOwnerRetirement(
                entry->accelerationStructure, m_pendingOwnerRetirements);
            entry->accelerationStructure = std::move(replacement);

            ++m_stats.createdBLASCount;
        }

        if (!EnsureScratchBuffer(entry->scratchBuffer, sizes.buildScratchSize, "RayTracingBLASScratch"))
            return nullptr;
    }
    else
    {
        ++m_stats.reusedBLASCount;
    }

    return entry;
}

void RayTracingSceneManager::ReleaseRetiredBLASScratchBuffers()
{
    for (BLASCacheEntry& entry : m_blasCache)
    {
        if (!entry.scratchBuffer || !entry.scratchReleasePending || entry.needsBuild)
            continue;

        m_stats.releasedBLASScratchBytes = AddSaturatingUint64(
            m_stats.releasedBLASScratchBytes,
            entry.scratchBuffer->GetSize(),
            m_stats.resourceByteAccountingOverflowed);
        QueueRenderOwnerRetirement(
            entry.scratchBuffer, m_pendingOwnerRetirements);
        entry.scratchReleasePending = false;
        entry.scratchLastUsedFrame = 0;
        ++m_stats.releasedBLASScratchCount;
    }
}

void RayTracingSceneManager::EvictUnusedBLAS(const std::vector<RayTracingBLASBuild>& requestedBuilds)
{
    if (m_blasCache.empty())
        return;

    auto isRequestedThisFrame = [&requestedBuilds](const RayTracingBLASKey& key)
    {
        return std::any_of(requestedBuilds.begin(), requestedBuilds.end(), [&key](const RayTracingBLASBuild& build)
        {
            return build.key == key;
        });
    };

    for (auto it = m_blasCache.begin(); it != m_blasCache.end();)
    {
        const bool requestedThisFrame = isRequestedThisFrame(it->key);
        const bool usedInPast = it->lastUsedFrame > 0;
        const bool unusedLongEnough =
            usedInPast &&
            m_frameCounter > it->lastUsedFrame &&
            (m_frameCounter - it->lastUsedFrame) > m_blasCacheEvictionFrameThreshold;

        if (!requestedThisFrame && unusedLongEnough)
        {
            QueueRenderOwnerRetirement(
                it->accelerationStructure, m_pendingOwnerRetirements);
            QueueRenderOwnerRetirement(
                it->scratchBuffer, m_pendingOwnerRetirements);
            it = m_blasCache.erase(it);
            ++m_stats.evictedBLASCount;
            continue;
        }

        ++it;
    }
}

void RayTracingSceneManager::EvictUnusedBLASForResourceBudget(
    const std::vector<RayTracingBLASBuild>& requestedBuilds)
{
    if (m_trackedResourceBudget == 0 || m_blasCache.empty())
        return;

    UpdateResourceStats();
    if (!m_stats.resourceBudgetExceeded)
        return;

    auto isRequestedThisFrame = [&requestedBuilds](const RayTracingBLASKey& key)
    {
        return std::any_of(requestedBuilds.begin(), requestedBuilds.end(), [&key](const RayTracingBLASBuild& build)
        {
            return build.key == key;
        });
    };

    auto blasResourceBytes = [](const BLASCacheEntry& entry) -> uint64
    {
        bool overflowed = false;
        uint64 totalBLASResourceBytes = 0;
        totalBLASResourceBytes = AddSaturatingUint64(
            totalBLASResourceBytes,
            entry.accelerationStructure ? entry.accelerationStructure->GetSize() : 0,
            overflowed);
        totalBLASResourceBytes = AddSaturatingUint64(
            totalBLASResourceBytes,
            entry.scratchBuffer ? entry.scratchBuffer->GetSize() : 0,
            overflowed);
        return totalBLASResourceBytes;
    };

    while (m_stats.resourceBudgetExceeded)
    {
        auto evictionCandidate = m_blasCache.end();
        for (auto it = m_blasCache.begin(); it != m_blasCache.end(); ++it)
        {
            if (isRequestedThisFrame(it->key))
                continue;

            if (evictionCandidate == m_blasCache.end() ||
                it->lastUsedFrame < evictionCandidate->lastUsedFrame ||
                (it->lastUsedFrame == evictionCandidate->lastUsedFrame &&
                 blasResourceBytes(*it) > blasResourceBytes(*evictionCandidate)))
            {
                evictionCandidate = it;
            }
        }

        if (evictionCandidate == m_blasCache.end())
            break;

        m_stats.resourceBudgetEvictionAttempted = true;
        QueueRenderOwnerRetirement(
            evictionCandidate->accelerationStructure,
            m_pendingOwnerRetirements);
        QueueRenderOwnerRetirement(
            evictionCandidate->scratchBuffer,
            m_pendingOwnerRetirements);
        m_blasCache.erase(evictionCandidate);
        ++m_stats.evictedBLASCount;
        ++m_stats.resourceBudgetEvictedBLASCount;
        UpdateResourceStats();
    }
}

bool RayTracingSceneManager::EnsureScratchBuffer(RHIBufferRef& buffer, uint64 size, const char* debugName)
{
    if (size == 0)
        return false;

    if (buffer && buffer->GetSize() >= size)
        return true;

    RHIBufferDesc desc;
    desc.size = size;
    desc.usage = RHIBufferUsage::UnorderedAccess | RHIBufferUsage::DeviceAddress;
    desc.memoryType = RHIMemoryType::Default;
    desc.debugName = debugName;
    RHIBufferRef replacement = m_device->CreateBuffer(desc);
    if (!replacement)
        return false;

    QueueRenderOwnerRetirement(buffer, m_pendingOwnerRetirements);
    buffer = std::move(replacement);
    return true;
}

bool RayTracingSceneManager::EnsureInstanceBuffer(const std::vector<RHIRayTracingInstanceRecord>& records)
{
    if (records.empty())
        return false;

    uint64 dataSize = 0;
    if (!TryMultiplyUint64(static_cast<uint64>(records.size()), sizeof(RHIRayTracingInstanceRecord), dataSize))
        return false;

    uint64 requiredSize = 0;
    if (!TryAlignUpUint64(dataSize, RVX_RAY_TRACING_INSTANCE_BUFFER_ALIGNMENT, requiredSize))
        return false;
    if (!m_instanceBuffer || m_instanceBufferSize < requiredSize)
    {
        RHIBufferDesc desc;
        desc.size = requiredSize;
        desc.usage = RHIBufferUsage::AccelerationStructureInput |
                     RHIBufferUsage::DeviceAddress |
                     RHIBufferUsage::ShaderResource;
        desc.memoryType = RHIMemoryType::Upload;
        desc.stride = sizeof(RHIRayTracingInstanceRecord);
        desc.debugName = "RayTracingTLASInstances";
        RHIBufferRef replacement = m_device->CreateBuffer(desc);
        if (!replacement)
            return false;
        QueueRenderOwnerRetirement(
            m_instanceBuffer, m_pendingOwnerRetirements);
        m_instanceBuffer = std::move(replacement);
        m_instanceBufferSize = requiredSize;
    }

    if (!m_instanceBuffer)
        return false;

    void* mapped = m_instanceBuffer->Map();
    if (!mapped)
        return false;

    std::memcpy(mapped, records.data(), static_cast<size_t>(dataSize));
    return m_instanceBuffer->CommitMappedWrite();
}

bool RayTracingSceneManager::EnsureInstanceMaterialMetadataBuffer(
    const std::vector<RayTracingInstanceMaterialMetadata>& records)
{
    if (records.empty())
        return false;

    uint64 dataSize = 0;
    if (!TryMultiplyUint64(static_cast<uint64>(records.size()), sizeof(RayTracingInstanceMaterialMetadata), dataSize))
        return false;
    if (!m_instanceMaterialMetadataBuffer || m_instanceMaterialMetadataBufferSize < dataSize)
    {
        RHIBufferDesc desc;
        desc.size = dataSize;
        desc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
        desc.memoryType = RHIMemoryType::Upload;
        desc.stride = sizeof(RayTracingInstanceMaterialMetadata);
        desc.debugName = "RayTracingInstanceMaterialMetadata";
        RHIBufferRef replacement = m_device->CreateBuffer(desc);
        if (!replacement)
            return false;
        QueueRenderOwnerRetirement(
            m_instanceMaterialMetadataBuffer, m_pendingOwnerRetirements);
        m_instanceMaterialMetadataBuffer = std::move(replacement);
        m_instanceMaterialMetadataBufferSize = dataSize;
    }

    if (!m_instanceMaterialMetadataBuffer)
        return false;

    void* mapped = m_instanceMaterialMetadataBuffer->Map();
    if (!mapped)
        return false;

    std::memcpy(mapped, records.data(), static_cast<size_t>(dataSize));
    return m_instanceMaterialMetadataBuffer->CommitMappedWrite();
}

bool RayTracingSceneManager::EnsureInstanceAlphaMetadataBuffer(
    const std::vector<RayTracingInstanceAlphaMetadata>& records)
{
    if (records.empty())
        return false;

    uint64 dataSize = 0;
    if (!TryMultiplyUint64(static_cast<uint64>(records.size()), sizeof(RayTracingInstanceAlphaMetadata), dataSize))
        return false;
    if (!m_instanceAlphaMetadataBuffer || m_instanceAlphaMetadataBufferSize < dataSize)
    {
        RHIBufferDesc desc;
        desc.size = dataSize;
        desc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
        desc.memoryType = RHIMemoryType::Upload;
        desc.stride = sizeof(RayTracingInstanceAlphaMetadata);
        desc.debugName = "RayTracingInstanceAlphaMetadata";
        RHIBufferRef replacement = m_device->CreateBuffer(desc);
        if (!replacement)
            return false;
        QueueRenderOwnerRetirement(
            m_instanceAlphaMetadataBuffer, m_pendingOwnerRetirements);
        m_instanceAlphaMetadataBuffer = std::move(replacement);
        m_instanceAlphaMetadataBufferSize = dataSize;
    }

    if (!m_instanceAlphaMetadataBuffer)
        return false;

    void* mapped = m_instanceAlphaMetadataBuffer->Map();
    if (!mapped)
        return false;

    std::memcpy(mapped, records.data(), static_cast<size_t>(dataSize));
    return m_instanceAlphaMetadataBuffer->CommitMappedWrite();
}

bool RayTracingSceneManager::EnsureTopLevelAS(const RHITopLevelASDesc& desc)
{
    const RHIAccelerationStructureBuildSizes sizes = m_device->GetTopLevelASBuildSizes(desc);
    if (!sizes.IsValid())
        return false;

    const bool needsAS =
        !m_topLevelAS ||
        m_topLevelAS->GetSize() < sizes.accelerationStructureSize;
    if (needsAS)
    {
        RHIAccelerationStructureDesc asDesc;
        asDesc.type = RHIAccelerationStructureType::TopLevel;
        asDesc.size = sizes.accelerationStructureSize;
        asDesc.debugName = "RayTracingSceneTLAS";
        RHIAccelerationStructureRef replacement =
            m_device->CreateAccelerationStructure(asDesc);
        if (!replacement)
            return false;
        QueueRenderOwnerRetirement(
            m_topLevelAS, m_pendingOwnerRetirements);
        m_topLevelAS = std::move(replacement);
    }

    m_topLevelSizes = sizes;
    return EnsureScratchBuffer(m_topLevelScratchBuffer, sizes.buildScratchSize, "RayTracingTLASScratch");
}

bool RayTracingSceneManager::AreBLASDescriptionsEquivalent(const RHIBottomLevelASDesc& lhs,
                                                           const RHIBottomLevelASDesc& rhs) const
{
    if (lhs.buildFlags != rhs.buildFlags || lhs.geometries.size() != rhs.geometries.size())
        return false;

    for (size_t i = 0; i < lhs.geometries.size(); ++i)
    {
        if (!AreGeometriesEquivalent(lhs.geometries[i], rhs.geometries[i]))
            return false;
    }

    return true;
}

void RayTracingSceneManager::UpdateResourceStats()
{
    bool trackedResourceByteAccountingOverflowed = false;
    uint64 cachedBLASAccelerationStructureBytes = 0;
    uint64 cachedBLASScratchBytes = 0;
    size_t pendingBLASScratchReleaseCount = 0;
    for (const BLASCacheEntry& entry : m_blasCache)
    {
        if (entry.accelerationStructure)
        {
            cachedBLASAccelerationStructureBytes = AddSaturatingUint64(
                cachedBLASAccelerationStructureBytes,
                entry.accelerationStructure->GetSize(),
                trackedResourceByteAccountingOverflowed);
        }
        if (entry.scratchBuffer)
        {
            cachedBLASScratchBytes = AddSaturatingUint64(
                cachedBLASScratchBytes,
                entry.scratchBuffer->GetSize(),
                trackedResourceByteAccountingOverflowed);
            if (entry.scratchReleasePending)
            {
                ++pendingBLASScratchReleaseCount;
            }
        }
    }

    m_stats.cachedBLASCount = m_blasCache.size();
    m_stats.pendingBLASScratchReleaseCount = pendingBLASScratchReleaseCount;
    m_stats.cachedBLASAccelerationStructureBytes = cachedBLASAccelerationStructureBytes;
    m_stats.cachedBLASScratchBytes = cachedBLASScratchBytes;
    m_stats.topLevelAccelerationStructureBytes = m_topLevelAS ? m_topLevelAS->GetSize() : 0;
    m_stats.topLevelScratchBytes = m_topLevelScratchBuffer ? m_topLevelScratchBuffer->GetSize() : 0;
    m_stats.instanceBufferBytes = m_instanceBuffer ? m_instanceBuffer->GetSize() : 0;
    m_stats.materialMetadataBufferBytes =
        m_instanceMaterialMetadataBuffer ? m_instanceMaterialMetadataBuffer->GetSize() : 0;
    m_stats.alphaMetadataBufferBytes =
        m_instanceAlphaMetadataBuffer ? m_instanceAlphaMetadataBuffer->GetSize() : 0;

    uint64 totalTrackedResourceBytes = 0;
    totalTrackedResourceBytes = AddSaturatingUint64(
        totalTrackedResourceBytes,
        m_stats.cachedBLASAccelerationStructureBytes,
        trackedResourceByteAccountingOverflowed);
    totalTrackedResourceBytes = AddSaturatingUint64(
        totalTrackedResourceBytes,
        m_stats.cachedBLASScratchBytes,
        trackedResourceByteAccountingOverflowed);
    totalTrackedResourceBytes = AddSaturatingUint64(
        totalTrackedResourceBytes,
        m_stats.topLevelAccelerationStructureBytes,
        trackedResourceByteAccountingOverflowed);
    totalTrackedResourceBytes = AddSaturatingUint64(
        totalTrackedResourceBytes,
        m_stats.topLevelScratchBytes,
        trackedResourceByteAccountingOverflowed);
    totalTrackedResourceBytes = AddSaturatingUint64(
        totalTrackedResourceBytes,
        m_stats.instanceBufferBytes,
        trackedResourceByteAccountingOverflowed);
    totalTrackedResourceBytes = AddSaturatingUint64(
        totalTrackedResourceBytes,
        m_stats.materialMetadataBufferBytes,
        trackedResourceByteAccountingOverflowed);
    totalTrackedResourceBytes = AddSaturatingUint64(
        totalTrackedResourceBytes,
        m_stats.alphaMetadataBufferBytes,
        trackedResourceByteAccountingOverflowed);

    m_stats.totalTrackedResourceBytes = totalTrackedResourceBytes;
    m_stats.trackedResourceBudget = m_trackedResourceBudget;
    m_stats.resourceByteAccountingOverflowed =
        m_stats.resourceByteAccountingOverflowed || trackedResourceByteAccountingOverflowed;
    m_stats.resourceBudgetExceeded =
        m_trackedResourceBudget > 0 &&
        (trackedResourceByteAccountingOverflowed ||
         m_stats.totalTrackedResourceBytes > m_trackedResourceBudget);
}

void RayTracingSceneManager::ResetFrameState()
{
    m_frameBLAS.clear();
    m_pendingBLASBuilds.clear();
    m_instanceRecords.clear();
    m_instanceMaterialMetadataRecords.clear();
    m_instanceMaterialTextureIds.clear();
    m_instanceAlphaMetadataRecords.clear();
    m_instanceAlphaTextureIds.clear();
    m_instanceAlphaIndexBuffers.clear();
    m_instanceAlphaUVBuffers.clear();
    m_instanceAlphaNormalBuffers.clear();
    m_instanceAlphaTangentBuffers.clear();
    m_topLevelBuildDesc = {};
    m_stats = {};
}

void RayTracingSceneManager::InvalidateFrameOutputs()
{
    m_frameBLAS.clear();
    m_pendingBLASBuilds.clear();
    m_instanceRecords.clear();
    m_instanceMaterialMetadataRecords.clear();
    m_instanceMaterialTextureIds.clear();
    m_instanceAlphaMetadataRecords.clear();
    m_instanceAlphaTextureIds.clear();
    m_instanceAlphaIndexBuffers.clear();
    m_instanceAlphaUVBuffers.clear();
    m_instanceAlphaNormalBuffers.clear();
    m_instanceAlphaTangentBuffers.clear();
    QueueRenderOwnerRetirement(
        m_instanceBuffer, m_pendingOwnerRetirements);
    m_instanceBufferSize = 0;
    QueueRenderOwnerRetirement(
        m_instanceMaterialMetadataBuffer, m_pendingOwnerRetirements);
    m_instanceMaterialMetadataBufferSize = 0;
    QueueRenderOwnerRetirement(
        m_instanceAlphaMetadataBuffer, m_pendingOwnerRetirements);
    m_instanceAlphaMetadataBufferSize = 0;
    QueueRenderOwnerRetirement(
        m_topLevelAS, m_pendingOwnerRetirements);
    m_topLevelSizes = {};
    QueueRenderOwnerRetirement(
        m_topLevelScratchBuffer, m_pendingOwnerRetirements);
    m_topLevelBuildDesc = {};
}

void RayTracingSceneManager::SetFallback(RayTracingSceneFallbackCode code, const char* reason)
{
    InvalidateFrameOutputs();
    m_stats.prepared = false;
    m_stats.hasTopLevelAS = false;
    m_stats.hasInstanceBuffer = false;
    m_stats.hasMaterialMetadataBuffer = false;
    m_stats.hasAlphaMetadataBuffer = false;
    m_stats.cachedBLASCount = m_blasCache.size();
    m_stats.pendingBLASBuildCount = 0;
    m_stats.materialTextureCount = 0;
    m_stats.alphaTextureCount = 0;
    m_stats.alphaIndexBufferCount = 0;
    m_stats.alphaUVBufferCount = 0;
    m_stats.alphaNormalBufferCount = 0;
    m_stats.alphaTangentBufferCount = 0;
    UpdateResourceStats();
    m_stats.fallbackCode = code;
    m_stats.fallbackReason = reason ? reason : "unknown ray tracing scene fallback";
}

} // namespace RVX
