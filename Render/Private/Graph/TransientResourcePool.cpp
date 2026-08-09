/** @file TransientResourcePool.cpp @brief Completion-safe transient resource pooling */

#include "Render/Graph/TransientResourcePool.h"

#include "Core/Assert.h"
#include "Core/Log.h"
#include "Resources/RenderRetirementQueue.h"
#include "Resources/RenderSubmissionTracker.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <functional>
#include <unordered_map>

namespace RVX
{
    namespace
    {
        std::atomic<uint64> s_nextTransientPoolIdentity{1};

        bool IsCompletionSatisfied(GPUCompletionStatus status)
        {
            return status == GPUCompletionStatus::Completed ||
                   status == GPUCompletionStatus::CompatibilityWaitIdle;
        }

        uint32 AdvanceLeaseGeneration(uint32 generation)
        {
            ++generation;
            return generation == 0 ? 1 : generation;
        }
    } // namespace

    class TransientResourceLeaseControl
    {
    public:
        virtual ~TransientResourceLeaseControl() = default;
        virtual bool CommitTexture(
            uint64 poolIdentity,
            uint64 slotId,
            uint32 generation,
            const GPUCompletionToken& completion,
            const RHITextureAccessSnapshot& finalAccess) = 0;
        virtual bool CanCommitTexture(
            uint64 poolIdentity,
            uint64 slotId,
            uint32 generation,
            const GPUCompletionToken& completion) const = 0;
        virtual bool CommitBuffer(
            uint64 poolIdentity,
            uint64 slotId,
            uint32 generation,
            const GPUCompletionToken& completion,
            const RHIBufferAccessSnapshot& finalAccess) = 0;
        virtual bool CanCommitBuffer(
            uint64 poolIdentity,
            uint64 slotId,
            uint32 generation,
            const GPUCompletionToken& completion) const = 0;
        virtual bool AbortTexture(uint64 poolIdentity,
                                  uint64 slotId,
                                  uint32 generation,
                                  bool automatic) = 0;
        virtual bool AbortBuffer(uint64 poolIdentity,
                                 uint64 slotId,
                                 uint32 generation,
                                 bool automatic) = 0;
        virtual bool LoseTexture(uint64 poolIdentity,
                                 uint64 slotId,
                                 uint32 generation) = 0;
        virtual bool LoseBuffer(uint64 poolIdentity,
                                uint64 slotId,
                                uint32 generation) = 0;
    };

    class TransientResourcePool::Impl final
        : public TransientResourceLeaseControl
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
            uint64 slotId = 0;
            uint32 generation = 0;
            TransientResourceLeaseState state =
                TransientResourceLeaseState::Free;
        };

        struct PooledBuffer
        {
            RHIBufferRef buffer;
            RHIBufferAccessSnapshot accessSnapshot;
            GPUCompletionToken availableAfter;
            uint64 descHash = 0;
            uint32 lastUsedFrame = 0;
            uint64 memorySize = 0;
            uint64 slotId = 0;
            uint32 generation = 0;
            TransientResourceLeaseState state =
                TransientResourceLeaseState::Free;
        };

        PooledTexture* FindTexture(uint64 slotId)
        {
            for (auto& [hash, pooled] : texturePool)
            {
                static_cast<void>(hash);
                if (pooled.slotId == slotId)
                    return &pooled;
            }
            return nullptr;
        }

        PooledBuffer* FindBuffer(uint64 slotId)
        {
            for (auto& [hash, pooled] : bufferPool)
            {
                static_cast<void>(hash);
                if (pooled.slotId == slotId)
                    return &pooled;
            }
            return nullptr;
        }

        const PooledTexture* FindTexture(uint64 slotId) const
        {
            for (const auto& [hash, pooled] : texturePool)
            {
                static_cast<void>(hash);
                if (pooled.slotId == slotId)
                    return &pooled;
            }
            return nullptr;
        }

        const PooledBuffer* FindBuffer(uint64 slotId) const
        {
            for (const auto& [hash, pooled] : bufferPool)
            {
                static_cast<void>(hash);
                if (pooled.slotId == slotId)
                    return &pooled;
            }
            return nullptr;
        }

        bool ValidateIdentity(uint64 candidate) const
        {
            return active && candidate == poolIdentity;
        }

        bool ValidateCompletion(const GPUCompletionToken& completion)
        {
            GPUCompletionToken normalized;
            return completion.count != 0 &&
                   MergeGPUCompletionToken(normalized, completion);
        }

        bool CanCommitTexture(
            uint64 candidatePoolIdentity,
            uint64 slotId,
            uint32 generation,
            const GPUCompletionToken& completion) const override
        {
            const PooledTexture* pooled = FindTexture(slotId);
            GPUCompletionToken normalized;
            return active && candidatePoolIdentity == poolIdentity && pooled &&
                   pooled->generation == generation &&
                   pooled->state == TransientResourceLeaseState::Recording &&
                   completion.count != 0 &&
                   MergeGPUCompletionToken(normalized, completion);
        }

        bool CanCommitBuffer(
            uint64 candidatePoolIdentity,
            uint64 slotId,
            uint32 generation,
            const GPUCompletionToken& completion) const override
        {
            const PooledBuffer* pooled = FindBuffer(slotId);
            GPUCompletionToken normalized;
            return active && candidatePoolIdentity == poolIdentity && pooled &&
                   pooled->generation == generation &&
                   pooled->state == TransientResourceLeaseState::Recording &&
                   completion.count != 0 &&
                   MergeGPUCompletionToken(normalized, completion);
        }

        bool CommitTexture(
            uint64 candidatePoolIdentity,
            uint64 slotId,
            uint32 generation,
            const GPUCompletionToken& completion,
            const RHITextureAccessSnapshot& finalAccess) override
        {
            PooledTexture* pooled = FindTexture(slotId);
            if (!ValidateIdentity(candidatePoolIdentity) || !pooled ||
                pooled->generation != generation ||
                pooled->state != TransientResourceLeaseState::Recording ||
                !ValidateCompletion(completion))
            {
                ++stats.leaseValidationFailureCount;
                return false;
            }
            pooled->state = TransientResourceLeaseState::InFlight;
            pooled->availableAfter = completion;
            pooled->accessSnapshot = finalAccess;
            if (stats.recordingTextureLeases > 0)
                --stats.recordingTextureLeases;
            if (stats.texturesInUse > 0)
                --stats.texturesInUse;
            ++stats.inFlightTextureLeases;
            ++stats.leaseCommitCount;
            return true;
        }

        bool CommitBuffer(
            uint64 candidatePoolIdentity,
            uint64 slotId,
            uint32 generation,
            const GPUCompletionToken& completion,
            const RHIBufferAccessSnapshot& finalAccess) override
        {
            PooledBuffer* pooled = FindBuffer(slotId);
            if (!ValidateIdentity(candidatePoolIdentity) || !pooled ||
                pooled->generation != generation ||
                pooled->state != TransientResourceLeaseState::Recording ||
                !ValidateCompletion(completion))
            {
                ++stats.leaseValidationFailureCount;
                return false;
            }
            pooled->state = TransientResourceLeaseState::InFlight;
            pooled->availableAfter = completion;
            pooled->accessSnapshot = finalAccess;
            if (stats.recordingBufferLeases > 0)
                --stats.recordingBufferLeases;
            if (stats.buffersInUse > 0)
                --stats.buffersInUse;
            ++stats.inFlightBufferLeases;
            ++stats.leaseCommitCount;
            return true;
        }

        bool AbortTexture(uint64 candidatePoolIdentity,
                          uint64 slotId,
                          uint32 generation,
                          bool automatic) override
        {
            PooledTexture* pooled = FindTexture(slotId);
            if (!ValidateIdentity(candidatePoolIdentity) || !pooled ||
                pooled->generation != generation ||
                pooled->state != TransientResourceLeaseState::Recording)
            {
                if (!automatic)
                    ++stats.leaseValidationFailureCount;
                return automatic;
            }
            pooled->state = TransientResourceLeaseState::Free;
            pooled->availableAfter = {};
            if (stats.recordingTextureLeases > 0)
                --stats.recordingTextureLeases;
            if (stats.texturesInUse > 0)
                --stats.texturesInUse;
            ++stats.leaseAbortCount;
            return true;
        }

        bool AbortBuffer(uint64 candidatePoolIdentity,
                         uint64 slotId,
                         uint32 generation,
                         bool automatic) override
        {
            PooledBuffer* pooled = FindBuffer(slotId);
            if (!ValidateIdentity(candidatePoolIdentity) || !pooled ||
                pooled->generation != generation ||
                pooled->state != TransientResourceLeaseState::Recording)
            {
                if (!automatic)
                    ++stats.leaseValidationFailureCount;
                return automatic;
            }
            pooled->state = TransientResourceLeaseState::Free;
            pooled->availableAfter = {};
            if (stats.recordingBufferLeases > 0)
                --stats.recordingBufferLeases;
            if (stats.buffersInUse > 0)
                --stats.buffersInUse;
            ++stats.leaseAbortCount;
            return true;
        }

        bool LoseTexture(uint64 candidatePoolIdentity,
                         uint64 slotId,
                         uint32 generation) override
        {
            PooledTexture* pooled = FindTexture(slotId);
            if (!ValidateIdentity(candidatePoolIdentity) || !pooled ||
                pooled->generation != generation ||
                pooled->state != TransientResourceLeaseState::Recording)
            {
                ++stats.leaseValidationFailureCount;
                return false;
            }
            pooled->state = TransientResourceLeaseState::Evicting;
            if (stats.recordingTextureLeases > 0)
                --stats.recordingTextureLeases;
            if (stats.texturesInUse > 0)
                --stats.texturesInUse;
            ++stats.leaseDeviceLostCount;
            return true;
        }

        bool LoseBuffer(uint64 candidatePoolIdentity,
                        uint64 slotId,
                        uint32 generation) override
        {
            PooledBuffer* pooled = FindBuffer(slotId);
            if (!ValidateIdentity(candidatePoolIdentity) || !pooled ||
                pooled->generation != generation ||
                pooled->state != TransientResourceLeaseState::Recording)
            {
                ++stats.leaseValidationFailureCount;
                return false;
            }
            pooled->state = TransientResourceLeaseState::Evicting;
            if (stats.recordingBufferLeases > 0)
                --stats.recordingBufferLeases;
            if (stats.buffersInUse > 0)
                --stats.buffersInUse;
            ++stats.leaseDeviceLostCount;
            return true;
        }

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
        uint64 poolIdentity =
            s_nextTransientPoolIdentity.fetch_add(1, std::memory_order_relaxed);
        uint64 nextSlotId = 1;
        bool active = false;
        uint32 currentFrame = 0;
        std::unordered_multimap<uint64, PooledTexture> texturePool;
        std::unordered_multimap<uint64, PooledBuffer> bufferPool;
        Stats stats;
    };

    TransientTextureLease::TransientTextureLease(
        std::shared_ptr<TransientResourceLeaseControl> control,
        uint64 poolIdentity,
        uint64 slotId,
        uint32 generation,
        RHITexture* acquiredTexture,
        RHITextureAccessSnapshot acquiredAccess,
        bool wasReused)
        : texture(acquiredTexture)
        , accessSnapshot(std::move(acquiredAccess))
        , reused(wasReused)
        , m_control(std::move(control))
        , m_poolIdentity(poolIdentity)
        , m_slotId(slotId)
        , m_generation(generation)
        , m_resolved(false)
    {
    }

    TransientTextureLease::~TransientTextureLease()
    {
        AutoAbort();
    }

    TransientTextureLease::TransientTextureLease(
        TransientTextureLease&& other) noexcept
    {
        *this = std::move(other);
    }

    TransientTextureLease& TransientTextureLease::operator=(
        TransientTextureLease&& other) noexcept
    {
        if (this == &other)
            return *this;
        AutoAbort();
        texture = other.texture;
        accessSnapshot = std::move(other.accessSnapshot);
        reused = other.reused;
        m_control = std::move(other.m_control);
        m_poolIdentity = other.m_poolIdentity;
        m_slotId = other.m_slotId;
        m_generation = other.m_generation;
        m_resolved = other.m_resolved;
        other.Reset();
        return *this;
    }

    bool TransientTextureLease::Commit(
        const GPUCompletionToken& completion,
        const RHITextureAccessSnapshot& finalAccess)
    {
        if (m_resolved || !m_control)
            return false;
        if (!m_control->CommitTexture(
                m_poolIdentity,
                m_slotId,
                m_generation,
                completion,
                finalAccess))
        {
            return false;
        }
        Reset();
        return true;
    }

    bool TransientTextureLease::CanCommit(
        const GPUCompletionToken& completion) const
    {
        return !m_resolved && m_control &&
               m_control->CanCommitTexture(
                   m_poolIdentity, m_slotId, m_generation, completion);
    }

    bool TransientTextureLease::AbortUnsubmitted()
    {
        if (m_resolved || !m_control)
            return false;
        if (!m_control->AbortTexture(
                m_poolIdentity, m_slotId, m_generation, false))
        {
            return false;
        }
        Reset();
        return true;
    }

    bool TransientTextureLease::MarkDeviceLost()
    {
        if (m_resolved || !m_control)
            return false;
        if (!m_control->LoseTexture(
                m_poolIdentity, m_slotId, m_generation))
        {
            return false;
        }
        Reset();
        return true;
    }

    void TransientTextureLease::AutoAbort() noexcept
    {
        if (!m_resolved && m_control)
        {
            static_cast<void>(m_control->AbortTexture(
                m_poolIdentity, m_slotId, m_generation, true));
        }
        Reset();
    }

    void TransientTextureLease::Reset() noexcept
    {
        texture = nullptr;
        accessSnapshot = {};
        reused = false;
        m_control.reset();
        m_poolIdentity = 0;
        m_slotId = 0;
        m_generation = 0;
        m_resolved = true;
    }

    TransientBufferLease::TransientBufferLease(
        std::shared_ptr<TransientResourceLeaseControl> control,
        uint64 poolIdentity,
        uint64 slotId,
        uint32 generation,
        RHIBuffer* acquiredBuffer,
        RHIBufferAccessSnapshot acquiredAccess,
        bool wasReused)
        : buffer(acquiredBuffer)
        , accessSnapshot(std::move(acquiredAccess))
        , reused(wasReused)
        , m_control(std::move(control))
        , m_poolIdentity(poolIdentity)
        , m_slotId(slotId)
        , m_generation(generation)
        , m_resolved(false)
    {
    }

    TransientBufferLease::~TransientBufferLease()
    {
        AutoAbort();
    }

    TransientBufferLease::TransientBufferLease(
        TransientBufferLease&& other) noexcept
    {
        *this = std::move(other);
    }

    TransientBufferLease& TransientBufferLease::operator=(
        TransientBufferLease&& other) noexcept
    {
        if (this == &other)
            return *this;
        AutoAbort();
        buffer = other.buffer;
        accessSnapshot = std::move(other.accessSnapshot);
        reused = other.reused;
        m_control = std::move(other.m_control);
        m_poolIdentity = other.m_poolIdentity;
        m_slotId = other.m_slotId;
        m_generation = other.m_generation;
        m_resolved = other.m_resolved;
        other.Reset();
        return *this;
    }

    bool TransientBufferLease::Commit(
        const GPUCompletionToken& completion,
        const RHIBufferAccessSnapshot& finalAccess)
    {
        if (m_resolved || !m_control)
            return false;
        if (!m_control->CommitBuffer(
                m_poolIdentity,
                m_slotId,
                m_generation,
                completion,
                finalAccess))
        {
            return false;
        }
        Reset();
        return true;
    }

    bool TransientBufferLease::CanCommit(
        const GPUCompletionToken& completion) const
    {
        return !m_resolved && m_control &&
               m_control->CanCommitBuffer(
                   m_poolIdentity, m_slotId, m_generation, completion);
    }

    bool TransientBufferLease::AbortUnsubmitted()
    {
        if (m_resolved || !m_control)
            return false;
        if (!m_control->AbortBuffer(
                m_poolIdentity, m_slotId, m_generation, false))
        {
            return false;
        }
        Reset();
        return true;
    }

    bool TransientBufferLease::MarkDeviceLost()
    {
        if (m_resolved || !m_control)
            return false;
        if (!m_control->LoseBuffer(
                m_poolIdentity, m_slotId, m_generation))
        {
            return false;
        }
        Reset();
        return true;
    }

    void TransientBufferLease::AutoAbort() noexcept
    {
        if (!m_resolved && m_control)
        {
            static_cast<void>(m_control->AbortBuffer(
                m_poolIdentity, m_slotId, m_generation, true));
        }
        Reset();
    }

    void TransientBufferLease::Reset() noexcept
    {
        buffer = nullptr;
        accessSnapshot = {};
        reused = false;
        m_control.reset();
        m_poolIdentity = 0;
        m_slotId = 0;
        m_generation = 0;
        m_resolved = true;
    }

    TransientResourcePool::TransientResourcePool()
        : m_impl(std::make_shared<Impl>())
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
        m_impl->active = true;
        RVX_CORE_DEBUG("TransientResourcePool: Initialized");
    }

    void TransientResourcePool::Shutdown()
    {
        if (!m_impl->device)
        {
            return;
        }

        m_legacyTextureLeases.clear();
        m_legacyBufferLeases.clear();

        for (auto& [hash, pooled] : m_impl->texturePool)
        {
            static_cast<void>(hash);
            const GPUCompletionToken& completion =
                pooled.state == TransientResourceLeaseState::InFlight
                    ? pooled.availableAfter
                    : GPUCompletionToken{};
            m_impl->Retire(pooled.texture, completion, pooled.memorySize);
        }
        for (auto& [hash, pooled] : m_impl->bufferPool)
        {
            static_cast<void>(hash);
            const GPUCompletionToken& completion =
                pooled.state == TransientResourceLeaseState::InFlight
                    ? pooled.availableAfter
                    : GPUCompletionToken{};
            m_impl->Retire(pooled.buffer, completion, pooled.memorySize);
        }
        m_impl->texturePool.clear();
        m_impl->bufferPool.clear();
        m_impl->device = nullptr;
        m_impl->submissionTracker = nullptr;
        m_impl->retirementQueue = nullptr;
        m_impl->lastSubmission = {};
        m_impl->active = false;
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
        TransientTextureLease lease = AcquireTextureLease(desc);
        RHITexture* texture = lease.texture;
        if (!texture)
            return nullptr;

        const auto [it, inserted] =
            m_legacyTextureLeases.emplace(texture, std::move(lease));
        if (!inserted)
        {
            RVX_CORE_ERROR(
                "TransientResourcePool: duplicate legacy texture acquisition");
            return nullptr;
        }
        return it->first;
    }

    TransientTextureLease TransientResourcePool::AcquireTextureLease(
        const RHITextureDesc& desc)
    {
        if (!m_impl->device)
        {
            return {};
        }

        const uint64 hash = HashTextureDesc(desc);
        const auto control =
            std::static_pointer_cast<TransientResourceLeaseControl>(m_impl);
        const auto range = m_impl->texturePool.equal_range(hash);
        for (auto it = range.first; it != range.second; ++it)
        {
            if (!AreRHITextureDescsEquivalent(it->second.desc, desc))
                continue;
            if (it->second.state == TransientResourceLeaseState::InFlight &&
                m_impl->CanReuse(it->second.availableAfter))
            {
                it->second.state = TransientResourceLeaseState::Free;
                it->second.availableAfter = {};
                if (m_impl->stats.inFlightTextureLeases > 0)
                    --m_impl->stats.inFlightTextureLeases;
            }
            if (it->second.state == TransientResourceLeaseState::Free)
            {
                it->second.state = TransientResourceLeaseState::Recording;
                it->second.generation =
                    AdvanceLeaseGeneration(it->second.generation);
                it->second.lastUsedFrame = m_impl->currentFrame;
                ++m_impl->stats.textureHits;
                ++m_impl->stats.texturesInUse;
                ++m_impl->stats.recordingTextureLeases;
                return TransientTextureLease(
                    control,
                    m_impl->poolIdentity,
                    it->second.slotId,
                    it->second.generation,
                    it->second.texture.Get(),
                    it->second.accessSnapshot,
                    true);
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
        pooled.slotId = m_impl->nextSlotId++;
        pooled.generation = 1;
        pooled.state = TransientResourceLeaseState::Recording;
        pooled.accessSnapshot = MakeRHITextureAccessSnapshot(
            RHIResourceState::Undefined,
            RHIShaderStage::None,
            GPUQueueDomain::Graphics,
            RHIContentValidity::Invalid);
        RHITexture* result = pooled.texture.Get();
        RHITextureAccessSnapshot accessSnapshot = pooled.accessSnapshot;
        const uint64 slotId = pooled.slotId;
        const uint32 generation = pooled.generation;
        m_impl->texturePool.emplace(hash, std::move(pooled));
        ++m_impl->stats.textureMisses;
        ++m_impl->stats.texturesInUse;
        ++m_impl->stats.recordingTextureLeases;
        return TransientTextureLease(
            control,
            m_impl->poolIdentity,
            slotId,
            generation,
            result,
            std::move(accessSnapshot),
            false);
    }

    RHIBuffer* TransientResourcePool::AcquireBuffer(
        const RHIBufferDesc& desc)
    {
        TransientBufferLease lease = AcquireBufferLease(desc);
        RHIBuffer* buffer = lease.buffer;
        if (!buffer)
            return nullptr;

        const auto [it, inserted] =
            m_legacyBufferLeases.emplace(buffer, std::move(lease));
        if (!inserted)
        {
            RVX_CORE_ERROR(
                "TransientResourcePool: duplicate legacy buffer acquisition");
            return nullptr;
        }
        return it->first;
    }

    TransientBufferLease TransientResourcePool::AcquireBufferLease(
        const RHIBufferDesc& desc)
    {
        if (!m_impl->device)
        {
            return {};
        }

        const uint64 hash = HashBufferDesc(desc);
        const auto control =
            std::static_pointer_cast<TransientResourceLeaseControl>(m_impl);
        const auto range = m_impl->bufferPool.equal_range(hash);
        for (auto it = range.first; it != range.second; ++it)
        {
            if (it->second.state == TransientResourceLeaseState::InFlight &&
                m_impl->CanReuse(it->second.availableAfter))
            {
                it->second.state = TransientResourceLeaseState::Free;
                it->second.availableAfter = {};
                if (m_impl->stats.inFlightBufferLeases > 0)
                    --m_impl->stats.inFlightBufferLeases;
            }
            if (it->second.state == TransientResourceLeaseState::Free)
            {
                it->second.state = TransientResourceLeaseState::Recording;
                it->second.generation =
                    AdvanceLeaseGeneration(it->second.generation);
                it->second.lastUsedFrame = m_impl->currentFrame;
                ++m_impl->stats.bufferHits;
                ++m_impl->stats.buffersInUse;
                ++m_impl->stats.recordingBufferLeases;
                return TransientBufferLease(
                    control,
                    m_impl->poolIdentity,
                    it->second.slotId,
                    it->second.generation,
                    it->second.buffer.Get(),
                    it->second.accessSnapshot,
                    true);
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
        pooled.slotId = m_impl->nextSlotId++;
        pooled.generation = 1;
        pooled.state = TransientResourceLeaseState::Recording;
        pooled.accessSnapshot = MakeRHIBufferAccessSnapshot(
            RHIResourceState::Undefined,
            RHIShaderStage::None,
            GPUQueueDomain::Graphics,
            RHIContentValidity::Invalid);
        RHIBuffer* result = pooled.buffer.Get();
        RHIBufferAccessSnapshot accessSnapshot = pooled.accessSnapshot;
        const uint64 slotId = pooled.slotId;
        const uint32 generation = pooled.generation;
        m_impl->bufferPool.emplace(hash, std::move(pooled));
        ++m_impl->stats.bufferMisses;
        ++m_impl->stats.buffersInUse;
        ++m_impl->stats.recordingBufferLeases;
        return TransientBufferLease(
            control,
            m_impl->poolIdentity,
            slotId,
            generation,
            result,
            std::move(accessSnapshot),
            false);
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
        const auto it = m_legacyTextureLeases.find(texture);
        if (it == m_legacyTextureLeases.end())
        {
            // Transitional adapter for callers that acquired a typed lease but
            // still release through the old raw-pointer API. Generation-safe
            // production ownership uses TransientTextureLease directly.
            for (auto& [hash, pooled] : m_impl->texturePool)
            {
                static_cast<void>(hash);
                if (pooled.texture.Get() != texture ||
                    pooled.state != TransientResourceLeaseState::Recording)
                {
                    continue;
                }

                const bool submitted = m_impl->lastSubmission.count != 0;
                pooled.state = submitted
                    ? TransientResourceLeaseState::InFlight
                    : TransientResourceLeaseState::Free;
                pooled.availableAfter = submitted
                    ? m_impl->lastSubmission
                    : GPUCompletionToken{};
                pooled.accessSnapshot = finalAccess;
                if (m_impl->stats.texturesInUse > 0)
                    --m_impl->stats.texturesInUse;
                if (m_impl->stats.recordingTextureLeases > 0)
                    --m_impl->stats.recordingTextureLeases;
                if (submitted)
                    ++m_impl->stats.inFlightTextureLeases;
                return;
            }
            RVX_CORE_WARN(
                "TransientResourcePool: ReleaseTexture called on unknown texture");
            return;
        }

        const bool resolved = m_impl->lastSubmission.count != 0
            ? it->second.Commit(m_impl->lastSubmission, finalAccess)
            : it->second.AbortUnsubmitted();
        if (!resolved)
        {
            RVX_CORE_ERROR(
                "TransientResourcePool: legacy texture lease release failed closed");
            return;
        }
        m_legacyTextureLeases.erase(it);
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
        const auto it = m_legacyBufferLeases.find(buffer);
        if (it == m_legacyBufferLeases.end())
        {
            // Transitional adapter for typed leases still released through
            // the old raw-pointer API. Removed with the legacy graph executor.
            for (auto& [hash, pooled] : m_impl->bufferPool)
            {
                static_cast<void>(hash);
                if (pooled.buffer.Get() != buffer ||
                    pooled.state != TransientResourceLeaseState::Recording)
                {
                    continue;
                }

                const bool submitted = m_impl->lastSubmission.count != 0;
                pooled.state = submitted
                    ? TransientResourceLeaseState::InFlight
                    : TransientResourceLeaseState::Free;
                pooled.availableAfter = submitted
                    ? m_impl->lastSubmission
                    : GPUCompletionToken{};
                pooled.accessSnapshot = finalAccess;
                if (m_impl->stats.buffersInUse > 0)
                    --m_impl->stats.buffersInUse;
                if (m_impl->stats.recordingBufferLeases > 0)
                    --m_impl->stats.recordingBufferLeases;
                if (submitted)
                    ++m_impl->stats.inFlightBufferLeases;
                return;
            }
            RVX_CORE_WARN(
                "TransientResourcePool: ReleaseBuffer called on unknown buffer");
            return;
        }

        const bool resolved = m_impl->lastSubmission.count != 0
            ? it->second.Commit(m_impl->lastSubmission, finalAccess)
            : it->second.AbortUnsubmitted();
        if (!resolved)
        {
            RVX_CORE_ERROR(
                "TransientResourcePool: legacy buffer lease release failed closed");
            return;
        }
        m_legacyBufferLeases.erase(it);
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
            if ((pooled.state == TransientResourceLeaseState::Free ||
                 pooled.state == TransientResourceLeaseState::InFlight) &&
                m_impl->currentFrame - pooled.lastUsedFrame >= frameThreshold)
            {
                freedMemory += pooled.memorySize;
                m_impl->Retire(pooled.texture,
                               pooled.availableAfter,
                               pooled.memorySize);
                if (pooled.state == TransientResourceLeaseState::InFlight &&
                    m_impl->stats.inFlightTextureLeases > 0)
                {
                    --m_impl->stats.inFlightTextureLeases;
                }
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
            if ((pooled.state == TransientResourceLeaseState::Free ||
                 pooled.state == TransientResourceLeaseState::InFlight) &&
                m_impl->currentFrame - pooled.lastUsedFrame >= frameThreshold)
            {
                freedMemory += pooled.memorySize;
                m_impl->Retire(pooled.buffer,
                               pooled.availableAfter,
                               pooled.memorySize);
                if (pooled.state == TransientResourceLeaseState::InFlight &&
                    m_impl->stats.inFlightBufferLeases > 0)
                {
                    --m_impl->stats.inFlightBufferLeases;
                }
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
