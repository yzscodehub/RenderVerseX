#include "Render/RayTracing/RayTracingSceneManager.h"

#include "RHI/RHICommandContext.h"
#include "Resources/RenderOwnerSnapshotRetirement.h"
#include "Resources/RenderSubmissionTracker.h"

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
    constexpr uint64 RVX_RAY_TRACING_COMPACTION_READBACK_SIZE = sizeof(uint64);

    [[nodiscard]] bool IsExactCurrentGraphicsSubmissionToken(
        const RenderSubmissionTracker& tracker,
        const GPUCompletionToken& token) noexcept
    {
        GPUCompletionToken normalized;
        if (!MergeGPUCompletionToken(normalized, token) || normalized.count == 0 ||
            tracker.Query(normalized) == GPUCompletionStatus::Lost)
        {
            return false;
        }

        bool hasGraphics = false;
        for (uint8 index = 0; index < normalized.count; ++index)
        {
            const GPUCompletionPoint point = normalized.points[index];
            if (!IsDeclaredGPUQueueDomain(point.domain) || point.value == 0 ||
                point.value != tracker.GetLastSubmittedValue(point.domain))
            {
                return false;
            }
            hasGraphics = hasGraphics || point.domain == GPUQueueDomain::Graphics;
        }
        return hasGraphics;
    }

    [[nodiscard]] bool IsCompletionSatisfied(GPUCompletionStatus status) noexcept
    {
        return status == GPUCompletionStatus::Completed ||
               status == GPUCompletionStatus::CompatibilityWaitIdle;
    }

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

struct RayTracingSceneManager::BLASCacheEntry::BLASCompactionState
{
    RayTracingBLASCompactionStatus status = RayTracingBLASCompactionStatus::Disabled;
    RHIAccelerationStructureCompactedSizeQueryRef query;
    RHIBufferRef readbackBuffer;
    RHIAccelerationStructureRef compactDestination;
    GPUCompletionToken queryCompletion;
    GPUCompletionToken copyCompletion;
    uint64 compactedSize = 0;
};

RayTracingSceneManager::RayTracingSceneManager() = default;

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
    m_recordedCompactionQueries.clear();
    m_recordedCompactionCopies.clear();
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
    m_preparedTLASStructuralSignature = {};
    m_submittedTLASStructuralSignature = {};
    m_preparedTLASUpdate = false;
    m_activeRecording = {};
    m_recordedBLASBuilds.clear();
    m_executionReceipt = {};
    m_pendingOwnerRetirements.clear();
    m_stats = {};
    m_frameCounter = 0;
    m_preparationGeneration = 0;
    m_recordingGeneration = 0;
    m_tlasGeneration = 0;
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
    // Recording does not imply queue submission.  A new frame abandons any
    // unsubmitted recording and deliberately leaves its BLAS entries dirty.
    if (m_activeRecording.IsValid())
    {
        ReleaseUnsubmitted(m_activeRecording);
    }
    ResetFrameState();
    ++m_frameCounter;
    ++m_preparationGeneration;
    if (m_preparationGeneration == 0)
    {
        ++m_preparationGeneration;
    }
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

    if (!plan.maskedMetadataComplete)
    {
        SetFallback(RayTracingSceneFallbackCode::IncompleteMaskedMetadata,
                    "masked ray tracing metadata is incomplete");
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

    if (m_device->GetCapabilities().supportsAccelerationStructureUpdate)
    {
        m_topLevelBuildDesc.buildFlags = m_topLevelBuildDesc.buildFlags |
            RHIAccelerationStructureBuildFlags::AllowUpdate;
    }

    bool topLevelReplaced = false;
    if (!EnsureTopLevelAS(m_topLevelBuildDesc, topLevelReplaced))
    {
        SetFallback(RayTracingSceneFallbackCode::TopLevelASCreationFailed,
                    "failed to create top-level acceleration structure");
        return false;
    }

    m_preparedTLASStructuralSignature = BuildRayTracingTLASStructuralSignature(plan);
    m_preparedTLASUpdate =
        !topLevelReplaced &&
        m_executionReceipt.submitted &&
        m_preparedTLASStructuralSignature == m_submittedTLASStructuralSignature &&
        m_device->GetCapabilities().supportsAccelerationStructureUpdate &&
        m_topLevelSizes.updateScratchSize != 0;

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
    m_stats.preparationGeneration = m_preparationGeneration;
    UpdateResourceStats();
    UpdateCompactionStats();
    EvictUnusedBLASForResourceBudget(plan.blasBuilds);
    if (!collectFrameBLAS(nullptr))
    {
        SetFallback(RayTracingSceneFallbackCode::BottomLevelASCreationFailed,
                    "failed to create bottom-level acceleration structure");
        return false;
    }
    m_stats.pendingBLASBuildCount = m_pendingBLASBuilds.size();
    UpdateCompactionStats();
    m_stats.fallbackCode = RayTracingSceneFallbackCode::None;
    m_stats.fallbackReason = "";
    return m_stats.prepared;
}

bool RayTracingSceneManager::RecordBuildCommands(
    RHICommandContext& ctx,
    RayTracingSceneRecordingToken& outToken)
{
    return RecordBuildCommands(
        ctx, outToken, RayTracingSceneRecordingFlags::None);
}

bool RayTracingSceneManager::RecordBuildCommands(
    RHICommandContext& ctx,
    RayTracingSceneRecordingToken& outToken,
    RayTracingSceneRecordingFlags flags)
{
    outToken = {};
    m_stats.recordedBLASBuildCount = 0;
    m_stats.recordedTLASBuild = false;
    m_stats.recordedTLASUpdate = false;

    if (!m_stats.prepared || !m_topLevelAS || !m_topLevelScratchBuffer ||
        m_activeRecording.IsValid())
        return false;

    for (const BLASCacheEntry* entry : m_pendingBLASBuilds)
    {
        if (!entry || !entry->accelerationStructure || !entry->scratchBuffer)
        {
            return false;
        }
    }

    m_recordedBLASBuilds.clear();
    m_recordedCompactionQueries.clear();
    m_recordedCompactionCopies.clear();

    for (BLASCacheEntry* entry : m_pendingBLASBuilds)
    {
        ctx.BuildBottomLevelAccelerationStructure(
            entry->accelerationStructure.Get(),
            entry->desc,
            entry->scratchBuffer.Get());
        // This entry remains dirty until the queue accepts this recording.
        m_recordedBLASBuilds.push_back(entry);
        ++m_stats.recordedBLASBuildCount;
    }

    if (HasFlag(flags, RayTracingSceneRecordingFlags::EnableBLASCompaction))
    {
        for (BLASCacheEntry& cachedEntry : m_blasCache)
        {
            BLASCacheEntry* entry = &cachedEntry;
            const bool buildRecorded = std::find(
                m_recordedBLASBuilds.begin(), m_recordedBLASBuilds.end(), entry) !=
                m_recordedBLASBuilds.end();
            if (!entry->compaction ||
                entry->compaction->status != RayTracingBLASCompactionStatus::AwaitingQueryRecord ||
                !entry->compaction->query || !entry->compaction->readbackBuffer ||
                (entry->needsBuild && !buildRecorded))
            {
                continue;
            }

            RHIAccelerationStructure* accelerationStructure =
                entry->accelerationStructure.Get();
            ctx.WriteAccelerationStructureCompactedSizes(
                entry->compaction->query.Get(),
                0,
                std::span<RHIAccelerationStructure* const>(
                    &accelerationStructure, 1));
            ctx.ResolveAccelerationStructureCompactedSizes(
                entry->compaction->query.Get(),
                0,
                1,
                entry->compaction->readbackBuffer.Get());
            entry->compaction->status = RayTracingBLASCompactionStatus::QueryRecorded;
            m_recordedCompactionQueries.push_back(entry);
            ++m_stats.compactionQueryRecordedCount;
        }

        for (BLASCacheEntry& entry : m_blasCache)
        {
            if (!entry.compaction ||
                entry.compaction->status != RayTracingBLASCompactionStatus::ReadyToRecordCopy ||
                !entry.accelerationStructure || !entry.compaction->compactDestination)
            {
                continue;
            }

            ctx.CopyAccelerationStructureCompact(
                entry.compaction->compactDestination.Get(),
                entry.accelerationStructure.Get(),
                entry.compaction->compactedSize);
            entry.compaction->status = RayTracingBLASCompactionStatus::CopyRecorded;
            m_recordedCompactionCopies.push_back(&entry);
            ++m_stats.compactionCopyRecordedCount;
        }
    }

    ctx.BuildTopLevelAccelerationStructure(
        m_topLevelAS.Get(),
        m_topLevelBuildDesc,
        m_topLevelScratchBuffer.Get(),
        0,
        m_preparedTLASUpdate ? m_topLevelAS.Get() : nullptr);
    m_stats.recordedTLASBuild = true;
    m_stats.recordedTLASUpdate = m_preparedTLASUpdate;
    ++m_recordingGeneration;
    if (m_recordingGeneration == 0)
    {
        ++m_recordingGeneration;
    }
    m_activeRecording = {m_preparationGeneration, m_recordingGeneration};
    outToken = m_activeRecording;
    UpdateResourceStats();
    return true;
}

void RayTracingSceneManager::RecordBuildCommands(RHICommandContext& ctx)
{
    RayTracingSceneRecordingToken ignored;
    if (RecordBuildCommands(ctx, ignored))
    {
        // This compatibility path has no queue-acceptance callback.  Release
        // immediately so it cannot block the next recording or claim success.
        ReleaseUnsubmitted(ignored);
    }
}

bool RayTracingSceneManager::NotifySubmission(const RayTracingSceneRecordingToken& token)
{
    if (!token.IsValid() ||
        token.preparationGeneration != m_activeRecording.preparationGeneration ||
        token.recordingGeneration != m_activeRecording.recordingGeneration)
    {
        m_stats.fallbackCode = RayTracingSceneFallbackCode::InvalidRecordingToken;
        m_stats.fallbackReason = "ray tracing submission token does not match active recording";
        return false;
    }

    if (!m_recordedCompactionQueries.empty() ||
        !m_recordedCompactionCopies.empty())
    {
        m_stats.fallbackCode = RayTracingSceneFallbackCode::InvalidRecordingToken;
        m_stats.fallbackReason =
            "BLAS compaction recording requires completion-aware submission";
        return false;
    }

    return AcceptRecordingSubmission(token);
}

bool RayTracingSceneManager::NotifySubmission(
    const RayTracingSceneRecordingToken& token,
    const GPUCompletionToken& completion,
    const RenderSubmissionTracker& tracker)
{
    if (!token.IsValid() ||
        token.preparationGeneration != m_activeRecording.preparationGeneration ||
        token.recordingGeneration != m_activeRecording.recordingGeneration ||
        !IsExactCurrentGraphicsSubmissionToken(tracker, completion))
    {
        m_stats.fallbackCode = RayTracingSceneFallbackCode::InvalidRecordingToken;
        m_stats.fallbackReason =
            "ray tracing compaction submission lacks exact accepted provenance";
        return false;
    }

    if (!AcceptRecordingSubmission(token))
    {
        return false;
    }

    for (BLASCacheEntry* entry : m_recordedCompactionQueries)
    {
        if (!entry || !entry->compaction ||
            entry->compaction->status != RayTracingBLASCompactionStatus::QueryRecorded)
        {
            continue;
        }
        entry->compaction->queryCompletion = completion;
        entry->compaction->status = RayTracingBLASCompactionStatus::AwaitingQueryCompletion;
    }
    for (BLASCacheEntry* entry : m_recordedCompactionCopies)
    {
        if (!entry || !entry->compaction ||
            entry->compaction->status != RayTracingBLASCompactionStatus::CopyRecorded)
        {
            continue;
        }
        entry->compaction->copyCompletion = completion;
        entry->compaction->status = RayTracingBLASCompactionStatus::AwaitingCopyCompletion;
    }
    m_recordedCompactionQueries.clear();
    m_recordedCompactionCopies.clear();
    UpdateCompactionStats();
    return true;
}

bool RayTracingSceneManager::AcceptRecordingSubmission(
    const RayTracingSceneRecordingToken& token)
{

    for (BLASCacheEntry* entry : m_recordedBLASBuilds)
    {
        if (!entry)
            continue;

        entry->needsBuild = false;
        entry->scratchReleasePending = true;
        entry->scratchLastUsedFrame = m_frameCounter;
    }
    m_pendingBLASBuilds.clear();
    m_recordedBLASBuilds.clear();
    m_submittedTLASStructuralSignature = m_preparedTLASStructuralSignature;
    ++m_tlasGeneration;
    if (m_tlasGeneration == 0)
    {
        ++m_tlasGeneration;
    }
    m_executionReceipt = {
        token.preparationGeneration,
        m_tlasGeneration,
        m_submittedTLASStructuralSignature,
        m_preparedTLASUpdate,
        true};
    m_stats.submittedTLASBuild = true;
    m_stats.submittedTLASUpdate = m_preparedTLASUpdate;
    m_stats.submittedGeneration = token.preparationGeneration;
    m_stats.tlasGeneration = m_tlasGeneration;
    m_stats.pendingBLASBuildCount = 0;
    m_activeRecording = {};
    UpdateResourceStats();
    return true;
}

void RayTracingSceneManager::ReleaseUnsubmitted(const RayTracingSceneRecordingToken& token)
{
    if (!token.IsValid() ||
        token.preparationGeneration != m_activeRecording.preparationGeneration ||
        token.recordingGeneration != m_activeRecording.recordingGeneration)
    {
        return;
    }

    // Do not release scratch or clear needsBuild: this work was never known to
    // have reached the queue and must be recorded again by the next frame.
    for (BLASCacheEntry* entry : m_recordedCompactionQueries)
    {
        if (entry && entry->compaction)
        {
            entry->compaction->status = ReleaseUnsubmittedRayTracingBLASCompaction(
                entry->compaction->status);
        }
    }
    for (BLASCacheEntry* entry : m_recordedCompactionCopies)
    {
        if (entry && entry->compaction)
        {
            entry->compaction->status = ReleaseUnsubmittedRayTracingBLASCompaction(
                entry->compaction->status);
        }
    }
    m_recordedBLASBuilds.clear();
    m_recordedCompactionQueries.clear();
    m_recordedCompactionCopies.clear();
    m_activeRecording = {};
    m_stats.recordedBLASBuildCount = 0;
    m_stats.recordedTLASBuild = false;
    m_stats.recordedTLASUpdate = false;
    UpdateCompactionStats();
}

bool RayTracingSceneManager::PollCompletedBLASCompaction(
    const RenderSubmissionTracker& tracker)
{
    bool progressed = false;
    for (BLASCacheEntry& entry : m_blasCache)
    {
        if (!entry.compaction)
        {
            continue;
        }

        BLASCacheEntry::BLASCompactionState& state = *entry.compaction;
        if (state.status == RayTracingBLASCompactionStatus::AwaitingQueryCompletion)
        {
            const GPUCompletionStatus completion = tracker.Query(state.queryCompletion);
            if (completion == GPUCompletionStatus::Pending)
            {
                continue;
            }
            if (!IsCompletionSatisfied(completion) || !state.readbackBuffer ||
                !entry.accelerationStructure)
            {
                state.status = RayTracingBLASCompactionStatus::Failed;
                continue;
            }

            const void* mapped = state.readbackBuffer->Map();
            if (!mapped)
            {
                state.status = RayTracingBLASCompactionStatus::Failed;
                continue;
            }
            uint64 compactedSize = 0;
            std::memcpy(&compactedSize, mapped, sizeof(compactedSize));
            state.readbackBuffer->Unmap();

            const uint64 sourceSize = entry.accelerationStructure->GetSize();
            const RayTracingBLASCompactedSizeDisposition sizeDisposition =
                ClassifyRayTracingBLASCompactedSize(sourceSize, compactedSize);
            if (sizeDisposition == RayTracingBLASCompactedSizeDisposition::Invalid)
            {
                state.status = RayTracingBLASCompactionStatus::Failed;
                state.query.Reset();
                state.readbackBuffer.Reset();
                continue;
            }
            if (sizeDisposition == RayTracingBLASCompactedSizeDisposition::Unchanged)
            {
                state.status = RayTracingBLASCompactionStatus::Skipped;
                state.query.Reset();
                state.readbackBuffer.Reset();
                progressed = true;
                continue;
            }

            RHIAccelerationStructureDesc compactDesc;
            compactDesc.type = RHIAccelerationStructureType::BottomLevel;
            compactDesc.size = compactedSize;
            compactDesc.debugName = "RayTracingSceneBLASCompacted";
            RHIAccelerationStructureRef destination =
                m_device->CreateAccelerationStructure(compactDesc);
            if (!destination)
            {
                state.status = RayTracingBLASCompactionStatus::Failed;
                state.query.Reset();
                state.readbackBuffer.Reset();
                continue;
            }

            state.compactedSize = compactedSize;
            state.compactDestination = std::move(destination);
            state.query.Reset();
            state.readbackBuffer.Reset();
            state.status = RayTracingBLASCompactionStatus::ReadyToRecordCopy;
            progressed = true;
            continue;
        }

        if (state.status == RayTracingBLASCompactionStatus::AwaitingCopyCompletion)
        {
            const GPUCompletionStatus completion = tracker.Query(state.copyCompletion);
            if (completion == GPUCompletionStatus::Pending)
            {
                continue;
            }
            if (!IsCompletionSatisfied(completion) || !state.compactDestination ||
                !entry.accelerationStructure ||
                state.compactDestination.Get() == entry.accelerationStructure.Get())
            {
                state.status = RayTracingBLASCompactionStatus::Failed;
                if (IsCompletionSatisfied(completion))
                {
                    state.compactDestination.Reset();
                }
                continue;
            }

            // The exact copy submission is complete; old owner can no longer
            // be accessed by that copy.  Retirement keeps an additional
            // conservative queue-safe lifetime until the caller flushes it.
            QueueRenderOwnerRetirement(
                entry.accelerationStructure, m_pendingOwnerRetirements);
            entry.accelerationStructure = std::move(state.compactDestination);
            state.copyCompletion = {};
            state.status = RayTracingBLASCompactionStatus::Compacted;
            progressed = true;
        }
    }

    UpdateCompactionStats();
    UpdateResourceStats();
    return progressed;
}

bool RayTracingSceneManager::HasPendingBLASCompaction() const
{
    for (const BLASCacheEntry& entry : m_blasCache)
    {
        if (!entry.compaction)
        {
            continue;
        }
        switch (entry.compaction->status)
        {
            case RayTracingBLASCompactionStatus::AwaitingQueryRecord:
            case RayTracingBLASCompactionStatus::QueryRecorded:
            case RayTracingBLASCompactionStatus::AwaitingQueryCompletion:
            case RayTracingBLASCompactionStatus::ReadyToRecordCopy:
            case RayTracingBLASCompactionStatus::CopyRecorded:
            case RayTracingBLASCompactionStatus::AwaitingCopyCompletion:
                return true;
            default:
                break;
        }
    }
    return false;
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
    RHIBottomLevelASDesc requestedDesc = build.desc;
    const bool compactionEligible =
        m_blasCompactionEnabled &&
        m_device->GetCapabilities().supportsAccelerationStructureCompaction;
    if (compactionEligible)
    {
        // The current planner excludes skinned geometry, so every cached BLAS
        // here is immutable mesh content with only rigid instance transforms.
        requestedDesc.buildFlags = requestedDesc.buildFlags |
            RHIAccelerationStructureBuildFlags::AllowCompaction;
    }

    BLASCacheEntry* entry = FindBLAS(build.key);
    const bool existing = entry != nullptr;
    if (!entry)
    {
        m_blasCache.push_back({});
        entry = &m_blasCache.back();
        entry->key = build.key;
    }

    entry->lastUsedFrame = m_frameCounter;

    const bool descChanged = existing && !AreBLASDescriptionsEquivalent(entry->desc, requestedDesc);
    const bool sizesMissing = !entry->sizes.IsValid();
    const bool hasCompactedOwner = entry->compaction &&
        entry->compaction->status == RayTracingBLASCompactionStatus::Compacted;
    const bool accelerationStructureMissing =
        !entry->accelerationStructure ||
        (!hasCompactedOwner && !sizesMissing &&
         entry->accelerationStructure->GetSize() < entry->sizes.accelerationStructureSize);
    const bool buildRequired = !existing || descChanged || sizesMissing || accelerationStructureMissing || entry->needsBuild;
    if (buildRequired)
    {
        const RHIAccelerationStructureBuildSizes sizes =
            m_device->GetBottomLevelASBuildSizes(requestedDesc);
        if (!sizes.IsValid())
            return nullptr;

        const bool needsAS =
            !entry->accelerationStructure ||
            entry->accelerationStructure->GetSize() < sizes.accelerationStructureSize;

        entry->desc = requestedDesc;
        entry->sizes = sizes;
        entry->scratchReleasePending = false;
        entry->needsBuild = true;
        if (entry->compaction)
        {
            const RayTracingBLASCompactionStatus status = entry->compaction->status;
            if (status == RayTracingBLASCompactionStatus::QueryRecorded ||
                status == RayTracingBLASCompactionStatus::AwaitingQueryCompletion ||
                status == RayTracingBLASCompactionStatus::CopyRecorded ||
                status == RayTracingBLASCompactionStatus::AwaitingCopyCompletion)
            {
                // A submitted query/copy may still reference this exact
                // source owner. Fail the new build request rather than mutate
                // or retire an in-flight compaction resource.
                return nullptr;
            }
            else
            {
                ResetBLASCompaction(*entry);
            }
        }
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

        if (compactionEligible && !EnsureBLASCompactionResources(*entry))
        {
            // Compaction is opportunistic.  Preserve the valid build path and
            // make the missing async resources explicit through diagnostics.
            if (entry->compaction)
            {
                entry->compaction->status = RayTracingBLASCompactionStatus::Failed;
            }
        }
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

bool RayTracingSceneManager::EnsureBLASCompactionResources(BLASCacheEntry& entry)
{
    if (!m_device || !m_device->GetCapabilities().supportsAccelerationStructureCompaction ||
        !HasFlag(entry.desc.buildFlags, RHIAccelerationStructureBuildFlags::AllowCompaction))
    {
        return false;
    }

    if (!entry.compaction)
    {
        entry.compaction = std::make_unique<BLASCacheEntry::BLASCompactionState>();
    }
    BLASCacheEntry::BLASCompactionState& state = *entry.compaction;
    if (state.status == RayTracingBLASCompactionStatus::AwaitingQueryRecord)
    {
        return state.query && state.readbackBuffer;
    }
    if (state.status != RayTracingBLASCompactionStatus::Disabled)
    {
        return false;
    }

    RHIAccelerationStructureCompactedSizeQueryDesc queryDesc;
    queryDesc.count = 1;
    queryDesc.queueType = RHICommandQueueType::Graphics;
    queryDesc.debugName = "RayTracingSceneBLASCompactSize";
    state.query = m_device->CreateAccelerationStructureCompactedSizeQuery(queryDesc);
    if (!state.query)
    {
        state.status = RayTracingBLASCompactionStatus::Failed;
        return false;
    }

    RHIBufferDesc readbackDesc;
    readbackDesc.size = RVX_RAY_TRACING_COMPACTION_READBACK_SIZE;
    readbackDesc.usage = RHIBufferUsage::CopyDst;
    readbackDesc.memoryType = RHIMemoryType::Readback;
    readbackDesc.stride = sizeof(uint64);
    readbackDesc.debugName = "RayTracingSceneBLASCompactReadback";
    state.readbackBuffer = m_device->CreateBuffer(readbackDesc);
    if (!state.readbackBuffer)
    {
        state.query.Reset();
        state.status = RayTracingBLASCompactionStatus::Failed;
        return false;
    }

    state.status = RayTracingBLASCompactionStatus::AwaitingQueryRecord;
    return true;
}

void RayTracingSceneManager::ResetBLASCompaction(BLASCacheEntry& entry)
{
    if (!entry.compaction)
    {
        return;
    }

    BLASCacheEntry::BLASCompactionState& state = *entry.compaction;
    state.query.Reset();
    state.readbackBuffer.Reset();
    state.compactDestination.Reset();
    state.queryCompletion = {};
    state.copyCompletion = {};
    state.compactedSize = 0;
    state.status = RayTracingBLASCompactionStatus::Disabled;
}

void RayTracingSceneManager::UpdateCompactionStats()
{
    m_stats.compactionQueryRecordedCount = 0;
    m_stats.compactionQueryPendingCount = 0;
    m_stats.compactionCopyRecordedCount = 0;
    m_stats.compactionCopyPendingCount = 0;
    m_stats.compactedBLASCount = 0;
    m_stats.skippedBLASCompactionCount = 0;
    m_stats.failedBLASCompactionCount = 0;

    for (const BLASCacheEntry& entry : m_blasCache)
    {
        if (!entry.compaction)
        {
            continue;
        }
        switch (entry.compaction->status)
        {
            case RayTracingBLASCompactionStatus::QueryRecorded:
                ++m_stats.compactionQueryRecordedCount;
                break;
            case RayTracingBLASCompactionStatus::AwaitingQueryCompletion:
                ++m_stats.compactionQueryPendingCount;
                break;
            case RayTracingBLASCompactionStatus::CopyRecorded:
                ++m_stats.compactionCopyRecordedCount;
                break;
            case RayTracingBLASCompactionStatus::AwaitingCopyCompletion:
                ++m_stats.compactionCopyPendingCount;
                break;
            case RayTracingBLASCompactionStatus::Compacted:
                ++m_stats.compactedBLASCount;
                break;
            case RayTracingBLASCompactionStatus::Skipped:
                ++m_stats.skippedBLASCompactionCount;
                break;
            case RayTracingBLASCompactionStatus::Failed:
                ++m_stats.failedBLASCompactionCount;
                break;
            default:
                break;
        }
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
        const bool compactionInFlight = it->compaction &&
            IsRayTracingBLASCompactionInFlight(it->compaction->status);

        if (!requestedThisFrame && unusedLongEnough && !compactionInFlight)
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
            const bool compactionInFlight = it->compaction &&
                IsRayTracingBLASCompactionInFlight(it->compaction->status);
            if (isRequestedThisFrame(it->key) || compactionInFlight)
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

bool RayTracingSceneManager::EnsureTopLevelAS(const RHITopLevelASDesc& desc, bool& outReplaced)
{
    outReplaced = false;
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
        outReplaced = true;
    }

    m_topLevelSizes = sizes;
    return EnsureScratchBuffer(
        m_topLevelScratchBuffer,
        std::max(sizes.buildScratchSize, sizes.updateScratchSize),
        "RayTracingTLASScratch");
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
    m_recordedCompactionQueries.clear();
    m_recordedCompactionCopies.clear();
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
    m_preparedTLASStructuralSignature = {};
    m_preparedTLASUpdate = false;
    m_recordedBLASBuilds.clear();
    m_stats = {};
}

void RayTracingSceneManager::InvalidateFrameOutputs()
{
    m_frameBLAS.clear();
    m_pendingBLASBuilds.clear();
    m_recordedCompactionQueries.clear();
    m_recordedCompactionCopies.clear();
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
    m_preparedTLASStructuralSignature = {};
    m_submittedTLASStructuralSignature = {};
    m_preparedTLASUpdate = false;
    m_activeRecording = {};
    m_recordedBLASBuilds.clear();
    m_executionReceipt = {};
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
