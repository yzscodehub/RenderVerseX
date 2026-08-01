#include "RenderGraphInternal.h"
#include "Core/Diagnostics/JsonWriter.h"
#include "Core/Log.h"
#include "Resources/RenderSubmissionResourceBatch.h"
#include <algorithm>
#include <fstream>
#include <sstream>

namespace RVX
{
    namespace
    {
        using Diagnostics::JsonBool;
        using Diagnostics::JsonString;

        uint64 AlignUp(uint64 value, uint64 alignment)
        {
            return alignment == 0 ? value : ((value + alignment - 1) / alignment) * alignment;
        }

        uint64 EstimateTextureMemorySize(const RHITextureDesc& desc)
        {
            uint64 bytesPerPixel = GetFormatBytesPerPixel(desc.format);
            if (bytesPerPixel == 0)
            {
                bytesPerPixel = 4;
            }

            uint64 totalSize = 0;
            uint32 width = std::max(1u, desc.width);
            uint32 height = std::max(1u, desc.height);
            uint32 depth = std::max(1u, desc.depth);
            const uint32 mipLevels = std::max(1u, desc.mipLevels);

            for (uint32 mip = 0; mip < mipLevels; ++mip)
            {
                totalSize += static_cast<uint64>(width) * height * depth * bytesPerPixel * std::max(1u, desc.arraySize);
                width = std::max(1u, width / 2);
                height = std::max(1u, height / 2);
                depth = std::max(1u, depth / 2);
            }

            totalSize *= static_cast<uint32>(desc.sampleCount);
            return AlignUp(totalSize, 65536);
        }

        uint64 EstimateBufferMemorySize(const RHIBufferDesc& desc)
        {
            return AlignUp(desc.size, 256);
        }

        RHICommandQueueType GetQueueType(RenderGraphPassType passType)
        {
            switch (passType)
            {
                case RenderGraphPassType::Compute: return RHICommandQueueType::Compute;
                case RenderGraphPassType::Copy: return RHICommandQueueType::Copy;
                case RenderGraphPassType::Graphics:
                case RenderGraphPassType::RayTracing:
                default: return RHICommandQueueType::Graphics;
            }
        }

        GPUQueueDomain GetPhysicalDomain(IRHIDevice* device, RenderGraphPassType passType)
        {
            GPUQueueDomain domain = GPUQueueDomain::Graphics;
            if (device)
            {
                TryGetGPUQueueDomain(
                    device->GetCapabilities().queueTopology,
                    GetQueueType(passType),
                    domain);
            }
            return domain;
        }

        RHIShaderStage GetDefaultShaderStages(RenderGraphPassType passType)
        {
            switch (passType)
            {
                case RenderGraphPassType::Compute: return RHIShaderStage::Compute;
                case RenderGraphPassType::RayTracing: return RHIShaderStage::AllRayTracing;
                case RenderGraphPassType::Copy: return RHIShaderStage::None;
                case RenderGraphPassType::Graphics:
                default: return RHIShaderStage::AllGraphics;
            }
        }

        RenderGraph::DiagnosticResourceType ToDiagnosticResourceType(ResourceType type)
        {
            return type == ResourceType::Texture
                       ? RenderGraph::DiagnosticResourceType::Texture
                       : RenderGraph::DiagnosticResourceType::Buffer;
        }

        RenderGraph::DiagnosticAccessType ToDiagnosticAccessType(RGAccessType access)
        {
            switch (access)
            {
                case RGAccessType::Write:
                    return RenderGraph::DiagnosticAccessType::Write;
                case RGAccessType::ReadWrite:
                    return RenderGraph::DiagnosticAccessType::ReadWrite;
                case RGAccessType::Read:
                default:
                    return RenderGraph::DiagnosticAccessType::Read;
            }
        }

        const char* ToDiagnosticString(RenderGraphPassType type)
        {
            switch (type)
            {
                case RenderGraphPassType::Graphics:
                    return "Graphics";
                case RenderGraphPassType::Compute:
                    return "Compute";
                case RenderGraphPassType::RayTracing:
                    return "RayTracing";
                case RenderGraphPassType::Copy:
                    return "Copy";
                default:
                    return "Unknown";
            }
        }

        const char* ToDiagnosticString(RenderGraph::DiagnosticResourceType type)
        {
            return type == RenderGraph::DiagnosticResourceType::Texture ? "Texture" : "Buffer";
        }

        const char* ToDiagnosticString(RenderGraph::DiagnosticAccessType access)
        {
            switch (access)
            {
                case RenderGraph::DiagnosticAccessType::Write:
                    return "Write";
                case RenderGraph::DiagnosticAccessType::ReadWrite:
                    return "ReadWrite";
                case RenderGraph::DiagnosticAccessType::Read:
                default:
                    return "Read";
            }
        }

        const char* ToDiagnosticString(RenderGraph::DiagnosticExecutionQueue queue)
        {
            switch (queue)
            {
                case RenderGraph::DiagnosticExecutionQueue::Graphics:
                    return "Graphics";
                case RenderGraph::DiagnosticExecutionQueue::Compute:
                    return "Compute";
                case RenderGraph::DiagnosticExecutionQueue::Unknown:
                default:
                    return "Unknown";
            }
        }

        RenderGraph::DiagnosticExecutionQueue ToDiagnosticExecutionQueue(RenderGraphPassType type)
        {
            return type == RenderGraphPassType::Compute
                       ? RenderGraph::DiagnosticExecutionQueue::Compute
                       : RenderGraph::DiagnosticExecutionQueue::Graphics;
        }

        const char* ToDiagnosticString(RenderGraph::DiagnosticSyncReason reason)
        {
            switch (reason)
            {
                case RenderGraph::DiagnosticSyncReason::FinalQueueJoin:
                    return "FinalQueueJoin";
                case RenderGraph::DiagnosticSyncReason::CrossQueueDependency:
                default:
                    return "CrossQueueDependency";
            }
        }

        const char* ToDiagnosticString(RenderGraph::AsyncComputeFallbackReason reason)
        {
            switch (reason)
            {
                case RenderGraph::AsyncComputeFallbackReason::None:
                    return "None";
                case RenderGraph::AsyncComputeFallbackReason::GraphNotCompiled:
                    return "GraphNotCompiled";
                case RenderGraph::AsyncComputeFallbackReason::BackendUnsupported:
                    return "BackendUnsupported";
                case RenderGraph::AsyncComputeFallbackReason::QueueFenceSignalUnsupported:
                    return "QueueFenceSignalUnsupported";
                case RenderGraph::AsyncComputeFallbackReason::QueueFenceWaitUnsupported:
                    return "QueueFenceWaitUnsupported";
                case RenderGraph::AsyncComputeFallbackReason::MissingComputeContext:
                    return "MissingComputeContext";
                case RenderGraph::AsyncComputeFallbackReason::MissingFence:
                    return "MissingFence";
                case RenderGraph::AsyncComputeFallbackReason::NoEligibleComputePasses:
                    return "NoEligibleComputePasses";
                default:
                    return "Unknown";
            }
        }

        void WriteOptionalIndex(std::ostringstream& ss, uint32 value)
        {
            if (value == RVX_INVALID_INDEX)
            {
                ss << "null";
            }
            else
            {
                ss << value;
            }
        }

        void WriteIndexArray(std::ostringstream& ss, const std::vector<uint32>& values)
        {
            ss << "[";
            for (size_t i = 0; i < values.size(); ++i)
            {
                if (i > 0)
                {
                    ss << ", ";
                }
                ss << values[i];
            }
            ss << "]";
        }

        std::string FormatPassIndexList(const std::vector<uint32>& values)
        {
            std::ostringstream ss;
            ss << "[";
            for (size_t i = 0; i < values.size(); ++i)
            {
                if (i > 0)
                {
                    ss << ",";
                }
                ss << values[i];
            }
            ss << "]";
            return ss.str();
        }

        std::vector<RenderGraph::PassDiagnostic> BuildPassDiagnostics(const RenderGraphImpl& graph)
        {
            std::vector<RenderGraph::PassDiagnostic> passDiagnostics;
            passDiagnostics.reserve(graph.passes.size());
            for (uint32 passIndex = 0; passIndex < graph.passes.size(); ++passIndex)
            {
                const Pass& pass = graph.passes[passIndex];
                RenderGraph::PassDiagnostic passDiagnostic;
                passDiagnostic.index = passIndex;
                passDiagnostic.name = pass.name;
                passDiagnostic.type = pass.type;
                passDiagnostic.culled = pass.culled;
                passDiagnostic.executedLastRun = pass.executedLastRun;
                passDiagnostic.executionQueue = pass.lastExecutionQueue;
                passDiagnostic.executionSerial = pass.lastExecutionSerial;
                passDiagnostic.cpuDurationNanoseconds = pass.lastCpuDurationNanoseconds;
                passDiagnostic.textureBarrierCount = static_cast<uint32>(pass.textureBarriers.size());
                passDiagnostic.bufferBarrierCount = static_cast<uint32>(pass.bufferBarriers.size());
                passDiagnostic.aliasingBarrierCount = static_cast<uint32>(pass.aliasingBarriers.size());
                if (passIndex < graph.passDependencies.size())
                {
                    passDiagnostic.dependencies = graph.passDependencies[passIndex];
                }
                if (passIndex < graph.passDependents.size())
                {
                    passDiagnostic.dependents = graph.passDependents[passIndex];
                }
                passDiagnostic.usages.reserve(pass.usages.size());

                for (const ResourceUsage& usage : pass.usages)
                {
                    RenderGraph::ResourceUsageDiagnostic usageDiagnostic;
                    usageDiagnostic.type = ToDiagnosticResourceType(usage.type);
                    usageDiagnostic.access = ToDiagnosticAccessType(usage.access);
                    usageDiagnostic.resourceIndex = usage.index;
                    usageDiagnostic.desiredState = usage.desiredState;
                    usageDiagnostic.desiredAccess = usage.desiredAccess;
                    usageDiagnostic.stages = usage.stages;
                    usageDiagnostic.hasSubresourceRange = usage.hasSubresourceRange;
                    usageDiagnostic.subresourceRange = usage.subresourceRange;
                    usageDiagnostic.hasRange = usage.hasRange;
                    usageDiagnostic.offset = usage.offset;
                    usageDiagnostic.size = usage.size;
                    passDiagnostic.usages.push_back(usageDiagnostic);
                }

                passDiagnostics.push_back(std::move(passDiagnostic));
            }
            return passDiagnostics;
        }

        RenderGraph::SubmissionPlan BuildSubmissionPlan(
            const std::vector<RenderGraph::PassDiagnostic>& passes,
            const std::vector<uint32>& executionOrder)
        {
            RenderGraph::SubmissionPlan plan;
            std::vector<uint32> plannedDependencyLevels(passes.size(), 0);
            std::vector<uint8> plannedLevelResolved(passes.size(), 0);
            for (uint32 passIndex : executionOrder)
            {
                if (passIndex >= passes.size())
                    continue;

                const RenderGraph::PassDiagnostic& pass = passes[passIndex];
                if (pass.culled)
                    continue;

                uint32 dependencyLevel = 0;
                for (uint32 dependencyIndex : pass.dependencies)
                {
                    if (dependencyIndex >= passes.size() ||
                        passes[dependencyIndex].culled ||
                        !plannedLevelResolved[dependencyIndex])
                    {
                        continue;
                    }

                    dependencyLevel = std::max(dependencyLevel, plannedDependencyLevels[dependencyIndex] + 1u);
                }

                plannedDependencyLevels[passIndex] = dependencyLevel;
                plannedLevelResolved[passIndex] = 1;

                const RenderGraph::DiagnosticExecutionQueue queue = ToDiagnosticExecutionQueue(pass.type);
                auto batchIt = std::find_if(
                    plan.queueBatches.begin(),
                    plan.queueBatches.end(),
                    [&](const RenderGraph::PlannedQueueBatchDiagnostic& batch)
                    {
                        return batch.dependencyLevel == dependencyLevel && batch.queue == queue;
                    });

                if (batchIt == plan.queueBatches.end())
                {
                    RenderGraph::PlannedQueueBatchDiagnostic batch;
                    batch.batchIndex = static_cast<uint32>(plan.queueBatches.size());
                    batch.dependencyLevel = dependencyLevel;
                    batch.queue = queue;
                    batch.passIndices.push_back(passIndex);
                    plan.queueBatches.push_back(std::move(batch));
                }
                else
                {
                    batchIt->passIndices.push_back(passIndex);
                }
            }

            plan.queueBatchCount = static_cast<uint32>(plan.queueBatches.size());
            uint32 maxPlannedDependencyLevel = 0;
            bool hasPlannedDependencyLevel = false;
            for (const RenderGraph::PlannedQueueBatchDiagnostic& batch : plan.queueBatches)
            {
                hasPlannedDependencyLevel = true;
                maxPlannedDependencyLevel = std::max(maxPlannedDependencyLevel, batch.dependencyLevel);
                if (batch.queue == RenderGraph::DiagnosticExecutionQueue::Compute)
                {
                    plan.computeBatchCount++;
                }
            }
            plan.dependencyLevelCount =
                hasPlannedDependencyLevel ? (maxPlannedDependencyLevel + 1u) : 0u;

            for (uint32 level = 0; level < plan.dependencyLevelCount; ++level)
            {
                bool hasGraphics = false;
                bool hasCompute = false;
                for (const RenderGraph::PlannedQueueBatchDiagnostic& batch : plan.queueBatches)
                {
                    if (batch.dependencyLevel != level)
                        continue;

                    hasGraphics = hasGraphics || batch.queue == RenderGraph::DiagnosticExecutionQueue::Graphics;
                    hasCompute = hasCompute || batch.queue == RenderGraph::DiagnosticExecutionQueue::Compute;
                }

                if (hasGraphics && hasCompute)
                {
                    plan.asyncOverlapCandidateLevelCount++;
                }
            }

            std::vector<uint32> plannedBatchIndexByPass(passes.size(), RVX_INVALID_INDEX);
            for (const RenderGraph::PlannedQueueBatchDiagnostic& batch : plan.queueBatches)
            {
                for (uint32 passIndex : batch.passIndices)
                {
                    if (passIndex < plannedBatchIndexByPass.size())
                    {
                        plannedBatchIndexByPass[passIndex] = batch.batchIndex;
                    }
                }
            }

            for (RenderGraph::PlannedQueueBatchDiagnostic& targetBatch : plan.queueBatches)
            {
                for (uint32 targetPassIndex : targetBatch.passIndices)
                {
                    if (targetPassIndex >= passes.size())
                        continue;

                    const RenderGraph::PassDiagnostic& targetPass = passes[targetPassIndex];
                    for (uint32 sourcePassIndex : targetPass.dependencies)
                    {
                        if (sourcePassIndex >= passes.size() ||
                            sourcePassIndex >= plannedBatchIndexByPass.size())
                        {
                            continue;
                        }

                        const uint32 sourceBatchIndex = plannedBatchIndexByPass[sourcePassIndex];
                        if (sourceBatchIndex == RVX_INVALID_INDEX ||
                            sourceBatchIndex == targetBatch.batchIndex ||
                            sourceBatchIndex >= plan.queueBatches.size())
                        {
                            continue;
                        }

                        const RenderGraph::PlannedQueueBatchDiagnostic& sourceBatch =
                            plan.queueBatches[sourceBatchIndex];
                        if (sourceBatch.queue == targetBatch.queue)
                            continue;

                        auto syncIt = std::find_if(
                            plan.queueSyncs.begin(),
                            plan.queueSyncs.end(),
                            [&](const RenderGraph::PlannedQueueSyncDiagnostic& sync)
                            {
                                return sync.sourceBatchIndex == sourceBatchIndex &&
                                       sync.targetBatchIndex == targetBatch.batchIndex;
                            });

                        uint32 syncIndex = RVX_INVALID_INDEX;
                        if (syncIt == plan.queueSyncs.end())
                        {
                            RenderGraph::PlannedQueueSyncDiagnostic sync;
                            sync.syncIndex = static_cast<uint32>(plan.queueSyncs.size());
                            sync.sourceBatchIndex = sourceBatchIndex;
                            sync.targetBatchIndex = targetBatch.batchIndex;
                            sync.sourceQueue = sourceBatch.queue;
                            sync.targetQueue = targetBatch.queue;
                            sync.reason = RenderGraph::DiagnosticSyncReason::CrossQueueDependency;
                            sync.sourcePassIndex = sourcePassIndex;
                            sync.targetPassIndex = targetPassIndex;
                            syncIndex = sync.syncIndex;
                            plan.queueSyncs.push_back(sync);
                        }
                        else
                        {
                            syncIndex = syncIt->syncIndex;
                        }

                        if (std::find(
                                targetBatch.prerequisiteSyncIndices.begin(),
                                targetBatch.prerequisiteSyncIndices.end(),
                                syncIndex) == targetBatch.prerequisiteSyncIndices.end())
                        {
                            targetBatch.prerequisiteSyncIndices.push_back(syncIndex);
                        }
                    }
                }
            }
            plan.queueSyncCount = static_cast<uint32>(plan.queueSyncs.size());
            for (const RenderGraph::PlannedQueueSyncDiagnostic& sync : plan.queueSyncs)
            {
                if (sync.reason == RenderGraph::DiagnosticSyncReason::CrossQueueDependency)
                {
                    plan.crossQueueSyncCount++;
                }
            }
            return plan;
        }
    } // namespace

    class RenderGraph::Impl : public RenderGraphImpl
    {
    };

    std::vector<RenderGraph::PassDiagnostic> BuildRenderGraphPassDiagnostics(const RenderGraphImpl& graph)
    {
        return BuildPassDiagnostics(graph);
    }

    RenderGraph::SubmissionPlan BuildRenderGraphSubmissionPlan(
        const std::vector<RenderGraph::PassDiagnostic>& passes,
        const std::vector<uint32>& executionOrder)
    {
        return BuildSubmissionPlan(passes, executionOrder);
    }

    RenderGraph::SubmissionPlan BuildRenderGraphSubmissionPlan(const RenderGraphImpl& graph)
    {
        std::vector<RenderGraph::PassDiagnostic> passDiagnostics = BuildRenderGraphPassDiagnostics(graph);
        return BuildRenderGraphSubmissionPlan(passDiagnostics, graph.executionOrder);
    }

    RGTextureHandle RGTextureHandle::Subresource(uint32 mipLevel, uint32 arraySlice) const
    {
        RGTextureHandle handle = *this;
        handle.hasSubresourceRange = true;
        handle.subresourceRange = RHISubresourceRange{mipLevel, 1, arraySlice, 1, RHITextureAspect::Color};
        return handle;
    }

    RGTextureHandle RGTextureHandle::MipRange(uint32 baseMip, uint32 mipCount) const
    {
        RGTextureHandle handle = *this;
        handle.hasSubresourceRange = true;
        handle.subresourceRange = RHISubresourceRange{baseMip, mipCount, 0, RVX_ALL_LAYERS, RHITextureAspect::Color};
        return handle;
    }

    RGBufferHandle RGBufferHandle::Range(uint64 offset, uint64 size) const
    {
        RGBufferHandle handle = *this;
        handle.hasRange = true;
        handle.rangeOffset = offset;
        handle.rangeSize = size;
        return handle;
    }

    class RenderGraphBuilder::Impl
    {
    public:
        std::vector<TextureResource>* textures = nullptr;
        std::vector<BufferResource>* buffers = nullptr;
        Pass* pass = nullptr;
        IRHIDevice* device = nullptr;
        uint32* compatibilityStateProjectionCount = nullptr;
    };

    RenderGraph::RenderGraph() : m_impl(std::make_unique<Impl>()) {}
    RenderGraph::~RenderGraph() = default;

    void RenderGraph::SetDevice(IRHIDevice* device)
    {
        m_impl->device = device;
        m_impl->enableMemoryAliasing =
            m_impl->memoryAliasingRequested && device &&
            device->GetCapabilities().supportsExplicitAliasingBarriers;
    }

    void RenderGraph::SetTransientResourcePool(TransientResourcePool* pool)
    {
        m_impl->transientResourcePool = pool;
    }

    RGTextureHandle RenderGraph::CreateTexture(const RHITextureDesc& desc)
    {
        TextureResource resource;
        resource.desc = desc;
        resource.initialState = RHIResourceState::Undefined;
        resource.currentState = resource.initialState;
        resource.initialAccessSnapshot = MakeRHITextureAccessSnapshot(
            RHIResourceState::Undefined,
            RHIShaderStage::None,
            GPUQueueDomain::Graphics,
            RHIContentValidity::Invalid);
        resource.currentAccessSnapshot = resource.initialAccessSnapshot;
        resource.imported = false;
        m_impl->textures.push_back(std::move(resource));
        return RGTextureHandle{static_cast<uint32>(m_impl->textures.size() - 1)};
    }

    RGBufferHandle RenderGraph::CreateBuffer(const RHIBufferDesc& desc)
    {
        BufferResource resource;
        resource.desc = desc;
        resource.initialState = RHIResourceState::Undefined;
        resource.currentState = resource.initialState;
        resource.initialAccessSnapshot = MakeRHIBufferAccessSnapshot(
            RHIResourceState::Undefined,
            RHIShaderStage::None,
            GPUQueueDomain::Graphics,
            RHIContentValidity::Invalid);
        resource.currentAccessSnapshot = resource.initialAccessSnapshot;
        resource.imported = false;
        m_impl->buffers.push_back(std::move(resource));
        return RGBufferHandle{static_cast<uint32>(m_impl->buffers.size() - 1)};
    }

    RGTextureHandle RenderGraph::ImportTexture(RHITexture* texture, RHIResourceState initialState)
    {
        ++m_impl->compatibilityStateProjectionCount;
        return ImportTexture(texture, MakeRHITextureAccessSnapshot(initialState));
    }

    RGTextureHandle RenderGraph::ImportTexture(
        RHITexture* texture,
        const RHITextureAccessSnapshot& initialAccess)
    {
        TextureResource resource;
        // For imported textures, store a raw pointer without taking ownership
        // The external owner (e.g., swap chain) is responsible for the texture's lifetime
        // We use importedRaw to store non-owning pointer for imported resources
        resource.importedRaw = texture;
        if (texture)
        {
            resource.desc.width = texture->GetWidth();
            resource.desc.height = texture->GetHeight();
            resource.desc.depth = texture->GetDepth();
            resource.desc.mipLevels = texture->GetMipLevels();
            resource.desc.arraySize = texture->GetArraySize();
            resource.desc.format = texture->GetFormat();
            resource.desc.usage = texture->GetUsage();
            resource.desc.dimension = texture->GetDimension();
            resource.desc.sampleCount = texture->GetSampleCount();
        }
        resource.initialAccessSnapshot = initialAccess;
        resource.currentAccessSnapshot = initialAccess;
        resource.initialState = ProjectRHIResourceState(initialAccess.uniformAccess);
        resource.currentState = resource.initialState;
        resource.imported = true;
        m_impl->textures.push_back(std::move(resource));
        return RGTextureHandle{static_cast<uint32>(m_impl->textures.size() - 1)};
    }

    RGBufferHandle RenderGraph::ImportBuffer(RHIBuffer* buffer, RHIResourceState initialState)
    {
        ++m_impl->compatibilityStateProjectionCount;
        return ImportBuffer(buffer, MakeRHIBufferAccessSnapshot(initialState));
    }

    RGBufferHandle RenderGraph::ImportBuffer(
        RHIBuffer* buffer,
        const RHIBufferAccessSnapshot& initialAccess)
    {
        BufferResource resource;
        // For imported buffers, store a raw pointer without taking ownership
        resource.importedRaw = buffer;
        if (buffer)
        {
            resource.desc.size = buffer->GetSize();
            resource.desc.usage = buffer->GetUsage();
            resource.desc.memoryType = buffer->GetMemoryType();
            resource.desc.stride = buffer->GetStride();
        }
        resource.initialAccessSnapshot = initialAccess;
        resource.currentAccessSnapshot = initialAccess;
        resource.initialState = ProjectRHIResourceState(initialAccess.uniformAccess);
        resource.currentState = resource.initialState;
        resource.imported = true;
        m_impl->buffers.push_back(std::move(resource));
        return RGBufferHandle{static_cast<uint32>(m_impl->buffers.size() - 1)};
    }

    void RenderGraph::SetExportState(RGTextureHandle texture, RHIResourceState finalState)
    {
        ++m_impl->compatibilityStateProjectionCount;
        SetExportAccess(texture, MakeRHIAccessSnapshot(finalState));
    }

    void RenderGraph::SetExportState(RGBufferHandle buffer, RHIResourceState finalState)
    {
        ++m_impl->compatibilityStateProjectionCount;
        SetExportAccess(buffer, MakeRHIAccessSnapshot(finalState));
    }

    void RenderGraph::SetExportAccess(
        RGTextureHandle texture,
        const RHIAccessSnapshot& finalAccess)
    {
        if (!texture.IsValid() || texture.index >= m_impl->textures.size())
            return;
        auto& resource = m_impl->textures[texture.index];
        resource.exportAccess = finalAccess;
        resource.exportState = ProjectRHIResourceState(finalAccess);
    }

    void RenderGraph::SetExportAccess(
        RGBufferHandle buffer,
        const RHIAccessSnapshot& finalAccess)
    {
        if (!buffer.IsValid() || buffer.index >= m_impl->buffers.size())
            return;
        auto& resource = m_impl->buffers[buffer.index];
        resource.exportAccess = finalAccess;
        resource.exportState = ProjectRHIResourceState(finalAccess);
    }

    RHITextureAccessSnapshot RenderGraph::GetRealizedAccess(RGTextureHandle texture) const
    {
        if (!texture.IsValid() || texture.index >= m_impl->textures.size())
            return {};

        const auto& resource = m_impl->textures[texture.index];
        if (!m_impl->executionRealized)
            return resource.initialAccessSnapshot;
        RHITextureAccessSnapshot result = resource.currentAccessSnapshot;
        result.subresourceOverrides.clear();
        for (const auto& [key, access] : resource.subresourceAccesses)
        {
            const uint32 mipLevels = std::max(1u, resource.desc.mipLevels);
            result.subresourceOverrides.push_back({
                RHISubresourceRange{key % mipLevels,
                                    1,
                                    key / mipLevels,
                                    1,
                                    IsDepthFormat(resource.desc.format)
                                        ? RHITextureAspect::Depth
                                        : RHITextureAspect::Color},
                access});
        }
        std::sort(
            result.subresourceOverrides.begin(),
            result.subresourceOverrides.end(),
            [](const RHITextureSubresourceAccessSnapshot& lhs,
               const RHITextureSubresourceAccessSnapshot& rhs)
            {
                if (lhs.range.baseArrayLayer != rhs.range.baseArrayLayer)
                    return lhs.range.baseArrayLayer < rhs.range.baseArrayLayer;
                if (lhs.range.baseMipLevel != rhs.range.baseMipLevel)
                    return lhs.range.baseMipLevel < rhs.range.baseMipLevel;
                return lhs.range.aspect < rhs.range.aspect;
            });
        return result;
    }

    RHIBufferAccessSnapshot RenderGraph::GetRealizedAccess(RGBufferHandle buffer) const
    {
        if (!buffer.IsValid() || buffer.index >= m_impl->buffers.size())
            return {};

        const auto& resource = m_impl->buffers[buffer.index];
        if (!m_impl->executionRealized)
            return resource.initialAccessSnapshot;
        RHIBufferAccessSnapshot result = resource.currentAccessSnapshot;
        result.rangeOverrides.clear();
        for (const auto& range : resource.rangeStates)
        {
            result.rangeOverrides.push_back({range.offset, range.size, range.access});
        }
        return result;
    }

    RHITexture* RenderGraph::GetTexture(RGTextureHandle handle) const
    {
        if (!handle.IsValid() || handle.index >= m_impl->textures.size())
            return nullptr;
        return m_impl->textures[handle.index].GetTexture();
    }

    RHIBuffer* RenderGraph::GetBuffer(RGBufferHandle handle) const
    {
        if (!handle.IsValid() || handle.index >= m_impl->buffers.size())
            return nullptr;
        return m_impl->buffers[handle.index].GetBuffer();
    }

    const RHITextureDesc* RenderGraph::GetTextureDesc(RGTextureHandle handle) const
    {
        if (!handle.IsValid() || handle.index >= m_impl->textures.size())
            return nullptr;
        return &m_impl->textures[handle.index].desc;
    }

    const RHIBufferDesc* RenderGraph::GetBufferDesc(RGBufferHandle handle) const
    {
        if (!handle.IsValid() || handle.index >= m_impl->buffers.size())
            return nullptr;
        return &m_impl->buffers[handle.index].desc;
    }

    void RenderGraph::AddPassInternal(
        const char* name,
        RenderGraphPassType type,
        std::function<void(RenderGraphBuilder&)> setup,
        std::function<void(RHICommandContext&)> execute)
    {
        Pass pass;
        pass.name = name ? name : "RenderPass";
        pass.type = type;
        pass.execute = std::move(execute);

        RenderGraphBuilder builder;
        RenderGraphBuilder::Impl builderImpl;
        builderImpl.textures = &m_impl->textures;
        builderImpl.buffers = &m_impl->buffers;
        builderImpl.pass = &pass;
        builderImpl.device = m_impl->device;
        builderImpl.compatibilityStateProjectionCount = &m_impl->compatibilityStateProjectionCount;
        builder.m_impl = &builderImpl;
        if (setup)
        {
            setup(builder);
        }

        m_impl->passes.push_back(std::move(pass));
    }

    RGTextureHandle RenderGraphBuilder::Read(RGTextureHandle texture, RHIShaderStage stages)
    {
        return Read(texture, RHIResourceState::ShaderResource, stages);
    }

    RGTextureHandle RenderGraphBuilder::Read(RGTextureHandle texture,
                                             RHIResourceState state,
                                             RHIShaderStage stages)
    {
        if (!m_impl || !m_impl->pass || !texture.IsValid())
            return texture;

        ResourceUsage usage;
        usage.type = ResourceType::Texture;
        usage.index = texture.index;
        usage.desiredState = state;
        usage.desiredAccess = MakeRHIAccessSnapshot(
            state,
            stages,
            GetPhysicalDomain(m_impl->device, m_impl->pass->type));
        usage.access = RGAccessType::Read;
        usage.stages = stages;  // Use the stages parameter for fine-grained barrier optimization
        usage.hasSubresourceRange = texture.hasSubresourceRange;
        if (texture.hasSubresourceRange)
            usage.subresourceRange = texture.subresourceRange;
        m_impl->pass->usages.push_back(usage);
        if (m_impl->compatibilityStateProjectionCount)
            ++*m_impl->compatibilityStateProjectionCount;
        return texture;
    }

    RGTextureHandle RenderGraphBuilder::Read(
        RGTextureHandle texture,
        const RHIAccessSnapshot& access)
    {
        if (!m_impl || !m_impl->pass || !texture.IsValid())
            return texture;

        ResourceUsage usage;
        usage.type = ResourceType::Texture;
        usage.index = texture.index;
        usage.desiredState = ProjectRHIResourceState(access);
        usage.desiredAccess = access;
        usage.access = RGAccessType::Read;
        usage.stages = GetDefaultShaderStages(m_impl->pass->type);
        usage.hasSubresourceRange = texture.hasSubresourceRange;
        if (texture.hasSubresourceRange)
            usage.subresourceRange = texture.subresourceRange;
        m_impl->pass->usages.push_back(usage);
        return texture;
    }

    RGBufferHandle RenderGraphBuilder::Read(RGBufferHandle buffer, RHIShaderStage stages)
    {
        return Read(buffer, RHIResourceState::ShaderResource, stages);
    }

    RGBufferHandle RenderGraphBuilder::Read(RGBufferHandle buffer,
                                            RHIResourceState state,
                                            RHIShaderStage stages)
    {
        if (!m_impl || !m_impl->pass || !buffer.IsValid())
            return buffer;

        ResourceUsage usage;
        usage.type = ResourceType::Buffer;
        usage.index = buffer.index;
        usage.desiredState = state;
        usage.desiredAccess = MakeRHIAccessSnapshot(
            state,
            stages,
            GetPhysicalDomain(m_impl->device, m_impl->pass->type));
        usage.access = RGAccessType::Read;
        usage.stages = stages;  // Use the stages parameter for fine-grained barrier optimization
        usage.hasRange = buffer.hasRange;
        if (buffer.hasRange)
        {
            usage.offset = buffer.rangeOffset;
            usage.size = buffer.rangeSize;
        }
        m_impl->pass->usages.push_back(usage);
        if (m_impl->compatibilityStateProjectionCount)
            ++*m_impl->compatibilityStateProjectionCount;
        return buffer;
    }

    RGBufferHandle RenderGraphBuilder::Read(
        RGBufferHandle buffer,
        const RHIAccessSnapshot& access)
    {
        if (!m_impl || !m_impl->pass || !buffer.IsValid())
            return buffer;

        ResourceUsage usage;
        usage.type = ResourceType::Buffer;
        usage.index = buffer.index;
        usage.desiredState = ProjectRHIResourceState(access);
        usage.desiredAccess = access;
        usage.access = RGAccessType::Read;
        usage.stages = GetDefaultShaderStages(m_impl->pass->type);
        usage.hasRange = buffer.hasRange;
        if (buffer.hasRange)
        {
            usage.offset = buffer.rangeOffset;
            usage.size = buffer.rangeSize;
        }
        m_impl->pass->usages.push_back(usage);
        return buffer;
    }

    RGTextureHandle RenderGraphBuilder::Write(
        RGTextureHandle texture,
        RHIResourceState state,
        RHIDiscardIntent discardIntent)
    {
        if (!m_impl || !m_impl->pass || !texture.IsValid())
            return texture;

        ResourceUsage usage;
        usage.type = ResourceType::Texture;
        usage.index = texture.index;
        usage.desiredState = state;
        usage.stages = GetDefaultShaderStages(m_impl->pass->type);
        usage.desiredAccess = MakeRHIAccessSnapshot(
            state,
            usage.stages,
            GetPhysicalDomain(m_impl->device, m_impl->pass->type));
        usage.access = RGAccessType::Write;
        usage.discardIntent = discardIntent;
        usage.hasSubresourceRange = texture.hasSubresourceRange;
        if (texture.hasSubresourceRange)
            usage.subresourceRange = texture.subresourceRange;
        m_impl->pass->usages.push_back(usage);
        if (m_impl->compatibilityStateProjectionCount)
            ++*m_impl->compatibilityStateProjectionCount;
        return texture;
    }

    RGTextureHandle RenderGraphBuilder::Write(
        RGTextureHandle texture,
        const RHIAccessSnapshot& access,
        RHIDiscardIntent discardIntent)
    {
        if (!m_impl || !m_impl->pass || !texture.IsValid())
            return texture;

        ResourceUsage usage;
        usage.type = ResourceType::Texture;
        usage.index = texture.index;
        usage.desiredState = ProjectRHIResourceState(access);
        usage.desiredAccess = access;
        usage.access = RGAccessType::Write;
        usage.discardIntent = discardIntent;
        usage.stages = GetDefaultShaderStages(m_impl->pass->type);
        usage.hasSubresourceRange = texture.hasSubresourceRange;
        if (texture.hasSubresourceRange)
            usage.subresourceRange = texture.subresourceRange;
        m_impl->pass->usages.push_back(usage);
        return texture;
    }

    RGBufferHandle RenderGraphBuilder::Write(
        RGBufferHandle buffer,
        RHIResourceState state,
        RHIDiscardIntent discardIntent)
    {
        if (!m_impl || !m_impl->pass || !buffer.IsValid())
            return buffer;

        ResourceUsage usage{};
        usage.type = ResourceType::Buffer;
        usage.index = buffer.index;
        usage.desiredState = state;
        usage.stages = GetDefaultShaderStages(m_impl->pass->type);
        usage.desiredAccess = MakeRHIAccessSnapshot(
            state,
            usage.stages,
            GetPhysicalDomain(m_impl->device, m_impl->pass->type));
        usage.access = RGAccessType::Write;
        usage.discardIntent = discardIntent;
        usage.hasRange = buffer.hasRange;
        if (buffer.hasRange)
        {
            usage.offset = buffer.rangeOffset;
            usage.size = buffer.rangeSize;
        }
        m_impl->pass->usages.push_back(usage);
        if (m_impl->compatibilityStateProjectionCount)
            ++*m_impl->compatibilityStateProjectionCount;
        return buffer;
    }

    RGBufferHandle RenderGraphBuilder::Write(
        RGBufferHandle buffer,
        const RHIAccessSnapshot& access,
        RHIDiscardIntent discardIntent)
    {
        if (!m_impl || !m_impl->pass || !buffer.IsValid())
            return buffer;

        ResourceUsage usage;
        usage.type = ResourceType::Buffer;
        usage.index = buffer.index;
        usage.desiredState = ProjectRHIResourceState(access);
        usage.desiredAccess = access;
        usage.access = RGAccessType::Write;
        usage.discardIntent = discardIntent;
        usage.stages = GetDefaultShaderStages(m_impl->pass->type);
        usage.hasRange = buffer.hasRange;
        if (buffer.hasRange)
        {
            usage.offset = buffer.rangeOffset;
            usage.size = buffer.rangeSize;
        }
        m_impl->pass->usages.push_back(usage);
        return buffer;
    }
    RGTextureHandle RenderGraphBuilder::ReadWrite(RGTextureHandle texture)
    {
        if (m_impl && m_impl->compatibilityStateProjectionCount)
            ++*m_impl->compatibilityStateProjectionCount;
        const RHIShaderStage stages = m_impl && m_impl->pass
            ? GetDefaultShaderStages(m_impl->pass->type)
            : RHIShaderStage::AllGraphics;
        const GPUQueueDomain domain = m_impl && m_impl->pass
            ? GetPhysicalDomain(m_impl->device, m_impl->pass->type)
            : GPUQueueDomain::Graphics;
        return ReadWrite(texture, MakeRHIAccessSnapshot(
            RHIResourceState::UnorderedAccess, stages, domain));
    }

    RGTextureHandle RenderGraphBuilder::ReadWrite(
        RGTextureHandle texture,
        const RHIAccessSnapshot& access)
    {
        if (!m_impl || !m_impl->pass || !texture.IsValid())
            return texture;
        ResourceUsage usage;
        usage.type = ResourceType::Texture;
        usage.index = texture.index;
        usage.desiredState = ProjectRHIResourceState(access);
        usage.desiredAccess = access;
        usage.access = RGAccessType::ReadWrite;
        usage.stages = GetDefaultShaderStages(m_impl->pass->type);
        usage.hasSubresourceRange = texture.hasSubresourceRange;
        if (texture.hasSubresourceRange)
            usage.subresourceRange = texture.subresourceRange;
        m_impl->pass->usages.push_back(usage);
        return texture;
    }
    RGBufferHandle RenderGraphBuilder::ReadWrite(RGBufferHandle buffer)
    {
        if (m_impl && m_impl->compatibilityStateProjectionCount)
            ++*m_impl->compatibilityStateProjectionCount;
        const RHIShaderStage stages = m_impl && m_impl->pass
            ? GetDefaultShaderStages(m_impl->pass->type)
            : RHIShaderStage::AllGraphics;
        const GPUQueueDomain domain = m_impl && m_impl->pass
            ? GetPhysicalDomain(m_impl->device, m_impl->pass->type)
            : GPUQueueDomain::Graphics;
        return ReadWrite(buffer, MakeRHIAccessSnapshot(
            RHIResourceState::UnorderedAccess, stages, domain));
    }

    RGBufferHandle RenderGraphBuilder::ReadWrite(
        RGBufferHandle buffer,
        const RHIAccessSnapshot& access)
    {
        if (!m_impl || !m_impl->pass || !buffer.IsValid())
            return buffer;
        ResourceUsage usage;
        usage.type = ResourceType::Buffer;
        usage.index = buffer.index;
        usage.desiredState = ProjectRHIResourceState(access);
        usage.desiredAccess = access;
        usage.access = RGAccessType::ReadWrite;
        usage.stages = GetDefaultShaderStages(m_impl->pass->type);
        usage.hasRange = buffer.hasRange;
        if (buffer.hasRange)
        {
            usage.offset = buffer.rangeOffset;
            usage.size = buffer.rangeSize;
        }
        m_impl->pass->usages.push_back(usage);
        return buffer;
    }

    RGTextureHandle RenderGraphBuilder::ReadMip(RGTextureHandle texture, uint32 mipLevel)
    {
        RGTextureHandle handle = texture;
        handle.hasSubresourceRange = true;
        handle.subresourceRange = RHISubresourceRange::Mip(mipLevel);
        return Read(handle);
    }

    RGTextureHandle RenderGraphBuilder::WriteMip(RGTextureHandle texture, uint32 mipLevel)
    {
        RGTextureHandle handle = texture;
        handle.hasSubresourceRange = true;
        handle.subresourceRange = RHISubresourceRange::Mip(mipLevel);
        return Write(handle, RHIResourceState::RenderTarget);
    }

    void RenderGraphBuilder::SetDepthStencil(RGTextureHandle texture, bool depthWrite, bool stencilWrite)
    {
        (void)stencilWrite;
        Write(texture, depthWrite ? RHIResourceState::DepthWrite : RHIResourceState::DepthRead);
    }

    void RenderGraph::Compile()
    {
        CompileRenderGraph(*m_impl);
    }

    void RenderGraph::Execute(RHICommandContext& ctx)
    {
        ExecuteRenderGraph(*m_impl, ctx);
    }

    void RenderGraph::ExecuteAsync(RHICommandContext& graphicsCtx,
                                   RHICommandContext* computeCtx,
                                   RHIFence* computeFence,
                                   uint64 frameIndex)
    {
        ExecuteRenderGraphAsync(*m_impl, graphicsCtx, computeCtx, computeFence, frameIndex);
    }

    const RenderGraph::CompileStats& RenderGraph::GetCompileStats() const
    {
        return m_impl->stats;
    }

    const std::vector<std::string>& RenderGraph::GetCompileDiagnostics() const
    {
        return m_impl->compileDiagnostics;
    }

    RenderGraph::SubmissionPlan RenderGraph::GetSubmissionPlan() const
    {
        return BuildRenderGraphSubmissionPlan(*m_impl);
    }

    RenderGraph::Diagnostics RenderGraph::GetDiagnostics() const
    {
        Diagnostics diagnostics;
        diagnostics.compileStats = m_impl->stats;
        diagnostics.executionOrder = m_impl->executionOrder;

        diagnostics.passes = BuildRenderGraphPassDiagnostics(*m_impl);
        SubmissionPlan submissionPlan = BuildRenderGraphSubmissionPlan(diagnostics.passes, diagnostics.executionOrder);
        diagnostics.plannedQueueBatches = submissionPlan.queueBatches;
        diagnostics.plannedQueueSyncs = submissionPlan.queueSyncs;
        diagnostics.plannedQueueBatchCount = submissionPlan.queueBatchCount;
        diagnostics.plannedDependencyLevelCount = submissionPlan.dependencyLevelCount;
        diagnostics.plannedAsyncOverlapCandidateLevelCount = submissionPlan.asyncOverlapCandidateLevelCount;
        diagnostics.plannedComputeBatchCount = submissionPlan.computeBatchCount;
        diagnostics.plannedQueueSyncCount = submissionPlan.queueSyncCount;
        diagnostics.plannedCrossQueueSyncCount = submissionPlan.crossQueueSyncCount;

        std::vector<uint32> executedPassIndices;
        executedPassIndices.reserve(diagnostics.passes.size());
        for (const PassDiagnostic& pass : diagnostics.passes)
        {
            if (pass.executedLastRun && pass.executionSerial != RVX_INVALID_INDEX)
            {
                executedPassIndices.push_back(pass.index);
            }
        }
        std::sort(
            executedPassIndices.begin(),
            executedPassIndices.end(),
            [&](uint32 lhs, uint32 rhs)
            {
                return diagnostics.passes[lhs].executionSerial < diagnostics.passes[rhs].executionSerial;
            });

        for (uint32 passIndex : executedPassIndices)
        {
            const PassDiagnostic& pass = diagnostics.passes[passIndex];
            if (diagnostics.queueBatches.empty() ||
                diagnostics.queueBatches.back().queue != pass.executionQueue ||
                diagnostics.queueBatches.back().lastExecutionSerial + 1u != pass.executionSerial)
            {
                QueueBatchDiagnostic batch;
                batch.batchIndex = static_cast<uint32>(diagnostics.queueBatches.size());
                batch.queue = pass.executionQueue;
                batch.firstExecutionSerial = pass.executionSerial;
                batch.lastExecutionSerial = pass.executionSerial;
                diagnostics.queueBatches.push_back(std::move(batch));
            }

            QueueBatchDiagnostic& batch = diagnostics.queueBatches.back();
            batch.lastExecutionSerial = pass.executionSerial;
            batch.cpuDurationNanoseconds += pass.cpuDurationNanoseconds;
            batch.passIndices.push_back(pass.index);
        }

        diagnostics.actualQueueBatchCount = static_cast<uint32>(diagnostics.queueBatches.size());
        if (!diagnostics.queueBatches.empty())
        {
            for (size_t batchIndex = 1; batchIndex < diagnostics.queueBatches.size(); ++batchIndex)
            {
                if (diagnostics.queueBatches[batchIndex - 1].queue != diagnostics.queueBatches[batchIndex].queue)
                {
                    diagnostics.actualQueueSwitchCount++;
                }
            }
        }

        diagnostics.queueSyncs.reserve(m_impl->lastQueueSyncs.size());
        for (uint32 syncIndex = 0; syncIndex < m_impl->lastQueueSyncs.size(); ++syncIndex)
        {
            const RenderGraphImpl::QueueSyncPoint& syncPoint = m_impl->lastQueueSyncs[syncIndex];
            QueueSyncDiagnostic syncDiagnostic;
            syncDiagnostic.syncIndex = syncIndex;
            syncDiagnostic.sourceQueue = syncPoint.sourceQueue;
            syncDiagnostic.targetQueue = syncPoint.targetQueue;
            syncDiagnostic.reason = syncPoint.reason;
            syncDiagnostic.fenceValue = syncPoint.fenceValue;
            syncDiagnostic.sourcePassIndex = syncPoint.sourcePassIndex;
            syncDiagnostic.targetPassIndex = syncPoint.targetPassIndex;
            diagnostics.queueSyncs.push_back(syncDiagnostic);
        }
        diagnostics.actualQueueSyncCount = static_cast<uint32>(diagnostics.queueSyncs.size());
        for (const QueueSyncDiagnostic& sync : diagnostics.queueSyncs)
        {
            if (sync.reason == DiagnosticSyncReason::CrossQueueDependency)
            {
                diagnostics.actualCrossQueueSyncCount++;
            }
        }
        for (QueueSyncDiagnostic& actualSync : diagnostics.queueSyncs)
        {
            if (actualSync.reason == DiagnosticSyncReason::FinalQueueJoin)
            {
                diagnostics.actualConservativeFinalJoinCount++;
            }

            auto plannedIt = std::find_if(
                diagnostics.plannedQueueSyncs.begin(),
                diagnostics.plannedQueueSyncs.end(),
                [&](const PlannedQueueSyncDiagnostic& plannedSync)
                {
                    if (plannedSync.sourceQueue != actualSync.sourceQueue ||
                        plannedSync.targetQueue != actualSync.targetQueue ||
                        plannedSync.reason != actualSync.reason ||
                        plannedSync.targetPassIndex != actualSync.targetPassIndex ||
                        plannedSync.sourceBatchIndex >= diagnostics.plannedQueueBatches.size())
                    {
                        return false;
                    }

                    const PlannedQueueBatchDiagnostic& sourceBatch =
                        diagnostics.plannedQueueBatches[plannedSync.sourceBatchIndex];
                    return std::find(
                               sourceBatch.passIndices.begin(),
                               sourceBatch.passIndices.end(),
                               actualSync.sourcePassIndex) != sourceBatch.passIndices.end();
                });

            if (plannedIt != diagnostics.plannedQueueSyncs.end())
            {
                actualSync.coversPlannedSync = true;
                actualSync.plannedSyncIndex = plannedIt->syncIndex;
                plannedIt->coveredByActualSync = true;
                plannedIt->actualSyncIndex = actualSync.syncIndex;
                diagnostics.actualMatchedPlannedSyncCount++;
            }
            else
            {
                diagnostics.actualUnplannedQueueSyncCount++;
            }
        }
        for (const PlannedQueueSyncDiagnostic& plannedSync : diagnostics.plannedQueueSyncs)
        {
            if (plannedSync.coveredByActualSync)
            {
                diagnostics.plannedQueueSyncCoveredCount++;
            }
            else
            {
                diagnostics.plannedQueueSyncUncoveredCount++;
            }
        }

        diagnostics.resources.reserve(m_impl->textures.size() + m_impl->buffers.size());
        for (uint32 textureIndex = 0; textureIndex < m_impl->textures.size(); ++textureIndex)
        {
            const TextureResource& texture = m_impl->textures[textureIndex];
            ResourceDiagnostic resource;
            resource.type = DiagnosticResourceType::Texture;
            resource.index = textureIndex;
            resource.name = texture.desc.debugName ? texture.desc.debugName : ("Texture" + std::to_string(textureIndex));
            resource.imported = texture.imported;
            resource.pooled = texture.pooled;
            resource.used = texture.lifetime.isUsed;
            resource.firstUsePass = texture.lifetime.isUsed ? texture.lifetime.firstUsePass : RVX_INVALID_INDEX;
            resource.lastUsePass = texture.lifetime.isUsed ? texture.lifetime.lastUsePass : RVX_INVALID_INDEX;
            resource.estimatedMemoryBytes = texture.lifetime.memorySize != 0
                                                ? texture.lifetime.memorySize
                                                : EstimateTextureMemorySize(texture.desc);
            resource.aliased = texture.alias.isAliased;
            resource.aliasHeapIndex = texture.alias.heapIndex;
            resource.aliasHeapOffset = texture.alias.heapOffset;
            resource.initialState = texture.initialState;
            resource.currentState = texture.currentState;
            resource.initialAccess = texture.initialAccessSnapshot.uniformAccess;
            resource.currentAccess = texture.currentAccessSnapshot.uniformAccess;
            resource.hasExportState = texture.exportState.has_value();
            resource.exportState = texture.exportState.value_or(RHIResourceState::Undefined);
            resource.hasExportAccess = texture.exportAccess.has_value();
            resource.exportAccess = texture.exportAccess.value_or(RHIAccessSnapshot{});
            resource.width = texture.desc.width;
            resource.height = texture.desc.height;
            resource.depth = texture.desc.depth;
            resource.mipLevels = texture.desc.mipLevels;
            resource.arraySize = texture.desc.arraySize;
            resource.format = texture.desc.format;

            if (resource.imported)
            {
                diagnostics.estimatedImportedMemoryBytes += resource.estimatedMemoryBytes;
            }
            else
            {
                diagnostics.estimatedTransientMemoryBytes += resource.estimatedMemoryBytes;
                if (resource.used)
                {
                    diagnostics.estimatedUsedTransientMemoryBytes += resource.estimatedMemoryBytes;
                }
            }

            diagnostics.resources.push_back(std::move(resource));
        }

        for (uint32 bufferIndex = 0; bufferIndex < m_impl->buffers.size(); ++bufferIndex)
        {
            const BufferResource& buffer = m_impl->buffers[bufferIndex];
            ResourceDiagnostic resource;
            resource.type = DiagnosticResourceType::Buffer;
            resource.index = bufferIndex;
            resource.name = buffer.desc.debugName ? buffer.desc.debugName : ("Buffer" + std::to_string(bufferIndex));
            resource.imported = buffer.imported;
            resource.pooled = buffer.pooled;
            resource.used = buffer.lifetime.isUsed;
            resource.firstUsePass = buffer.lifetime.isUsed ? buffer.lifetime.firstUsePass : RVX_INVALID_INDEX;
            resource.lastUsePass = buffer.lifetime.isUsed ? buffer.lifetime.lastUsePass : RVX_INVALID_INDEX;
            resource.estimatedMemoryBytes = buffer.lifetime.memorySize != 0
                                                ? buffer.lifetime.memorySize
                                                : EstimateBufferMemorySize(buffer.desc);
            resource.aliased = buffer.alias.isAliased;
            resource.aliasHeapIndex = buffer.alias.heapIndex;
            resource.aliasHeapOffset = buffer.alias.heapOffset;
            resource.initialState = buffer.initialState;
            resource.currentState = buffer.currentState;
            resource.initialAccess = buffer.initialAccessSnapshot.uniformAccess;
            resource.currentAccess = buffer.currentAccessSnapshot.uniformAccess;
            resource.hasExportState = buffer.exportState.has_value();
            resource.exportState = buffer.exportState.value_or(RHIResourceState::Undefined);
            resource.hasExportAccess = buffer.exportAccess.has_value();
            resource.exportAccess = buffer.exportAccess.value_or(RHIAccessSnapshot{});
            resource.bufferSize = buffer.desc.size;
            resource.stride = buffer.desc.stride;

            if (resource.imported)
            {
                diagnostics.estimatedImportedMemoryBytes += resource.estimatedMemoryBytes;
            }
            else
            {
                diagnostics.estimatedTransientMemoryBytes += resource.estimatedMemoryBytes;
                if (resource.used)
                {
                    diagnostics.estimatedUsedTransientMemoryBytes += resource.estimatedMemoryBytes;
                }
            }

            diagnostics.resources.push_back(std::move(resource));
        }

        return diagnostics;
    }

    std::string RenderGraph::ExportDiagnosticsText() const
    {
        Diagnostics diagnostics = GetDiagnostics();
        std::ostringstream ss;

        ss << "RenderGraph Diagnostics\n";
        ss << "Schema: " << diagnostics.schemaVersion << " (" << diagnostics.schemaId << ")\n";
        ss << "Passes: " << diagnostics.passes.size()
           << " total, " << diagnostics.compileStats.culledPasses << " culled\n";
        ss << "Resources: " << diagnostics.resources.size()
           << " total, " << diagnostics.compileStats.totalTransientTextures << " transient textures, "
           << diagnostics.compileStats.totalTransientBuffers << " transient buffers\n";
        ss << "Barriers: " << diagnostics.compileStats.barrierCount
           << " total (" << diagnostics.compileStats.textureBarrierCount << " texture, "
           << diagnostics.compileStats.bufferBarrierCount << " buffer)\n";
        ss << "Scoped access: mismatches="
           << diagnostics.compileStats.accessSnapshotMismatchCount
           << ", compatibility projections="
           << diagnostics.compileStats.compatibilityStateProjectionCount << "\n";
        ss << "Last execution: passes=" << diagnostics.compileStats.lastExecutedPassCount
           << ", cpuNs=" << diagnostics.compileStats.lastExecutionCpuDurationNanoseconds << "\n";
        ss << "Estimated transient memory: " << diagnostics.estimatedTransientMemoryBytes << " bytes\n";
        ss << "Estimated used transient memory: " << diagnostics.estimatedUsedTransientMemoryBytes << " bytes\n";
        ss << "Estimated imported memory: " << diagnostics.estimatedImportedMemoryBytes << " bytes\n";
        ss << "Aliasing: " << (diagnostics.compileStats.memoryAliasingEnabled ? "enabled" : "disabled")
           << ", memory without aliasing " << diagnostics.compileStats.memoryWithoutAliasing
           << " bytes, memory with aliasing " << diagnostics.compileStats.memoryWithAliasing << " bytes\n";
        ss << "Async compute: supported=" << (diagnostics.compileStats.asyncComputeSupported ? "true" : "false")
           << ", fallback=" << (diagnostics.compileStats.asyncFallbackUsed ? "true" : "false")
           << ", reason=" << ToDiagnosticString(diagnostics.compileStats.asyncFallbackReason)
           << ", eligible=" << diagnostics.compileStats.asyncComputeEligiblePasses
           << ", scheduled=" << diagnostics.compileStats.asyncComputeScheduledPasses
           << ", graphicsScheduled=" << diagnostics.compileStats.asyncGraphicsScheduledPasses
           << ", fenceSignals=" << diagnostics.compileStats.asyncFenceSignalCount
           << ", fenceWaits=" << diagnostics.compileStats.asyncFenceWaitCount
           << ", crossQueueDeps=" << diagnostics.compileStats.asyncCrossQueueDependencyCount
           << ", finalJoins=" << diagnostics.compileStats.asyncFinalQueueJoinCount << "\n";
        ss << "Schedule efficiency: plannedBatches=" << diagnostics.plannedQueueBatchCount
           << ", plannedLevels=" << diagnostics.plannedDependencyLevelCount
           << ", plannedComputeBatches=" << diagnostics.plannedComputeBatchCount
           << ", plannedOverlapLevels=" << diagnostics.plannedAsyncOverlapCandidateLevelCount
           << ", plannedSyncs=" << diagnostics.plannedQueueSyncCount
           << ", plannedCrossQueueSyncs=" << diagnostics.plannedCrossQueueSyncCount
           << ", plannedCoveredSyncs=" << diagnostics.plannedQueueSyncCoveredCount
           << ", plannedUncoveredSyncs=" << diagnostics.plannedQueueSyncUncoveredCount
           << ", actualBatches=" << diagnostics.actualQueueBatchCount
           << ", actualSwitches=" << diagnostics.actualQueueSwitchCount
           << ", actualSyncs=" << diagnostics.actualQueueSyncCount
           << ", actualCrossQueueSyncs=" << diagnostics.actualCrossQueueSyncCount
           << ", actualMatchedPlannedSyncs=" << diagnostics.actualMatchedPlannedSyncCount
           << ", actualUnplannedSyncs=" << diagnostics.actualUnplannedQueueSyncCount
           << ", actualConservativeFinalJoins=" << diagnostics.actualConservativeFinalJoinCount << "\n";

        ss << "\nExecution Order:\n";
        for (uint32 passIndex : diagnostics.executionOrder)
        {
            ss << "  " << passIndex << "\n";
        }

        ss << "\nPlanned Queue Batches:\n";
        for (const PlannedQueueBatchDiagnostic& batch : diagnostics.plannedQueueBatches)
        {
            ss << "  [" << batch.batchIndex << "] level=" << batch.dependencyLevel
               << " queue=" << ToDiagnosticString(batch.queue)
               << " passes=" << FormatPassIndexList(batch.passIndices)
               << " prerequisites=" << FormatPassIndexList(batch.prerequisiteSyncIndices) << "\n";
        }

        ss << "\nPlanned Queue Syncs:\n";
        for (const PlannedQueueSyncDiagnostic& sync : diagnostics.plannedQueueSyncs)
        {
            ss << "  [" << sync.syncIndex << "] batch" << sync.sourceBatchIndex
               << " " << ToDiagnosticString(sync.sourceQueue)
               << " -> batch" << sync.targetBatchIndex
               << " " << ToDiagnosticString(sync.targetQueue)
               << " reason=" << ToDiagnosticString(sync.reason)
               << " sourcePass=";
            if (sync.sourcePassIndex == RVX_INVALID_INDEX)
            {
                ss << "invalid";
            }
            else
            {
                ss << sync.sourcePassIndex;
            }
            ss << " targetPass=";
            if (sync.targetPassIndex == RVX_INVALID_INDEX)
            {
                ss << "invalid";
            }
            else
            {
                ss << sync.targetPassIndex;
            }
            ss << " covered=" << (sync.coveredByActualSync ? "true" : "false")
               << " actualSync=";
            if (sync.actualSyncIndex == RVX_INVALID_INDEX)
            {
                ss << "invalid";
            }
            else
            {
                ss << sync.actualSyncIndex;
            }
            ss << "\n";
        }

        ss << "\nQueue Batches:\n";
        for (const QueueBatchDiagnostic& batch : diagnostics.queueBatches)
        {
            ss << "  [" << batch.batchIndex << "] queue=" << ToDiagnosticString(batch.queue)
               << " serial=[" << batch.firstExecutionSerial << "," << batch.lastExecutionSerial << "]"
               << " passes=" << FormatPassIndexList(batch.passIndices)
               << " cpuNs=" << batch.cpuDurationNanoseconds << "\n";
        }

        ss << "\nQueue Syncs:\n";
        for (const QueueSyncDiagnostic& sync : diagnostics.queueSyncs)
        {
            ss << "  [" << sync.syncIndex << "] "
               << ToDiagnosticString(sync.sourceQueue) << " -> "
               << ToDiagnosticString(sync.targetQueue)
               << " reason=" << ToDiagnosticString(sync.reason)
               << " fence=" << sync.fenceValue
               << " sourcePass=";
            if (sync.sourcePassIndex == RVX_INVALID_INDEX)
            {
                ss << "invalid";
            }
            else
            {
                ss << sync.sourcePassIndex;
            }
            ss << " targetPass=";
            if (sync.targetPassIndex == RVX_INVALID_INDEX)
            {
                ss << "invalid";
            }
            else
            {
                ss << sync.targetPassIndex;
            }
            ss << " coversPlanned=" << (sync.coversPlannedSync ? "true" : "false")
               << " plannedSync=";
            if (sync.plannedSyncIndex == RVX_INVALID_INDEX)
            {
                ss << "invalid";
            }
            else
            {
                ss << sync.plannedSyncIndex;
            }
            ss << "\n";
        }

        ss << "\nPasses:\n";
        for (const PassDiagnostic& pass : diagnostics.passes)
        {
            ss << "  [" << pass.index << "] " << pass.name
               << " type=" << ToDiagnosticString(pass.type)
               << " culled=" << (pass.culled ? "true" : "false")
               << " executed=" << (pass.executedLastRun ? "true" : "false")
               << " queue=" << ToDiagnosticString(pass.executionQueue)
               << " serial=";
            if (pass.executionSerial == RVX_INVALID_INDEX)
            {
                ss << "invalid";
            }
            else
            {
                ss << pass.executionSerial;
            }
            ss << " cpuNs=" << pass.cpuDurationNanoseconds
               << " dependencies=" << FormatPassIndexList(pass.dependencies)
               << " dependents=" << FormatPassIndexList(pass.dependents)
               << " usages=" << pass.usages.size()
               << " barriers=" << (pass.textureBarrierCount + pass.bufferBarrierCount)
               << "\n";

            for (const ResourceUsageDiagnostic& usage : pass.usages)
            {
                ss << "    " << ToDiagnosticString(usage.access)
                   << " " << ToDiagnosticString(usage.type)
                   << "[" << usage.resourceIndex << "]"
                   << " state=" << static_cast<uint32>(usage.desiredState)
                   << "\n";
            }
        }

        ss << "\nResources:\n";
        for (const ResourceDiagnostic& resource : diagnostics.resources)
        {
            ss << "  " << ToDiagnosticString(resource.type)
               << "[" << resource.index << "] " << resource.name
               << " imported=" << (resource.imported ? "true" : "false")
               << " used=" << (resource.used ? "true" : "false")
               << " memory=" << resource.estimatedMemoryBytes;
            if (resource.used)
            {
                ss << " lifetime=[" << resource.firstUsePass << "," << resource.lastUsePass << "]";
            }
            if (resource.aliased)
            {
                ss << " aliasHeap=" << resource.aliasHeapIndex
                   << " offset=" << resource.aliasHeapOffset;
            }
            ss << "\n";
        }

        return ss.str();
    }

    std::string RenderGraph::ExportDiagnosticsJson() const
    {
        const Diagnostics diagnostics = GetDiagnostics();
        const CompileStats& stats = diagnostics.compileStats;
        std::ostringstream ss;

        ss << "{\n";
        ss << "  \"schemaVersion\": " << diagnostics.schemaVersion << ",\n";
        ss << "  \"schemaId\": " << JsonString(diagnostics.schemaId) << ",\n";
        ss << "  \"id\": \"renderGraphDiagnosticsJson\",\n";
        ss << "  \"kind\": \"RenderGraphDiagnosticsJson\",\n";
        ss << "  \"contentType\": \"application/json\",\n";
        ss << "  \"contentHash\": \"\",\n";
        ss << "  \"relativePath\": \"\",\n";
        ss << "  \"compileStats\": {\n";
        ss << "    \"compileValid\": " << JsonBool(stats.compileValid) << ",\n";
        ss << "    \"executionOrderFallbackUsed\": " << JsonBool(stats.executionOrderFallbackUsed) << ",\n";
        ss << "    \"totalPasses\": " << stats.totalPasses << ",\n";
        ss << "    \"culledPasses\": " << stats.culledPasses << ",\n";
        ss << "    \"validationWarningCount\": " << stats.validationWarningCount << ",\n";
        ss << "    \"validationErrorCount\": " << stats.validationErrorCount << ",\n";
        ss << "    \"barrierCount\": " << stats.barrierCount << ",\n";
        ss << "    \"textureBarrierCount\": " << stats.textureBarrierCount << ",\n";
        ss << "    \"bufferBarrierCount\": " << stats.bufferBarrierCount << ",\n";
        ss << "    \"accessSnapshotMismatchCount\": " << stats.accessSnapshotMismatchCount << ",\n";
        ss << "    \"compatibilityStateProjectionCount\": " << stats.compatibilityStateProjectionCount << ",\n";
        ss << "    \"asyncComputeSupported\": " << JsonBool(stats.asyncComputeSupported) << ",\n";
        ss << "    \"asyncFallbackUsed\": " << JsonBool(stats.asyncFallbackUsed) << ",\n";
        ss << "    \"asyncFallbackReason\": " << JsonString(ToDiagnosticString(stats.asyncFallbackReason)) << ",\n";
        ss << "    \"asyncComputeEligiblePasses\": " << stats.asyncComputeEligiblePasses << ",\n";
        ss << "    \"asyncComputeScheduledPasses\": " << stats.asyncComputeScheduledPasses << ",\n";
        ss << "    \"asyncGraphicsScheduledPasses\": " << stats.asyncGraphicsScheduledPasses << ",\n";
        ss << "    \"asyncFenceSignalCount\": " << stats.asyncFenceSignalCount << ",\n";
        ss << "    \"asyncFenceWaitCount\": " << stats.asyncFenceWaitCount << ",\n";
        ss << "    \"asyncCrossQueueDependencyCount\": " << stats.asyncCrossQueueDependencyCount << ",\n";
        ss << "    \"asyncFinalQueueJoinCount\": " << stats.asyncFinalQueueJoinCount << ",\n";
        ss << "    \"lastExecutedPassCount\": " << stats.lastExecutedPassCount << ",\n";
        ss << "    \"lastExecutionCpuDurationNanoseconds\": " << stats.lastExecutionCpuDurationNanoseconds << "\n";
        ss << "  },\n";

        ss << "  \"memory\": {\n";
        ss << "    \"memoryAliasingEnabled\": " << JsonBool(stats.memoryAliasingEnabled) << ",\n";
        ss << "    \"memoryAliasingUnsupportedRequested\": "
           << JsonBool(stats.memoryAliasingUnsupportedRequested) << ",\n";
        ss << "    \"explicitAliasingBarriersSupported\": "
           << JsonBool(stats.explicitAliasingBarriersSupported) << ",\n";
        ss << "    \"totalTransientTextures\": " << stats.totalTransientTextures << ",\n";
        ss << "    \"totalTransientBuffers\": " << stats.totalTransientBuffers << ",\n";
        ss << "    \"aliasedTextureCount\": " << stats.aliasedTextureCount << ",\n";
        ss << "    \"aliasedBufferCount\": " << stats.aliasedBufferCount << ",\n";
        ss << "    \"transientHeapCount\": " << stats.transientHeapCount << ",\n";
        ss << "    \"memoryWithoutAliasing\": " << stats.memoryWithoutAliasing << ",\n";
        ss << "    \"memoryWithAliasing\": " << stats.memoryWithAliasing << ",\n";
        ss << "    \"estimatedTransientMemoryBytes\": " << diagnostics.estimatedTransientMemoryBytes << ",\n";
        ss << "    \"estimatedUsedTransientMemoryBytes\": " << diagnostics.estimatedUsedTransientMemoryBytes << ",\n";
        ss << "    \"estimatedImportedMemoryBytes\": " << diagnostics.estimatedImportedMemoryBytes << "\n";
        ss << "  },\n";

        ss << "  \"schedule\": {\n";
        ss << "    \"plannedQueueBatchCount\": " << diagnostics.plannedQueueBatchCount << ",\n";
        ss << "    \"plannedDependencyLevelCount\": " << diagnostics.plannedDependencyLevelCount << ",\n";
        ss << "    \"plannedAsyncOverlapCandidateLevelCount\": "
           << diagnostics.plannedAsyncOverlapCandidateLevelCount << ",\n";
        ss << "    \"plannedComputeBatchCount\": " << diagnostics.plannedComputeBatchCount << ",\n";
        ss << "    \"plannedQueueSyncCount\": " << diagnostics.plannedQueueSyncCount << ",\n";
        ss << "    \"plannedCrossQueueSyncCount\": " << diagnostics.plannedCrossQueueSyncCount << ",\n";
        ss << "    \"plannedQueueSyncCoveredCount\": " << diagnostics.plannedQueueSyncCoveredCount << ",\n";
        ss << "    \"plannedQueueSyncUncoveredCount\": " << diagnostics.plannedQueueSyncUncoveredCount << ",\n";
        ss << "    \"actualQueueBatchCount\": " << diagnostics.actualQueueBatchCount << ",\n";
        ss << "    \"actualQueueSwitchCount\": " << diagnostics.actualQueueSwitchCount << ",\n";
        ss << "    \"actualQueueSyncCount\": " << diagnostics.actualQueueSyncCount << ",\n";
        ss << "    \"actualCrossQueueSyncCount\": " << diagnostics.actualCrossQueueSyncCount << ",\n";
        ss << "    \"actualMatchedPlannedSyncCount\": "
           << diagnostics.actualMatchedPlannedSyncCount << ",\n";
        ss << "    \"actualUnplannedQueueSyncCount\": "
           << diagnostics.actualUnplannedQueueSyncCount << ",\n";
        ss << "    \"actualConservativeFinalJoinCount\": "
           << diagnostics.actualConservativeFinalJoinCount << "\n";
        ss << "  },\n";

        ss << "  \"executionOrder\": ";
        WriteIndexArray(ss, diagnostics.executionOrder);
        ss << ",\n";

        ss << "  \"passes\": [\n";
        for (size_t i = 0; i < diagnostics.passes.size(); ++i)
        {
            const PassDiagnostic& pass = diagnostics.passes[i];
            ss << "    {\n";
            ss << "      \"index\": " << pass.index << ",\n";
            ss << "      \"name\": " << JsonString(pass.name) << ",\n";
            ss << "      \"type\": " << JsonString(ToDiagnosticString(pass.type)) << ",\n";
            ss << "      \"culled\": " << JsonBool(pass.culled) << ",\n";
            ss << "      \"executedLastRun\": " << JsonBool(pass.executedLastRun) << ",\n";
            ss << "      \"executionQueue\": " << JsonString(ToDiagnosticString(pass.executionQueue)) << ",\n";
            ss << "      \"executionSerial\": ";
            WriteOptionalIndex(ss, pass.executionSerial);
            ss << ",\n";
            ss << "      \"cpuDurationNanoseconds\": " << pass.cpuDurationNanoseconds << ",\n";
            ss << "      \"textureBarrierCount\": " << pass.textureBarrierCount << ",\n";
            ss << "      \"bufferBarrierCount\": " << pass.bufferBarrierCount << ",\n";
            ss << "      \"aliasingBarrierCount\": " << pass.aliasingBarrierCount << ",\n";
            ss << "      \"dependencies\": ";
            WriteIndexArray(ss, pass.dependencies);
            ss << ",\n";
            ss << "      \"dependents\": ";
            WriteIndexArray(ss, pass.dependents);
            ss << ",\n";
            ss << "      \"usages\": [\n";
            for (size_t usageIndex = 0; usageIndex < pass.usages.size(); ++usageIndex)
            {
                const ResourceUsageDiagnostic& usage = pass.usages[usageIndex];
                ss << "        {\n";
                ss << "          \"type\": " << JsonString(ToDiagnosticString(usage.type)) << ",\n";
                ss << "          \"access\": " << JsonString(ToDiagnosticString(usage.access)) << ",\n";
                ss << "          \"resourceIndex\": ";
                WriteOptionalIndex(ss, usage.resourceIndex);
                ss << ",\n";
                ss << "          \"desiredState\": " << static_cast<uint32>(usage.desiredState) << ",\n";
                ss << "          \"desiredAccess\": "
                   << JsonString(DescribeRHIAccessSnapshot(usage.desiredAccess)) << ",\n";
                ss << "          \"shaderStages\": " << static_cast<uint32>(usage.stages) << ",\n";
                ss << "          \"hasSubresourceRange\": " << JsonBool(usage.hasSubresourceRange) << ",\n";
                ss << "          \"hasRange\": " << JsonBool(usage.hasRange) << ",\n";
                ss << "          \"offset\": " << usage.offset << ",\n";
                ss << "          \"size\": " << usage.size << "\n";
                ss << "        }" << (usageIndex + 1 < pass.usages.size() ? "," : "") << "\n";
            }
            ss << "      ]\n";
            ss << "    }" << (i + 1 < diagnostics.passes.size() ? "," : "") << "\n";
        }
        ss << "  ],\n";

        ss << "  \"resources\": [\n";
        for (size_t i = 0; i < diagnostics.resources.size(); ++i)
        {
            const ResourceDiagnostic& resource = diagnostics.resources[i];
            ss << "    {\n";
            ss << "      \"type\": " << JsonString(ToDiagnosticString(resource.type)) << ",\n";
            ss << "      \"index\": " << resource.index << ",\n";
            ss << "      \"name\": " << JsonString(resource.name) << ",\n";
            ss << "      \"imported\": " << JsonBool(resource.imported) << ",\n";
            ss << "      \"pooled\": " << JsonBool(resource.pooled) << ",\n";
            ss << "      \"used\": " << JsonBool(resource.used) << ",\n";
            ss << "      \"firstUsePass\": ";
            WriteOptionalIndex(ss, resource.firstUsePass);
            ss << ",\n";
            ss << "      \"lastUsePass\": ";
            WriteOptionalIndex(ss, resource.lastUsePass);
            ss << ",\n";
            ss << "      \"estimatedMemoryBytes\": " << resource.estimatedMemoryBytes << ",\n";
            ss << "      \"aliased\": " << JsonBool(resource.aliased) << ",\n";
            ss << "      \"aliasHeapIndex\": ";
            WriteOptionalIndex(ss, resource.aliasHeapIndex);
            ss << ",\n";
            ss << "      \"aliasHeapOffset\": " << resource.aliasHeapOffset << ",\n";
            ss << "      \"initialState\": " << static_cast<uint32>(resource.initialState) << ",\n";
            ss << "      \"currentState\": " << static_cast<uint32>(resource.currentState) << ",\n";
            ss << "      \"initialAccess\": " << JsonString(DescribeRHIAccessSnapshot(resource.initialAccess)) << ",\n";
            ss << "      \"currentAccess\": " << JsonString(DescribeRHIAccessSnapshot(resource.currentAccess)) << ",\n";
            ss << "      \"hasExportState\": " << JsonBool(resource.hasExportState) << ",\n";
            ss << "      \"exportState\": " << static_cast<uint32>(resource.exportState) << ",\n";
            ss << "      \"hasExportAccess\": " << JsonBool(resource.hasExportAccess) << ",\n";
            ss << "      \"exportAccess\": " << JsonString(DescribeRHIAccessSnapshot(resource.exportAccess)) << ",\n";
            ss << "      \"width\": " << resource.width << ",\n";
            ss << "      \"height\": " << resource.height << ",\n";
            ss << "      \"depth\": " << resource.depth << ",\n";
            ss << "      \"mipLevels\": " << resource.mipLevels << ",\n";
            ss << "      \"arraySize\": " << resource.arraySize << ",\n";
            ss << "      \"format\": " << static_cast<uint32>(resource.format) << ",\n";
            ss << "      \"bufferSize\": " << resource.bufferSize << ",\n";
            ss << "      \"stride\": " << resource.stride << "\n";
            ss << "    }" << (i + 1 < diagnostics.resources.size() ? "," : "") << "\n";
        }
        ss << "  ],\n";

        ss << "  \"plannedQueueBatches\": [\n";
        for (size_t i = 0; i < diagnostics.plannedQueueBatches.size(); ++i)
        {
            const PlannedQueueBatchDiagnostic& batch = diagnostics.plannedQueueBatches[i];
            ss << "    {\n";
            ss << "      \"batchIndex\": " << batch.batchIndex << ",\n";
            ss << "      \"dependencyLevel\": " << batch.dependencyLevel << ",\n";
            ss << "      \"queue\": " << JsonString(ToDiagnosticString(batch.queue)) << ",\n";
            ss << "      \"passIndices\": ";
            WriteIndexArray(ss, batch.passIndices);
            ss << ",\n";
            ss << "      \"prerequisiteSyncIndices\": ";
            WriteIndexArray(ss, batch.prerequisiteSyncIndices);
            ss << "\n";
            ss << "    }" << (i + 1 < diagnostics.plannedQueueBatches.size() ? "," : "") << "\n";
        }
        ss << "  ],\n";

        ss << "  \"plannedQueueSyncs\": [\n";
        for (size_t i = 0; i < diagnostics.plannedQueueSyncs.size(); ++i)
        {
            const PlannedQueueSyncDiagnostic& sync = diagnostics.plannedQueueSyncs[i];
            ss << "    {\n";
            ss << "      \"syncIndex\": " << sync.syncIndex << ",\n";
            ss << "      \"sourceBatchIndex\": ";
            WriteOptionalIndex(ss, sync.sourceBatchIndex);
            ss << ",\n";
            ss << "      \"targetBatchIndex\": ";
            WriteOptionalIndex(ss, sync.targetBatchIndex);
            ss << ",\n";
            ss << "      \"sourceQueue\": " << JsonString(ToDiagnosticString(sync.sourceQueue)) << ",\n";
            ss << "      \"targetQueue\": " << JsonString(ToDiagnosticString(sync.targetQueue)) << ",\n";
            ss << "      \"reason\": " << JsonString(ToDiagnosticString(sync.reason)) << ",\n";
            ss << "      \"sourcePassIndex\": ";
            WriteOptionalIndex(ss, sync.sourcePassIndex);
            ss << ",\n";
            ss << "      \"targetPassIndex\": ";
            WriteOptionalIndex(ss, sync.targetPassIndex);
            ss << ",\n";
            ss << "      \"coveredByActualSync\": " << JsonBool(sync.coveredByActualSync) << ",\n";
            ss << "      \"actualSyncIndex\": ";
            WriteOptionalIndex(ss, sync.actualSyncIndex);
            ss << "\n";
            ss << "    }" << (i + 1 < diagnostics.plannedQueueSyncs.size() ? "," : "") << "\n";
        }
        ss << "  ],\n";

        ss << "  \"queueBatches\": [\n";
        for (size_t i = 0; i < diagnostics.queueBatches.size(); ++i)
        {
            const QueueBatchDiagnostic& batch = diagnostics.queueBatches[i];
            ss << "    {\n";
            ss << "      \"batchIndex\": " << batch.batchIndex << ",\n";
            ss << "      \"queue\": " << JsonString(ToDiagnosticString(batch.queue)) << ",\n";
            ss << "      \"firstExecutionSerial\": ";
            WriteOptionalIndex(ss, batch.firstExecutionSerial);
            ss << ",\n";
            ss << "      \"lastExecutionSerial\": ";
            WriteOptionalIndex(ss, batch.lastExecutionSerial);
            ss << ",\n";
            ss << "      \"cpuDurationNanoseconds\": " << batch.cpuDurationNanoseconds << ",\n";
            ss << "      \"passIndices\": ";
            WriteIndexArray(ss, batch.passIndices);
            ss << "\n";
            ss << "    }" << (i + 1 < diagnostics.queueBatches.size() ? "," : "") << "\n";
        }
        ss << "  ],\n";

        ss << "  \"queueSyncs\": [\n";
        for (size_t i = 0; i < diagnostics.queueSyncs.size(); ++i)
        {
            const QueueSyncDiagnostic& sync = diagnostics.queueSyncs[i];
            ss << "    {\n";
            ss << "      \"syncIndex\": " << sync.syncIndex << ",\n";
            ss << "      \"sourceQueue\": " << JsonString(ToDiagnosticString(sync.sourceQueue)) << ",\n";
            ss << "      \"targetQueue\": " << JsonString(ToDiagnosticString(sync.targetQueue)) << ",\n";
            ss << "      \"reason\": " << JsonString(ToDiagnosticString(sync.reason)) << ",\n";
            ss << "      \"fenceValue\": " << sync.fenceValue << ",\n";
            ss << "      \"sourcePassIndex\": ";
            WriteOptionalIndex(ss, sync.sourcePassIndex);
            ss << ",\n";
            ss << "      \"targetPassIndex\": ";
            WriteOptionalIndex(ss, sync.targetPassIndex);
            ss << ",\n";
            ss << "      \"coversPlannedSync\": " << JsonBool(sync.coversPlannedSync) << ",\n";
            ss << "      \"plannedSyncIndex\": ";
            WriteOptionalIndex(ss, sync.plannedSyncIndex);
            ss << "\n";
            ss << "    }" << (i + 1 < diagnostics.queueSyncs.size() ? "," : "") << "\n";
        }
        ss << "  ]\n";
        ss << "}\n";

        return ss.str();
    }

    bool RenderGraph::SaveDiagnosticsJson(const char* filename) const
    {
        if (!filename || filename[0] == '\0')
        {
            return false;
        }

        std::ofstream file(filename, std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }

        file << ExportDiagnosticsJson();
        return file.good();
    }

    void RenderGraph::SetMemoryAliasingEnabled(bool enabled)
    {
        m_impl->memoryAliasingRequested = enabled;
        m_impl->enableMemoryAliasing =
            enabled && m_impl->device &&
            m_impl->device->GetCapabilities().supportsExplicitAliasingBarriers;
        if (enabled && m_impl->device && !m_impl->enableMemoryAliasing)
        {
            RVX_CORE_WARN("RenderGraph memory aliasing requested, but explicit aliasing barriers are unsupported; keeping aliasing disabled");
        }
    }

    bool RenderGraph::IsMemoryAliasingEnabled() const
    {
        return m_impl->enableMemoryAliasing;
    }

    bool RenderGraph::RetainSubmissionResources(
        RenderSubmissionResourceBatch& batch) const
    {
        for (const TextureResource& texture : m_impl->textures)
        {
            if (!texture.imported && texture.texture &&
                !batch.Retain(Ref<RefCounted>(texture.texture),
                              EstimateTextureMemorySize(texture.desc)))
            {
                return false;
            }
        }
        for (const BufferResource& buffer : m_impl->buffers)
        {
            if (!buffer.imported && buffer.buffer &&
                !batch.Retain(Ref<RefCounted>(buffer.buffer),
                              buffer.desc.size))
            {
                return false;
            }
        }
        for (const TransientHeap& heap : m_impl->transientHeaps)
        {
            if (heap.heap &&
                !batch.Retain(Ref<RefCounted>(heap.heap), heap.size))
            {
                return false;
            }
        }
        return true;
    }

    void RenderGraph::Clear()
    {
        for (auto& texture : m_impl->textures)
        {
            if (!texture.imported && texture.pooled && texture.pooledRaw)
            {
                if (m_impl->transientResourcePool)
                {
                    m_impl->transientResourcePool->ReleaseTexture(
                        texture.pooledRaw,
                        GetRealizedAccess(RGTextureHandle{
                            static_cast<uint32>(&texture - m_impl->textures.data())}));
                }
                texture.pooledRaw = nullptr;
                texture.pooled = false;
            }
        }
        for (auto& buffer : m_impl->buffers)
        {
            if (!buffer.imported && buffer.pooled && buffer.pooledRaw)
            {
                if (m_impl->transientResourcePool)
                {
                    m_impl->transientResourcePool->ReleaseBuffer(
                        buffer.pooledRaw,
                        GetRealizedAccess(RGBufferHandle{
                            static_cast<uint32>(&buffer - m_impl->buffers.data())}));
                }
                buffer.pooledRaw = nullptr;
                buffer.pooled = false;
            }
        }

        m_impl->passes.clear();
        m_impl->textures.clear();
        m_impl->buffers.clear();
        m_impl->executionOrder.clear();
        m_impl->passDependencies.clear();
        m_impl->passDependents.clear();
        m_impl->lastQueueSyncs.clear();
        m_impl->compileDiagnostics.clear();
        m_impl->transientHeaps.clear();
        m_impl->stats = {};
        m_impl->totalMemoryWithoutAliasing = 0;
        m_impl->totalMemoryWithAliasing = 0;
        m_impl->aliasedTextureCount = 0;
        m_impl->aliasedBufferCount = 0;
        m_impl->compatibilityStateProjectionCount = 0;
        m_impl->executionRealized = false;
        m_impl->memoryAliasingRequested = false;
        m_impl->enableMemoryAliasing = false;
    }

    std::string RenderGraph::ExportGraphviz() const
    {
        std::ostringstream ss;
        ss << "digraph RenderGraph {\n";
        ss << "  rankdir=LR;\n";
        ss << "  node [fontname=\"Helvetica\", fontsize=10];\n";
        ss << "  edge [color=\"#666666\"];\n\n";
        
        // Subgraph for resources
        ss << "  subgraph cluster_resources {\n";
        ss << "    label=\"Resources\";\n";
        ss << "    style=dashed;\n";
        ss << "    color=\"#cccccc\";\n\n";
        
        // Texture nodes (ellipse)
        for (size_t i = 0; i < m_impl->textures.size(); ++i)
        {
            const auto& tex = m_impl->textures[i];
            std::string name = tex.desc.debugName ? tex.desc.debugName : ("Tex" + std::to_string(i));
            std::string fillColor = tex.imported ? "#b3d9ff" : (tex.alias.isAliased ? "#ffffb3" : "#b3ffb3");
            
            ss << "    tex" << i << " [shape=ellipse, style=filled, fillcolor=\"" << fillColor << "\", ";
            ss << "label=\"" << name << "\\n" << tex.desc.width << "x" << tex.desc.height;
            if (tex.alias.isAliased)
            {
                ss << "\\n(aliased H" << tex.alias.heapIndex << ")";
            }
            ss << "\"];\n";
        }
        
        // Buffer nodes (ellipse with different shape)
        for (size_t i = 0; i < m_impl->buffers.size(); ++i)
        {
            const auto& buf = m_impl->buffers[i];
            std::string name = buf.desc.debugName ? buf.desc.debugName : ("Buf" + std::to_string(i));
            std::string fillColor = buf.imported ? "#b3d9ff" : (buf.alias.isAliased ? "#ffffb3" : "#b3ffb3");
            
            ss << "    buf" << i << " [shape=box, style=\"filled,rounded\", fillcolor=\"" << fillColor << "\", ";
            ss << "label=\"" << name << "\\n" << (buf.desc.size / 1024) << " KB";
            if (buf.alias.isAliased)
            {
                ss << "\\n(aliased H" << buf.alias.heapIndex << ")";
            }
            ss << "\"];\n";
        }
        
        ss << "  }\n\n";
        
        // Pass nodes (boxes)
        ss << "  // Passes\n";
        for (size_t i = 0; i < m_impl->passes.size(); ++i)
        {
            const auto& pass = m_impl->passes[i];
            
            std::string color;
            if (pass.culled)
                color = "#e0e0e0";
            else if (pass.type == RenderGraphPassType::Compute)
                color = "#fff2cc";
            else if (pass.type == RenderGraphPassType::RayTracing)
                color = "#d9d2e9";
            else if (pass.type == RenderGraphPassType::Copy)
                color = "#d9ead3";
            else
                color = "#f4cccc";
            
            std::string style = pass.culled ? "dashed" : "filled";
            
            ss << "  pass" << i << " [shape=box, style=\"" << style << "\", fillcolor=\"" << color << "\", ";
            ss << "label=\"" << pass.name;
            if (pass.culled)
                ss << "\\n(CULLED)";
            ss << "\"];\n";
        }
        
        // Dependencies (resource -> pass for reads, pass -> resource for writes)
        ss << "\n  // Read edges (resource -> pass)\n";
        for (size_t i = 0; i < m_impl->passes.size(); ++i)
        {
            const auto& pass = m_impl->passes[i];
            for (uint32 texIdx : pass.readTextures)
            {
                ss << "  tex" << texIdx << " -> pass" << i << " [color=\"#3366cc\"];\n";
            }
            for (uint32 bufIdx : pass.readBuffers)
            {
                ss << "  buf" << bufIdx << " -> pass" << i << " [color=\"#3366cc\"];\n";
            }
        }
        
        ss << "\n  // Write edges (pass -> resource)\n";
        for (size_t i = 0; i < m_impl->passes.size(); ++i)
        {
            const auto& pass = m_impl->passes[i];
            for (uint32 texIdx : pass.writeTextures)
            {
                ss << "  pass" << i << " -> tex" << texIdx << " [color=\"#cc3333\", style=bold];\n";
            }
            for (uint32 bufIdx : pass.writeBuffers)
            {
                ss << "  pass" << i << " -> buf" << bufIdx << " [color=\"#cc3333\", style=bold];\n";
            }
        }
        
        // Execution order edges
        if (m_impl->executionOrder.size() > 1)
        {
            ss << "\n  // Execution order (invisible edges for layout)\n";
            ss << "  edge [style=invis];\n";
            for (size_t i = 1; i < m_impl->executionOrder.size(); ++i)
            {
                ss << "  pass" << m_impl->executionOrder[i - 1] << " -> pass" << m_impl->executionOrder[i] << ";\n";
            }
        }
        
        // Legend
        ss << "\n  // Legend\n";
        ss << "  subgraph cluster_legend {\n";
        ss << "    label=\"Legend\";\n";
        ss << "    style=solid;\n";
        ss << "    rank=sink;\n";
        ss << "    legend_imported [shape=ellipse, style=filled, fillcolor=\"#b3d9ff\", label=\"Imported\"];\n";
        ss << "    legend_transient [shape=ellipse, style=filled, fillcolor=\"#b3ffb3\", label=\"Transient\"];\n";
        ss << "    legend_aliased [shape=ellipse, style=filled, fillcolor=\"#ffffb3\", label=\"Aliased\"];\n";
        ss << "    legend_graphics [shape=box, style=filled, fillcolor=\"#f4cccc\", label=\"Graphics\"];\n";
        ss << "    legend_compute [shape=box, style=filled, fillcolor=\"#fff2cc\", label=\"Compute\"];\n";
        ss << "    legend_raytracing [shape=box, style=filled, fillcolor=\"#d9d2e9\", label=\"RayTracing\"];\n";
        ss << "    legend_copy [shape=box, style=filled, fillcolor=\"#d9ead3\", label=\"Copy\"];\n";
        ss << "    legend_imported -> legend_transient -> legend_aliased -> legend_graphics -> legend_compute -> legend_raytracing -> legend_copy [style=invis];\n";
        ss << "  }\n";
        
        ss << "}\n";
        
        return ss.str();
    }

    bool RenderGraph::SaveGraphviz(const char* filename) const
    {
        std::string dot = ExportGraphviz();
        
        std::ofstream file(filename);
        if (!file.is_open())
        {
            return false;
        }
        
        file << dot;
        return file.good();
    }

} // namespace RVX
