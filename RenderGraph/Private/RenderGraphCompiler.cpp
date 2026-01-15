#include "RenderGraphInternal.h"
#include "Core/Assert.h"
#include <algorithm>

namespace RVX
{
    namespace
    {
        bool IsAllSubresourceRange(const RHISubresourceRange& range)
        {
            return range.baseMipLevel == 0 &&
                   range.mipLevelCount == RVX_ALL_MIPS &&
                   range.baseArrayLayer == 0 &&
                   range.arrayLayerCount == RVX_ALL_LAYERS;
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
            layerCount = (range.arrayLayerCount == RVX_ALL_LAYERS) ? resource.desc.arraySize : range.arrayLayerCount;
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

        void EnsureBufferRangeTracking(BufferResource& resource)
        {
            if (resource.hasRangeTracking)
                return;
            resource.rangeStates.clear();
            resource.rangeStates.push_back({0, resource.desc.size, resource.currentState});
            resource.hasRangeTracking = true;
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
                if (back.state == current.state && back.offset + back.size == current.offset)
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
            RHIResourceState desiredState,
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
                    updated.push_back({rangeStart, offset - rangeStart, range.state});
                }

                uint64 overlapStart = std::max(offset, rangeStart);
                uint64 overlapEnd = std::min(end, rangeEnd);
                uint64 overlapSize = overlapEnd - overlapStart;
                if (range.state != desiredState)
                {
                    outBarriers.push_back({resource.buffer.Get(), range.state, desiredState, overlapStart, overlapSize});
                }
                updated.push_back({overlapStart, overlapSize, desiredState});

                if (overlapEnd < rangeEnd)
                {
                    updated.push_back({overlapEnd, rangeEnd - overlapEnd, range.state});
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

            if (passType == RenderGraphPassType::Compute)
            {
                return state == RHIResourceState::ShaderResource ||
                       state == RHIResourceState::UnorderedAccess ||
                       state == RHIResourceState::ConstantBuffer ||
                       state == RHIResourceState::IndirectArgument ||
                       state == RHIResourceState::CopySource ||
                       state == RHIResourceState::CopyDest ||
                       state == RHIResourceState::Common ||
                       state == RHIResourceState::Undefined;
            }

            return true;
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

        uint32 MergeTextureBarriers(std::vector<RHITextureBarrier>& barriers)
        {
            if (barriers.empty())
                return 0;

            auto beforeCount = static_cast<uint32>(barriers.size());
            std::sort(
                barriers.begin(),
                barriers.end(),
                [](const RHITextureBarrier& a, const RHITextureBarrier& b)
                {
                    if (a.texture != b.texture)
                        return a.texture < b.texture;
                    if (a.stateBefore != b.stateBefore)
                        return a.stateBefore < b.stateBefore;
                    if (a.stateAfter != b.stateAfter)
                        return a.stateAfter < b.stateAfter;
                    if (a.subresourceRange.aspect != b.subresourceRange.aspect)
                        return a.subresourceRange.aspect < b.subresourceRange.aspect;
                    if (a.subresourceRange.baseArrayLayer != b.subresourceRange.baseArrayLayer)
                        return a.subresourceRange.baseArrayLayer < b.subresourceRange.baseArrayLayer;
                    if (a.subresourceRange.arrayLayerCount != b.subresourceRange.arrayLayerCount)
                        return a.subresourceRange.arrayLayerCount < b.subresourceRange.arrayLayerCount;
                    return a.subresourceRange.baseMipLevel < b.subresourceRange.baseMipLevel;
                });

            std::vector<RHITextureBarrier> merged;
            merged.reserve(barriers.size());

            for (const auto& barrier : barriers)
            {
                if (merged.empty())
                {
                    merged.push_back(barrier);
                    continue;
                }

                auto& last = merged.back();
                if (last.texture == barrier.texture &&
                    last.stateBefore == barrier.stateBefore &&
                    last.stateAfter == barrier.stateAfter &&
                    last.subresourceRange.aspect == barrier.subresourceRange.aspect)
                {
                    if (IsAllRange(last.subresourceRange))
                        continue;

                    if (IsAllRange(barrier.subresourceRange))
                    {
                        last.subresourceRange = RHISubresourceRange::All();
                        continue;
                    }

                    bool sameLayerRange = last.subresourceRange.baseArrayLayer == barrier.subresourceRange.baseArrayLayer &&
                                          last.subresourceRange.arrayLayerCount == barrier.subresourceRange.arrayLayerCount;
                    bool sameMipRange = last.subresourceRange.baseMipLevel == barrier.subresourceRange.baseMipLevel &&
                                        last.subresourceRange.mipLevelCount == barrier.subresourceRange.mipLevelCount;
                    bool adjacentMip = sameLayerRange &&
                                       last.subresourceRange.baseMipLevel + last.subresourceRange.mipLevelCount ==
                                           barrier.subresourceRange.baseMipLevel;
                    bool adjacentLayer = sameMipRange &&
                                         last.subresourceRange.baseArrayLayer + last.subresourceRange.arrayLayerCount ==
                                             barrier.subresourceRange.baseArrayLayer;
                    if (adjacentMip)
                    {
                        last.subresourceRange.mipLevelCount += barrier.subresourceRange.mipLevelCount;
                        continue;
                    }

                    if (adjacentLayer)
                    {
                        last.subresourceRange.arrayLayerCount += barrier.subresourceRange.arrayLayerCount;
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

        uint32 MergeBufferBarriers(std::vector<RHIBufferBarrier>& barriers)
        {
            if (barriers.empty())
                return 0;

            auto beforeCount = static_cast<uint32>(barriers.size());
            std::sort(
                barriers.begin(),
                barriers.end(),
                [](const RHIBufferBarrier& a, const RHIBufferBarrier& b)
                {
                    if (a.buffer != b.buffer)
                        return a.buffer < b.buffer;
                    if (a.stateBefore != b.stateBefore)
                        return a.stateBefore < b.stateBefore;
                    if (a.stateAfter != b.stateAfter)
                        return a.stateAfter < b.stateAfter;
                    return a.offset < b.offset;
                });

            std::vector<RHIBufferBarrier> merged;
            merged.reserve(barriers.size());

            for (const auto& barrier : barriers)
            {
                if (merged.empty())
                {
                    merged.push_back(barrier);
                    continue;
                }

                auto& last = merged.back();
                if (last.buffer == barrier.buffer &&
                    last.stateBefore == barrier.stateBefore &&
                    last.stateAfter == barrier.stateAfter)
                {
                    if (IsAllBufferRange(last))
                        continue;

                    if (IsAllBufferRange(barrier))
                    {
                        last.offset = 0;
                        last.size = RVX_WHOLE_SIZE;
                        continue;
                    }

                    bool adjacent = last.offset + last.size == barrier.offset;
                    bool sameRange = last.offset == barrier.offset && last.size == barrier.size;
                    if (adjacent)
                    {
                        last.size += barrier.size;
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
            const std::unordered_map<RHITexture*, RHIResourceState>& prevStates,
            std::vector<RHITextureBarrier>& barriers)
        {
            if (barriers.empty() || prevStates.empty())
                return 0;

            auto beforeCount = static_cast<uint32>(barriers.size());
            std::vector<RHITextureBarrier> filtered;
            filtered.reserve(barriers.size());

            for (const auto& barrier : barriers)
            {
                auto it = prevStates.find(barrier.texture);
                if (it == prevStates.end())
                {
                    filtered.push_back(barrier);
                    continue;
                }

                if (barrier.stateBefore == it->second)
                {
                    filtered.push_back(barrier);
                }
            }

            barriers.swap(filtered);
            return beforeCount - static_cast<uint32>(barriers.size());
        }

        uint32 RemoveRedundantBufferBarriers(
            const std::unordered_map<RHIBuffer*, RHIResourceState>& prevStates,
            std::vector<RHIBufferBarrier>& barriers)
        {
            if (barriers.empty() || prevStates.empty())
                return 0;

            auto beforeCount = static_cast<uint32>(barriers.size());
            std::vector<RHIBufferBarrier> filtered;
            filtered.reserve(barriers.size());

            for (const auto& barrier : barriers)
            {
                auto it = prevStates.find(barrier.buffer);
                if (it == prevStates.end())
                {
                    filtered.push_back(barrier);
                    continue;
                }

                if (barrier.stateBefore == it->second)
                {
                    filtered.push_back(barrier);
                }
            }

            barriers.swap(filtered);
            return beforeCount - static_cast<uint32>(barriers.size());
        }
    }

    void CompileRenderGraph(RenderGraphImpl& graph)
    {
        graph.stats = {};
        graph.stats.totalPasses = static_cast<uint32>(graph.passes.size());
        if (graph.device)
        {
            for (auto& texture : graph.textures)
            {
                if (!texture.imported && !texture.texture)
                {
                    texture.texture = graph.device->CreateTexture(texture.desc);
                    texture.initialState = RHIResourceState::Undefined;
                    texture.currentState = texture.initialState;
                }
            }

            for (auto& buffer : graph.buffers)
            {
                if (!buffer.imported && !buffer.buffer)
                {
                    buffer.buffer = graph.device->CreateBuffer(buffer.desc);
                    buffer.initialState = RHIResourceState::Undefined;
                    buffer.currentState = buffer.initialState;
                }
            }
        }

        std::vector<std::vector<uint32>> textureWriters(graph.textures.size());
        std::vector<std::vector<uint32>> bufferWriters(graph.buffers.size());

        for (auto& pass : graph.passes)
        {
            pass.textureBarriers.clear();
            pass.bufferBarriers.clear();
            pass.readTextures.clear();
            pass.writeTextures.clear();
            pass.readBuffers.clear();
            pass.writeBuffers.clear();
            pass.culled = false;
        }

        for (uint32 passIndex = 0; passIndex < graph.passes.size(); ++passIndex)
        {
            auto& pass = graph.passes[passIndex];
            for (const auto& usage : pass.usages)
            {
                if (usage.type == ResourceType::Texture)
                {
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

        std::vector<std::vector<uint32>> adjacency(graph.passes.size());
        std::vector<uint32> indegree(graph.passes.size(), 0);
        std::vector<int32> lastWriterTex(graph.textures.size(), -1);
        std::vector<int32> lastWriterBuf(graph.buffers.size(), -1);

        for (uint32 passIndex = 0; passIndex < graph.passes.size(); ++passIndex)
        {
            if (!passNeeded.empty() && passNeeded[passIndex] == 0)
                continue;
            const auto& pass = graph.passes[passIndex];

            for (uint32 texIndex : pass.readTextures)
            {
                int32 writer = lastWriterTex[texIndex];
                if (writer >= 0 && static_cast<uint32>(writer) != passIndex)
                {
                    adjacency[static_cast<uint32>(writer)].push_back(passIndex);
                    indegree[passIndex]++;
                }
            }
            for (uint32 bufIndex : pass.readBuffers)
            {
                int32 writer = lastWriterBuf[bufIndex];
                if (writer >= 0 && static_cast<uint32>(writer) != passIndex)
                {
                    adjacency[static_cast<uint32>(writer)].push_back(passIndex);
                    indegree[passIndex]++;
                }
            }

            for (uint32 texIndex : pass.writeTextures)
                lastWriterTex[texIndex] = static_cast<int32>(passIndex);
            for (uint32 bufIndex : pass.writeBuffers)
                lastWriterBuf[bufIndex] = static_cast<int32>(passIndex);
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
            graph.executionOrder.clear();
            for (uint32 passIndex = 0; passIndex < graph.passes.size(); ++passIndex)
            {
                if (!passNeeded.empty() && passNeeded[passIndex] == 0)
                    continue;
                graph.executionOrder.push_back(passIndex);
            }
        }

        for (auto& pass : graph.passes)
        {
            uint32 passIndex = static_cast<uint32>(&pass - graph.passes.data());
            if (!passNeeded.empty() && passNeeded[passIndex] == 0)
            {
                pass.culled = true;
                graph.stats.culledPasses++;
                continue;
            }

            for (const auto& usage : pass.usages)
            {
                RVX_ASSERT_MSG(
                    IsStateAllowedForPass(pass.type, usage.desiredState),
                    "RenderGraph pass '{}' uses resource state '{}' not allowed for this queue type",
                    pass.name.c_str(),
                    static_cast<uint32>(usage.desiredState));

                if (usage.type == ResourceType::Texture)
                {
                    auto& resource = graph.textures[usage.index];
                    if (!resource.texture)
                        continue;

                    RHISubresourceRange range = usage.hasSubresourceRange ? usage.subresourceRange : RHISubresourceRange::All();
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
                                auto it = resource.subresourceStates.find(key);
                                RHIResourceState current = (it != resource.subresourceStates.end()) ? it->second : resource.currentState;
                                if (current != usage.desiredState)
                                {
                                    pass.textureBarriers.push_back(
                                        {resource.texture.Get(),
                                         current,
                                         usage.desiredState,
                                         RHISubresourceRange{mip, 1, layer, 1, range.aspect}});
                                }
                                resource.subresourceStates[key] = usage.desiredState;
                            }
                        }

                        if (rangeIsAll)
                        {
                            resource.currentState = usage.desiredState;
                            resource.subresourceStates.clear();
                            resource.hasSubresourceTracking = false;
                        }
                    }
                    else if (resource.currentState != usage.desiredState)
                    {
                        pass.textureBarriers.push_back(
                            {resource.texture.Get(), resource.currentState, usage.desiredState, RHISubresourceRange::All()});
                        resource.currentState = usage.desiredState;
                    }
                }
                else
                {
                    auto& resource = graph.buffers[usage.index];
                    if (!resource.buffer)
                        continue;

                    uint64 offset = usage.hasRange ? usage.offset : 0;
                    uint64 size = usage.hasRange ? usage.size : RVX_WHOLE_SIZE;
                    uint64 rangeSize = ResolveBufferRangeSize(offset, size, resource.desc.size);
                    bool isWhole = IsWholeBufferRange(offset, size, resource.desc.size);

                    if (resource.hasRangeTracking || (usage.hasRange && !isWhole))
                    {
                        uint64 applySize = isWhole ? resource.desc.size : rangeSize;
                        ApplyBufferRangeTransition(resource, offset, applySize, usage.desiredState, pass.bufferBarriers);
                        if (isWhole)
                        {
                            resource.currentState = usage.desiredState;
                            resource.rangeStates.clear();
                            resource.hasRangeTracking = false;
                        }
                    }
                    else if (resource.currentState != usage.desiredState)
                    {
                        pass.bufferBarriers.push_back(
                            {resource.buffer.Get(), resource.currentState, usage.desiredState, offset, isWhole ? RVX_WHOLE_SIZE : rangeSize});
                        resource.currentState = usage.desiredState;
                    }
                }
            }

            uint32 mergedTex = MergeTextureBarriers(pass.textureBarriers);
            uint32 mergedBuf = MergeBufferBarriers(pass.bufferBarriers);
            graph.stats.mergedTextureBarrierCount += mergedTex;
            graph.stats.mergedBufferBarrierCount += mergedBuf;
            graph.stats.mergedBarrierCount += mergedTex + mergedBuf;
            graph.stats.textureBarrierCount += static_cast<uint32>(pass.textureBarriers.size());
            graph.stats.bufferBarrierCount += static_cast<uint32>(pass.bufferBarriers.size());
        }

        std::unordered_map<RHITexture*, RHIResourceState> lastTextureState;
        std::unordered_map<RHIBuffer*, RHIResourceState> lastBufferState;
        for (auto& pass : graph.passes)
        {
            if (pass.culled)
                continue;

            graph.stats.crossPassMergedBarrierCount += RemoveRedundantTextureBarriers(lastTextureState, pass.textureBarriers);
            graph.stats.crossPassMergedBarrierCount += RemoveRedundantBufferBarriers(lastBufferState, pass.bufferBarriers);

            for (const auto& barrier : pass.textureBarriers)
                lastTextureState[barrier.texture] = barrier.stateAfter;
            for (const auto& barrier : pass.bufferBarriers)
                lastBufferState[barrier.buffer] = barrier.stateAfter;
        }

        graph.stats.textureBarrierCount = 0;
        graph.stats.bufferBarrierCount = 0;
        for (const auto& pass : graph.passes)
        {
            if (pass.culled)
                continue;
            graph.stats.textureBarrierCount += static_cast<uint32>(pass.textureBarriers.size());
            graph.stats.bufferBarrierCount += static_cast<uint32>(pass.bufferBarriers.size());
        }
        graph.stats.barrierCount = graph.stats.textureBarrierCount + graph.stats.bufferBarrierCount;
    }

} // namespace RVX
