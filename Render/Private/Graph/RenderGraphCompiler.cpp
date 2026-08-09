#include "RenderGraphInternal.h"
#include "Core/Assert.h"
#include "Core/Log.h"
#include <algorithm>
#include <iterator>
#include <string>
#include <type_traits>

namespace RVX
{
    namespace
    {
        constexpr uint64 RVX_PLAN_HASH_OFFSET = 14695981039346656037ull;
        constexpr uint64 RVX_PLAN_HASH_PRIME = 1099511628211ull;

        void HashPlanBytes(uint64& hash, const void* data, size_t size)
        {
            const auto* bytes = static_cast<const uint8*>(data);
            for (size_t index = 0; index < size; ++index)
            {
                hash ^= bytes[index];
                hash *= RVX_PLAN_HASH_PRIME;
            }
        }

        template<typename T>
        void HashPlanValue(uint64& hash, T value)
        {
            static_assert(std::is_integral_v<T> || std::is_enum_v<T>);
            if constexpr (std::is_enum_v<T>)
            {
                using Underlying = std::underlying_type_t<T>;
                const Underlying underlying = static_cast<Underlying>(value);
                HashPlanBytes(hash, &underlying, sizeof(underlying));
            }
            else
            {
                HashPlanBytes(hash, &value, sizeof(value));
            }
        }

        void HashPlanString(uint64& hash, const std::string& value)
        {
            HashPlanValue(hash, static_cast<uint64>(value.size()));
            HashPlanBytes(hash, value.data(), value.size());
        }

        void HashPlanAccess(uint64& hash, const RHIAccessSnapshot& access)
        {
            HashPlanValue(hash, access.executionScope);
            HashPlanValue(hash, access.memoryAccess);
            HashPlanValue(hash, access.layout);
            HashPlanValue(hash, access.domain);
            HashPlanValue(hash, access.contentValidity);
        }

        void HashPlanRange(uint64& hash, const RHISubresourceRange& range)
        {
            HashPlanValue(hash, range.baseMipLevel);
            HashPlanValue(hash, range.mipLevelCount);
            HashPlanValue(hash, range.baseArrayLayer);
            HashPlanValue(hash, range.arrayLayerCount);
            HashPlanValue(hash, range.aspect);
        }

        uint64 ComputeCompiledPlanHash(const RenderGraphImpl& graph)
        {
            uint64 hash = RVX_PLAN_HASH_OFFSET;
            HashPlanValue(hash, graph.queueExecutionMode);
            HashPlanValue(hash, graph.enableMemoryAliasing);
            HashPlanValue(hash, graph.parallelRecordingEnabled);

            HashPlanValue(hash, static_cast<uint64>(graph.textures.size()));
            for (const TextureResource& texture : graph.textures)
            {
                HashPlanValue(hash, texture.desc.dimension);
                HashPlanValue(hash, texture.desc.width);
                HashPlanValue(hash, texture.desc.height);
                HashPlanValue(hash, texture.desc.depth);
                HashPlanValue(hash, texture.desc.mipLevels);
                HashPlanValue(hash, texture.desc.arraySize);
                HashPlanValue(hash, texture.desc.format);
                HashPlanValue(hash, texture.desc.sampleCount);
                HashPlanValue(hash, texture.desc.usage);
                HashPlanValue(hash, texture.imported);
                HashPlanAccess(hash,
                               texture.initialAccessSnapshot.uniformAccess);
                HashPlanValue(hash, texture.lifetime.firstUsePass);
                HashPlanValue(hash, texture.lifetime.lastUsePass);
                HashPlanValue(hash, texture.alias.heapIndex);
                HashPlanValue(hash, texture.alias.heapOffset);
            }

            HashPlanValue(hash, static_cast<uint64>(graph.buffers.size()));
            for (const BufferResource& buffer : graph.buffers)
            {
                HashPlanValue(hash, buffer.desc.size);
                HashPlanValue(hash, buffer.desc.stride);
                HashPlanValue(hash, buffer.desc.usage);
                HashPlanValue(hash, buffer.desc.memoryType);
                HashPlanValue(hash, buffer.imported);
                HashPlanAccess(hash,
                               buffer.initialAccessSnapshot.uniformAccess);
                HashPlanValue(hash, buffer.lifetime.firstUsePass);
                HashPlanValue(hash, buffer.lifetime.lastUsePass);
                HashPlanValue(hash, buffer.alias.heapIndex);
                HashPlanValue(hash, buffer.alias.heapOffset);
            }

            HashPlanValue(hash,
                          static_cast<uint64>(graph.textureViews.size()));
            for (const TextureViewResource& view : graph.textureViews)
            {
                HashPlanValue(hash, view.texture.index);
                HashPlanValue(hash, view.desc.format);
                HashPlanValue(hash, view.desc.dimension);
                HashPlanValue(hash, view.desc.type);
                HashPlanRange(hash, view.desc.subresourceRange);
            }

            HashPlanValue(hash, static_cast<uint64>(graph.passes.size()));
            for (const Pass& pass : graph.passes)
            {
                HashPlanString(hash, pass.name);
                HashPlanValue(hash, pass.type);
                HashPlanValue(hash, pass.plannedExecutionQueue);
                HashPlanValue(hash, pass.culled);
                HashPlanValue(hash, static_cast<uint64>(pass.usages.size()));
                for (const ResourceUsage& usage : pass.usages)
                {
                    HashPlanValue(hash, usage.type);
                    HashPlanValue(hash, usage.index);
                    HashPlanValue(hash, usage.access);
                    HashPlanValue(hash, usage.discardIntent);
                    HashPlanAccess(hash, usage.desiredAccess);
                    HashPlanValue(hash, usage.hasSubresourceRange);
                    if (usage.hasSubresourceRange)
                        HashPlanRange(hash, usage.subresourceRange);
                    HashPlanValue(hash, usage.hasRange);
                    if (usage.hasRange)
                    {
                        HashPlanValue(hash, usage.offset);
                        HashPlanValue(hash, usage.size);
                    }
                }

                const auto hashTextureBarriers = [&](const auto& barriers)
                {
                    HashPlanValue(hash,
                                  static_cast<uint64>(barriers.size()));
                    for (const PlannedTextureBarrier& planned : barriers)
                    {
                        HashPlanValue(hash, planned.resourceIndex);
                        HashPlanValue(hash, planned.barrier.stateBefore);
                        HashPlanValue(hash, planned.barrier.stateAfter);
                        HashPlanAccess(hash, planned.barrier.accessBefore);
                        HashPlanAccess(hash, planned.barrier.accessAfter);
                        HashPlanValue(hash, planned.barrier.dependencyKind);
                        HashPlanValue(hash, planned.barrier.discardIntent);
                        HashPlanRange(hash,
                                      planned.barrier.subresourceRange);
                    }
                };
                const auto hashBufferBarriers = [&](const auto& barriers)
                {
                    HashPlanValue(hash,
                                  static_cast<uint64>(barriers.size()));
                    for (const PlannedBufferBarrier& planned : barriers)
                    {
                        HashPlanValue(hash, planned.resourceIndex);
                        HashPlanValue(hash, planned.barrier.stateBefore);
                        HashPlanValue(hash, planned.barrier.stateAfter);
                        HashPlanAccess(hash, planned.barrier.accessBefore);
                        HashPlanAccess(hash, planned.barrier.accessAfter);
                        HashPlanValue(hash, planned.barrier.dependencyKind);
                        HashPlanValue(hash, planned.barrier.discardIntent);
                        HashPlanValue(hash, planned.barrier.offset);
                        HashPlanValue(hash, planned.barrier.size);
                    }
                };
                hashTextureBarriers(pass.textureBarriers);
                hashTextureBarriers(pass.postTextureBarriers);
                hashBufferBarriers(pass.bufferBarriers);
                hashBufferBarriers(pass.postBufferBarriers);
            }

            HashPlanValue(hash,
                          static_cast<uint64>(graph.executionOrder.size()));
            for (uint32 passIndex : graph.executionOrder)
                HashPlanValue(hash, passIndex);
            for (const std::vector<uint32>& dependencies :
                 graph.passDependencies)
            {
                HashPlanValue(hash,
                              static_cast<uint64>(dependencies.size()));
                for (uint32 dependency : dependencies)
                    HashPlanValue(hash, dependency);
            }
            return hash;
        }

        bool IsAllSubresourceRange(const RHISubresourceRange& range)
        {
            return range.baseMipLevel == 0 &&
                   range.mipLevelCount == RVX_ALL_MIPS &&
                   range.baseArrayLayer == 0 &&
                   range.arrayLayerCount == RVX_ALL_LAYERS;
        }

        RHITextureAspect GetDefaultTextureAspect(const RHITextureDesc& desc)
        {
            return IsDepthFormat(desc.format) ? RHITextureAspect::Depth : RHITextureAspect::Color;
        }

        RHISubresourceRange AllSubresourcesForTexture(const RHITextureDesc& desc)
        {
            RHISubresourceRange range = RHISubresourceRange::All();
            range.aspect = GetDefaultTextureAspect(desc);
            return range;
        }

        RenderGraph::DiagnosticExecutionQueue ToDiagnosticQueue(
            GPUQueueDomain domain)
        {
            switch (domain)
            {
                case GPUQueueDomain::Graphics:
                    return RenderGraph::DiagnosticExecutionQueue::Graphics;
                case GPUQueueDomain::Compute:
                    return RenderGraph::DiagnosticExecutionQueue::Compute;
                case GPUQueueDomain::Copy:
                    return RenderGraph::DiagnosticExecutionQueue::Copy;
            }
            return RenderGraph::DiagnosticExecutionQueue::Unknown;
        }

        void ResolveSubresourceRange(
            const RHISubresourceRange& range,
            const TextureResource& resource,
            uint32& baseMip,
            uint32& mipCount,
            uint32& baseLayer,
            uint32& layerCount)
        {
            baseMip = range.baseMipLevel;
            mipCount = (range.mipLevelCount == RVX_ALL_MIPS) ? resource.desc.mipLevels : range.mipLevelCount;
            baseLayer = range.baseArrayLayer;
            layerCount = (range.arrayLayerCount == RVX_ALL_LAYERS)
                ? GetTexturePhysicalLayerCount(resource.desc)
                : range.arrayLayerCount;
        }

        bool IsWholeBufferRange(uint64 offset, uint64 size, uint64 fullSize)
        {
            if (offset != 0)
                return false;
            if (size == RVX_WHOLE_SIZE)
                return true;
            return size >= fullSize;
        }

        uint64 ResolveBufferRangeSize(uint64 offset, uint64 size, uint64 fullSize)
        {
            if (size == RVX_WHOLE_SIZE || size == 0)
                return fullSize > offset ? fullSize - offset : 0;
            return std::min(size, fullSize > offset ? fullSize - offset : 0);
        }

        bool TextureRangesOverlap(const RHISubresourceRange& left,
                                  const RHISubresourceRange& right,
                                  const TextureResource& resource)
        {
            uint32 leftMip = 0;
            uint32 leftMipCount = 0;
            uint32 leftLayer = 0;
            uint32 leftLayerCount = 0;
            uint32 rightMip = 0;
            uint32 rightMipCount = 0;
            uint32 rightLayer = 0;
            uint32 rightLayerCount = 0;
            ResolveSubresourceRange(left,
                                    resource,
                                    leftMip,
                                    leftMipCount,
                                    leftLayer,
                                    leftLayerCount);
            ResolveSubresourceRange(right,
                                    resource,
                                    rightMip,
                                    rightMipCount,
                                    rightLayer,
                                    rightLayerCount);
            const bool mipOverlap =
                leftMip < rightMip + rightMipCount &&
                rightMip < leftMip + leftMipCount;
            const bool layerOverlap =
                leftLayer < rightLayer + rightLayerCount &&
                rightLayer < leftLayer + leftLayerCount;
            return mipOverlap && layerOverlap;
        }

        bool BufferRangesOverlap(uint64 leftOffset,
                                 uint64 leftSize,
                                 uint64 rightOffset,
                                 uint64 rightSize,
                                 uint64 fullSize)
        {
            const uint64 resolvedLeft = ResolveBufferRangeSize(
                leftOffset, leftSize, fullSize);
            const uint64 resolvedRight = ResolveBufferRangeSize(
                rightOffset, rightSize, fullSize);
            return resolvedLeft != 0 && resolvedRight != 0 &&
                   leftOffset < rightOffset + resolvedRight &&
                   rightOffset < leftOffset + resolvedLeft;
        }

        void EnsureBufferRangeTracking(BufferResource& resource)
        {
            if (resource.hasRangeTracking)
                return;
            resource.rangeStates.clear();
            resource.rangeStates.push_back({
                0,
                resource.desc.size,
                resource.currentState,
                resource.currentAccessSnapshot.uniformAccess});
            resource.hasRangeTracking = true;

            for (const RHIBufferRangeAccessSnapshot& accessOverride :
                 resource.currentAccessSnapshot.rangeOverrides)
            {
                const uint64 overrideSize = ResolveBufferRangeSize(
                    accessOverride.offset,
                    accessOverride.size,
                    resource.desc.size);
                if (overrideSize == 0)
                    continue;

                const uint64 overrideEnd = accessOverride.offset + overrideSize;
                std::vector<BufferResource::RangeState> updated;
                updated.reserve(resource.rangeStates.size() + 2);
                for (const auto& range : resource.rangeStates)
                {
                    const uint64 rangeEnd = range.offset + range.size;
                    if (overrideEnd <= range.offset || accessOverride.offset >= rangeEnd)
                    {
                        updated.push_back(range);
                        continue;
                    }
                    if (accessOverride.offset > range.offset)
                    {
                        updated.push_back({range.offset,
                                           accessOverride.offset - range.offset,
                                           range.state,
                                           range.access});
                    }
                    const uint64 overlapStart = std::max(accessOverride.offset, range.offset);
                    const uint64 overlapEnd = std::min(overrideEnd, rangeEnd);
                    updated.push_back({overlapStart,
                                       overlapEnd - overlapStart,
                                       ProjectRHIResourceState(accessOverride.access),
                                       accessOverride.access});
                    if (overlapEnd < rangeEnd)
                    {
                        updated.push_back({overlapEnd,
                                           rangeEnd - overlapEnd,
                                           range.state,
                                           range.access});
                    }
                }
                resource.rangeStates.swap(updated);
            }
            resource.currentAccessSnapshot.rangeOverrides.clear();
        }

        void EnsureTextureSubresourceTracking(TextureResource& resource)
        {
            if (resource.hasSubresourceTracking ||
                resource.currentAccessSnapshot.subresourceOverrides.empty())
            {
                return;
            }

            for (const RHITextureSubresourceAccessSnapshot& accessOverride :
                 resource.currentAccessSnapshot.subresourceOverrides)
            {
                uint32 baseMip = 0;
                uint32 mipCount = 0;
                uint32 baseLayer = 0;
                uint32 layerCount = 0;
                ResolveSubresourceRange(
                    accessOverride.range,
                    resource,
                    baseMip,
                    mipCount,
                    baseLayer,
                    layerCount);
                for (uint32 mip = baseMip; mip < baseMip + mipCount; ++mip)
                {
                    for (uint32 layer = baseLayer; layer < baseLayer + layerCount; ++layer)
                    {
                        const uint32 key = mip + layer * resource.desc.mipLevels;
                        resource.subresourceAccesses[key] = accessOverride.access;
                        resource.subresourceStates[key] =
                            ProjectRHIResourceState(accessOverride.access);
                    }
                }
            }
            resource.currentAccessSnapshot.subresourceOverrides.clear();
            resource.hasSubresourceTracking = true;
        }

        void MergeBufferRanges(std::vector<BufferResource::RangeState>& ranges)
        {
            if (ranges.empty())
                return;
            std::sort(
                ranges.begin(),
                ranges.end(),
                [](const BufferResource::RangeState& a, const BufferResource::RangeState& b)
                {
                    return a.offset < b.offset;
                });

            std::vector<BufferResource::RangeState> merged;
            merged.reserve(ranges.size());
            merged.push_back(ranges.front());
            for (size_t i = 1; i < ranges.size(); ++i)
            {
                auto& back = merged.back();
                const auto& current = ranges[i];
                if (back.access == current.access && back.offset + back.size == current.offset)
                {
                    back.size += current.size;
                }
                else
                {
                    merged.push_back(current);
                }
            }
            ranges.swap(merged);
        }

        void ApplyBufferRangeTransition(
            BufferResource& resource,
            uint64 offset,
            uint64 size,
            const RHIAccessSnapshot& desiredAccess,
            RHIDiscardIntent discardIntent,
            bool writeOnly,
            std::vector<RHIBufferBarrier>& outBarriers)
        {
            if (size == 0 || resource.desc.size == 0)
                return;

            EnsureBufferRangeTracking(resource);

            uint64 end = offset + size;
            std::vector<BufferResource::RangeState> updated;
            updated.reserve(resource.rangeStates.size() + 2);

            for (const auto& range : resource.rangeStates)
            {
                uint64 rangeStart = range.offset;
                uint64 rangeEnd = range.offset + range.size;
                if (end <= rangeStart || offset >= rangeEnd)
                {
                    updated.push_back(range);
                    continue;
                }

                if (offset > rangeStart)
                {
                    updated.push_back({rangeStart, offset - rangeStart, range.state, range.access});
                }

                uint64 overlapStart = std::max(offset, rangeStart);
                uint64 overlapEnd = std::min(end, rangeEnd);
                uint64 overlapSize = overlapEnd - overlapStart;
                RHIDiscardIntent effectiveDiscard = discardIntent;
                if (writeOnly &&
                    range.access.contentValidity != RHIContentValidity::Valid)
                {
                    effectiveDiscard = RHIDiscardIntent::Discard;
                }
                const RHIDependencyKind dependency = ClassifyRHIDependency(
                    range.access,
                    desiredAccess,
                    effectiveDiscard);
                if (dependency != RHIDependencyKind::None)
                {
                    outBarriers.push_back(MakeRHIBufferBarrier(
                        nullptr,
                        range.access,
                        desiredAccess,
                        overlapStart,
                        overlapSize,
                        effectiveDiscard));
                }
                updated.push_back({overlapStart,
                                   overlapSize,
                                   ProjectRHIResourceState(desiredAccess),
                                   desiredAccess});

                if (overlapEnd < rangeEnd)
                {
                    updated.push_back({overlapEnd,
                                       rangeEnd - overlapEnd,
                                       range.state,
                                       range.access});
                }
            }

            MergeBufferRanges(updated);
            resource.rangeStates.swap(updated);
        }

        bool IsStateAllowedForPass(RenderGraphPassType passType, RHIResourceState state)
        {
            if (passType == RenderGraphPassType::Copy)
            {
                return state == RHIResourceState::CopySource ||
                       state == RHIResourceState::CopyDest ||
                       state == RHIResourceState::Common ||
                       state == RHIResourceState::Undefined;
            }

            if (passType == RenderGraphPassType::Compute || passType == RenderGraphPassType::RayTracing)
            {
                return state == RHIResourceState::ShaderResource ||
                       state == RHIResourceState::UnorderedAccess ||
                       state == RHIResourceState::ConstantBuffer ||
                       state == RHIResourceState::IndirectArgument ||
                       state == RHIResourceState::AccelerationStructureBuildRead ||
                       state == RHIResourceState::AccelerationStructureBuildWrite ||
                       state == RHIResourceState::AccelerationStructureRead ||
                       state == RHIResourceState::ShaderBindingTable ||
                       state == RHIResourceState::CopySource ||
                       state == RHIResourceState::CopyDest ||
                       state == RHIResourceState::Common ||
                       state == RHIResourceState::Undefined;
            }

            return true;
        }

        bool IsShaderVisibleState(RHIResourceState state)
        {
            return state == RHIResourceState::ShaderResource ||
                   state == RHIResourceState::UnorderedAccess ||
                   state == RHIResourceState::ConstantBuffer ||
                   state == RHIResourceState::AccelerationStructureRead ||
                   state == RHIResourceState::ShaderBindingTable;
        }

        bool HasShaderStageBits(RHIShaderStage stages, RHIShaderStage mask)
        {
            return (static_cast<uint32>(stages) & static_cast<uint32>(mask)) != 0u;
        }

        bool HasOnlyShaderStageBits(RHIShaderStage stages, RHIShaderStage mask)
        {
            const uint32 stageBits = static_cast<uint32>(stages);
            const uint32 maskBits = static_cast<uint32>(mask);
            return stageBits != 0u && (stageBits & ~maskBits) == 0u;
        }

        bool AreShaderStagesCompatibleWithPass(RenderGraphPassType passType, RHIShaderStage stages)
        {
            switch (passType)
            {
                case RenderGraphPassType::Graphics:
                    return HasOnlyShaderStageBits(stages, RHIShaderStage::AllGraphics);
                case RenderGraphPassType::Compute:
                    return stages == RHIShaderStage::Compute;
                case RenderGraphPassType::RayTracing:
                    return HasShaderStageBits(stages, RHIShaderStage::AllRayTracing) &&
                           HasOnlyShaderStageBits(stages, RHIShaderStage::AllRayTracing);
                case RenderGraphPassType::Copy:
                default:
                    return true;
            }
        }

        bool IsAllRange(const RHISubresourceRange& range)
        {
            return range.baseMipLevel == 0 &&
                   range.mipLevelCount == RVX_ALL_MIPS &&
                   range.baseArrayLayer == 0 &&
                   range.arrayLayerCount == RVX_ALL_LAYERS;
        }

        bool IsAllBufferRange(const RHIBufferBarrier& barrier)
        {
            return barrier.offset == 0 && barrier.size == RVX_WHOLE_SIZE;
        }

        uint32 MergeTextureBarriers(std::vector<PlannedTextureBarrier>& barriers)
        {
            if (barriers.empty())
                return 0;

            auto beforeCount = static_cast<uint32>(barriers.size());
            std::sort(
                barriers.begin(),
                barriers.end(),
                [](const PlannedTextureBarrier& a,
                   const PlannedTextureBarrier& b)
                {
                    if (a.resourceIndex != b.resourceIndex)
                        return a.resourceIndex < b.resourceIndex;
                    if (a.barrier.stateBefore != b.barrier.stateBefore)
                        return a.barrier.stateBefore < b.barrier.stateBefore;
                    if (a.barrier.stateAfter != b.barrier.stateAfter)
                        return a.barrier.stateAfter < b.barrier.stateAfter;
                    if (a.barrier.subresourceRange.aspect != b.barrier.subresourceRange.aspect)
                        return a.barrier.subresourceRange.aspect < b.barrier.subresourceRange.aspect;
                    if (a.barrier.subresourceRange.baseArrayLayer != b.barrier.subresourceRange.baseArrayLayer)
                        return a.barrier.subresourceRange.baseArrayLayer < b.barrier.subresourceRange.baseArrayLayer;
                    if (a.barrier.subresourceRange.arrayLayerCount != b.barrier.subresourceRange.arrayLayerCount)
                        return a.barrier.subresourceRange.arrayLayerCount < b.barrier.subresourceRange.arrayLayerCount;
                    return a.barrier.subresourceRange.baseMipLevel < b.barrier.subresourceRange.baseMipLevel;
                });

            std::vector<PlannedTextureBarrier> merged;
            merged.reserve(barriers.size());

            for (const auto& barrier : barriers)
            {
                if (merged.empty())
                {
                    merged.push_back(barrier);
                    continue;
                }

                auto& last = merged.back();
                if (last.resourceIndex == barrier.resourceIndex &&
                    last.barrier.stateBefore == barrier.barrier.stateBefore &&
                    last.barrier.stateAfter == barrier.barrier.stateAfter &&
                    last.barrier.hasScopedAccess == barrier.barrier.hasScopedAccess &&
                    (!last.barrier.hasScopedAccess ||
                     (last.barrier.accessBefore == barrier.barrier.accessBefore &&
                      last.barrier.accessAfter == barrier.barrier.accessAfter &&
                      last.barrier.dependencyKind == barrier.barrier.dependencyKind &&
                      last.barrier.discardIntent == barrier.barrier.discardIntent)) &&
                    last.barrier.subresourceRange.aspect == barrier.barrier.subresourceRange.aspect)
                {
                    if (IsAllRange(last.barrier.subresourceRange))
                        continue;

                    if (IsAllRange(barrier.barrier.subresourceRange))
                    {
                        last.barrier.subresourceRange = RHISubresourceRange::All();
                        last.barrier.subresourceRange.aspect =
                            barrier.barrier.subresourceRange.aspect;
                        continue;
                    }

                    bool sameLayerRange = last.barrier.subresourceRange.baseArrayLayer == barrier.barrier.subresourceRange.baseArrayLayer &&
                                          last.barrier.subresourceRange.arrayLayerCount == barrier.barrier.subresourceRange.arrayLayerCount;
                    bool sameMipRange = last.barrier.subresourceRange.baseMipLevel == barrier.barrier.subresourceRange.baseMipLevel &&
                                        last.barrier.subresourceRange.mipLevelCount == barrier.barrier.subresourceRange.mipLevelCount;
                    bool adjacentMip = sameLayerRange &&
                                       last.barrier.subresourceRange.baseMipLevel + last.barrier.subresourceRange.mipLevelCount ==
                                           barrier.barrier.subresourceRange.baseMipLevel;
                    bool adjacentLayer = sameMipRange &&
                                         last.barrier.subresourceRange.baseArrayLayer + last.barrier.subresourceRange.arrayLayerCount ==
                                             barrier.barrier.subresourceRange.baseArrayLayer;
                    if (adjacentMip)
                    {
                        last.barrier.subresourceRange.mipLevelCount += barrier.barrier.subresourceRange.mipLevelCount;
                        continue;
                    }

                    if (adjacentLayer)
                    {
                        last.barrier.subresourceRange.arrayLayerCount += barrier.barrier.subresourceRange.arrayLayerCount;
                        continue;
                    }

                    if (sameLayerRange && sameMipRange)
                        continue;
                }

                merged.push_back(barrier);
            }

            barriers.swap(merged);
            return beforeCount - static_cast<uint32>(barriers.size());
        }

        uint32 MergeBufferBarriers(std::vector<PlannedBufferBarrier>& barriers)
        {
            if (barriers.empty())
                return 0;

            auto beforeCount = static_cast<uint32>(barriers.size());
            std::sort(
                barriers.begin(),
                barriers.end(),
                [](const PlannedBufferBarrier& a,
                   const PlannedBufferBarrier& b)
                {
                    if (a.resourceIndex != b.resourceIndex)
                        return a.resourceIndex < b.resourceIndex;
                    if (a.barrier.stateBefore != b.barrier.stateBefore)
                        return a.barrier.stateBefore < b.barrier.stateBefore;
                    if (a.barrier.stateAfter != b.barrier.stateAfter)
                        return a.barrier.stateAfter < b.barrier.stateAfter;
                    return a.barrier.offset < b.barrier.offset;
                });

            std::vector<PlannedBufferBarrier> merged;
            merged.reserve(barriers.size());

            for (const auto& barrier : barriers)
            {
                if (merged.empty())
                {
                    merged.push_back(barrier);
                    continue;
                }

                auto& last = merged.back();
                if (last.resourceIndex == barrier.resourceIndex &&
                    last.barrier.stateBefore == barrier.barrier.stateBefore &&
                    last.barrier.stateAfter == barrier.barrier.stateAfter &&
                    last.barrier.hasScopedAccess == barrier.barrier.hasScopedAccess &&
                    (!last.barrier.hasScopedAccess ||
                     (last.barrier.accessBefore == barrier.barrier.accessBefore &&
                      last.barrier.accessAfter == barrier.barrier.accessAfter &&
                      last.barrier.dependencyKind == barrier.barrier.dependencyKind &&
                      last.barrier.discardIntent == barrier.barrier.discardIntent)))
                {
                    if (IsAllBufferRange(last.barrier))
                        continue;

                    if (IsAllBufferRange(barrier.barrier))
                    {
                        last.barrier.offset = 0;
                        last.barrier.size = RVX_WHOLE_SIZE;
                        continue;
                    }

                    bool adjacent = last.barrier.offset + last.barrier.size ==
                        barrier.barrier.offset;
                    bool sameRange = last.barrier.offset == barrier.barrier.offset &&
                        last.barrier.size == barrier.barrier.size;
                    if (adjacent)
                    {
                        last.barrier.size += barrier.barrier.size;
                        continue;
                    }

                    if (sameRange)
                        continue;
                }

                merged.push_back(barrier);
            }

            barriers.swap(merged);
            return beforeCount - static_cast<uint32>(barriers.size());
        }

        uint32 RemoveRedundantTextureBarriers(
            const std::unordered_map<uint32, RHIResourceState>& prevStates,
            std::vector<PlannedTextureBarrier>& barriers)
        {
            if (barriers.empty() || prevStates.empty())
                return 0;

            auto beforeCount = static_cast<uint32>(barriers.size());
            std::vector<PlannedTextureBarrier> filtered;
            filtered.reserve(barriers.size());

            for (const auto& barrier : barriers)
            {
                if (barrier.barrier.hasScopedAccess &&
                    barrier.barrier.dependencyKind != RHIDependencyKind::Transition)
                {
                    filtered.push_back(barrier);
                    continue;
                }
                if (!IsAllRange(barrier.barrier.subresourceRange))
                {
                    filtered.push_back(barrier);
                    continue;
                }

                auto it = prevStates.find(barrier.resourceIndex);
                if (it == prevStates.end())
                {
                    filtered.push_back(barrier);
                    continue;
                }

                if (barrier.barrier.stateBefore == it->second)
                {
                    filtered.push_back(barrier);
                }
            }

            barriers.swap(filtered);
            return beforeCount - static_cast<uint32>(barriers.size());
        }

        uint32 RemoveRedundantBufferBarriers(
            const std::unordered_map<uint32, RHIResourceState>& prevStates,
            std::vector<PlannedBufferBarrier>& barriers)
        {
            if (barriers.empty() || prevStates.empty())
                return 0;

            auto beforeCount = static_cast<uint32>(barriers.size());
            std::vector<PlannedBufferBarrier> filtered;
            filtered.reserve(barriers.size());

            for (const auto& barrier : barriers)
            {
                if (barrier.barrier.hasScopedAccess &&
                    barrier.barrier.dependencyKind != RHIDependencyKind::Transition)
                {
                    filtered.push_back(barrier);
                    continue;
                }
                if (!IsAllBufferRange(barrier.barrier))
                {
                    filtered.push_back(barrier);
                    continue;
                }

                auto it = prevStates.find(barrier.resourceIndex);
                if (it == prevStates.end())
                {
                    filtered.push_back(barrier);
                    continue;
                }

                if (barrier.barrier.stateBefore == it->second)
                {
                    filtered.push_back(barrier);
                }
            }

            barriers.swap(filtered);
            return beforeCount - static_cast<uint32>(barriers.size());
        }

        // Estimate texture memory size based on format and dimensions
        uint64 EstimateTextureMemorySize(const RHITextureDesc& desc)
        {
            uint64 bpp = GetFormatBytesPerPixel(desc.format);
            if (bpp == 0) bpp = 4;  // Fallback for compressed formats
            
            uint64 totalSize = 0;
            uint32 width = desc.width;
            uint32 height = desc.height;
            uint32 depth = desc.depth;
            
            for (uint32 mip = 0; mip < desc.mipLevels; ++mip)
            {
                uint64 mipSize = static_cast<uint64>(width) * height * depth * bpp;
                totalSize += mipSize * desc.arraySize;
                
                width = std::max(1u, width / 2);
                height = std::max(1u, height / 2);
                depth = std::max(1u, depth / 2);
            }
            
            // Account for MSAA
            totalSize *= static_cast<uint32>(desc.sampleCount);
            
            // Align to 64KB (D3D12 default heap alignment)
            return (totalSize + 65535) & ~65535ULL;
        }

        // Estimate buffer memory size
        uint64 EstimateBufferMemorySize(const RHIBufferDesc& desc)
        {
            // Align to 256 bytes (constant buffer alignment)
            return (desc.size + 255) & ~255ULL;
        }

        // Check if two lifetimes overlap
        bool LifetimesOverlap(const ResourceLifetime& a, const ResourceLifetime& b)
        {
            if (!a.isUsed || !b.isUsed) return false;
            return !(a.lastUsePass < b.firstUsePass || b.lastUsePass < a.firstUsePass);
        }

        void AddCompileWarning(RenderGraphImpl& graph, const std::string& message)
        {
            graph.compileDiagnostics.push_back(message);
            RVX_CORE_WARN("{}", message);
        }

        void AddCompileError(RenderGraphImpl& graph, const std::string& message)
        {
            graph.compileDiagnostics.push_back(message);
            RVX_CORE_ERROR("{}", message);
        }

        void ValidatePassUsages(RenderGraphImpl& graph)
        {
            for (uint32 passIndex = 0; passIndex < graph.passes.size(); ++passIndex)
            {
                const auto& pass = graph.passes[passIndex];

                if (pass.usages.empty())
                {
                    graph.stats.emptyPassUsageCount++;
                    graph.stats.validationWarningCount++;
                    AddCompileWarning(graph,
                                      "RenderGraph pass '" + pass.name +
                                          "' declares no resource usage and may be culled");
                }

                for (const auto& usage : pass.usages)
                {
                    if (usage.type == ResourceType::Texture)
                    {
                        if (usage.index >= graph.textures.size())
                        {
                            graph.stats.invalidResourceUsageCount++;
                            graph.stats.validationErrorCount++;
                            AddCompileError(graph,
                                            "RenderGraph pass '" + pass.name +
                                                "' references invalid texture handle " +
                                                std::to_string(usage.index));
                        }
                    }
                    else
                    {
                        if (usage.index >= graph.buffers.size())
                        {
                            graph.stats.invalidResourceUsageCount++;
                            graph.stats.validationErrorCount++;
                            AddCompileError(graph,
                                            "RenderGraph pass '" + pass.name +
                                                "' references invalid buffer handle " +
                                                std::to_string(usage.index));
                        }
                    }

                    if (!IsStateAllowedForPass(pass.type, usage.desiredState))
                    {
                        graph.stats.incompatibleStateUsageCount++;
                        graph.stats.validationErrorCount++;
                        AddCompileError(graph,
                                        "RenderGraph pass '" + pass.name + "' requests state " +
                                            std::to_string(static_cast<int>(usage.desiredState)) +
                                            " that is incompatible with pass type " +
                                            std::to_string(static_cast<int>(pass.type)));
                    }

                    if (usage.access == RGAccessType::Read &&
                        IsShaderVisibleState(usage.desiredState) &&
                        !AreShaderStagesCompatibleWithPass(pass.type, usage.stages))
                    {
                        graph.stats.shaderStageMismatchUsageCount++;
                        graph.stats.validationWarningCount++;
                        AddCompileWarning(graph,
                                          "RenderGraph pass '" + pass.name + "' reads resource with shader stage mask " +
                                              std::to_string(static_cast<uint32>(usage.stages)) +
                                              " that does not match pass type " +
                                              std::to_string(static_cast<int>(pass.type)));
                    }
                }
            }
        }

        bool ContainsIndex(const std::vector<uint32>& values, uint32 target)
        {
            return std::find(values.begin(), values.end(), target) != values.end();
        }

        void AddDependencyEdge(
            std::vector<std::vector<uint32>>& adjacency,
            std::vector<uint32>& indegree,
            uint32 beforePass,
            uint32 afterPass)
        {
            if (beforePass == afterPass)
                return;

            auto& edges = adjacency[beforePass];
            if (std::find(edges.begin(), edges.end(), afterPass) != edges.end())
                return;

            edges.push_back(afterPass);
            indegree[afterPass]++;
        }

        void ValidateResourceLifetimes(RenderGraphImpl& graph, const std::vector<uint8>& passNeeded)
        {
            std::vector<uint8> initializedTextures(graph.textures.size(), 0);
            std::vector<uint8> initializedBuffers(graph.buffers.size(), 0);

            for (uint32 textureIndex = 0; textureIndex < graph.textures.size(); ++textureIndex)
            {
                initializedTextures[textureIndex] = graph.textures[textureIndex].imported ? 1 : 0;
            }
            for (uint32 bufferIndex = 0; bufferIndex < graph.buffers.size(); ++bufferIndex)
            {
                initializedBuffers[bufferIndex] = graph.buffers[bufferIndex].imported ? 1 : 0;
            }

            for (uint32 passIndex = 0; passIndex < graph.passes.size(); ++passIndex)
            {
                if (!passNeeded.empty() && passNeeded[passIndex] == 0)
                    continue;

                const auto& pass = graph.passes[passIndex];
                std::vector<uint32> writtenTextures;
                std::vector<uint32> writtenBuffers;

                for (const auto& usage : pass.usages)
                {
                    const bool readsResource =
                        usage.access == RGAccessType::Read || usage.access == RGAccessType::ReadWrite;
                    const bool writesResource =
                        usage.access == RGAccessType::Write || usage.access == RGAccessType::ReadWrite;

                    if (usage.type == ResourceType::Texture)
                    {
                        if (usage.index >= graph.textures.size())
                            continue;

                        const auto& resource = graph.textures[usage.index];
                        if (!resource.imported && readsResource && !initializedTextures[usage.index])
                        {
                            graph.stats.readBeforeWriteHazardCount++;
                            graph.stats.validationErrorCount++;
                            AddCompileError(graph,
                                            "RenderGraph pass '" + pass.name +
                                                "' reads transient texture " +
                                                std::to_string(usage.index) +
                                                " before any producing write");
                        }

                        if (!resource.imported && writesResource &&
                            !ContainsIndex(writtenTextures, usage.index))
                        {
                            writtenTextures.push_back(usage.index);
                        }
                    }
                    else
                    {
                        if (usage.index >= graph.buffers.size())
                            continue;

                        const auto& resource = graph.buffers[usage.index];
                        if (!resource.imported && readsResource && !initializedBuffers[usage.index])
                        {
                            graph.stats.readBeforeWriteHazardCount++;
                            graph.stats.validationErrorCount++;
                            AddCompileError(graph,
                                            "RenderGraph pass '" + pass.name +
                                                "' reads transient buffer " +
                                                std::to_string(usage.index) +
                                                " before any producing write");
                        }

                        if (!resource.imported && writesResource &&
                            !ContainsIndex(writtenBuffers, usage.index))
                        {
                            writtenBuffers.push_back(usage.index);
                        }
                    }
                }

                for (uint32 textureIndex : writtenTextures)
                {
                    initializedTextures[textureIndex] = 1;
                }
                for (uint32 bufferIndex : writtenBuffers)
                {
                    initializedBuffers[bufferIndex] = 1;
                }
            }

            for (uint32 textureIndex = 0; textureIndex < graph.textures.size(); ++textureIndex)
            {
                const auto& resource = graph.textures[textureIndex];
                if (!resource.imported && resource.exportState.has_value() &&
                    !initializedTextures[textureIndex])
                {
                    graph.stats.uninitializedExportCount++;
                    graph.stats.validationErrorCount++;
                    AddCompileError(graph,
                                    "RenderGraph exports transient texture " +
                                        std::to_string(textureIndex) +
                                        " without any producing write");
                }
            }

            for (uint32 bufferIndex = 0; bufferIndex < graph.buffers.size(); ++bufferIndex)
            {
                const auto& resource = graph.buffers[bufferIndex];
                if (!resource.imported && resource.exportState.has_value() &&
                    !initializedBuffers[bufferIndex])
                {
                    graph.stats.uninitializedExportCount++;
                    graph.stats.validationErrorCount++;
                    AddCompileError(graph,
                                    "RenderGraph exports transient buffer " +
                                        std::to_string(bufferIndex) +
                                        " without any producing write");
                }
            }
        }
    }

    // =============================================================================
    // Calculate Resource Lifetimes
    // =============================================================================
    void CalculateResourceLifetimes(RenderGraphImpl& graph)
    {
        // Reset lifetimes
        for (auto& texture : graph.textures)
        {
            texture.lifetime = ResourceLifetime{};
            texture.alias = MemoryAlias{};
        }
        for (auto& buffer : graph.buffers)
        {
            buffer.lifetime = ResourceLifetime{};
            buffer.alias = MemoryAlias{};
        }

        // Compilation is allocation- and device-call-free. The immutable plan
        // uses conservative backend-neutral estimates; realization validates
        // physical allocation requirements before creating placed resources.
        auto getTextureMemReqs = [&](const RHITextureDesc& desc) -> std::pair<uint64, uint64> {
            return {EstimateTextureMemorySize(desc), 65536};
        };

        auto getBufferMemReqs = [&](const RHIBufferDesc& desc) -> std::pair<uint64, uint64> {
            return {EstimateBufferMemorySize(desc), 256};
        };

        // Calculate lifetimes based on execution order (not insertion order)
        // This is critical for correct memory aliasing after topological sort
        if (graph.executionOrder.empty())
        {
            // Fallback to insertion order if execution order not yet computed
            for (uint32 passIndex = 0; passIndex < graph.passes.size(); ++passIndex)
            {
                const auto& pass = graph.passes[passIndex];
                if (pass.culled) continue;

                for (const auto& usage : pass.usages)
                {
                    if (usage.type == ResourceType::Texture)
                    {
                        if (usage.index >= graph.textures.size()) continue;
                        auto& texture = graph.textures[usage.index];
                        if (texture.imported) continue;

                        texture.lifetime.isUsed = true;
                        texture.lifetime.firstUsePass = std::min(texture.lifetime.firstUsePass, passIndex);
                        texture.lifetime.lastUsePass = std::max(texture.lifetime.lastUsePass, passIndex);
                        auto [size, alignment] = getTextureMemReqs(texture.desc);
                        texture.lifetime.memorySize = size;
                        texture.lifetime.alignment = alignment;
                    }
                    else
                    {
                        if (usage.index >= graph.buffers.size()) continue;
                        auto& buffer = graph.buffers[usage.index];
                        if (buffer.imported) continue;

                        buffer.lifetime.isUsed = true;
                        buffer.lifetime.firstUsePass = std::min(buffer.lifetime.firstUsePass, passIndex);
                        buffer.lifetime.lastUsePass = std::max(buffer.lifetime.lastUsePass, passIndex);
                        auto [size, alignment] = getBufferMemReqs(buffer.desc);
                        buffer.lifetime.memorySize = size;
                        buffer.lifetime.alignment = alignment;
                    }
                }
            }
            return;
        }

        // Use execution order index as the timeline for lifetime calculation
        for (uint32 order = 0; order < static_cast<uint32>(graph.executionOrder.size()); ++order)
        {
            uint32 passIndex = graph.executionOrder[order];
            const auto& pass = graph.passes[passIndex];
            if (pass.culled) continue;

            for (const auto& usage : pass.usages)
            {
                if (usage.type == ResourceType::Texture)
                {
                    if (usage.index >= graph.textures.size()) continue;
                    auto& texture = graph.textures[usage.index];
                    if (texture.imported) continue;  // Don't alias imported resources

                    texture.lifetime.isUsed = true;
                    // Use 'order' (execution timeline) not 'passIndex' (insertion order)
                    texture.lifetime.firstUsePass = std::min(texture.lifetime.firstUsePass, order);
                    texture.lifetime.lastUsePass = std::max(texture.lifetime.lastUsePass, order);
                    auto [size, alignment] = getTextureMemReqs(texture.desc);
                    texture.lifetime.memorySize = size;
                    texture.lifetime.alignment = alignment;
                }
                else
                {
                    if (usage.index >= graph.buffers.size()) continue;
                    auto& buffer = graph.buffers[usage.index];
                    if (buffer.imported) continue;

                    buffer.lifetime.isUsed = true;
                    buffer.lifetime.firstUsePass = std::min(buffer.lifetime.firstUsePass, order);
                    buffer.lifetime.lastUsePass = std::max(buffer.lifetime.lastUsePass, order);
                    auto [size, alignment] = getBufferMemReqs(buffer.desc);
                    buffer.lifetime.memorySize = size;
                    buffer.lifetime.alignment = alignment;
                }
            }
        }
    }

    // =============================================================================
    // Compute Memory Aliases using Interval Graph Coloring
    // =============================================================================
    void ComputeMemoryAliases(RenderGraphImpl& graph)
    {
        if (!graph.enableMemoryAliasing)
            return;

        // Collect all transient resources with their lifetimes
        struct ResourceInfo
        {
            ResourceType type;
            uint32 index;
            ResourceLifetime* lifetime;
            MemoryAlias* alias;
        };
        std::vector<ResourceInfo> resources;

        for (uint32 i = 0; i < graph.textures.size(); ++i)
        {
            auto& texture = graph.textures[i];
            if (!texture.imported && texture.lifetime.isUsed)
            {
                resources.push_back({ResourceType::Texture, i, &texture.lifetime, &texture.alias});
                graph.totalMemoryWithoutAliasing += texture.lifetime.memorySize;
                graph.stats.totalTransientTextures++;
            }
        }
        for (uint32 i = 0; i < graph.buffers.size(); ++i)
        {
            auto& buffer = graph.buffers[i];
            if (!buffer.imported && buffer.lifetime.isUsed)
            {
                resources.push_back({ResourceType::Buffer, i, &buffer.lifetime, &buffer.alias});
                graph.totalMemoryWithoutAliasing += buffer.lifetime.memorySize;
                graph.stats.totalTransientBuffers++;
            }
        }

        if (resources.empty())
            return;

        // Sort by first use pass (earliest first), then by memory size (largest first)
        std::sort(resources.begin(), resources.end(),
            [](const ResourceInfo& a, const ResourceInfo& b) {
                if (a.lifetime->firstUsePass != b.lifetime->firstUsePass)
                    return a.lifetime->firstUsePass < b.lifetime->firstUsePass;
                return a.lifetime->memorySize > b.lifetime->memorySize;
            });

        // Greedy interval coloring algorithm
        // Each "color" is a heap with a list of (offset, size, lastUsePass) allocations
        struct HeapAllocation
        {
            uint64 offset;
            uint64 size;
            uint32 firstUsePass;
            uint32 lastUsePass;
            ResourceType type;
            uint32 resourceIndex;
            MemoryAlias* alias;
        };
        struct Heap
        {
            uint64 totalSize = 0;
            std::vector<HeapAllocation> allocations;
        };
        std::vector<Heap> heaps;

        for (auto& res : resources)
        {
            uint64 requiredSize = res.lifetime->memorySize;
            uint64 alignment = res.lifetime->alignment;
            uint32 firstUse = res.lifetime->firstUsePass;
            uint32 lastUse = res.lifetime->lastUsePass;

            // Try to find an existing heap with a suitable gap
            int32 bestHeap = -1;
            uint64 bestOffset = 0;
            uint64 bestWaste = UINT64_MAX;

            for (size_t heapIdx = 0; heapIdx < heaps.size(); ++heapIdx)
            {
                auto& heap = heaps[heapIdx];

                // Check if we can reuse any freed allocation's space
                for (const auto& alloc : heap.allocations)
                {
                    // If this allocation's lifetime doesn't overlap with ours
                    if (alloc.lastUsePass < firstUse)
                    {
                        // Check if we can fit in this space
                        uint64 alignedOffset = (alloc.offset + alignment - 1) & ~(alignment - 1);
                        if (alignedOffset >= alloc.offset &&
                            alignedOffset + requiredSize <= alloc.offset + alloc.size)
                        {
                            const bool overlapsLiveAllocation =
                                std::any_of(
                                    heap.allocations.begin(),
                                    heap.allocations.end(),
                                    [alignedOffset,
                                     requiredSize,
                                     firstUse](const HeapAllocation& existing)
                                    {
                                        const bool rangesOverlap =
                                            alignedOffset < existing.offset + existing.size &&
                                            existing.offset < alignedOffset + requiredSize;
                                        return rangesOverlap &&
                                               existing.lastUsePass >= firstUse;
                                    });
                            if (overlapsLiveAllocation)
                            {
                                continue;
                            }

                            const uint64 usableSize =
                                alloc.offset + alloc.size - alignedOffset;
                            uint64 waste = usableSize - requiredSize;
                            if (waste < bestWaste)
                            {
                                bestHeap = static_cast<int32>(heapIdx);
                                bestOffset = alignedOffset;
                                bestWaste = waste;
                            }
                        }
                    }
                }

                // Note: Could also check appending at the end, but we prefer aliasing over just appending
            }

            // If no good fit found, try to find a heap where we can append without overlap
            if (bestHeap < 0)
            {
                for (size_t heapIdx = 0; heapIdx < heaps.size(); ++heapIdx)
                {
                    auto& heap = heaps[heapIdx];
                    
                    // Find the end offset of all overlapping allocations
                    uint64 maxEndOffset = 0;
                    for (const auto& alloc : heap.allocations)
                    {
                        if (alloc.lastUsePass >= firstUse)
                        {
                            // Lifetime overlaps, need to place after this allocation
                            maxEndOffset = std::max(maxEndOffset, alloc.offset + alloc.size);
                        }
                    }

                    // Try to fit after all overlapping allocations
                    uint64 alignedOffset = (maxEndOffset + alignment - 1) & ~(alignment - 1);
                    
                    // Accept this heap if the growth is reasonable
                    if (alignedOffset + requiredSize <= heap.totalSize * 2 + requiredSize)
                    {
                        bestHeap = static_cast<int32>(heapIdx);
                        bestOffset = alignedOffset;
                        break;
                    }
                }
            }

            // If still no heap found, create a new one
            if (bestHeap < 0)
            {
                bestHeap = static_cast<int32>(heaps.size());
                bestOffset = 0;
                heaps.push_back(Heap{});
            }

            // Assign the resource to this heap
            auto& heap = heaps[static_cast<size_t>(bestHeap)];
            const HeapAllocation* predecessor = nullptr;
            for (const HeapAllocation& allocation : heap.allocations)
            {
                const bool rangesOverlap =
                    bestOffset < allocation.offset + allocation.size &&
                    allocation.offset < bestOffset + requiredSize;
                if (rangesOverlap && allocation.lastUsePass < firstUse &&
                    (!predecessor ||
                     allocation.lastUsePass > predecessor->lastUsePass))
                {
                    predecessor = &allocation;
                }
            }

            if (predecessor)
            {
                res.alias->predecessorType = predecessor->type;
                res.alias->predecessorResourceIndex =
                    predecessor->resourceIndex;
            }

            heap.allocations.push_back({bestOffset,
                                        requiredSize,
                                        firstUse,
                                        lastUse,
                                        res.type,
                                        res.index,
                                        res.alias});
            heap.totalSize = std::max(heap.totalSize, bestOffset + requiredSize);

            res.alias->heapIndex = static_cast<uint32>(bestHeap);
            res.alias->heapOffset = bestOffset;
        }

        // A heap may contain many resources at disjoint offsets. Only resources
        // whose byte ranges actually overlap participate in aliasing and require
        // ownership barriers.
        for (const Heap& heap : heaps)
        {
            for (size_t first = 0; first < heap.allocations.size(); ++first)
            {
                for (size_t second = first + 1;
                     second < heap.allocations.size();
                     ++second)
                {
                    const HeapAllocation& a = heap.allocations[first];
                    const HeapAllocation& b = heap.allocations[second];
                    const bool rangesOverlap =
                        a.offset < b.offset + b.size &&
                        b.offset < a.offset + a.size;
                    if (rangesOverlap)
                    {
                        a.alias->isAliased = true;
                        b.alias->isAliased = true;
                    }
                }
            }
        }

        for (const ResourceInfo& resource : resources)
        {
            if (!resource.alias->isAliased)
            {
                continue;
            }
            if (resource.type == ResourceType::Texture)
            {
                ++graph.aliasedTextureCount;
            }
            else
            {
                ++graph.aliasedBufferCount;
            }
        }

        // Create transient heaps
        graph.transientHeaps.clear();
        graph.transientHeaps.reserve(heaps.size());
        for (const auto& heap : heaps)
        {
            TransientHeap th;
            th.size = heap.totalSize;
            th.resourceCount = static_cast<uint32>(heap.allocations.size());
            graph.transientHeaps.push_back(th);
            graph.totalMemoryWithAliasing += heap.totalSize;
        }

        // Update stats
        graph.stats.aliasedTextureCount = graph.aliasedTextureCount;
        graph.stats.aliasedBufferCount = graph.aliasedBufferCount;
        graph.stats.memoryWithoutAliasing = graph.totalMemoryWithoutAliasing;
        graph.stats.memoryWithAliasing = graph.totalMemoryWithAliasing;
        graph.stats.transientHeapCount = static_cast<uint32>(graph.transientHeaps.size());
    }

    // =============================================================================
    // Compute Aliasing Barriers
    // When multiple resources share the same heap memory, we need to ensure proper
    // synchronization when switching from one resource to another.
    // =============================================================================
    void ComputeAliasingBarriers(RenderGraphImpl& graph)
    {
        if (!graph.enableMemoryAliasing)
            return;

        const auto appendBarrier = [&graph](ResourceType afterType,
                                            uint32 afterResourceIndex,
                                            const ResourceLifetime& lifetime,
                                            const MemoryAlias& alias)
        {
            if (!alias.isAliased ||
                alias.predecessorResourceIndex == RVX_INVALID_INDEX ||
                lifetime.firstUsePass >= graph.executionOrder.size())
            {
                return;
            }

            const uint32 passIndex =
                graph.executionOrder[lifetime.firstUsePass];
            AliasingBarrier barrier;
            barrier.beforeType = alias.predecessorType;
            barrier.afterType = afterType;
            barrier.beforeResourceIndex = alias.predecessorResourceIndex;
            barrier.afterResourceIndex = afterResourceIndex;
            graph.passes[passIndex].aliasingBarriers.push_back(barrier);
        };

        for (Pass& pass : graph.passes)
        {
            pass.aliasingBarriers.clear();
        }
        for (uint32 index = 0; index < graph.textures.size(); ++index)
        {
            const TextureResource& texture = graph.textures[index];
            appendBarrier(ResourceType::Texture,
                          index,
                          texture.lifetime,
                          texture.alias);
        }
        for (uint32 index = 0; index < graph.buffers.size(); ++index)
        {
            const BufferResource& buffer = graph.buffers[index];
            appendBarrier(ResourceType::Buffer,
                          index,
                          buffer.lifetime,
                          buffer.alias);
        }
    }

    // =============================================================================
    // Compile Render Graph
    // =============================================================================
    void CompileRenderGraph(RenderGraphImpl& graph)
    {
        graph.executionRealized = false;
        graph.resourcesRealized = false;
        graph.compileDiagnostics.clear();
        graph.stats = {};
        graph.stats.totalPasses = static_cast<uint32>(graph.passes.size());
        graph.stats.compileValid = true;
        graph.stats.asyncComputeSupported = false;
        graph.stats.memoryAliasingEnabled = graph.enableMemoryAliasing;
        graph.stats.memoryAliasingUnsupportedRequested = graph.memoryAliasingRequested && !graph.enableMemoryAliasing;
        graph.stats.explicitAliasingBarriersSupported =
            graph.hasCapabilitySnapshot &&
            graph.capabilitySnapshot.supportsExplicitAliasingBarriers;
        graph.stats.compatibilityStateProjectionCount =
            graph.compatibilityStateProjectionCount;
        graph.passDependencies.clear();
        graph.passDependents.clear();
        graph.initialQueueReleaseBatches.clear();
        graph.totalMemoryWithoutAliasing = 0;
        graph.totalMemoryWithAliasing = 0;
        graph.aliasedTextureCount = 0;
        graph.aliasedBufferCount = 0;

        ValidatePassUsages(graph);
        if (graph.stats.validationErrorCount > 0)
        {
            graph.stats.compileValid = false;
            graph.executionOrder.clear();
            graph.passDependencies.clear();
            graph.passDependents.clear();
            return;
        }

        std::vector<std::vector<uint32>> textureWriters(graph.textures.size());
        std::vector<std::vector<uint32>> bufferWriters(graph.buffers.size());

        for (auto& pass : graph.passes)
        {
            pass.textureBarriers.clear();
            pass.bufferBarriers.clear();
            pass.postTextureBarriers.clear();
            pass.postBufferBarriers.clear();
            pass.readTextures.clear();
            pass.writeTextures.clear();
            pass.readBuffers.clear();
            pass.writeBuffers.clear();
            pass.culled = false;
        }

        for (uint32 passIndex = 0; passIndex < graph.passes.size(); ++passIndex)
        {
            auto& pass = graph.passes[passIndex];
            for (auto& usage : pass.usages)
            {
                usage.hasPlannedBeforeAccess = false;
                if (usage.type == ResourceType::Texture)
                {
                    if (usage.index >= graph.textures.size())
                        continue;

                    if (usage.access == RGAccessType::Read || usage.access == RGAccessType::ReadWrite)
                    {
                        if (std::find(pass.readTextures.begin(), pass.readTextures.end(), usage.index) == pass.readTextures.end())
                        {
                            pass.readTextures.push_back(usage.index);
                        }
                    }
                    if (usage.access == RGAccessType::Write || usage.access == RGAccessType::ReadWrite)
                    {
                        if (std::find(pass.writeTextures.begin(), pass.writeTextures.end(), usage.index) == pass.writeTextures.end())
                        {
                            pass.writeTextures.push_back(usage.index);
                            textureWriters[usage.index].push_back(passIndex);
                        }
                    }
                }
                else
                {
                    if (usage.index >= graph.buffers.size())
                        continue;

                    if (usage.access == RGAccessType::Read || usage.access == RGAccessType::ReadWrite)
                    {
                        if (std::find(pass.readBuffers.begin(), pass.readBuffers.end(), usage.index) == pass.readBuffers.end())
                        {
                            pass.readBuffers.push_back(usage.index);
                        }
                    }
                    if (usage.access == RGAccessType::Write || usage.access == RGAccessType::ReadWrite)
                    {
                        if (std::find(pass.writeBuffers.begin(), pass.writeBuffers.end(), usage.index) == pass.writeBuffers.end())
                        {
                            pass.writeBuffers.push_back(usage.index);
                            bufferWriters[usage.index].push_back(passIndex);
                        }
                    }
                }
            }
        }

        std::vector<uint8> passNeeded(graph.passes.size(), 0);
        std::vector<uint32> queue;
        queue.reserve(graph.passes.size());

        for (uint32 passIndex = 0; passIndex < graph.passes.size(); ++passIndex)
        {
            const auto& pass = graph.passes[passIndex];
            bool needed = false;
            for (uint32 texIndex : pass.writeTextures)
            {
                const auto& resource = graph.textures[texIndex];
                if (resource.exportState.has_value() || resource.imported)
                {
                    needed = true;
                    break;
                }
            }
            if (!needed)
            {
                for (uint32 bufIndex : pass.writeBuffers)
                {
                    const auto& resource = graph.buffers[bufIndex];
                    if (resource.exportState.has_value() || resource.imported)
                    {
                        needed = true;
                        break;
                    }
                }
            }
            if (needed)
            {
                passNeeded[passIndex] = 1;
                queue.push_back(passIndex);
            }
        }

        for (size_t i = 0; i < queue.size(); ++i)
        {
            uint32 passIndex = queue[i];
            const auto& pass = graph.passes[passIndex];
            for (uint32 texIndex : pass.readTextures)
            {
                for (uint32 writer : textureWriters[texIndex])
                {
                    if (!passNeeded[writer])
                    {
                        passNeeded[writer] = 1;
                        queue.push_back(writer);
                    }
                }
            }
            for (uint32 bufIndex : pass.readBuffers)
            {
                for (uint32 writer : bufferWriters[bufIndex])
                {
                    if (!passNeeded[writer])
                    {
                        passNeeded[writer] = 1;
                        queue.push_back(writer);
                    }
                }
            }
        }

        ValidateResourceLifetimes(graph, passNeeded);
        if (graph.stats.validationErrorCount > 0)
        {
            graph.stats.compileValid = false;
            graph.executionOrder.clear();
            graph.passDependencies.clear();
            graph.passDependents.clear();
            return;
        }

        std::vector<std::vector<uint32>> adjacency(graph.passes.size());
        std::vector<uint32> indegree(graph.passes.size(), 0);
        std::vector<int32> lastWriterTex(graph.textures.size(), -1);
        std::vector<int32> lastWriterBuf(graph.buffers.size(), -1);
        std::vector<std::vector<uint32>> lastReadersTex(graph.textures.size());
        std::vector<std::vector<uint32>> lastReadersBuf(graph.buffers.size());
        std::vector<int32> lastUserTex(graph.textures.size(), -1);
        std::vector<int32> lastUserBuf(graph.buffers.size(), -1);
        std::vector<GPUQueueDomain> lastUserDomainTex(
            graph.textures.size(), GPUQueueDomain::Graphics);
        std::vector<GPUQueueDomain> lastUserDomainBuf(
            graph.buffers.size(), GPUQueueDomain::Graphics);

        for (uint32 passIndex = 0; passIndex < graph.passes.size(); ++passIndex)
        {
            if (!passNeeded.empty() && passNeeded[passIndex] == 0)
                continue;
            const auto& pass = graph.passes[passIndex];

            // Exclusive queue ownership is itself an ordering dependency,
            // including read-to-read transfers that ordinary hazard analysis
            // would otherwise consider independent.
            for (const ResourceUsage& usage : pass.usages)
            {
                if (usage.type == ResourceType::Texture)
                {
                    const int32 previous = lastUserTex[usage.index];
                    if (previous >= 0 &&
                        previous != static_cast<int32>(passIndex) &&
                        lastUserDomainTex[usage.index] != usage.desiredAccess.domain)
                    {
                        AddDependencyEdge(adjacency,
                                          indegree,
                                          static_cast<uint32>(previous),
                                          passIndex);
                    }
                    lastUserTex[usage.index] = static_cast<int32>(passIndex);
                    lastUserDomainTex[usage.index] = usage.desiredAccess.domain;
                }
                else
                {
                    const int32 previous = lastUserBuf[usage.index];
                    if (previous >= 0 &&
                        previous != static_cast<int32>(passIndex) &&
                        lastUserDomainBuf[usage.index] != usage.desiredAccess.domain)
                    {
                        AddDependencyEdge(adjacency,
                                          indegree,
                                          static_cast<uint32>(previous),
                                          passIndex);
                    }
                    lastUserBuf[usage.index] = static_cast<int32>(passIndex);
                    lastUserDomainBuf[usage.index] = usage.desiredAccess.domain;
                }
            }

            for (uint32 texIndex : pass.readTextures)
            {
                int32 writer = lastWriterTex[texIndex];
                if (writer >= 0)
                {
                    AddDependencyEdge(adjacency, indegree, static_cast<uint32>(writer), passIndex);
                }

                if (!ContainsIndex(pass.writeTextures, texIndex) &&
                    !ContainsIndex(lastReadersTex[texIndex], passIndex))
                {
                    lastReadersTex[texIndex].push_back(passIndex);
                }
            }
            for (uint32 bufIndex : pass.readBuffers)
            {
                int32 writer = lastWriterBuf[bufIndex];
                if (writer >= 0)
                {
                    AddDependencyEdge(adjacency, indegree, static_cast<uint32>(writer), passIndex);
                }

                if (!ContainsIndex(pass.writeBuffers, bufIndex) &&
                    !ContainsIndex(lastReadersBuf[bufIndex], passIndex))
                {
                    lastReadersBuf[bufIndex].push_back(passIndex);
                }
            }

            for (uint32 texIndex : pass.writeTextures)
            {
                int32 writer = lastWriterTex[texIndex];
                if (writer >= 0)
                {
                    AddDependencyEdge(adjacency, indegree, static_cast<uint32>(writer), passIndex);
                }
                for (uint32 reader : lastReadersTex[texIndex])
                {
                    AddDependencyEdge(adjacency, indegree, reader, passIndex);
                }
                lastReadersTex[texIndex].clear();
                lastWriterTex[texIndex] = static_cast<int32>(passIndex);
            }
            for (uint32 bufIndex : pass.writeBuffers)
            {
                int32 writer = lastWriterBuf[bufIndex];
                if (writer >= 0)
                {
                    AddDependencyEdge(adjacency, indegree, static_cast<uint32>(writer), passIndex);
                }
                for (uint32 reader : lastReadersBuf[bufIndex])
                {
                    AddDependencyEdge(adjacency, indegree, reader, passIndex);
                }
                lastReadersBuf[bufIndex].clear();
                lastWriterBuf[bufIndex] = static_cast<int32>(passIndex);
            }
        }

        graph.passDependents = adjacency;
        graph.passDependencies.assign(graph.passes.size(), {});
        for (uint32 beforePass = 0; beforePass < graph.passDependents.size(); ++beforePass)
        {
            for (uint32 afterPass : graph.passDependents[beforePass])
            {
                if (afterPass < graph.passDependencies.size())
                {
                    graph.passDependencies[afterPass].push_back(beforePass);
                }
            }
        }

        graph.executionOrder.clear();
        graph.executionOrder.reserve(graph.passes.size());
        std::vector<uint32> topoQueue;
        topoQueue.reserve(graph.passes.size());
        for (uint32 passIndex = 0; passIndex < graph.passes.size(); ++passIndex)
        {
            if (!passNeeded.empty() && passNeeded[passIndex] == 0)
                continue;
            if (indegree[passIndex] == 0)
                topoQueue.push_back(passIndex);
        }

        for (size_t i = 0; i < topoQueue.size(); ++i)
        {
            uint32 passIndex = topoQueue[i];
            graph.executionOrder.push_back(passIndex);
            for (uint32 next : adjacency[passIndex])
            {
                if (--indegree[next] == 0)
                    topoQueue.push_back(next);
            }
        }

        if (graph.executionOrder.size() != queue.size())
        {
            graph.stats.compileValid = false;
            graph.stats.executionOrderFallbackUsed = false;
            graph.stats.validationErrorCount++;
            AddCompileError(graph, "RenderGraph compile failed: execution order topology is incomplete");
            graph.executionOrder.clear();
            graph.passDependencies.clear();
            graph.passDependents.clear();
            return;
        }

        // Mark culled passes first (needed for lifetime calculation)
        for (uint32 passIndex = 0; passIndex < graph.passes.size(); ++passIndex)
        {
            if (!passNeeded.empty() && passNeeded[passIndex] == 0)
            {
                graph.passes[passIndex].culled = true;
                graph.stats.culledPasses++;
            }
        }

        // Queue policy is finalized exactly once before barriers or physical
        // resources exist. A graph with no surviving non-Graphics work gains
        // no concurrency from a queue DAG and is compiled directly as the
        // canonical single Graphics batch.
        const bool hasNonGraphicsWork = std::any_of(
            graph.executionOrder.begin(),
            graph.executionOrder.end(),
            [&graph](uint32 passIndex)
            {
                return passIndex < graph.passes.size() &&
                       !graph.passes[passIndex].culled &&
                       graph.passes[passIndex].plannedExecutionQueue !=
                           RenderGraph::DiagnosticExecutionQueue::Graphics;
            });
        if (!hasNonGraphicsWork)
        {
            const auto textureOwnedByGraphics = [](
                const RHITextureAccessSnapshot& snapshot)
            {
                return snapshot.uniformAccess.domain ==
                           GPUQueueDomain::Graphics &&
                       std::all_of(
                           snapshot.subresourceOverrides.begin(),
                           snapshot.subresourceOverrides.end(),
                           [](const RHITextureSubresourceAccessSnapshot& entry)
                           {
                               return entry.access.domain ==
                                   GPUQueueDomain::Graphics;
                           });
            };
            const auto bufferOwnedByGraphics = [](
                const RHIBufferAccessSnapshot& snapshot)
            {
                return snapshot.uniformAccess.domain ==
                           GPUQueueDomain::Graphics &&
                       std::all_of(
                           snapshot.rangeOverrides.begin(),
                           snapshot.rangeOverrides.end(),
                           [](const RHIBufferRangeAccessSnapshot& entry)
                           {
                               return entry.access.domain ==
                                   GPUQueueDomain::Graphics;
                           });
            };
            bool illegalExternalOwnership = false;
            for (uint32 passIndex : graph.executionOrder)
            {
                if (passIndex >= graph.passes.size() ||
                    graph.passes[passIndex].culled)
                {
                    continue;
                }
                for (const ResourceUsage& usage :
                     graph.passes[passIndex].usages)
                {
                    if (usage.type == ResourceType::Texture &&
                        usage.index < graph.textures.size())
                    {
                        const TextureResource& resource =
                            graph.textures[usage.index];
                        illegalExternalOwnership =
                            illegalExternalOwnership ||
                            (resource.imported &&
                             !textureOwnedByGraphics(
                                 resource.initialAccessSnapshot));
                    }
                    else if (usage.type == ResourceType::Buffer &&
                             usage.index < graph.buffers.size())
                    {
                        const BufferResource& resource =
                            graph.buffers[usage.index];
                        illegalExternalOwnership =
                            illegalExternalOwnership ||
                            (resource.imported &&
                             !bufferOwnedByGraphics(
                                 resource.initialAccessSnapshot));
                    }
                }
            }
            if (illegalExternalOwnership)
            {
                graph.stats.compileValid = false;
                ++graph.stats.validationErrorCount;
                AddCompileError(
                    graph,
                    "RenderGraph GraphicsOnly plan cannot acquire an external resource owned by a non-Graphics queue");
                return;
            }
            graph.queueExecutionMode =
                RenderGraph::QueueExecutionMode::GraphicsOnly;
        }

        // Calculate resource lifetimes and memory aliases
        CalculateResourceLifetimes(graph);
        ComputeMemoryAliases(graph);

        for (auto& texture : graph.textures)
        {
            texture.currentState = texture.initialState;
            texture.currentAccessSnapshot = texture.initialAccessSnapshot;
            texture.subresourceStates.clear();
            texture.subresourceAccesses.clear();
            texture.hasSubresourceTracking = false;
        }
        for (auto& buffer : graph.buffers)
        {
            buffer.currentState = buffer.initialState;
            buffer.currentAccessSnapshot = buffer.initialAccessSnapshot;
            buffer.rangeStates.clear();
            buffer.hasRangeTracking = false;
        }

        // Compute aliasing barriers for resources that share memory
        ComputeAliasingBarriers(graph);

        const auto findTextureReleasePass =
            [&graph](uint32 targetPassIndex,
                     uint32 resourceIndex,
                     const RHISubresourceRange& range,
                     GPUQueueDomain sourceDomain)
        {
            uint32 releasePassIndex = RVX_INVALID_INDEX;
            for (uint32 candidatePassIndex : graph.executionOrder)
            {
                if (candidatePassIndex == targetPassIndex)
                    break;
                if (candidatePassIndex >= graph.passes.size())
                    continue;
                const Pass& candidate = graph.passes[candidatePassIndex];
                if (candidate.culled)
                    continue;
                for (const ResourceUsage& usage : candidate.usages)
                {
                    if (usage.type != ResourceType::Texture ||
                        usage.index != resourceIndex ||
                        usage.desiredAccess.domain != sourceDomain)
                    {
                        continue;
                    }
                    const RHISubresourceRange candidateRange =
                        usage.hasSubresourceRange
                            ? usage.subresourceRange
                            : AllSubresourcesForTexture(
                                  graph.textures[resourceIndex].desc);
                    if (TextureRangesOverlap(
                            candidateRange,
                            range,
                            graph.textures[resourceIndex]))
                    {
                        releasePassIndex = candidatePassIndex;
                    }
                }
            }
            return releasePassIndex;
        };

        const auto findBufferReleasePass =
            [&graph](uint32 targetPassIndex,
                     uint32 resourceIndex,
                     uint64 offset,
                     uint64 size,
                     GPUQueueDomain sourceDomain)
        {
            uint32 releasePassIndex = RVX_INVALID_INDEX;
            for (uint32 candidatePassIndex : graph.executionOrder)
            {
                if (candidatePassIndex == targetPassIndex)
                    break;
                if (candidatePassIndex >= graph.passes.size())
                    continue;
                const Pass& candidate = graph.passes[candidatePassIndex];
                if (candidate.culled)
                    continue;
                for (const ResourceUsage& usage : candidate.usages)
                {
                    if (usage.type != ResourceType::Buffer ||
                        usage.index != resourceIndex ||
                        usage.desiredAccess.domain != sourceDomain)
                    {
                        continue;
                    }
                    const uint64 candidateOffset = usage.hasRange
                        ? usage.offset
                        : 0;
                    const uint64 candidateSize = usage.hasRange
                        ? usage.size
                        : RVX_WHOLE_SIZE;
                    if (BufferRangesOverlap(
                            candidateOffset,
                            candidateSize,
                            offset,
                            size,
                            graph.buffers[resourceIndex].desc.size))
                    {
                        releasePassIndex = candidatePassIndex;
                    }
                }
            }
            return releasePassIndex;
        };

        std::vector<uint8> textureLeaseBeforeResolved(
            graph.textures.size(), 0);
        std::vector<uint8> bufferLeaseBeforeResolved(
            graph.buffers.size(), 0);

        const auto appendTextureBarrier =
            [&graph,
             &findTextureReleasePass,
             &textureLeaseBeforeResolved](uint32 targetPassIndex,
                                               uint32 resourceIndex,
                                               RHITextureBarrier barrier)
        {
            if (HasDependencyKind(
                    barrier.dependencyKind,
                    RHIDependencyKind::Ownership) &&
                barrier.accessBefore.domain != barrier.accessAfter.domain)
            {
                const uint32 releasePassIndex = findTextureReleasePass(
                    targetPassIndex,
                    resourceIndex,
                    barrier.subresourceRange,
                    barrier.accessBefore.domain);
                if (releasePassIndex != RVX_INVALID_INDEX)
                {
                    graph.passes[releasePassIndex]
                        .postTextureBarriers.push_back(
                            PlannedTextureBarrier{resourceIndex, barrier});
                }
                else
                {
                    const RenderGraph::DiagnosticExecutionQueue sourceQueue =
                        ToDiagnosticQueue(barrier.accessBefore.domain);
                    auto releaseBatch = std::find_if(
                        graph.initialQueueReleaseBatches.begin(),
                        graph.initialQueueReleaseBatches.end(),
                        [sourceQueue](const InitialQueueReleaseBatch& candidate)
                        {
                            return candidate.queue == sourceQueue;
                        });
                    if (releaseBatch == graph.initialQueueReleaseBatches.end())
                    {
                        InitialQueueReleaseBatch batch;
                        batch.queue = sourceQueue;
                        graph.initialQueueReleaseBatches.push_back(
                            std::move(batch));
                        releaseBatch = std::prev(
                            graph.initialQueueReleaseBatches.end());
                    }
                    if (std::find(releaseBatch->targetPassIndices.begin(),
                                  releaseBatch->targetPassIndices.end(),
                                  targetPassIndex) ==
                        releaseBatch->targetPassIndices.end())
                    {
                        releaseBatch->targetPassIndices.push_back(
                            targetPassIndex);
                    }
                    releaseBatch->textureBarriers.push_back(
                        PlannedTextureBarrier{resourceIndex, barrier});
                }
            }
            const bool resolveBeforeFromLease =
                resourceIndex < graph.textures.size() &&
                !graph.textures[resourceIndex].imported &&
                textureLeaseBeforeResolved[resourceIndex] == 0;
            if (resolveBeforeFromLease)
            {
                textureLeaseBeforeResolved[resourceIndex] = 1;
            }
            graph.passes[targetPassIndex].textureBarriers.push_back(
                PlannedTextureBarrier{
                    resourceIndex,
                    std::move(barrier),
                    resolveBeforeFromLease});
        };

        const auto appendBufferBarrier =
            [&graph,
             &findBufferReleasePass,
             &bufferLeaseBeforeResolved](uint32 targetPassIndex,
                                             uint32 resourceIndex,
                                             RHIBufferBarrier barrier)
        {
            if (HasDependencyKind(
                    barrier.dependencyKind,
                    RHIDependencyKind::Ownership) &&
                barrier.accessBefore.domain != barrier.accessAfter.domain)
            {
                const uint32 releasePassIndex = findBufferReleasePass(
                    targetPassIndex,
                    resourceIndex,
                    barrier.offset,
                    barrier.size,
                    barrier.accessBefore.domain);
                if (releasePassIndex != RVX_INVALID_INDEX)
                {
                    graph.passes[releasePassIndex]
                        .postBufferBarriers.push_back(
                            PlannedBufferBarrier{resourceIndex, barrier});
                }
                else
                {
                    const RenderGraph::DiagnosticExecutionQueue sourceQueue =
                        ToDiagnosticQueue(barrier.accessBefore.domain);
                    auto releaseBatch = std::find_if(
                        graph.initialQueueReleaseBatches.begin(),
                        graph.initialQueueReleaseBatches.end(),
                        [sourceQueue](const InitialQueueReleaseBatch& candidate)
                        {
                            return candidate.queue == sourceQueue;
                        });
                    if (releaseBatch == graph.initialQueueReleaseBatches.end())
                    {
                        InitialQueueReleaseBatch batch;
                        batch.queue = sourceQueue;
                        graph.initialQueueReleaseBatches.push_back(
                            std::move(batch));
                        releaseBatch = std::prev(
                            graph.initialQueueReleaseBatches.end());
                    }
                    if (std::find(releaseBatch->targetPassIndices.begin(),
                                  releaseBatch->targetPassIndices.end(),
                                  targetPassIndex) ==
                        releaseBatch->targetPassIndices.end())
                    {
                        releaseBatch->targetPassIndices.push_back(
                            targetPassIndex);
                    }
                    releaseBatch->bufferBarriers.push_back(
                        PlannedBufferBarrier{resourceIndex, barrier});
                }
            }
            const bool resolveBeforeFromLease =
                resourceIndex < graph.buffers.size() &&
                !graph.buffers[resourceIndex].imported &&
                bufferLeaseBeforeResolved[resourceIndex] == 0;
            if (resolveBeforeFromLease)
            {
                bufferLeaseBeforeResolved[resourceIndex] = 1;
            }
            graph.passes[targetPassIndex].bufferBarriers.push_back(
                PlannedBufferBarrier{
                    resourceIndex,
                    std::move(barrier),
                    resolveBeforeFromLease});
        };

        auto generatePassBarriers = [&](uint32 passIndex, Pass& pass)
        {
            if (pass.culled)
                return;

            for (auto& usage : pass.usages)
            {
                RVX_ASSERT_MSG(
                    IsStateAllowedForPass(pass.type, usage.desiredState),
                    "RenderGraph pass '{}' uses resource state '{}' not allowed for this queue type",
                    pass.name.c_str(),
                    static_cast<uint32>(usage.desiredState));

                if (usage.type == ResourceType::Texture)
                {
                    auto& resource = graph.textures[usage.index];

                    EnsureTextureSubresourceTracking(resource);

                    const RHIAccessSnapshot desiredAccess = usage.desiredAccess;

                    RHISubresourceRange range = usage.hasSubresourceRange
                                                    ? usage.subresourceRange
                                                    : AllSubresourcesForTexture(resource.desc);
                    bool rangeIsAll = IsAllSubresourceRange(range);

                    if (resource.hasSubresourceTracking || !rangeIsAll)
                    {
                        resource.hasSubresourceTracking = true;
                        uint32 baseMip = 0;
                        uint32 mipCount = 0;
                        uint32 baseLayer = 0;
                        uint32 layerCount = 0;
                        ResolveSubresourceRange(range, resource, baseMip, mipCount, baseLayer, layerCount);

                        for (uint32 mip = baseMip; mip < baseMip + mipCount; ++mip)
                        {
                            for (uint32 layer = baseLayer; layer < baseLayer + layerCount; ++layer)
                            {
                                uint32 key = mip + layer * resource.desc.mipLevels;
                                auto accessIt = resource.subresourceAccesses.find(key);
                                const RHIAccessSnapshot currentAccess =
                                    accessIt != resource.subresourceAccesses.end()
                                        ? accessIt->second
                                        : resource.currentAccessSnapshot.uniformAccess;
                                const RHIDiscardIntent effectiveDiscard =
                                    usage.access == RGAccessType::Write &&
                                            currentAccess.contentValidity != RHIContentValidity::Valid
                                        ? RHIDiscardIntent::Discard
                                        : usage.discardIntent;
                                if (ClassifyRHIDependency(
                                        currentAccess,
                                        desiredAccess,
                                        effectiveDiscard) != RHIDependencyKind::None)
                                {
                                    RHIAccessSnapshot barrierBefore = currentAccess;
                                    if (barrierBefore.contentValidity !=
                                            RHIContentValidity::Valid &&
                                        effectiveDiscard == RHIDiscardIntent::Discard)
                                    {
                                        barrierBefore.domain = desiredAccess.domain;
                                    }
                                    appendTextureBarrier(
                                        passIndex,
                                        usage.index,
                                        MakeRHITextureBarrier(
                                            nullptr,
                                            barrierBefore,
                                            desiredAccess,
                                            RHISubresourceRange{mip, 1, layer, 1, range.aspect},
                                            effectiveDiscard));
                                }
                                resource.subresourceAccesses[key] = desiredAccess;
                                resource.subresourceStates[key] =
                                    ProjectRHIResourceState(desiredAccess);
                            }
                        }

                        if (rangeIsAll)
                        {
                            resource.currentAccessSnapshot.uniformAccess = desiredAccess;
                            resource.currentAccessSnapshot.subresourceOverrides.clear();
                            resource.currentState = ProjectRHIResourceState(desiredAccess);
                            resource.subresourceStates.clear();
                            resource.subresourceAccesses.clear();
                            resource.hasSubresourceTracking = false;
                        }
                    }
                    else
                    {
                        const RHIAccessSnapshot currentAccess =
                            resource.currentAccessSnapshot.uniformAccess;
                        usage.plannedBeforeAccess = currentAccess;
                        usage.hasPlannedBeforeAccess = true;
                        const RHIDiscardIntent effectiveDiscard =
                            usage.access == RGAccessType::Write &&
                                    currentAccess.contentValidity != RHIContentValidity::Valid
                                ? RHIDiscardIntent::Discard
                                : usage.discardIntent;
                        if (ClassifyRHIDependency(
                                currentAccess,
                                desiredAccess,
                                effectiveDiscard) != RHIDependencyKind::None)
                        {
                            RHIAccessSnapshot barrierBefore = currentAccess;
                            if (barrierBefore.contentValidity !=
                                    RHIContentValidity::Valid &&
                                effectiveDiscard == RHIDiscardIntent::Discard)
                            {
                                barrierBefore.domain = desiredAccess.domain;
                            }
                            appendTextureBarrier(
                                passIndex,
                                usage.index,
                                MakeRHITextureBarrier(
                                    nullptr,
                                    barrierBefore,
                                    desiredAccess,
                                    AllSubresourcesForTexture(resource.desc),
                                    effectiveDiscard));
                        }
                        resource.currentAccessSnapshot.uniformAccess = desiredAccess;
                        resource.currentState = ProjectRHIResourceState(desiredAccess);
                    }
                }
                else
                {
                    auto& resource = graph.buffers[usage.index];

                    const RHIAccessSnapshot desiredAccess = usage.desiredAccess;

                    uint64 offset = usage.hasRange ? usage.offset : 0;
                    uint64 size = usage.hasRange ? usage.size : RVX_WHOLE_SIZE;
                    uint64 rangeSize = ResolveBufferRangeSize(offset, size, resource.desc.size);
                    bool isWhole = IsWholeBufferRange(offset, size, resource.desc.size);

                    if (resource.hasRangeTracking || (usage.hasRange && !isWhole))
                    {
                        uint64 applySize = isWhole ? resource.desc.size : rangeSize;
                        EnsureBufferRangeTracking(resource);
                        std::vector<RHIBufferBarrier> generatedBarriers;
                        ApplyBufferRangeTransition(
                            resource,
                            offset,
                            applySize,
                            desiredAccess,
                            usage.discardIntent,
                            usage.access == RGAccessType::Write,
                            generatedBarriers);
                        for (RHIBufferBarrier& barrier : generatedBarriers)
                        {
                            if (barrier.accessBefore.contentValidity !=
                                    RHIContentValidity::Valid &&
                                barrier.discardIntent == RHIDiscardIntent::Discard)
                            {
                                barrier.accessBefore.domain =
                                    barrier.accessAfter.domain;
                                barrier.dependencyKind = ClassifyRHIDependency(
                                    barrier.accessBefore,
                                    barrier.accessAfter,
                                    barrier.discardIntent);
                            }
                            appendBufferBarrier(
                                passIndex, usage.index, std::move(barrier));
                        }
                        if (isWhole)
                        {
                            resource.currentAccessSnapshot.uniformAccess = desiredAccess;
                            resource.currentAccessSnapshot.rangeOverrides.clear();
                            resource.currentState = ProjectRHIResourceState(desiredAccess);
                            resource.rangeStates.clear();
                            resource.hasRangeTracking = false;
                        }
                    }
                    else
                    {
                        const RHIAccessSnapshot currentAccess =
                            resource.currentAccessSnapshot.uniformAccess;
                        usage.plannedBeforeAccess = currentAccess;
                        usage.hasPlannedBeforeAccess = true;
                        const RHIDiscardIntent effectiveDiscard =
                            usage.access == RGAccessType::Write &&
                                    currentAccess.contentValidity != RHIContentValidity::Valid
                                ? RHIDiscardIntent::Discard
                                : usage.discardIntent;
                        if (ClassifyRHIDependency(
                                currentAccess,
                                desiredAccess,
                                effectiveDiscard) != RHIDependencyKind::None)
                        {
                            RHIAccessSnapshot barrierBefore = currentAccess;
                            if (barrierBefore.contentValidity !=
                                    RHIContentValidity::Valid &&
                                effectiveDiscard == RHIDiscardIntent::Discard)
                            {
                                barrierBefore.domain = desiredAccess.domain;
                            }
                            appendBufferBarrier(
                                passIndex,
                                usage.index,
                                MakeRHIBufferBarrier(
                                    nullptr,
                                    barrierBefore,
                                    desiredAccess,
                                    offset,
                                    isWhole ? RVX_WHOLE_SIZE : rangeSize,
                                    effectiveDiscard));
                        }
                        resource.currentAccessSnapshot.uniformAccess = desiredAccess;
                        resource.currentState = ProjectRHIResourceState(desiredAccess);
                    }
                }
            }

        };

        if (!graph.executionOrder.empty())
        {
            for (uint32 passIndex : graph.executionOrder)
            {
                if (passIndex < graph.passes.size())
                {
                    generatePassBarriers(passIndex, graph.passes[passIndex]);
                }
            }
        }
        else
        {
            for (uint32 passIndex = 0;
                 passIndex < static_cast<uint32>(graph.passes.size());
                 ++passIndex)
            {
                generatePassBarriers(passIndex, graph.passes[passIndex]);
            }
        }

        // Export transitions execute on the synthetic terminal Graphics batch.
        // Publish the release half on the last in-graph owner now; the executor
        // records the matching acquire half on the terminal context.
        for (uint32 resourceIndex = 0;
             resourceIndex < static_cast<uint32>(graph.textures.size());
             ++resourceIndex)
        {
            TextureResource& resource = graph.textures[resourceIndex];
            if (!resource.exportAccess)
                continue;
            RHIAccessSnapshot desired = *resource.exportAccess;
            const auto appendRelease = [&](const RHIAccessSnapshot& current,
                                           const RHISubresourceRange& range)
            {
                desired.contentValidity = current.contentValidity;
                RHITextureBarrier barrier = MakeRHITextureBarrier(
                    nullptr, current, desired, range);
                if (!HasDependencyKind(
                        barrier.dependencyKind,
                        RHIDependencyKind::Ownership) ||
                    current.domain == desired.domain)
                {
                    return;
                }
                const uint32 releasePassIndex = findTextureReleasePass(
                    RVX_INVALID_INDEX,
                    resourceIndex,
                    range,
                    current.domain);
                if (releasePassIndex != RVX_INVALID_INDEX)
                {
                    graph.passes[releasePassIndex]
                        .postTextureBarriers.push_back(
                            PlannedTextureBarrier{
                                resourceIndex, std::move(barrier)});
                }
                else
                {
                    const RenderGraph::DiagnosticExecutionQueue sourceQueue =
                        ToDiagnosticQueue(current.domain);
                    auto releaseBatch = std::find_if(
                        graph.initialQueueReleaseBatches.begin(),
                        graph.initialQueueReleaseBatches.end(),
                        [sourceQueue](const InitialQueueReleaseBatch& candidate)
                        {
                            return candidate.queue == sourceQueue;
                        });
                    if (releaseBatch == graph.initialQueueReleaseBatches.end())
                    {
                        InitialQueueReleaseBatch batch;
                        batch.queue = sourceQueue;
                        graph.initialQueueReleaseBatches.push_back(
                            std::move(batch));
                        releaseBatch = std::prev(
                            graph.initialQueueReleaseBatches.end());
                    }
                    releaseBatch->targetsTerminal = true;
                    releaseBatch->textureBarriers.push_back(
                        PlannedTextureBarrier{
                            resourceIndex, std::move(barrier)});
                }
            };
            if (resource.hasSubresourceTracking)
            {
                for (uint32 mip = 0; mip < resource.desc.mipLevels; ++mip)
                {
                    for (uint32 layer = 0;
                         layer < GetTexturePhysicalLayerCount(resource.desc);
                         ++layer)
                    {
                        const uint32 key = mip + layer * resource.desc.mipLevels;
                        const auto it = resource.subresourceAccesses.find(key);
                        const RHIAccessSnapshot& current =
                            it != resource.subresourceAccesses.end()
                                ? it->second
                                : resource.currentAccessSnapshot.uniformAccess;
                        appendRelease(
                            current,
                            RHISubresourceRange{
                                mip,
                                1,
                                layer,
                                1,
                                GetDefaultTextureAspect(resource.desc)});
                    }
                }
            }
            else
            {
                appendRelease(
                    resource.currentAccessSnapshot.uniformAccess,
                    AllSubresourcesForTexture(resource.desc));
            }
        }

        for (uint32 resourceIndex = 0;
             resourceIndex < static_cast<uint32>(graph.buffers.size());
             ++resourceIndex)
        {
            BufferResource& resource = graph.buffers[resourceIndex];
            if (!resource.exportAccess)
                continue;
            RHIAccessSnapshot desired = *resource.exportAccess;
            const auto appendRelease = [&](const RHIAccessSnapshot& current,
                                           uint64 offset,
                                           uint64 size)
            {
                desired.contentValidity = current.contentValidity;
                RHIBufferBarrier barrier = MakeRHIBufferBarrier(
                    nullptr, current, desired, offset, size);
                if (!HasDependencyKind(
                        barrier.dependencyKind,
                        RHIDependencyKind::Ownership) ||
                    current.domain == desired.domain)
                {
                    return;
                }
                const uint32 releasePassIndex = findBufferReleasePass(
                    RVX_INVALID_INDEX,
                    resourceIndex,
                    offset,
                    size,
                    current.domain);
                if (releasePassIndex != RVX_INVALID_INDEX)
                {
                    graph.passes[releasePassIndex]
                        .postBufferBarriers.push_back(
                            PlannedBufferBarrier{
                                resourceIndex, std::move(barrier)});
                }
                else
                {
                    const RenderGraph::DiagnosticExecutionQueue sourceQueue =
                        ToDiagnosticQueue(current.domain);
                    auto releaseBatch = std::find_if(
                        graph.initialQueueReleaseBatches.begin(),
                        graph.initialQueueReleaseBatches.end(),
                        [sourceQueue](const InitialQueueReleaseBatch& candidate)
                        {
                            return candidate.queue == sourceQueue;
                        });
                    if (releaseBatch == graph.initialQueueReleaseBatches.end())
                    {
                        InitialQueueReleaseBatch batch;
                        batch.queue = sourceQueue;
                        graph.initialQueueReleaseBatches.push_back(
                            std::move(batch));
                        releaseBatch = std::prev(
                            graph.initialQueueReleaseBatches.end());
                    }
                    releaseBatch->targetsTerminal = true;
                    releaseBatch->bufferBarriers.push_back(
                        PlannedBufferBarrier{
                            resourceIndex, std::move(barrier)});
                }
            };
            if (resource.hasRangeTracking)
            {
                for (const BufferResource::RangeState& range :
                     resource.rangeStates)
                {
                    appendRelease(
                        range.access, range.offset, range.size);
                }
            }
            else
            {
                appendRelease(
                    resource.currentAccessSnapshot.uniformAccess,
                    0,
                    RVX_WHOLE_SIZE);
            }
        }

        for (Pass& pass : graph.passes)
        {
            const uint32 mergedTexture =
                MergeTextureBarriers(pass.textureBarriers) +
                MergeTextureBarriers(pass.postTextureBarriers);
            const uint32 mergedBuffer =
                MergeBufferBarriers(pass.bufferBarriers) +
                MergeBufferBarriers(pass.postBufferBarriers);
            graph.stats.mergedTextureBarrierCount += mergedTexture;
            graph.stats.mergedBufferBarrierCount += mergedBuffer;
            graph.stats.mergedBarrierCount += mergedTexture + mergedBuffer;
        }

        std::unordered_map<uint32, RHIResourceState> lastTextureState;
        std::unordered_map<uint32, RHIResourceState> lastBufferState;
        auto applyCrossPassBarrierFiltering = [&](Pass& pass)
        {
            if (pass.culled)
                return;

            graph.stats.crossPassMergedBarrierCount += RemoveRedundantTextureBarriers(lastTextureState, pass.textureBarriers);
            graph.stats.crossPassMergedBarrierCount += RemoveRedundantBufferBarriers(lastBufferState, pass.bufferBarriers);

            for (const auto& barrier : pass.textureBarriers)
            {
                if (IsAllRange(barrier.barrier.subresourceRange))
                {
                    lastTextureState[barrier.resourceIndex] =
                        barrier.barrier.stateAfter;
                }
            }
            for (const auto& barrier : pass.bufferBarriers)
            {
                if (IsAllBufferRange(barrier.barrier))
                {
                    lastBufferState[barrier.resourceIndex] =
                        barrier.barrier.stateAfter;
                }
            }
        };

        if (!graph.executionOrder.empty())
        {
            for (uint32 passIndex : graph.executionOrder)
            {
                if (passIndex < graph.passes.size())
                {
                    applyCrossPassBarrierFiltering(graph.passes[passIndex]);
                }
            }
        }
        else
        {
            for (auto& pass : graph.passes)
            {
                applyCrossPassBarrierFiltering(pass);
            }
        }

        graph.stats.textureBarrierCount = 0;
        graph.stats.bufferBarrierCount = 0;
        for (const auto& pass : graph.passes)
        {
            if (pass.culled)
                continue;
            graph.stats.textureBarrierCount += static_cast<uint32>(pass.textureBarriers.size());
            graph.stats.textureBarrierCount += static_cast<uint32>(pass.postTextureBarriers.size());
            graph.stats.bufferBarrierCount += static_cast<uint32>(pass.bufferBarriers.size());
            graph.stats.bufferBarrierCount += static_cast<uint32>(pass.postBufferBarriers.size());
        }
        graph.stats.barrierCount = graph.stats.textureBarrierCount + graph.stats.bufferBarrierCount;
        graph.stats.planHash = ComputeCompiledPlanHash(graph);
    }

} // namespace RVX
