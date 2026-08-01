/** @file TransientResourcePool.cpp @brief Completion-safe transient resource pooling */

#include "Render/Graph/TransientResourcePool.h"

#include "Core/Assert.h"
#include "Core/Log.h"
#include "Resources/RenderRetirementQueue.h"
#include "Resources/RenderSubmissionTracker.h"

#include <algorithm>
#include <bit>
#include <functional>
#include <unordered_map>

namespace RVX
{
    namespace
    {
        bool IsCompletionSatisfied(GPUCompletionStatus status)
        {
            return status == GPUCompletionStatus::Completed ||
                   status == GPUCompletionStatus::CompatibilityWaitIdle;
        }
    } // namespace

    class TransientResourcePool::Impl
    {
    public:
        struct PooledTexture
        {
            RHITextureRef texture;
            RHITextureDesc desc;
            RHITextureAccessSnapshot accessSnapshot;
            GPUCompletionToken availableAfter;
            uint64 descHash = 0;
            uint32 lastUsedFrame = 0;
            uint64 memorySize = 0;
            bool inUse = false;
        };

        struct PooledBuffer
        {
            RHIBufferRef buffer;
            RHIBufferAccessSnapshot accessSnapshot;
            GPUCompletionToken availableAfter;
            uint64 descHash = 0;
            uint32 lastUsedFrame = 0;
            uint64 memorySize = 0;
            bool inUse = false;
        };

        [[nodiscard]] bool CanReuse(const GPUCompletionToken& token)
        {
            if (token.count == 0)
            {
                return true;
            }
            if (submissionTracker == nullptr)
            {
                return false;
            }

            GPUCompletionStatus status = submissionTracker->Query(token);
            if (status == GPUCompletionStatus::Pending &&
                submissionTracker->GetTopology().completionMode ==
                    RHIQueueCompletionMode::CompatibilityWaitIdle)
            {
                status = submissionTracker->Wait(token);
            }
            return IsCompletionSatisfied(status);
        }

        template<typename T>
        void Retire(Ref<T>& resource,
                    const GPUCompletionToken& token,
                    uint64 estimatedBytes)
        {
            if (!resource)
            {
                return;
            }
            if (retirementQueue == nullptr)
            {
                resource.Reset();
                return;
            }

            Ref<RefCounted> object(std::move(resource));
            RenderRetirementEntry entry{token, std::move(object), estimatedBytes};
            RVX_ASSERT_MSG(retirementQueue->Enqueue(std::move(entry)),
                           "Transient pool retirement transfer failed");
        }

        IRHIDevice* device = nullptr;
        RenderSubmissionTracker* submissionTracker = nullptr;
        RenderRetirementQueue* retirementQueue = nullptr;
        GPUCompletionToken lastSubmission;
        uint32 currentFrame = 0;
        std::unordered_multimap<uint64, PooledTexture> texturePool;
        std::unordered_multimap<uint64, PooledBuffer> bufferPool;
        Stats stats;
    };

    TransientResourcePool::TransientResourcePool()
        : m_impl(std::make_unique<Impl>())
    {
    }

    TransientResourcePool::~TransientResourcePool()
    {
        Shutdown();
    }

    void TransientResourcePool::Initialize(
        IRHIDevice* device,
        RenderSubmissionTracker* submissionTracker,
        RenderRetirementQueue* retirementQueue)
    {
        if (m_impl->device)
        {
            RVX_CORE_WARN("TransientResourcePool: Already initialized");
            return;
        }

        m_impl->device = device;
        m_impl->submissionTracker = submissionTracker;
        m_impl->retirementQueue = retirementQueue;
        m_impl->lastSubmission = {};
        m_impl->currentFrame = 0;
        m_impl->stats = {};
        RVX_CORE_DEBUG("TransientResourcePool: Initialized");
    }

    void TransientResourcePool::Shutdown()
    {
        if (!m_impl->device)
        {
            return;
        }

        for (auto& [hash, pooled] : m_impl->texturePool)
        {
            static_cast<void>(hash);
            const GPUCompletionToken& completion = pooled.inUse
                ? m_impl->lastSubmission
                : pooled.availableAfter;
            m_impl->Retire(pooled.texture, completion, pooled.memorySize);
        }
        for (auto& [hash, pooled] : m_impl->bufferPool)
        {
            static_cast<void>(hash);
            const GPUCompletionToken& completion = pooled.inUse
                ? m_impl->lastSubmission
                : pooled.availableAfter;
            m_impl->Retire(pooled.buffer, completion, pooled.memorySize);
        }
        m_impl->texturePool.clear();
        m_impl->bufferPool.clear();
        m_impl->device = nullptr;
        m_impl->submissionTracker = nullptr;
        m_impl->retirementQueue = nullptr;
        m_impl->lastSubmission = {};
        m_impl->stats = {};
        RVX_CORE_DEBUG("TransientResourcePool: Shutdown");
    }

    bool TransientResourcePool::IsInitialized() const
    {
        return m_impl->device != nullptr;
    }

    void TransientResourcePool::NotifySubmission(
        const GPUCompletionToken& completion)
    {
        GPUCompletionToken normalized;
        RVX_ASSERT_MSG(MergeGPUCompletionToken(normalized, completion),
                       "Transient pool received invalid submission completion");
        m_impl->lastSubmission = normalized;
    }

    void TransientResourcePool::BeginFrame()
    {
        ++m_impl->currentFrame;
        ResetFrameStats();
    }

    void TransientResourcePool::EndFrame()
    {
        m_impl->stats.texturePoolSize =
            static_cast<uint32>(m_impl->texturePool.size());
        m_impl->stats.bufferPoolSize =
            static_cast<uint32>(m_impl->bufferPool.size());

        uint64 totalMemory = 0;
        for (const auto& [hash, pooled] : m_impl->texturePool)
        {
            static_cast<void>(hash);
            totalMemory += pooled.memorySize;
        }
        for (const auto& [hash, pooled] : m_impl->bufferPool)
        {
            static_cast<void>(hash);
            totalMemory += pooled.memorySize;
        }
        m_impl->stats.totalPooledMemory = totalMemory;
    }

    RHITexture* TransientResourcePool::AcquireTexture(
        const RHITextureDesc& desc)
    {
        return AcquireTextureLease(desc).texture;
    }

    TransientTextureLease TransientResourcePool::AcquireTextureLease(
        const RHITextureDesc& desc)
    {
        if (!m_impl->device)
        {
            return {};
        }

        const uint64 hash = HashTextureDesc(desc);
        const auto range = m_impl->texturePool.equal_range(hash);
        for (auto it = range.first; it != range.second; ++it)
        {
            if (!it->second.inUse &&
                AreRHITextureDescsEquivalent(it->second.desc, desc) &&
                m_impl->CanReuse(it->second.availableAfter))
            {
                it->second.inUse = true;
                it->second.lastUsedFrame = m_impl->currentFrame;
                ++m_impl->stats.textureHits;
                ++m_impl->stats.texturesInUse;
                return {it->second.texture.Get(), it->second.accessSnapshot, true};
            }
        }

        RHITextureRef texture = m_impl->device->CreateTexture(desc);
        if (!texture)
        {
            RVX_CORE_ERROR("TransientResourcePool: Failed to create texture");
            return {};
        }

        Impl::PooledTexture pooled;
        pooled.texture = std::move(texture);
        pooled.desc = desc;
        pooled.descHash = hash;
        pooled.lastUsedFrame = m_impl->currentFrame;
        pooled.memorySize = EstimateTextureMemory(desc);
        pooled.inUse = true;
        pooled.accessSnapshot = MakeRHITextureAccessSnapshot(
            RHIResourceState::Undefined,
            RHIShaderStage::None,
            GPUQueueDomain::Graphics,
            RHIContentValidity::Invalid);
        RHITexture* result = pooled.texture.Get();
        RHITextureAccessSnapshot accessSnapshot = pooled.accessSnapshot;
        m_impl->texturePool.emplace(hash, std::move(pooled));
        ++m_impl->stats.textureMisses;
        ++m_impl->stats.texturesInUse;
        return {result, std::move(accessSnapshot), false};
    }

    RHIBuffer* TransientResourcePool::AcquireBuffer(
        const RHIBufferDesc& desc)
    {
        return AcquireBufferLease(desc).buffer;
    }

    TransientBufferLease TransientResourcePool::AcquireBufferLease(
        const RHIBufferDesc& desc)
    {
        if (!m_impl->device)
        {
            return {};
        }

        const uint64 hash = HashBufferDesc(desc);
        const auto range = m_impl->bufferPool.equal_range(hash);
        for (auto it = range.first; it != range.second; ++it)
        {
            if (!it->second.inUse &&
                m_impl->CanReuse(it->second.availableAfter))
            {
                it->second.inUse = true;
                it->second.lastUsedFrame = m_impl->currentFrame;
                ++m_impl->stats.bufferHits;
                ++m_impl->stats.buffersInUse;
                return {it->second.buffer.Get(), it->second.accessSnapshot, true};
            }
        }

        RHIBufferRef buffer = m_impl->device->CreateBuffer(desc);
        if (!buffer)
        {
            RVX_CORE_ERROR("TransientResourcePool: Failed to create buffer");
            return {};
        }

        Impl::PooledBuffer pooled;
        pooled.buffer = std::move(buffer);
        pooled.descHash = hash;
        pooled.lastUsedFrame = m_impl->currentFrame;
        pooled.memorySize = EstimateBufferMemory(desc);
        pooled.inUse = true;
        pooled.accessSnapshot = MakeRHIBufferAccessSnapshot(
            RHIResourceState::Undefined,
            RHIShaderStage::None,
            GPUQueueDomain::Graphics,
            RHIContentValidity::Invalid);
        RHIBuffer* result = pooled.buffer.Get();
        RHIBufferAccessSnapshot accessSnapshot = pooled.accessSnapshot;
        m_impl->bufferPool.emplace(hash, std::move(pooled));
        ++m_impl->stats.bufferMisses;
        ++m_impl->stats.buffersInUse;
        return {result, std::move(accessSnapshot), false};
    }

    void TransientResourcePool::ReleaseTexture(RHITexture* texture)
    {
        RHITextureAccessSnapshot unknown = MakeRHITextureAccessSnapshot(
            RHIResourceState::Common,
            RHIShaderStage::All,
            GPUQueueDomain::Graphics,
            RHIContentValidity::Unknown);
        ReleaseTexture(texture, unknown);
    }

    void TransientResourcePool::ReleaseTexture(
        RHITexture* texture,
        const RHITextureAccessSnapshot& finalAccess)
    {
        if (!texture)
        {
            return;
        }
        for (auto& [hash, pooled] : m_impl->texturePool)
        {
            static_cast<void>(hash);
            if (pooled.texture.Get() == texture && pooled.inUse)
            {
                pooled.inUse = false;
                pooled.availableAfter = m_impl->lastSubmission;
                pooled.accessSnapshot = finalAccess;
                if (m_impl->stats.texturesInUse > 0)
                {
                    --m_impl->stats.texturesInUse;
                }
                return;
            }
        }
        RVX_CORE_WARN("TransientResourcePool: ReleaseTexture called on unknown texture");
    }

    void TransientResourcePool::ReleaseBuffer(RHIBuffer* buffer)
    {
        RHIBufferAccessSnapshot unknown = MakeRHIBufferAccessSnapshot(
            RHIResourceState::Common,
            RHIShaderStage::All,
            GPUQueueDomain::Graphics,
            RHIContentValidity::Unknown);
        ReleaseBuffer(buffer, unknown);
    }

    void TransientResourcePool::ReleaseBuffer(
        RHIBuffer* buffer,
        const RHIBufferAccessSnapshot& finalAccess)
    {
        if (!buffer)
        {
            return;
        }
        for (auto& [hash, pooled] : m_impl->bufferPool)
        {
            static_cast<void>(hash);
            if (pooled.buffer.Get() == buffer && pooled.inUse)
            {
                pooled.inUse = false;
                pooled.availableAfter = m_impl->lastSubmission;
                pooled.accessSnapshot = finalAccess;
                if (m_impl->stats.buffersInUse > 0)
                {
                    --m_impl->stats.buffersInUse;
                }
                return;
            }
        }
        RVX_CORE_WARN("TransientResourcePool: ReleaseBuffer called on unknown buffer");
    }

    void TransientResourcePool::EvictUnused(uint32 frameThreshold)
    {
        uint32 evictedTextures = 0;
        uint32 evictedBuffers = 0;
        uint64 freedMemory = 0;
        for (auto it = m_impl->texturePool.begin();
             it != m_impl->texturePool.end();)
        {
            auto& pooled = it->second;
            if (!pooled.inUse &&
                m_impl->currentFrame - pooled.lastUsedFrame >= frameThreshold)
            {
                freedMemory += pooled.memorySize;
                m_impl->Retire(pooled.texture,
                               pooled.availableAfter,
                               pooled.memorySize);
                it = m_impl->texturePool.erase(it);
                ++evictedTextures;
            }
            else
            {
                ++it;
            }
        }
        for (auto it = m_impl->bufferPool.begin();
             it != m_impl->bufferPool.end();)
        {
            auto& pooled = it->second;
            if (!pooled.inUse &&
                m_impl->currentFrame - pooled.lastUsedFrame >= frameThreshold)
            {
                freedMemory += pooled.memorySize;
                m_impl->Retire(pooled.buffer,
                               pooled.availableAfter,
                               pooled.memorySize);
                it = m_impl->bufferPool.erase(it);
                ++evictedBuffers;
            }
            else
            {
                ++it;
            }
        }

        if (evictedTextures != 0 || evictedBuffers != 0)
        {
            RVX_CORE_DEBUG(
                "TransientResourcePool: Evicted {} textures, {} buffers, freed {} KB",
                evictedTextures,
                evictedBuffers,
                freedMemory / 1024);
        }
    }

    TransientResourcePool::Stats TransientResourcePool::GetStats() const
    {
        return m_impl->stats;
    }

    void TransientResourcePool::ResetFrameStats()
    {
        m_impl->stats.textureHits = 0;
        m_impl->stats.textureMisses = 0;
        m_impl->stats.bufferHits = 0;
        m_impl->stats.bufferMisses = 0;
    }

    uint64 TransientResourcePool::HashTextureDesc(const RHITextureDesc& desc)
    {
        uint64 hash = 0;
        const auto hashCombine = [&hash](uint64 value)
        {
            hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
        };
        hashCombine(desc.width);
        hashCombine(desc.height);
        hashCombine(desc.depth);
        hashCombine(desc.mipLevels);
        hashCombine(desc.arraySize);
        hashCombine(static_cast<uint64>(desc.format));
        hashCombine(static_cast<uint64>(desc.dimension));
        hashCombine(static_cast<uint64>(desc.usage));
        hashCombine(static_cast<uint64>(desc.sampleCount));
        hashCombine(static_cast<uint64>(desc.optimizedClearValue.type));
        if (desc.optimizedClearValue.type == RHIOptimizedClearValueType::Color)
        {
            hashCombine(std::bit_cast<uint32>(desc.optimizedClearValue.color.r));
            hashCombine(std::bit_cast<uint32>(desc.optimizedClearValue.color.g));
            hashCombine(std::bit_cast<uint32>(desc.optimizedClearValue.color.b));
            hashCombine(std::bit_cast<uint32>(desc.optimizedClearValue.color.a));
        }
        else if (desc.optimizedClearValue.type ==
                 RHIOptimizedClearValueType::DepthStencil)
        {
            hashCombine(std::bit_cast<uint32>(
                desc.optimizedClearValue.depthStencil.depth));
            hashCombine(desc.optimizedClearValue.depthStencil.stencil);
        }
        return hash;
    }

    uint64 TransientResourcePool::HashBufferDesc(const RHIBufferDesc& desc)
    {
        uint64 hash = 0;
        const auto hashCombine = [&hash](uint64 value)
        {
            hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
        };
        hashCombine(desc.size);
        hashCombine(static_cast<uint64>(desc.usage));
        hashCombine(static_cast<uint64>(desc.memoryType));
        hashCombine(desc.stride);
        return hash;
    }

    uint64 TransientResourcePool::EstimateTextureMemory(
        const RHITextureDesc& desc)
    {
        uint64 bytesPerPixel = GetFormatBytesPerPixel(desc.format);
        if (bytesPerPixel == 0)
        {
            bytesPerPixel = 4;
        }

        uint64 totalSize = 0;
        uint32 width = desc.width;
        uint32 height = desc.height;
        uint32 depth = desc.depth;
        for (uint32 mip = 0; mip < desc.mipLevels; ++mip)
        {
            totalSize += static_cast<uint64>(width) * height * depth *
                         bytesPerPixel * desc.arraySize;
            width = std::max(1u, width / 2);
            height = std::max(1u, height / 2);
            depth = std::max(1u, depth / 2);
        }
        return totalSize * static_cast<uint32>(desc.sampleCount);
    }

    uint64 TransientResourcePool::EstimateBufferMemory(
        const RHIBufferDesc& desc)
    {
        return desc.size;
    }
} // namespace RVX
