#include "RenderGraphInternal.h"
#include "Core/Assert.h"
#include "Core/Log.h"
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

        // Helper to get texture memory requirements (use device query if available, else estimate)
        auto getTextureMemReqs = [&](const RHITextureDesc& desc) -> std::pair<uint64, uint64> {
            if (graph.device)
            {
                auto reqs = graph.device->GetTextureMemoryRequirements(desc);
                return {reqs.size, reqs.alignment};
            }
            return {EstimateTextureMemorySize(desc), 65536};
        };

        auto getBufferMemReqs = [&](const RHIBufferDesc& desc) -> std::pair<uint64, uint64> {
            if (graph.device)
            {
                auto reqs = graph.device->GetBufferMemoryRequirements(desc);
                return {reqs.size, reqs.alignment};
            }
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
            uint32 lastUsePass;
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
                uint64 currentOffset = 0;
                for (const auto& alloc : heap.allocations)
                {
                    // If this allocation's lifetime doesn't overlap with ours
                    if (alloc.lastUsePass < firstUse)
                    {
                        // Check if we can fit in this space
                        uint64 alignedOffset = (currentOffset + alignment - 1) & ~(alignment - 1);
                        if (alignedOffset >= alloc.offset && 
                            alignedOffset + requiredSize <= alloc.offset + alloc.size)
                        {
                            uint64 waste = alloc.size - requiredSize;
                            if (waste < bestWaste)
                            {
                                bestHeap = static_cast<int32>(heapIdx);
                                bestOffset = alignedOffset;
                                bestWaste = waste;
                            }
                        }
                    }
                    currentOffset = alloc.offset + alloc.size;
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
            heap.allocations.push_back({bestOffset, requiredSize, lastUse});
            heap.totalSize = std::max(heap.totalSize, bestOffset + requiredSize);

            res.alias->heapIndex = static_cast<uint32>(bestHeap);
            res.alias->heapOffset = bestOffset;
            res.alias->isAliased = (heap.allocations.size() > 1);

            if (res.alias->isAliased)
            {
                if (res.type == ResourceType::Texture)
                    graph.aliasedTextureCount++;
                else
                    graph.aliasedBufferCount++;
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

        // Track which resource last used each (heapIndex, offset) location
        // This is a simplified implementation - a full implementation would track
        // exact memory ranges, but for now we just track per-resource
        std::unordered_map<uint64, std::pair<ResourceType, uint32>> lastResourceAtLocation;

        auto makeLocationKey = [](uint32 heapIndex, uint64 offset) -> uint64 {
            return (static_cast<uint64>(heapIndex) << 48) | (offset & 0x0000FFFFFFFFFFFF);
        };

        for (uint32 order = 0; order < static_cast<uint32>(graph.executionOrder.size()); ++order)
        {
            uint32 passIndex = graph.executionOrder[order];
            auto& pass = graph.passes[passIndex];
            if (pass.culled) continue;

            pass.aliasingBarriers.clear();

            for (const auto& usage : pass.usages)
            {
                if (usage.type == ResourceType::Texture)
                {
                    if (usage.index >= graph.textures.size()) continue;
                    auto& texture = graph.textures[usage.index];
                    if (!texture.alias.isAliased) continue;

                    uint64 key = makeLocationKey(texture.alias.heapIndex, texture.alias.heapOffset);
                    auto it = lastResourceAtLocation.find(key);

                    if (it != lastResourceAtLocation.end())
                    {
                        // Different resource was using this memory location before
                        if (it->second.second != usage.index)
                        {
                            AliasingBarrier ab;
                            ab.type = ResourceType::Texture;
                            ab.beforeResourceIndex = it->second.second;
                            ab.afterResourceIndex = usage.index;
                            pass.aliasingBarriers.push_back(ab);
                        }
                    }

                    // Update the last resource at this location
                    lastResourceAtLocation[key] = {ResourceType::Texture, usage.index};
                }
                else
                {
                    if (usage.index >= graph.buffers.size()) continue;
                    auto& buffer = graph.buffers[usage.index];
                    if (!buffer.alias.isAliased) continue;

                    uint64 key = makeLocationKey(buffer.alias.heapIndex, buffer.alias.heapOffset);
                    auto it = lastResourceAtLocation.find(key);

                    if (it != lastResourceAtLocation.end())
                    {
                        if (it->second.second != usage.index)
                        {
                            AliasingBarrier ab;
                            ab.type = ResourceType::Buffer;
                            ab.beforeResourceIndex = it->second.second;
                            ab.afterResourceIndex = usage.index;
                            pass.aliasingBarriers.push_back(ab);
                        }
                    }

                    lastResourceAtLocation[key] = {ResourceType::Buffer, usage.index};
                }
            }
        }
    }

    // =============================================================================
    // Create Transient Resources (with optional memory aliasing)
    // =============================================================================
    void CreateTransientResources(RenderGraphImpl& graph)
    {
        if (!graph.device)
            return;

        // If memory aliasing is enabled and heaps have been computed, use placed resources
        if (graph.enableMemoryAliasing && !graph.transientHeaps.empty())
        {
            // Create RHI Heaps
            for (auto& th : graph.transientHeaps)
            {
                if (!th.heap && th.size > 0)
                {
                    RHIHeapDesc heapDesc;
                    heapDesc.size = th.size;
                    heapDesc.type = RHIHeapType::Default;
                    heapDesc.flags = RHIHeapFlags::AllowAll;
                    heapDesc.debugName = "TransientHeap";
                    
                    th.heap = graph.device->CreateHeap(heapDesc);
                    if (!th.heap)
                    {
                        RVX_CORE_WARN("RenderGraph: Failed to create transient heap, falling back to independent resources");
                    }
                }
            }

            // Create Placed Textures
            for (auto& texture : graph.textures)
            {
                if (texture.imported || texture.texture)
                    continue;

                if (texture.alias.heapIndex < graph.transientHeaps.size() &&
                    graph.transientHeaps[texture.alias.heapIndex].heap)
                {
                    auto* heap = graph.transientHeaps[texture.alias.heapIndex].heap.Get();
                    texture.texture = graph.device->CreatePlacedTexture(
                        heap,
                        texture.alias.heapOffset,
                        texture.desc);
                }
                
                // Fallback to independent resource if placed creation fails
                if (!texture.texture)
                {
                    texture.texture = graph.device->CreateTexture(texture.desc);
                }
                
                texture.initialState = RHIResourceState::Undefined;
                texture.currentState = texture.initialState;
            }

            // Create Placed Buffers
            for (auto& buffer : graph.buffers)
            {
                if (buffer.imported || buffer.buffer)
                    continue;

                if (buffer.alias.heapIndex < graph.transientHeaps.size() &&
                    graph.transientHeaps[buffer.alias.heapIndex].heap)
                {
                    auto* heap = graph.transientHeaps[buffer.alias.heapIndex].heap.Get();
                    buffer.buffer = graph.device->CreatePlacedBuffer(
                        heap,
                        buffer.alias.heapOffset,
                        buffer.desc);
                }
                
                // Fallback to independent resource if placed creation fails
                if (!buffer.buffer)
                {
                    buffer.buffer = graph.device->CreateBuffer(buffer.desc);
                }
                
                buffer.initialState = RHIResourceState::Undefined;
                buffer.currentState = buffer.initialState;
            }
        }
        else
        {
            // No aliasing: create independent resources
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
    }

    // =============================================================================
    // Compile Render Graph
    // =============================================================================
    void CompileRenderGraph(RenderGraphImpl& graph)
    {
        graph.stats = {};
        graph.stats.totalPasses = static_cast<uint32>(graph.passes.size());
        graph.totalMemoryWithoutAliasing = 0;
        graph.totalMemoryWithAliasing = 0;
        graph.aliasedTextureCount = 0;
        graph.aliasedBufferCount = 0;

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

        // Mark culled passes first (needed for lifetime calculation)
        for (uint32 passIndex = 0; passIndex < graph.passes.size(); ++passIndex)
        {
            if (!passNeeded.empty() && passNeeded[passIndex] == 0)
            {
                graph.passes[passIndex].culled = true;
                graph.stats.culledPasses++;
            }
        }

        // Calculate resource lifetimes and memory aliases
        CalculateResourceLifetimes(graph);
        ComputeMemoryAliases(graph);

        // Create transient resources (with optional placed resource aliasing)
        CreateTransientResources(graph);

        // Compute aliasing barriers for resources that share memory
        ComputeAliasingBarriers(graph);

        for (auto& pass : graph.passes)
        {
            if (pass.culled)
                continue;

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
