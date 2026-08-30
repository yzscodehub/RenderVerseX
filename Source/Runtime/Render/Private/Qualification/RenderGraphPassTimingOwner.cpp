#include "Render/Qualification/RenderGraphPassTimingOwner.h"

#include "Resources/RenderSubmissionTracker.h"

#include "RHI/RHIBuffer.h"
#include "RHI/RHICommandContext.h"
#include "RHI/RHIDevice.h"
#include "RHI/RHIQuery.h"

#include <limits>
#include <utility>

namespace RVX
{
namespace
{
    [[nodiscard]] bool IsGraphicsContext(
        const RHICommandContext& context) noexcept
    {
        return context.GetQueueType() == RHICommandQueueType::Graphics;
    }

    [[nodiscard]] bool IsExactGraphicsToken(
        const GPUCompletionToken& token) noexcept
    {
        return token.count == 1 &&
            token.points[0].domain == GPUQueueDomain::Graphics &&
            token.points[0].value != 0;
    }

    [[nodiscard]] uint64 MaskTimestamp(
        uint64 timestamp,
        uint8 validBits) noexcept
    {
        if (validBits == 64)
            return timestamp;
        return timestamp & ((uint64{1} << validBits) - 1u);
    }
} // namespace

bool RenderGraphPassTimingOwner::Initialize(
    IRHIDevice* device,
    uint32 maxPassCount)
{
    if (m_enabled &&
        (m_diagnostics.state == RenderGraphPassTimingState::Recording ||
         m_diagnostics.state == RenderGraphPassTimingState::AwaitingSubmission ||
         m_diagnostics.state == RenderGraphPassTimingState::PendingCompletion))
    {
        return false;
    }
    Shutdown(false);
    m_diagnostics = {};
    m_diagnostics.state = RenderGraphPassTimingState::Unavailable;
    m_diagnostics.failure = RenderGraphPassTimingFailure::Unsupported;

    if (device == nullptr || maxPassCount == 0 ||
        maxPassCount > std::numeric_limits<uint32>::max() / 2u)
    {
        return false;
    }

    const RHICapabilities& capabilities = device->GetCapabilities();
    if (!capabilities.supportsTimestampQueries ||
        capabilities.timestampFrequency == 0)
    {
        return false;
    }

    RHIQueryPoolDesc queryDesc;
    queryDesc.type = RHIQueryType::Timestamp;
    queryDesc.queueType = RHICommandQueueType::Graphics;
    queryDesc.count = maxPassCount * 2u;
    queryDesc.debugName = "RenderGraphExactPassTiming";
    RHIQueryPoolRef queryPool = device->CreateQueryPool(queryDesc);
    if (!queryPool || queryPool->GetType() != RHIQueryType::Timestamp ||
        queryPool->GetQueueType() != RHICommandQueueType::Graphics ||
        queryPool->GetCount() != queryDesc.count)
    {
        return false;
    }

    const uint64 timestampFrequency = queryPool->GetTimestampFrequency();
    const uint8 timestampValidBits = queryPool->GetTimestampValidBits();
    if (timestampFrequency == 0 ||
        timestampFrequency != capabilities.timestampFrequency ||
        timestampValidBits != 64)
    {
        return false;
    }

    const uint64 readbackSize = static_cast<uint64>(queryDesc.count) *
        sizeof(uint64);
    RHIBufferDesc readbackDesc;
    readbackDesc.size = readbackSize;
    readbackDesc.usage = RHIBufferUsage::CopyDst;
    readbackDesc.memoryType = RHIMemoryType::Readback;
    readbackDesc.debugName = "RenderGraphExactPassTimingReadback";
    RHIBufferRef readbackBuffer = device->CreateBuffer(readbackDesc);
    if (!readbackBuffer || readbackBuffer->GetSize() != readbackSize)
    {
        return false;
    }

    try
    {
        m_samples.reserve(maxPassCount);
    }
    catch (...)
    {
        return false;
    }

    m_device = device;
    m_queryPool = std::move(queryPool);
    m_readbackBuffer = std::move(readbackBuffer);
    m_maxPassCount = maxPassCount;
    m_timestampFrequency = timestampFrequency;
    m_timestampValidBits = timestampValidBits;
    m_terminalObserverSealed = false;
    m_enabled = true;
    m_diagnostics.state = RenderGraphPassTimingState::Idle;
    m_diagnostics.failure = RenderGraphPassTimingFailure::None;
    m_diagnostics.timestampFrequency = timestampFrequency;
    m_diagnostics.timestampValidBits = timestampValidBits;
    return true;
}

void RenderGraphPassTimingOwner::Shutdown(bool deviceLost) noexcept
{
    if (deviceLost && m_enabled &&
        (m_diagnostics.state == RenderGraphPassTimingState::Recording ||
         m_diagnostics.state == RenderGraphPassTimingState::AwaitingSubmission ||
         m_diagnostics.state == RenderGraphPassTimingState::PendingCompletion))
    {
        static_cast<void>(Reject(RenderGraphPassTimingFailure::CompletionLost));
    }
    m_queryPool.Reset();
    m_readbackBuffer.Reset();
    m_samples.clear();
    m_device = nullptr;
    m_submissionTracker = nullptr;
    m_maxPassCount = 0;
    m_openPassIndex = RVX_INVALID_INDEX;
    m_completionValue = 0;
    m_timestampFrequency = 0;
    m_timestampValidBits = 0;
    m_terminalObserverSealed = false;
    m_enabled = false;
    m_diagnostics = {};
    m_diagnostics.state = RenderGraphPassTimingState::Unavailable;
    m_diagnostics.failure = RenderGraphPassTimingFailure::Unsupported;
}

bool RenderGraphPassTimingOwner::BeginExactGraphicsExecution(
    RHICommandContext& graphicsContext,
    uint32 expectedPassCount) noexcept
{
    if (m_diagnostics.state != RenderGraphPassTimingState::Idle)
        return false;
    if (!m_enabled || !m_queryPool || !m_readbackBuffer)
        return Reject(RenderGraphPassTimingFailure::Unsupported);
    if (!IsGraphicsContext(graphicsContext))
        return Reject(RenderGraphPassTimingFailure::InvalidContext);
    if (expectedPassCount == 0 || expectedPassCount > m_maxPassCount)
        return Reject(RenderGraphPassTimingFailure::InvalidPassCount);
    ClearActiveRecording();
    m_samples.clear();
    if (!graphicsContext.TryResetQueries(
            m_queryPool.Get(), 0, expectedPassCount * 2u))
    {
        return Reject(RenderGraphPassTimingFailure::QueryCommandRejected);
    }
    m_terminalObserverSealed = false;
    m_diagnostics.state = RenderGraphPassTimingState::Recording;
    m_diagnostics.failure = RenderGraphPassTimingFailure::None;
    m_diagnostics.expectedPassCount = expectedPassCount;
    m_diagnostics.recordedPassCount = 0;
    m_diagnostics.completedPassCount = 0;
    m_diagnostics.completionValue = 0;
    return true;
}

bool RenderGraphPassTimingOwner::RecordPassTimestamp(
    RHICommandContext& graphicsContext,
    uint32 executionIndex,
    std::string_view passName,
    RenderGraphPassTimestampBoundary boundary) noexcept
{
    if (!m_enabled || m_diagnostics.state != RenderGraphPassTimingState::Recording)
        return Reject(RenderGraphPassTimingFailure::InvalidBoundary);
    if (!IsGraphicsContext(graphicsContext))
        return Reject(RenderGraphPassTimingFailure::InvalidContext);
    if (passName.empty())
        return Reject(RenderGraphPassTimingFailure::PassNameMissing);

    if (boundary == RenderGraphPassTimestampBoundary::Begin)
    {
        if (m_openPassIndex != RVX_INVALID_INDEX ||
            m_samples.size() >= m_diagnostics.expectedPassCount)
        {
            return Reject(RenderGraphPassTimingFailure::PassOrderMismatch);
        }
        try
        {
            m_samples.push_back(RenderGraphPassTimingSample{
                executionIndex, std::string(passName) });
        }
        catch (...)
        {
            return Reject(RenderGraphPassTimingFailure::Unsupported);
        }
        m_openPassIndex = static_cast<uint32>(m_samples.size() - 1u);
        if (executionIndex != m_openPassIndex ||
            !graphicsContext.TryWriteTimestamp(
                m_queryPool.Get(), m_openPassIndex * 2u))
        {
            return Reject(RenderGraphPassTimingFailure::QueryCommandRejected);
        }
        return true;
    }

    if (m_openPassIndex == RVX_INVALID_INDEX ||
        m_openPassIndex >= m_samples.size() ||
        m_samples[m_openPassIndex].executionIndex != executionIndex ||
        m_samples[m_openPassIndex].passName != passName)
    {
        return Reject(RenderGraphPassTimingFailure::PassOrderMismatch);
    }
    if (!graphicsContext.TryWriteTimestamp(
            m_queryPool.Get(), m_openPassIndex * 2u + 1u))
    {
        return Reject(RenderGraphPassTimingFailure::QueryCommandRejected);
    }
    ++m_diagnostics.recordedPassCount;
    m_openPassIndex = RVX_INVALID_INDEX;
    return true;
}

bool RenderGraphPassTimingOwner::FinalizeExactGraphicsExecution(
    RHICommandContext& graphicsContext) noexcept
{
    if (!m_enabled || m_diagnostics.state != RenderGraphPassTimingState::Recording)
        return Reject(RenderGraphPassTimingFailure::InvalidBoundary);
    if (!IsGraphicsContext(graphicsContext))
        return Reject(RenderGraphPassTimingFailure::InvalidContext);
    if (m_openPassIndex != RVX_INVALID_INDEX ||
        m_diagnostics.recordedPassCount != m_diagnostics.expectedPassCount ||
        m_samples.size() != m_diagnostics.expectedPassCount)
    {
        return Reject(RenderGraphPassTimingFailure::MissingPass);
    }

    const uint32 queryCount = m_diagnostics.expectedPassCount * 2u;
    if (!m_readbackBuffer || queryCount == 0 ||
        static_cast<uint64>(queryCount) > m_readbackBuffer->GetSize() / sizeof(uint64))
    {
        return Reject(RenderGraphPassTimingFailure::QueryResolveFailed);
    }
    if (!graphicsContext.TryResolveQueries(
            m_queryPool.Get(), 0, queryCount, m_readbackBuffer.Get(), 0))
    {
        return Reject(RenderGraphPassTimingFailure::QueryCommandRejected);
    }
    m_diagnostics.state = RenderGraphPassTimingState::AwaitingSubmission;
    return true;
}

std::shared_ptr<IRenderGraphExecutionTerminalObserver>
RenderGraphPassTimingOwner::SealExecutionTerminalObserver() noexcept
{
    if (!m_enabled ||
        m_diagnostics.state != RenderGraphPassTimingState::AwaitingSubmission ||
        m_terminalObserverSealed)
    {
        return {};
    }
    try
    {
        std::shared_ptr<RenderGraphPassTimingOwner> self = shared_from_this();
        m_terminalObserverSealed = true;
        return self;
    }
    catch (...)
    {
        static_cast<void>(Reject(
            RenderGraphPassTimingFailure::SubmissionRejected));
        return {};
    }
}

void RenderGraphPassTimingOwner::AbortExactGraphicsExecution(
    const char* reason) noexcept
{
    (void)reason;
    if (m_enabled &&
        m_diagnostics.state != RenderGraphPassTimingState::Completed &&
        m_diagnostics.state != RenderGraphPassTimingState::Rejected)
    {
        static_cast<void>(Reject(RenderGraphPassTimingFailure::QueryResolveFailed));
    }
}

void RenderGraphPassTimingOwner::OnRenderGraphSubmitted(
    const GPUCompletionToken& completion) noexcept
{
    if (!m_enabled ||
        m_diagnostics.state != RenderGraphPassTimingState::AwaitingSubmission ||
        !m_terminalObserverSealed ||
        !IsExactGraphicsToken(completion))
    {
        static_cast<void>(Reject(
            RenderGraphPassTimingFailure::SubmissionRejected));
        return;
    }
    m_completionValue = completion.points[0].value;
    m_diagnostics.completionValue = m_completionValue;
    m_diagnostics.state = RenderGraphPassTimingState::PendingCompletion;
}

void RenderGraphPassTimingOwner::OnRenderGraphAborted() noexcept
{
    if (m_enabled && m_diagnostics.state != RenderGraphPassTimingState::Completed)
    {
        static_cast<void>(Reject(
            RenderGraphPassTimingFailure::SubmissionRejected));
    }
}

void RenderGraphPassTimingOwner::OnRenderGraphDeviceLost() noexcept
{
    if (m_enabled && m_diagnostics.state != RenderGraphPassTimingState::Completed)
    {
        static_cast<void>(Reject(RenderGraphPassTimingFailure::CompletionLost));
    }
}

void RenderGraphPassTimingOwner::OnRenderGraphRetired() noexcept
{
    if (m_enabled &&
        m_diagnostics.state == RenderGraphPassTimingState::PendingCompletion)
    {
        static_cast<void>(CompleteReadback());
    }
}

bool RenderGraphPassTimingOwner::PollCompletion(
    const RenderSubmissionTracker& tracker) noexcept
{
    if (m_diagnostics.state == RenderGraphPassTimingState::Completed)
        return true;
    if (!m_enabled ||
        m_diagnostics.state != RenderGraphPassTimingState::PendingCompletion ||
        m_completionValue == 0)
    {
        return Reject(RenderGraphPassTimingFailure::SubmissionRejected);
    }

    const GPUCompletionToken completion{
        { GPUCompletionPoint{ GPUQueueDomain::Graphics, m_completionValue } }, 1 };
    const GPUCompletionStatus status = tracker.Query(completion);
    if (status == GPUCompletionStatus::Pending)
        return false;
    if (status != GPUCompletionStatus::Completed)
        return Reject(RenderGraphPassTimingFailure::CompletionLost);
    m_submissionTracker = &tracker;
    return CompleteReadback();
}

bool RenderGraphPassTimingOwner::CompleteReadback() noexcept
{
    if (!m_readbackBuffer)
        return Reject(RenderGraphPassTimingFailure::ReadbackMapFailed);

    const auto* timestamps = static_cast<const uint64*>(m_readbackBuffer->Map());
    if (timestamps == nullptr)
        return Reject(RenderGraphPassTimingFailure::ReadbackMapFailed);

    bool valid = true;
    bool nonAdvancing = false;
    for (uint32 index = 0;
         index < static_cast<uint32>(m_samples.size());
         ++index)
    {
        const uint64 start = MaskTimestamp(timestamps[index * 2u], m_timestampValidBits);
        const uint64 end = MaskTimestamp(timestamps[index * 2u + 1u], m_timestampValidBits);
        // Consecutive timestamps around a culled-adjacent or command-light
        // pass may legitimately quantize to the same non-zero device tick.
        // Preserve that exact 0 ms sample. A zero/zero pair remains an
        // unwritten query and a decreasing pair remains a wrap/corruption.
        if (end < start || (start == 0 && end == 0))
        {
            valid = false;
            nonAdvancing = end == start;
            break;
        }
        RenderGraphPassTimingSample& sample = m_samples[index];
        sample.startTimestamp = start;
        sample.endTimestamp = end;
        sample.elapsedTimestampTicks = end - start;
        sample.durationMilliseconds =
            (static_cast<float64>(sample.elapsedTimestampTicks) * 1000.0) /
            static_cast<float64>(m_timestampFrequency);
    }
    m_readbackBuffer->Unmap();

    if (!valid)
    {
        return Reject(nonAdvancing
            ? RenderGraphPassTimingFailure::TimestampNonAdvancing
            : RenderGraphPassTimingFailure::TimestampWrapped);
    }

    m_diagnostics.completedPassCount = static_cast<uint32>(m_samples.size());
    m_diagnostics.failure = RenderGraphPassTimingFailure::None;
    m_diagnostics.state = RenderGraphPassTimingState::Completed;
    return true;
}

void RenderGraphPassTimingOwner::ResetCompletedTiming() noexcept
{
    if (!m_enabled ||
        (m_diagnostics.state != RenderGraphPassTimingState::Completed &&
         m_diagnostics.state != RenderGraphPassTimingState::Rejected))
    {
        return;
    }
    ClearActiveRecording();
    m_samples.clear();
    m_diagnostics.state = RenderGraphPassTimingState::Idle;
    m_diagnostics.failure = RenderGraphPassTimingFailure::None;
    m_diagnostics.expectedPassCount = 0;
    m_diagnostics.recordedPassCount = 0;
    m_diagnostics.completedPassCount = 0;
    m_diagnostics.completionValue = 0;
    m_terminalObserverSealed = false;
}

bool RenderGraphPassTimingOwner::Reject(
    RenderGraphPassTimingFailure failure) noexcept
{
    ClearActiveRecording();
    m_samples.clear();
    m_diagnostics.state = RenderGraphPassTimingState::Rejected;
    m_diagnostics.failure = failure;
    m_diagnostics.completedPassCount = 0;
    m_diagnostics.completionValue = 0;
    m_terminalObserverSealed = false;
    return false;
}

void RenderGraphPassTimingOwner::ClearActiveRecording() noexcept
{
    m_submissionTracker = nullptr;
    m_openPassIndex = RVX_INVALID_INDEX;
    m_completionValue = 0;
}
} // namespace RVX
