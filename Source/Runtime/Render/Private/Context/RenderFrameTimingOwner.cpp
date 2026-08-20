/**
 * @file RenderFrameTimingOwner.cpp
 * @brief Completion-owned whole-frame Graphics timestamp implementation.
 */

#include "Context/RenderFrameTimingOwner.h"

#include "RHI/RHIBuffer.h"
#include "RHI/RHICommandContext.h"
#include "RHI/RHIDevice.h"
#include "RHI/RHIQuery.h"

#include <utility>

namespace RVX
{
    namespace
    {
        constexpr uint64 TimestampPairByteSize = sizeof(uint64) * 2u;
    } // namespace

    bool RenderFrameTimingOwner::Initialize(IRHIDevice* device, uint32 frameCount)
    {
        Shutdown(false);
        m_diagnostics = {};
        SetUnavailable("Graphics timestamp queries are unavailable on this device.");

        if (device == nullptr || frameCount == 0 || frameCount > RVX_MAX_FRAME_COUNT)
        {
            SetUnavailable("Render frame timing received an invalid device or frame-slot count.");
            return false;
        }

        const RHICapabilities& capabilities = device->GetCapabilities();
        if (!capabilities.supportsTimestampQueries ||
            capabilities.timestampFrequency == 0)
        {
            SetUnavailable(
                "The active RHI device does not expose coherent timestamp-query capability.");
            return false;
        }

        RHIQueryPoolDesc queryDesc;
        queryDesc.type = RHIQueryType::Timestamp;
        queryDesc.queueType = RHICommandQueueType::Graphics;
        queryDesc.count = frameCount * 2u;
        queryDesc.debugName = "RenderContextWholeFrameGraphicsTiming";
        RHIQueryPoolRef queryPool = device->CreateQueryPool(queryDesc);
        if (!queryPool || queryPool->GetType() != RHIQueryType::Timestamp ||
            queryPool->GetCount() != queryDesc.count ||
            queryPool->GetQueueType() != RHICommandQueueType::Graphics)
        {
            SetUnavailable("The RHI device could not create a Graphics timestamp query pool.");
            return false;
        }

        const uint64 timestampFrequency = queryPool->GetTimestampFrequency();
        const uint32 timestampValidBits = queryPool->GetTimestampValidBits();
        if (timestampFrequency == 0 || timestampValidBits == 0 ||
            timestampValidBits > 64u ||
            timestampFrequency != capabilities.timestampFrequency)
        {
            SetUnavailable("The Graphics timestamp query pool reported incoherent timing metadata.");
            return false;
        }

        std::array<Slot, RVX_MAX_FRAME_COUNT> slots;
        for (uint32 frameIndex = 0; frameIndex < frameCount; ++frameIndex)
        {
            RHIBufferDesc readbackDesc;
            readbackDesc.size = TimestampPairByteSize;
            readbackDesc.usage = RHIBufferUsage::CopyDst;
            readbackDesc.memoryType = RHIMemoryType::Readback;
            readbackDesc.debugName = "RenderContextWholeFrameGraphicsTimingReadback";
            slots[frameIndex].readbackBuffer = device->CreateBuffer(readbackDesc);
            if (!slots[frameIndex].readbackBuffer)
            {
                SetUnavailable("The RHI device could not create a frame-timing readback buffer.");
                return false;
            }
        }

        m_device = device;
        m_queryPool = std::move(queryPool);
        m_slots = std::move(slots);
        m_frameCount = frameCount;
        m_timestampFrequency = timestampFrequency;
        m_timestampValidBits = timestampValidBits;
        m_enabled = true;
        SetUnavailable("No completed Graphics frame-timing sample is available yet.");
        return true;
    }

    void RenderFrameTimingOwner::Shutdown(bool deviceLost) noexcept
    {
        if (deviceLost)
        {
            MarkAllLost();
        }
        else
        {
            for (Slot& slot : m_slots)
            {
                ClearSlot(slot);
            }
        }

        m_queryPool.Reset();
        for (Slot& slot : m_slots)
        {
            slot.readbackBuffer.Reset();
        }
        m_device = nullptr;
        m_frameCount = 0;
        m_timestampFrequency = 0;
        m_timestampValidBits = 0;
        m_enabled = false;
    }

    void RenderFrameTimingOwner::BeginFrame(
        uint32 frameIndex,
        RHICommandContext& graphicsContext) noexcept
    {
        if (!m_enabled || frameIndex >= m_frameCount || !m_queryPool ||
            graphicsContext.GetQueueType() != RHICommandQueueType::Graphics)
        {
            return;
        }

        Slot& slot = m_slots[frameIndex];
        if (slot.state != SlotState::Idle)
        {
            // RenderContext always drains this slot after WaitForFrame before it
            // can be re-recorded. Preserve rendering if that invariant is broken,
            // but never reuse unread data as a fresh timing sample.
            ++m_diagnostics.droppedSampleCount;
            SetUnavailable("A Graphics frame-timing slot was reused before its prior sample drained.");
            ClearSlot(slot);
        }

        const uint32 firstQuery = frameIndex * 2u;
        graphicsContext.ResetQueries(m_queryPool.Get(), firstQuery, 2u);
        graphicsContext.WriteTimestamp(m_queryPool.Get(), firstQuery);
        slot.state = SlotState::Recording;
    }

    void RenderFrameTimingOwner::EndFrame(
        uint32 frameIndex,
        RHICommandContext& graphicsContext) noexcept
    {
        if (!m_enabled || frameIndex >= m_frameCount || !m_queryPool ||
            graphicsContext.GetQueueType() != RHICommandQueueType::Graphics)
        {
            return;
        }

        Slot& slot = m_slots[frameIndex];
        if (slot.state != SlotState::Recording || !slot.readbackBuffer)
        {
            return;
        }

        const uint32 firstQuery = frameIndex * 2u;
        graphicsContext.WriteTimestamp(m_queryPool.Get(), firstQuery + 1u);
        graphicsContext.ResolveQueries(m_queryPool.Get(), firstQuery, 2u,
                                      slot.readbackBuffer.Get(), 0u);
        slot.state = SlotState::AwaitingSubmission;
    }

    void RenderFrameTimingOwner::MarkSubmitted(
        uint32 frameIndex,
        GPUCompletionPoint submittedPoint) noexcept
    {
        if (!m_enabled || frameIndex >= m_frameCount)
        {
            return;
        }

        Slot& slot = m_slots[frameIndex];
        if (slot.state != SlotState::AwaitingSubmission)
        {
            return;
        }

        if (submittedPoint.domain != GPUQueueDomain::Graphics ||
            submittedPoint.value == 0)
        {
            ++m_diagnostics.droppedSampleCount;
            ClearSlot(slot);
            return;
        }

        slot.submittedPoint = submittedPoint;
        slot.state = SlotState::Submitted;
    }

    bool RenderFrameTimingOwner::BindSubmittedFrame(
        GPUCompletionPoint submittedPoint,
        uint64 sourceFrameSequence) noexcept
    {
        if (!m_enabled || submittedPoint.domain != GPUQueueDomain::Graphics ||
            submittedPoint.value == 0)
        {
            return false;
        }

        for (uint32 frameIndex = 0; frameIndex < m_frameCount; ++frameIndex)
        {
            Slot& slot = m_slots[frameIndex];
            if (slot.state != SlotState::Submitted ||
                slot.submittedPoint != submittedPoint)
            {
                continue;
            }

            if (!slot.sourceBound)
            {
                slot.sourceBound = true;
                slot.sourceFrameSequence = sourceFrameSequence;
                return true;
            }
            return slot.sourceFrameSequence == sourceFrameSequence;
        }

        return false;
    }

    void RenderFrameTimingOwner::DrainCompletedSlot(
        uint32 frameIndex,
        GPUCompletionPoint completedPoint) noexcept
    {
        if (!m_enabled || frameIndex >= m_frameCount)
        {
            return;
        }

        Slot& slot = m_slots[frameIndex];
        if (slot.state != SlotState::Submitted ||
            slot.submittedPoint != completedPoint ||
            completedPoint.domain != GPUQueueDomain::Graphics ||
            completedPoint.value == 0)
        {
            return;
        }

        if (!slot.readbackBuffer)
        {
            ++m_diagnostics.droppedSampleCount;
            SetUnavailable("The completed Graphics frame-timing sample lost its readback buffer.");
            ClearSlot(slot);
            return;
        }

        const auto* timestamps = static_cast<const uint64*>(slot.readbackBuffer->Map());
        if (timestamps == nullptr)
        {
            ++m_diagnostics.droppedSampleCount;
            SetUnavailable("Mapping the completed Graphics frame-timing readback buffer failed.");
            ClearSlot(slot);
            return;
        }

        const uint64 startTimestamp = timestamps[0];
        const uint64 endTimestamp = timestamps[1];
        slot.readbackBuffer->Unmap();

        // Completion counts physical timestamp pairs read from a proven
        // completion point. An unbound pair intentionally remains invisible
        // as a source-frame metric, but still proves that the slot drained.
        ++m_diagnostics.completedSampleCount;
        if (slot.sourceBound)
        {
            PublishSample(slot, startTimestamp, endTimestamp);
        }
        ClearSlot(slot);
    }

    void RenderFrameTimingOwner::DiscardFrame(uint32 frameIndex, bool lost) noexcept
    {
        if (!m_enabled || frameIndex >= m_frameCount)
        {
            return;
        }

        Slot& slot = m_slots[frameIndex];
        if (slot.state == SlotState::Idle)
        {
            return;
        }

        if (lost)
        {
            ++m_diagnostics.lostSampleCount;
            SetUnavailable("The Graphics completion timeline was lost before frame timing could be read.");
        }
        else
        {
            ++m_diagnostics.droppedSampleCount;
        }
        ClearSlot(slot);
    }

    void RenderFrameTimingOwner::MarkAllLost() noexcept
    {
        for (uint32 frameIndex = 0; frameIndex < m_frameCount; ++frameIndex)
        {
            DiscardFrame(frameIndex, true);
        }
    }

    void RenderFrameTimingOwner::SetUnavailable(const char* reason) noexcept
    {
        try
        {
            m_diagnostics.terminalCriticalPathMilliseconds =
                DiagnosticValue<float64>::Unavailable(
                    reason ? reason : "Graphics frame timing is unavailable.");

            // Raw provenance belongs only to an available value. Clearing it
            // prevents consumers from pairing a stale source/completion point
            // with a newer unavailable reason.
            m_diagnostics.sourceFrameSequence = 0;
            m_diagnostics.completionValue = 0;
            m_diagnostics.startTimestamp = 0;
            m_diagnostics.endTimestamp = 0;
            m_diagnostics.elapsedTimestampTicks = 0;
            m_diagnostics.timestampFrequency = 0;
            m_diagnostics.timestampValidBits = 0;
            m_diagnostics.usedForAutoDecision = false;
        }
        catch (...)
        {
            // Timestamp telemetry is optional. Allocation failure while
            // describing an unavailable state must leave the last coherent
            // snapshot intact rather than terminate a noexcept render path.
        }
    }

    void RenderFrameTimingOwner::ClearSlot(Slot& slot) noexcept
    {
        slot.submittedPoint = {};
        slot.state = SlotState::Idle;
        slot.sourceBound = false;
        slot.sourceFrameSequence = 0;
    }

    void RenderFrameTimingOwner::PublishSample(
        const Slot& slot,
        uint64 startTimestamp,
        uint64 endTimestamp) noexcept
    {
        const uint64 elapsedTimestampTicks = CalculateRHITimestampElapsedDelta(
            startTimestamp, endTimestamp,
            static_cast<uint8>(m_timestampValidBits));
        const float64 elapsedMilliseconds =
            (static_cast<float64>(elapsedTimestampTicks) * 1000.0) /
            static_cast<float64>(m_timestampFrequency);

        try
        {
            m_diagnostics.terminalCriticalPathMilliseconds =
                DiagnosticValue<float64>::Available(elapsedMilliseconds);
        }
        catch (...)
        {
            // Preserve the prior coherent diagnostics snapshot if optional
            // telemetry storage cannot be refreshed.
            return;
        }
        m_diagnostics.sourceFrameSequence = slot.sourceFrameSequence;
        m_diagnostics.completionValue = slot.submittedPoint.value;
        m_diagnostics.startTimestamp = startTimestamp;
        m_diagnostics.endTimestamp = endTimestamp;
        m_diagnostics.elapsedTimestampTicks = elapsedTimestampTicks;
        m_diagnostics.timestampFrequency = m_timestampFrequency;
        m_diagnostics.timestampValidBits = m_timestampValidBits;
        m_diagnostics.usedForAutoDecision = false;
    }
} // namespace RVX
