#include "Resources/RenderRetirementQueue.h"

#include "Core/Assert.h"

#include <algorithm>
#include <cstddef>
#include <limits>

namespace RVX
{
    namespace
    {
        constexpr uint8 DomainIndex(GPUQueueDomain domain)
        {
            return static_cast<uint8>(domain);
        }

        bool IsSatisfied(GPUCompletionStatus status)
        {
            return status == GPUCompletionStatus::Completed ||
                   status == GPUCompletionStatus::CompatibilityWaitIdle;
        }
    } // namespace

    RenderRetirementQueue::~RenderRetirementQueue()
    {
        RVX_ASSERT_MSG(m_entries.empty(),
                       "Render retirement queue requires explicit teardown");
    }

    bool RenderRetirementQueue::Initialize(RenderSubmissionTracker* tracker)
    {
        if (!tracker || !m_entries.empty() ||
            tracker->GetTopology().completionMode == RHIQueueCompletionMode::None ||
            tracker->GetTopology().activeDomainCount == 0)
        {
            return false;
        }

        if (m_renderThreadGuard.BindCurrentThread() != RenderThreadGuardCode::Owner)
        {
            return false;
        }

        if (m_tracker && m_tracker != tracker)
        {
            return false;
        }

        m_tracker = tracker;
        return true;
    }

    bool RenderRetirementQueue::Enqueue(RenderRetirementEntry&& entry)
    {
        if (!m_tracker || !IsOnRenderThread() || !entry.object)
        {
            return false;
        }

        GPUCompletionToken normalized;
        if (!MergeGPUCompletionToken(normalized, entry.completion))
        {
            return false;
        }
        entry.completion = normalized;

        if (entry.completion.count == 0)
        {
            entry.object.Reset();
            return true;
        }

        m_entries.push_back(std::move(entry));
        return true;
    }

    GPUCompletionStatus RenderRetirementQueue::Poll()
    {
        if (!m_tracker || !IsOnRenderThread())
        {
            return GPUCompletionStatus::Lost;
        }

        bool pending = false;
        bool lost = false;
        bool usedCompatibility = false;
        for (auto entry = m_entries.begin(); entry != m_entries.end();)
        {
            GPUCompletionStatus status = m_tracker->Query(entry->completion);
            if (status == GPUCompletionStatus::Pending &&
                m_tracker->GetTopology().completionMode ==
                    RHIQueueCompletionMode::CompatibilityWaitIdle)
            {
                status = m_tracker->Wait(entry->completion);
            }

            if (status == GPUCompletionStatus::Lost)
            {
                lost = true;
                ++entry;
                continue;
            }
            if (!IsSatisfied(status))
            {
                pending = true;
                ++entry;
                continue;
            }

            usedCompatibility |=
                status == GPUCompletionStatus::CompatibilityWaitIdle;
            entry = m_entries.erase(entry);
        }

        if (lost)
        {
            return GPUCompletionStatus::Lost;
        }
        if (pending)
        {
            return GPUCompletionStatus::Pending;
        }
        return usedCompatibility
            ? GPUCompletionStatus::CompatibilityWaitIdle
            : GPUCompletionStatus::Completed;
    }

    GPUCompletionStatus RenderRetirementQueue::ForceDeviceLostTeardown()
    {
        if (!m_tracker || !IsOnRenderThread())
        {
            return GPUCompletionStatus::Lost;
        }

        m_entries.clear();
        return GPUCompletionStatus::Lost;
    }

    RenderRetirementDiagnostics RenderRetirementQueue::GetDiagnostics() const
    {
        RenderRetirementDiagnostics diagnostics;
        if (!m_tracker || !IsOnRenderThread())
        {
            return diagnostics;
        }

        diagnostics.entryCount = static_cast<uint32>(std::min<std::size_t>(
            m_entries.size(), std::numeric_limits<uint32>::max()));
        for (const RenderRetirementEntry& entry : m_entries)
        {
            diagnostics.estimatedBytes =
                entry.estimatedBytes >
                    std::numeric_limits<uint64>::max() - diagnostics.estimatedBytes
                ? std::numeric_limits<uint64>::max()
                : diagnostics.estimatedBytes + entry.estimatedBytes;

            for (uint8 i = 0; i < entry.completion.count; ++i)
            {
                const GPUCompletionPoint point = entry.completion.points[i];
                const GPUCompletionStatus status = m_tracker->Query(point);
                if (status == GPUCompletionStatus::Completed ||
                    status == GPUCompletionStatus::CompatibilityWaitIdle)
                {
                    continue;
                }
                const uint8 domain = DomainIndex(point.domain);
                uint64& oldest = diagnostics.oldestPendingCompletionValues[domain];
                if (oldest == 0 || point.value < oldest)
                {
                    oldest = point.value;
                }
            }
        }
        return diagnostics;
    }

    bool RenderRetirementQueue::IsOnRenderThread() const
    {
        return m_renderThreadGuard.QueryCurrentThread() ==
               RenderThreadGuardCode::Owner;
    }
} // namespace RVX
