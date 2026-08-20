/** @file RenderGraphExecution.cpp @brief Completion-owned RenderGraph realization */

#include "Render/Graph/RenderGraphExecution.h"

#include "Core/Assert.h"
#include "Render/Graph/TransientResourcePool.h"
#include "Resources/RenderSubmissionTracker.h"
#include "RHI/RHI.h"

#include <utility>
#include <vector>

namespace RVX
{
    class RenderGraphExecution::Impl
    {
    public:
        struct TextureLeaseRecord
        {
            TransientTextureLease lease;
            RHITextureAccessSnapshot finalAccess;
        };

        struct BufferLeaseRecord
        {
            TransientBufferLease lease;
            RHIBufferAccessSnapshot finalAccess;
        };

        void ClearOwnership()
        {
            textureLeases.clear();
            bufferLeases.clear();
            textures.clear();
            buffers.clear();
            heaps.clear();
            resources.clear();
            queuePlan = {};
            ownedContexts.clear();
        }

        RenderGraphExecutionState state = RenderGraphExecutionState::Invalid;
        std::vector<TextureLeaseRecord> textureLeases;
        std::vector<BufferLeaseRecord> bufferLeases;
        std::vector<RHITextureRef> textures;
        std::vector<RHIBufferRef> buffers;
        std::vector<RHIHeapRef> heaps;
        std::vector<Ref<RefCounted>> resources;
        RHIQueueSubmissionPlan queuePlan;
        std::vector<RHICommandContextRef> ownedContexts;
    };

    RenderGraphExecution::RenderGraphExecution()
        : m_impl(std::make_unique<Impl>())
    {
    }

    RenderGraphExecution::~RenderGraphExecution()
    {
        if (!m_impl)
            return;
        if (m_impl->state == RenderGraphExecutionState::Prepared ||
            m_impl->state == RenderGraphExecutionState::Recorded ||
            m_impl->state == RenderGraphExecutionState::Adopted)
        {
            static_cast<void>(AbortUnsubmitted());
        }
        RVX_ASSERT_MSG(
            m_impl->state != RenderGraphExecutionState::Submitted,
            "Submitted RenderGraphExecution must retire after GPU completion");
    }

    RenderGraphExecution::RenderGraphExecution(
        RenderGraphExecution&& other) noexcept = default;

    RenderGraphExecution& RenderGraphExecution::operator=(
        RenderGraphExecution&& other) noexcept
    {
        if (this == &other)
            return *this;
        if (m_impl &&
            (m_impl->state == RenderGraphExecutionState::Prepared ||
             m_impl->state == RenderGraphExecutionState::Recorded ||
             m_impl->state == RenderGraphExecutionState::Adopted))
        {
            static_cast<void>(AbortUnsubmitted());
        }
        RVX_ASSERT_MSG(
            !m_impl || m_impl->state != RenderGraphExecutionState::Submitted,
            "Cannot overwrite a submitted RenderGraphExecution");
        m_impl = std::move(other.m_impl);
        return *this;
    }

    RenderGraphExecution::operator bool() const noexcept
    {
        return m_impl && m_impl->state != RenderGraphExecutionState::Invalid;
    }

    RenderGraphExecutionState RenderGraphExecution::GetState() const noexcept
    {
        return m_impl ? m_impl->state : RenderGraphExecutionState::Invalid;
    }

    bool RenderGraphExecution::HasQueueSubmissionPlan() const noexcept
    {
        return m_impl && !m_impl->queuePlan.batches.empty();
    }

    void RenderGraphExecution::Prepare()
    {
        RVX_ASSERT_MSG(m_impl != nullptr, "Execution implementation is missing");
        RVX_ASSERT_MSG(m_impl->state == RenderGraphExecutionState::Invalid,
                       "Execution can only be prepared once");
        m_impl->state = RenderGraphExecutionState::Prepared;
    }

    bool RenderGraphExecution::MarkRecorded()
    {
        if (!m_impl || m_impl->state != RenderGraphExecutionState::Prepared)
            return false;
        m_impl->state = RenderGraphExecutionState::Recorded;
        return true;
    }

    bool RenderGraphExecution::MarkAdopted()
    {
        if (!m_impl || m_impl->state != RenderGraphExecutionState::Recorded)
            return false;
        m_impl->state = RenderGraphExecutionState::Adopted;
        return true;
    }

    bool RenderGraphExecution::Commit(
        const GPUCompletionToken& completion)
    {
        if (!m_impl || m_impl->state != RenderGraphExecutionState::Adopted ||
            completion.count == 0)
        {
            return false;
        }

        for (const Impl::TextureLeaseRecord& record : m_impl->textureLeases)
        {
            if (!record.lease.CanCommit(completion))
                return false;
        }
        for (const Impl::BufferLeaseRecord& record : m_impl->bufferLeases)
        {
            if (!record.lease.CanCommit(completion))
                return false;
        }

        for (Impl::TextureLeaseRecord& record : m_impl->textureLeases)
        {
            if (!record.lease.Commit(completion, record.finalAccess))
                return false;
        }
        for (Impl::BufferLeaseRecord& record : m_impl->bufferLeases)
        {
            if (!record.lease.Commit(completion, record.finalAccess))
                return false;
        }
        m_impl->textureLeases.clear();
        m_impl->bufferLeases.clear();
        m_impl->state = RenderGraphExecutionState::Submitted;
        return true;
    }

    bool RenderGraphExecution::AbortUnsubmitted()
    {
        if (!m_impl ||
            (m_impl->state != RenderGraphExecutionState::Prepared &&
             m_impl->state != RenderGraphExecutionState::Recorded &&
             m_impl->state != RenderGraphExecutionState::Adopted))
        {
            return false;
        }
        bool success = true;
        for (Impl::TextureLeaseRecord& record : m_impl->textureLeases)
            success = record.lease.AbortUnsubmitted() && success;
        for (Impl::BufferLeaseRecord& record : m_impl->bufferLeases)
            success = record.lease.AbortUnsubmitted() && success;
        m_impl->ClearOwnership();
        m_impl->state = RenderGraphExecutionState::AbortUnsubmitted;
        return success;
    }

    bool RenderGraphExecution::MarkDeviceLost()
    {
        if (!m_impl || m_impl->state == RenderGraphExecutionState::Invalid ||
            m_impl->state == RenderGraphExecutionState::Retired ||
            m_impl->state == RenderGraphExecutionState::AbortUnsubmitted ||
            m_impl->state == RenderGraphExecutionState::DeviceLost)
        {
            return false;
        }
        bool success = true;
        for (Impl::TextureLeaseRecord& record : m_impl->textureLeases)
            success = record.lease.MarkDeviceLost() && success;
        for (Impl::BufferLeaseRecord& record : m_impl->bufferLeases)
            success = record.lease.MarkDeviceLost() && success;
        m_impl->ClearOwnership();
        m_impl->state = RenderGraphExecutionState::DeviceLost;
        return success;
    }

    bool RenderGraphExecution::Retire()
    {
        if (!m_impl || m_impl->state != RenderGraphExecutionState::Submitted)
            return false;
        m_impl->ClearOwnership();
        m_impl->state = RenderGraphExecutionState::Retired;
        return true;
    }

    void RenderGraphExecution::AddTextureLease(
        TransientTextureLease&& lease,
        RHITextureAccessSnapshot finalAccess)
    {
        RVX_ASSERT_MSG(m_impl &&
                           m_impl->state == RenderGraphExecutionState::Prepared,
                       "Texture leases must be added while preparing execution");
        m_impl->textureLeases.push_back(
            {std::move(lease), std::move(finalAccess)});
    }

    void RenderGraphExecution::AddBufferLease(
        TransientBufferLease&& lease,
        RHIBufferAccessSnapshot finalAccess)
    {
        RVX_ASSERT_MSG(m_impl &&
                           m_impl->state == RenderGraphExecutionState::Prepared,
                       "Buffer leases must be added while preparing execution");
        m_impl->bufferLeases.push_back(
            {std::move(lease), std::move(finalAccess)});
    }

    void RenderGraphExecution::RetainTexture(RHITextureRef texture)
    {
        if (texture)
            m_impl->textures.push_back(std::move(texture));
    }

    void RenderGraphExecution::RetainBuffer(RHIBufferRef buffer)
    {
        if (buffer)
            m_impl->buffers.push_back(std::move(buffer));
    }

    void RenderGraphExecution::RetainHeap(RHIHeapRef heap)
    {
        if (heap)
            m_impl->heaps.push_back(std::move(heap));
    }

    void RenderGraphExecution::RetainResource(Ref<RefCounted> resource)
    {
        if (resource)
            m_impl->resources.push_back(std::move(resource));
    }

    void RenderGraphExecution::SetQueueSubmission(
        RHIQueueSubmissionPlan plan,
        std::vector<RHICommandContextRef> ownedContexts)
    {
        RVX_ASSERT_MSG(m_impl &&
                           m_impl->state == RenderGraphExecutionState::Recorded,
                       "Queue submission attaches only to a recorded execution");
        m_impl->queuePlan = std::move(plan);
        m_impl->ownedContexts = std::move(ownedContexts);
    }

    bool RenderGraphExecution::TakeQueueSubmission(
        RHIQueueSubmissionPlan& plan,
        std::vector<RHICommandContextRef>& ownedContexts)
    {
        if (!m_impl || m_impl->state != RenderGraphExecutionState::Recorded ||
            m_impl->queuePlan.batches.empty())
        {
            return false;
        }
        plan = std::move(m_impl->queuePlan);
        ownedContexts = std::move(m_impl->ownedContexts);
        return true;
    }
} // namespace RVX
