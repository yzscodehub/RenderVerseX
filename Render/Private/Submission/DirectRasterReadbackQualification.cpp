#include "Render/Submission/DirectRasterReadbackQualification.h"

#include "Resources/RenderSubmissionTracker.h"
#include "RHI/RHICommandContext.h"
#include "RHI/RHIDevice.h"

#include <cstring>
#include <limits>

namespace RVX
{
namespace
{
    [[nodiscard]] bool IsExactCurrentGraphicsSubmissionToken(
        const RenderSubmissionTracker& tracker,
        const GPUCompletionToken& token,
        GPUCompletionPoint& outPoint) noexcept
    {
        outPoint = {};
        GPUCompletionToken normalized;
        if (!MergeGPUCompletionToken(normalized, token) || normalized.count == 0 ||
            tracker.Query(normalized) == GPUCompletionStatus::Lost)
        {
            return false;
        }
        bool hasGraphicsPoint = false;
        for (uint32 index = 0; index < normalized.count; ++index)
        {
            const GPUCompletionPoint point = normalized.points[index];
            if (!IsDeclaredGPUQueueDomain(point.domain) || point.value == 0 ||
                point.value != tracker.GetLastSubmittedValue(point.domain))
            {
                return false;
            }
            if (point.domain == GPUQueueDomain::Graphics)
            {
                if (hasGraphicsPoint)
                {
                    return false;
                }
                hasGraphicsPoint = true;
                outPoint = point;
            }
        }
        return hasGraphicsPoint;
    }

    [[nodiscard]] bool IsCompletionSatisfied(GPUCompletionStatus status) noexcept
    {
        return status == GPUCompletionStatus::Completed ||
               status == GPUCompletionStatus::CompatibilityWaitIdle;
    }

    [[nodiscard]] bool DigestsMatch(const RasterTranscriptDigest& left,
                                    const RasterTranscriptDigest& right) noexcept
    {
        return left.available == right.available &&
               left.entryCount == right.entryCount &&
               left.orderedIdentityHash == right.orderedIdentityHash &&
               left.consumedPayloadHash == right.consumedPayloadHash &&
               left.unorderedIdentityHash == right.unorderedIdentityHash &&
               left.unorderedIdentityHashSecondary ==
                   right.unorderedIdentityHashSecondary &&
               left.unorderedConsumedPayloadHash ==
                   right.unorderedConsumedPayloadHash &&
               left.unorderedConsumedPayloadHashSecondary ==
                   right.unorderedConsumedPayloadHashSecondary;
    }

    template <typename T>
    [[nodiscard]] uint32 FindFirstByteMismatch(const std::vector<T>& expected,
                                                const T* observed) noexcept
    {
        for (uint32 index = 0; index < expected.size(); ++index)
        {
            if (std::memcmp(&expected[index], &observed[index], sizeof(T)) != 0)
            {
                return index;
            }
        }
        return RVX_INVALID_INDEX;
    }

    [[nodiscard]] bool AddPayloadBytes(uint64& target, uint64 value) noexcept
    {
        if (value > std::numeric_limits<uint64>::max() - target)
        {
            return false;
        }
        target += value;
        return true;
    }
} // namespace

bool DirectRasterReadbackQualification::Arm() noexcept
{
    if (m_armed || m_recording.identity.IsValid() ||
        m_pending.submissionAccepted)
    {
        return false;
    }

    m_armed = true;
    m_diagnostics = {};
    m_diagnostics.requested = true;
    m_diagnostics.mismatch =
        DirectRasterReadbackQualificationMismatch::CopyNotRecorded;
    return true;
}

void DirectRasterReadbackQualification::RejectNoDirectLane(
    const DirectRasterReadbackRecordingIdentity& identity) noexcept
{
    if (!m_armed)
    {
        return;
    }
    m_armed = false;
    m_diagnostics.requested = true;
    m_diagnostics.required = true;
    m_diagnostics.identity = identity.IsValid();
    m_diagnostics.frameSequence = identity.frameSequence;
    m_diagnostics.recordEpoch = identity.recordEpoch;
    m_diagnostics.sourceFrameSlot = identity.sourceFrameSlot;
    m_diagnostics.mismatch = identity.IsValid()
        ? DirectRasterReadbackQualificationMismatch::DirectLaneUnavailable
        : DirectRasterReadbackQualificationMismatch::IdentityRejected;
}

bool DirectRasterReadbackQualification::Prepare(
    IRHIDevice& device,
    const DirectRasterReadbackRecordingIdentity& identity,
    const RasterInstanceStream& stream,
    const RasterInstanceStreamCache& cache,
    std::span<const DirectRasterReadbackDraw> draws,
    const RasterTranscriptDigest& cpuDirectReference)
{
    if (!m_armed)
    {
        return true;
    }

    m_armed = false;
    m_diagnostics = {};
    m_diagnostics.requested = true;
    m_diagnostics.required = true;
    m_diagnostics.identity = identity.IsValid();
    m_diagnostics.frameSequence = identity.frameSequence;
    m_diagnostics.recordEpoch = identity.recordEpoch;
    m_diagnostics.sourceFrameSlot = identity.sourceFrameSlot;
    m_diagnostics.mismatch =
        DirectRasterReadbackQualificationMismatch::ReferenceUnavailable;

    const bool allInstanced = !draws.empty() && std::all_of(
        draws.begin(), draws.end(), [](const DirectRasterReadbackDraw& draw)
        {
            return draw.instanced && draw.arguments.instanceCount != 0;
        });
    m_diagnostics.allDirectDrawsInstanced = allInstanced;
    if (!identity.IsValid() || !allInstanced)
    {
        m_diagnostics.mismatch = identity.IsValid()
            ? DirectRasterReadbackQualificationMismatch::UnsupportedNonInstancedDraw
            : DirectRasterReadbackQualificationMismatch::IdentityRejected;
        return false;
    }
    if (!stream.IsValid() || stream.activeFrameSlot != identity.sourceFrameSlot ||
        stream.activeFrameSlot >= RVX_MAX_FRAME_COUNT || !cpuDirectReference.available)
    {
        return false;
    }

    const RasterInstanceStreamFrameSlot& resident =
        cache.frameSlots[stream.activeFrameSlot];
    if (!resident.IsValid() ||
        resident.instances.Get() != stream.instances.Get() ||
        resident.instanceIndices.Get() != stream.instanceIndices.Get() ||
        resident.instanceCapacity == 0 || resident.indexCapacity == 0)
    {
        return false;
    }

    Capture capture;
    capture.identity = identity;
    capture.expectedTranscript = cpuDirectReference;
    capture.sourceInstances = stream.instances;
    capture.sourceIndices = stream.instanceIndices;
    try
    {
        capture.expectedInstances = resident.residentInstances;
        capture.expectedIndices = resident.residentDrawOrder;
        capture.residentKeys = resident.residentKeys;
        capture.residentSemanticIdentities =
            resident.residentRasterSemanticIdentities;
        capture.draws.assign(draws.begin(), draws.end());
    }
    catch (...)
    {
        return false;
    }

    if (capture.expectedInstances.size() != resident.instanceCapacity ||
        capture.expectedIndices.size() != resident.indexCapacity ||
        capture.residentKeys.size() != resident.instanceCapacity ||
        capture.residentSemanticIdentities.size() != resident.instanceCapacity)
    {
        return false;
    }

    const auto createReadback = [&device](uint64 size,
                                           uint32 stride,
                                           const char* debugName)
    {
        RHIBufferDesc desc;
        desc.size = size;
        desc.usage = RHIBufferUsage::CopyDst;
        desc.memoryType = RHIMemoryType::Readback;
        desc.stride = stride;
        desc.debugName = debugName;
        return device.CreateBuffer(desc);
    };
    try
    {
        capture.instanceReadback = createReadback(
            stream.instances->GetSize(), sizeof(GPUInstanceData),
            "DirectOpaqueRasterQualification.InstanceReadback");
        capture.indexReadback = createReadback(
            stream.instanceIndices->GetSize(), sizeof(uint32),
            "DirectOpaqueRasterQualification.Slot6IndexReadback");
    }
    catch (...)
    {
        m_diagnostics.mismatch =
            DirectRasterReadbackQualificationMismatch::ReadbackAllocationFailed;
        return false;
    }
    if (!capture.IsAllocated() ||
        capture.instanceReadback->GetSize() != stream.instances->GetSize() ||
        capture.indexReadback->GetSize() != stream.instanceIndices->GetSize())
    {
        m_diagnostics.mismatch =
            DirectRasterReadbackQualificationMismatch::ReadbackAllocationFailed;
        return false;
    }

    if (!AddPayloadBytes(capture.cpuPayloadBytes,
                         static_cast<uint64>(capture.expectedInstances.size()) *
                             sizeof(GPUInstanceData)) ||
        !AddPayloadBytes(capture.cpuPayloadBytes,
                         static_cast<uint64>(capture.expectedIndices.size()) *
                             sizeof(uint32)) ||
        !AddPayloadBytes(capture.cpuPayloadBytes,
                         static_cast<uint64>(capture.residentKeys.size()) *
                             sizeof(RasterInstanceStreamKey)) ||
        !AddPayloadBytes(capture.cpuPayloadBytes,
                         static_cast<uint64>(capture.residentSemanticIdentities.size()) *
                             sizeof(uint64)) ||
        !AddPayloadBytes(capture.cpuPayloadBytes,
                         static_cast<uint64>(capture.draws.size()) *
                             sizeof(DirectRasterReadbackDraw)))
    {
        return false;
    }

    m_recording = std::move(capture);
    m_diagnostics.readbackAllocated = true;
    m_diagnostics.cpuPayloadBytes = m_recording.cpuPayloadBytes;
    m_diagnostics.expectedTranscript = m_recording.expectedTranscript;
    m_diagnostics.mismatch =
        DirectRasterReadbackQualificationMismatch::CopyNotRecorded;
    return true;
}

bool DirectRasterReadbackQualification::RecordPostRenderCopy(
    RHICommandContext& ctx,
    const DirectRasterReadbackRecordingIdentity& identity)
{
    if (!m_recording.identity.IsValid())
    {
        return true;
    }
    if (m_recording.identity != identity || !m_recording.IsAllocated())
    {
        SetFailure(DirectRasterReadbackQualificationMismatch::IdentityRejected);
        return false;
    }

    const RHIAccessSnapshot instanceShaderAccess = MakeRHIAccessSnapshot(
        RHIResourceState::ShaderResource, RHIShaderStage::Vertex,
        GPUQueueDomain::Graphics);
    const RHIAccessSnapshot indexVertexAccess = MakeRHIAccessSnapshot(
        RHIResourceState::VertexBuffer, RHIShaderStage::Vertex,
        GPUQueueDomain::Graphics);
    const RHIAccessSnapshot copySourceAccess = MakeRHIAccessSnapshot(
        RHIResourceState::CopySource, RHIShaderStage::All,
        GPUQueueDomain::Graphics);

    if (!m_recording.sourceInstances || !m_recording.sourceIndices ||
        m_recording.sourceInstances->GetSize() !=
            m_recording.instanceReadback->GetSize() ||
        m_recording.sourceIndices->GetSize() !=
            m_recording.indexReadback->GetSize())
    {
        SetFailure(DirectRasterReadbackQualificationMismatch::ReferenceUnavailable);
        return false;
    }

    ctx.BufferBarrier(m_recording.sourceInstances.Get(),
                      instanceShaderAccess, copySourceAccess);
    ctx.BufferBarrier(m_recording.sourceIndices.Get(),
                      indexVertexAccess, copySourceAccess);
    ctx.BufferBarrier(m_recording.instanceReadback.Get(),
                      RHIResourceState::Undefined, RHIResourceState::CopyDest);
    ctx.BufferBarrier(m_recording.indexReadback.Get(),
                      RHIResourceState::Undefined, RHIResourceState::CopyDest);
    ctx.CopyBuffer(m_recording.sourceInstances.Get(),
                   m_recording.instanceReadback.Get(),
                   0, 0, m_recording.sourceInstances->GetSize());
    ctx.CopyBuffer(m_recording.sourceIndices.Get(),
                   m_recording.indexReadback.Get(),
                   0, 0, m_recording.sourceIndices->GetSize());
    ctx.BufferBarrier(m_recording.sourceInstances.Get(),
                      copySourceAccess, instanceShaderAccess);
    ctx.BufferBarrier(m_recording.sourceIndices.Get(),
                      copySourceAccess, indexVertexAccess);
    m_recording.copyRecorded = true;
    m_diagnostics.copyRecorded = true;
    m_diagnostics.mismatch =
        DirectRasterReadbackQualificationMismatch::CompletionPending;
    return true;
}

bool DirectRasterReadbackQualification::NotifySubmission(
    const DirectRasterReadbackRecordingIdentity& identity,
    const GPUCompletionToken& completion,
    const RenderSubmissionTracker& tracker)
{
    if (!m_recording.identity.IsValid())
    {
        return true;
    }
    GPUCompletionPoint graphicsPoint;
    if (!IsExactCurrentGraphicsSubmissionToken(tracker, completion, graphicsPoint))
    {
        // This completion has not proved that the recording was submitted.
        // Preserve it for the caller's exact-current retry or explicit release.
        SetFailure(DirectRasterReadbackQualificationMismatch::CompletionRejected);
        return false;
    }

    DirectRasterReadbackQualificationMismatch closureMismatch =
        DirectRasterReadbackQualificationMismatch::None;
    if (m_recording.identity != identity)
    {
        closureMismatch = DirectRasterReadbackQualificationMismatch::IdentityRejected;
    }
    else if (!m_recording.copyRecorded || !m_recording.IsAllocated() ||
             m_pending.submissionAccepted)
    {
        closureMismatch = DirectRasterReadbackQualificationMismatch::CopyNotRecorded;
    }
    m_recording.completionPoint = graphicsPoint;
    m_recording.submissionAccepted = true;
    m_recording.compareEligible = closureMismatch ==
        DirectRasterReadbackQualificationMismatch::None;
    m_recording.closureOnly = !m_recording.compareEligible;
    m_pending = std::move(m_recording);
    m_recording = {};
    m_diagnostics.submissionAccepted = true;
    m_diagnostics.completionValue = graphicsPoint.value;
    if (m_pending.compareEligible)
    {
        m_diagnostics.mismatch =
            DirectRasterReadbackQualificationMismatch::CompletionPending;
        return true;
    }
    SetFailure(closureMismatch);
    return false;
}

void DirectRasterReadbackQualification::ReleaseUnsubmitted(
    const DirectRasterReadbackRecordingIdentity& identity) noexcept
{
    if (m_recording.identity.IsValid() && m_recording.identity == identity)
    {
        m_recording = {};
        SetFailure(DirectRasterReadbackQualificationMismatch::CompletionRejected);
    }
}

bool DirectRasterReadbackQualification::PollCompletion(
    const RenderSubmissionTracker& tracker)
{
    if (!m_pending.submissionAccepted)
    {
        return true;
    }
    const GPUCompletionStatus status = tracker.Query(m_pending.completionPoint);
    if (status == GPUCompletionStatus::Pending)
    {
        if (!m_pending.closureOnly)
        {
            SetFailure(DirectRasterReadbackQualificationMismatch::CompletionPending);
        }
        return false;
    }
    if (status == GPUCompletionStatus::Lost)
    {
        SetFailure(DirectRasterReadbackQualificationMismatch::CompletionLost);
        m_pending = {};
        return false;
    }
    if (!IsCompletionSatisfied(status))
    {
        if (!m_pending.closureOnly)
        {
            SetFailure(DirectRasterReadbackQualificationMismatch::CompletionPending);
        }
        return false;
    }

    if (m_pending.closureOnly || !m_pending.compareEligible)
    {
        m_diagnostics.completionObserved = true;
        m_pending = {};
        return false;
    }

    const bool matched = CompareCompletedReadback();
    m_pending = {};
    return matched;
}

bool DirectRasterReadbackQualification::HasPendingCompletion() const noexcept
{
    return m_pending.submissionAccepted;
}

bool DirectRasterReadbackQualification::CompareCompletedReadback()
{
    if (!m_pending.IsAllocated())
    {
        SetFailure(DirectRasterReadbackQualificationMismatch::ReadbackAllocationFailed);
        return false;
    }

    void* const mappedInstances = m_pending.instanceReadback->Map();
    void* const mappedIndices = m_pending.indexReadback->Map();
    if (mappedInstances == nullptr || mappedIndices == nullptr)
    {
        if (mappedInstances != nullptr) m_pending.instanceReadback->Unmap();
        if (mappedIndices != nullptr) m_pending.indexReadback->Unmap();
        SetFailure(DirectRasterReadbackQualificationMismatch::ReadbackMapFailed);
        return false;
    }

    const auto* const observedInstances =
        static_cast<const GPUInstanceData*>(mappedInstances);
    const auto* const observedIndices = static_cast<const uint32*>(mappedIndices);
    const uint32 firstIndexMismatch = FindFirstByteMismatch(
        m_pending.expectedIndices, observedIndices);
    const uint32 firstInstanceMismatch = FindFirstByteMismatch(
        m_pending.expectedInstances, observedInstances);

    RasterTranscriptDigest observedTranscript{};
    bool transcriptReadable = true;
    BeginRasterTranscript(observedTranscript);
    for (const DirectRasterReadbackDraw& draw : m_pending.draws)
    {
        const uint64 end = static_cast<uint64>(draw.arguments.firstInstance) +
            draw.arguments.instanceCount;
        if (!draw.instanced || draw.representedPacketCount == 0 ||
            end > m_pending.expectedIndices.size())
        {
            transcriptReadable = false;
            break;
        }
        const uint64 groupIdentity = GetRasterTranscriptGroupIdentity(draw.key);
        for (uint32 offset = 0; offset < draw.arguments.instanceCount; ++offset)
        {
            const uint32 index = draw.arguments.firstInstance + offset;
            const uint32 residentRow = observedIndices[index];
            if (residentRow >= m_pending.expectedInstances.size())
            {
                transcriptReadable = false;
                break;
            }
            AppendRasterTranscriptEntry(
                observedTranscript,
                MakeRasterTranscriptEntryDigest(
                    groupIdentity,
                    m_pending.residentKeys[residentRow],
                    observedInstances[residentRow],
                    m_pending.residentSemanticIdentities[residentRow],
                    draw.key.usesMaterialParameterTable,
                    draw.arguments.indexCount,
                    draw.arguments.firstIndex,
                    draw.arguments.vertexOffset));
        }
        if (!transcriptReadable)
        {
            break;
        }
    }
    if (!transcriptReadable)
    {
        observedTranscript = {};
    }

    uint32 firstMismatchRow = firstInstanceMismatch;
    if (firstIndexMismatch != RVX_INVALID_INDEX &&
        firstIndexMismatch < m_pending.expectedIndices.size())
    {
        firstMismatchRow = observedIndices[firstIndexMismatch];
    }

    m_pending.instanceReadback->Unmap();
    m_pending.indexReadback->Unmap();

    m_diagnostics.completionObserved = true;
    m_diagnostics.compared = true;
    m_diagnostics.observedTranscript = observedTranscript;
    m_diagnostics.firstMismatchIndex = firstIndexMismatch;
    m_diagnostics.firstMismatchRow = firstMismatchRow;

    const bool transcriptMatched = transcriptReadable &&
        DigestsMatch(m_pending.expectedTranscript, observedTranscript);
    m_diagnostics.matched = firstIndexMismatch == RVX_INVALID_INDEX &&
        firstInstanceMismatch == RVX_INVALID_INDEX && transcriptMatched;
    if (m_diagnostics.matched)
    {
        m_diagnostics.mismatch = DirectRasterReadbackQualificationMismatch::None;
        return true;
    }
    if (firstIndexMismatch != RVX_INVALID_INDEX || !transcriptReadable)
    {
        SetFailure(DirectRasterReadbackQualificationMismatch::InstanceIndexPayload);
    }
    else if (firstInstanceMismatch != RVX_INVALID_INDEX)
    {
        SetFailure(DirectRasterReadbackQualificationMismatch::InstancePayload);
    }
    else
    {
        SetFailure(DirectRasterReadbackQualificationMismatch::RasterTranscript);
    }
    return false;
}

void DirectRasterReadbackQualification::SetFailure(
    DirectRasterReadbackQualificationMismatch mismatch) noexcept
{
    m_diagnostics.mismatch = mismatch;
    if (m_diagnostics.compared)
    {
        m_diagnostics.matched = false;
    }
}
} // namespace RVX
