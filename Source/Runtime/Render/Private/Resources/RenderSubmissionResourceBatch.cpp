#include "Resources/RenderSubmissionResourceBatch.h"

#include "Core/Assert.h"
#include "Resources/RenderRetirementQueue.h"

#include <algorithm>
#include <limits>

namespace RVX
{
    RenderSubmissionResourceBatch::RenderSubmissionResourceBatch()
    {
        const RenderThreadGuardCode code = m_renderThreadGuard.BindCurrentThread();
        RVX_ASSERT_MSG(code == RenderThreadGuardCode::Owner,
                       "Submission resource batch must bind to Render Thread");
    }

    RenderSubmissionResourceBatch::~RenderSubmissionResourceBatch()
    {
        RVX_ASSERT_MSG(m_retained.empty(),
                       "Submission resource batch requires explicit transfer or release");
    }

    bool RenderSubmissionResourceBatch::Retain(Ref<RefCounted> object,
                                                uint64 estimatedBytes)
    {
        if (!object || m_sealed || !IsOnRenderThread())
        {
            return false;
        }

        const auto existing = std::find_if(
            m_retained.begin(), m_retained.end(),
            [&object](const RetainedObject& retained)
            {
                return retained.object.Get() == object.Get();
            });
        if (existing != m_retained.end())
        {
            existing->estimatedBytes = std::max(existing->estimatedBytes,
                                                estimatedBytes);
            return true;
        }

        m_retained.push_back({std::move(object), estimatedBytes});
        return true;
    }

    void RenderSubmissionResourceBatch::SealAndTransfer(
        const GPUCompletionToken& completion,
        RenderRetirementQueue& retirement)
    {
        Transfer(completion, retirement);
    }

    void RenderSubmissionResourceBatch::ReleaseUnsubmitted(
        RenderRetirementQueue& retirement)
    {
        Transfer({}, retirement);
    }

    uint32 RenderSubmissionResourceBatch::GetRetainedObjectCount() const noexcept
    {
        return static_cast<uint32>(std::min<std::size_t>(
            m_retained.size(), std::numeric_limits<uint32>::max()));
    }

    bool RenderSubmissionResourceBatch::IsOnRenderThread() const noexcept
    {
        return m_renderThreadGuard.QueryCurrentThread() ==
               RenderThreadGuardCode::Owner;
    }

    void RenderSubmissionResourceBatch::Transfer(
        const GPUCompletionToken& completion,
        RenderRetirementQueue& retirement)
    {
        RVX_ASSERT_MSG(!m_sealed, "Submission resource batch cannot be sealed twice");
        RVX_ASSERT_MSG(IsOnRenderThread(),
                       "Submission resource batch transfer must run on Render Thread");

        GPUCompletionToken normalized;
        RVX_ASSERT_MSG(MergeGPUCompletionToken(normalized, completion),
                       "Submission resource batch received an invalid completion token");

        m_sealed = true;
        for (RetainedObject& retained : m_retained)
        {
            RenderRetirementEntry entry{
                normalized,
                std::move(retained.object),
                retained.estimatedBytes,
            };
            RVX_ASSERT_MSG(retirement.Enqueue(std::move(entry)),
                           "Submission resource batch retirement transfer failed");
        }
        m_retained.clear();
    }
} // namespace RVX
