#include "Render/Qualification/RenderQualityPerformanceSession.h"

#include "Core/Diagnostics/ContentHash.h"
#include "Render/Qualification/RenderExecutionReceiptDigest.h"
#include "Resources/RenderSubmissionTracker.h"
#include "RHI/RHIDevice.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace RVX
{
namespace
{
    inline constexpr uint32 RVX_RENDER_QUALITY_PERFORMANCE_TOTAL_FRAME_COUNT =
        RVX_RENDER_QUALITY_PERFORMANCE_WARMUP_SAMPLE_COUNT +
        RVX_RENDER_QUALITY_PERFORMANCE_MEASURED_SAMPLE_COUNT;

    [[nodiscard]] const char* GetPerformanceBackendName(
        RHIBackendType backend) noexcept
    {
        switch (backend)
        {
            case RHIBackendType::DX11: return "dx11";
            case RHIBackendType::DX12: return "dx12";
            case RHIBackendType::Vulkan: return "vulkan";
            case RHIBackendType::Metal: return "metal";
            case RHIBackendType::OpenGL: return "opengl";
            case RHIBackendType::None: break;
        }
        return "invalid";
    }

    [[nodiscard]] RenderQualityPerformanceReceiptBinding MakeReceiptBinding(
        const RenderExecutionReceipt& receipt)
    {
        RenderQualityPerformanceReceiptBinding binding;
        binding.frameIdentity = receipt.frameIdentity;
        binding.viewIdentity = receipt.viewIdentity;
        binding.receiptSchemaVersion = receipt.schemaVersion;
        binding.receiptDigest = ComputeRenderExecutionReceiptDigest(receipt);
        binding.sourceCommit = receipt.build.sourceCommit;
        binding.preset = receipt.build.preset;
        binding.backend = GetPerformanceBackendName(receipt.backend);
        binding.recipe = receipt.resolvedRecipe;
        binding.quality = receipt.quality;
        binding.profileHash = receipt.profileHash;
        return binding;
    }

    [[nodiscard]] bool HasMatchingExecutionIdentity(
        const RenderQualityPerformanceReceiptBinding& execution,
        const RenderQualityPerformanceReceiptBinding& presented) noexcept
    {
        return execution.frameIdentity == presented.frameIdentity &&
               execution.viewIdentity == presented.viewIdentity &&
               execution.receiptSchemaVersion ==
                   presented.receiptSchemaVersion &&
               execution.sourceCommit == presented.sourceCommit &&
               execution.preset == presented.preset &&
               execution.backend == presented.backend &&
               execution.recipe == presented.recipe &&
               execution.quality == presented.quality &&
               execution.profileHash == presented.profileHash;
    }

    [[nodiscard]] uint64 HashPassSchema(
        std::span<const RenderGraphPassTimingSample> timings) noexcept
    {
        uint64 hash = Diagnostics::RVX_DIAGNOSTICS_FNV1A64_OFFSET_BASIS;
        Diagnostics::MixContentHashValue(hash, timings.size());
        for (const RenderGraphPassTimingSample& timing : timings)
        {
            Diagnostics::MixContentHashValue(hash, timing.executionIndex);
            Diagnostics::MixContentHashString(hash, timing.passName);
        }
        return hash;
    }

    [[nodiscard]] RenderQualityPerformanceFailure MapTimingFailure(
        RenderGraphPassTimingFailure failure) noexcept
    {
        switch (failure)
        {
            case RenderGraphPassTimingFailure::CompletionLost:
                return RenderQualityPerformanceFailure::CompletionLost;
            case RenderGraphPassTimingFailure::Unsupported:
                return RenderQualityPerformanceFailure::TimestampQueriesUnavailable;
            case RenderGraphPassTimingFailure::SubmissionRejected:
            case RenderGraphPassTimingFailure::MultiQueueSubmission:
                return RenderQualityPerformanceFailure::SubmissionRejected;
            case RenderGraphPassTimingFailure::ReadbackMapFailed:
            case RenderGraphPassTimingFailure::TimestampWrapped:
            case RenderGraphPassTimingFailure::TimestampNonAdvancing:
                return RenderQualityPerformanceFailure::QueryReadbackFailed;
            case RenderGraphPassTimingFailure::None:
                return RenderQualityPerformanceFailure::None;
            default:
                return RenderQualityPerformanceFailure::GraphRecordingFailed;
        }
    }

    [[nodiscard]] bool TryAccumulateTrackedUploadBytes(
        uint64& inOutTotal,
        uint64 value) noexcept
    {
        if (value > std::numeric_limits<uint64>::max() - inOutTotal)
            return false;
        inOutTotal += value;
        return true;
    }

    [[nodiscard]] bool IsGraphicsTerminalCommandSpanValid(
        const RenderGpuFrameTimingSample& sample) noexcept
    {
        if (sample.sourceFrameSequence == 0 || sample.completionValue == 0)
        {
            return false;
        }
        return IsRenderQualityPerformanceGraphicsTerminalSpanValid(
            sample.startTimestamp,
            sample.endTimestamp,
            sample.elapsedTimestampTicks,
            sample.timestampFrequency,
            static_cast<uint8>(sample.timestampValidBits),
            sample.terminalCriticalPathMilliseconds);
    }

    [[nodiscard]] std::string DescribeResourceCountMismatch(
        const RenderQualityPerformanceResourceCounts& expected,
        const RenderQualityPerformanceResourceCounts& observed) noexcept
    {
        try
        {
            return "Stable resource counts changed after performance warmup: "
                "transientTextures " + std::to_string(expected.transientTextureCount) +
                "->" + std::to_string(observed.transientTextureCount) +
                ", transientBuffers " +
                std::to_string(expected.transientBufferCount) + "->" +
                std::to_string(observed.transientBufferCount) +
                ", importedTextures " +
                std::to_string(expected.importedTextureCount) + "->" +
                std::to_string(observed.importedTextureCount) +
                ", importedBuffers " +
                std::to_string(expected.importedBufferCount) + "->" +
                std::to_string(observed.importedBufferCount) +
                ", residentMeshes " +
                std::to_string(expected.residentMeshCount) + "->" +
                std::to_string(observed.residentMeshCount) +
                ", residentTextures " +
                std::to_string(expected.residentTextureCount) + "->" +
                std::to_string(observed.residentTextureCount) +
                ", residentMaterials " +
                std::to_string(expected.residentMaterialCount) + "->" +
                std::to_string(observed.residentMaterialCount) + ".";
        }
        catch (...)
        {
            return {};
        }
    }

    [[nodiscard]] const char* DescribeIncompleteEvidence(
        const RenderQualityPerformanceEvidence& evidence) noexcept
    {
        if (evidence.schemaVersion != RVX_RENDER_QUALITY_PERFORMANCE_SCHEMA_VERSION)
            return "The immutable performance evidence has an unexpected schema version.";
        if (evidence.requiredWarmupSamples !=
                RVX_RENDER_QUALITY_PERFORMANCE_WARMUP_SAMPLE_COUNT ||
            evidence.requiredMeasuredSamples !=
                RVX_RENDER_QUALITY_PERFORMANCE_MEASURED_SAMPLE_COUNT ||
            evidence.completedWarmupSamples != evidence.requiredWarmupSamples ||
            evidence.completedMeasuredSamples != evidence.requiredMeasuredSamples)
        {
            return "The immutable performance evidence does not contain the required completed sample counts.";
        }
        if (evidence.slotCapacity == 0 || evidence.slotHighWaterMark == 0 ||
            evidence.slotHighWaterMark > evidence.slotCapacity)
        {
            return "The immutable performance evidence has invalid timestamp slot occupancy.";
        }
        if (!evidence.hardware.IsValid())
            return "The immutable performance evidence has invalid hardware provenance.";
        if (evidence.passSchemaHash == 0)
            return "The immutable performance evidence has no RenderGraph pass schema.";
        if (evidence.samples.size() != evidence.requiredMeasuredSamples)
            return "The immutable performance evidence has an unexpected measured sample vector size.";

        uint64 previousSubmittedFrame = 0;
        uint64 previousCompletedFrame = 0;
        for (uint32 index = 0; index < evidence.samples.size(); ++index)
        {
            const RenderQualityPerformanceSample& sample = evidence.samples[index];
            if (sample.sampleIndex != index)
                return "A measured performance sample has a noncanonical sample index.";
            if (sample.passSchemaHash != evidence.passSchemaHash)
                return "A measured performance sample has a different RenderGraph pass schema.";
            if (sample.binding.backend != evidence.hardware.backend)
                return "A measured performance sample backend does not match the armed hardware identity.";
            if (sample.submittedFrame <= previousSubmittedFrame ||
                sample.completedFrame <= previousCompletedFrame)
            {
                return "Measured performance sample submission/completion ordinals are not strictly increasing.";
            }
            if (!sample.binding.IsValid())
                return "A measured performance sample has invalid receipt provenance.";
            if (!sample.cpu.HasAllRequiredMeasurements())
                return "A measured performance sample lacks a required CPU measurement.";
            if (sample.submittedFrame == 0 ||
                sample.completedFrame < sample.submittedFrame ||
                sample.completionValue == 0 || sample.timestampFrequency == 0 ||
                sample.passSchemaHash == 0 ||
                sample.trackedUploadCoverageMask !=
                    RVX_RENDER_QUALITY_PERFORMANCE_REQUIRED_UPLOAD_COVERAGE ||
                !sample.trackedEstimatedFootprint.IsValid() ||
                !sample.resourceCounts.IsValid())
            {
                return "A measured performance sample has invalid completion, telemetry, or resource-count evidence.";
            }
            if ((sample.graphicsTerminalCommandSpanStartTimestamp == 0 &&
                 sample.graphicsTerminalCommandSpanEndTimestamp == 0) ||
                sample.graphicsTerminalCommandSpanTimestampValidBits == 0 ||
                sample.graphicsTerminalCommandSpanTimestampValidBits > 64 ||
                !std::isfinite(
                    sample.graphicsTerminalCommandSpanMilliseconds) ||
                sample.graphicsTerminalCommandSpanMilliseconds < 0.0 ||
                sample.graphEndTimestamp < sample.graphStartTimestamp ||
                !std::isfinite(sample.graphSpanMilliseconds) ||
                sample.graphSpanMilliseconds < 0.0 || sample.passTimings.empty())
            {
                return "A measured performance sample has an invalid Graphics terminal span or RenderGraph pass envelope.";
            }
            for (uint32 timingIndex = 0; timingIndex < sample.passTimings.size();
                 ++timingIndex)
            {
                if (sample.passTimings[timingIndex].executionIndex != timingIndex ||
                    !sample.passTimings[timingIndex].IsValid())
                {
                    return "A measured performance sample has invalid per-pass timestamp evidence.";
                }
            }
            previousSubmittedFrame = sample.submittedFrame;
            previousCompletedFrame = sample.completedFrame;
        }
        return "The immutable performance evidence failed an unclassified structural invariant.";
    }
} // namespace

RenderQualityPerformanceSession::RenderQualityPerformanceSession()
{
    for (Slot& slot : m_slots)
        slot.timing = std::make_shared<RenderGraphPassTimingOwner>();
    m_diagnostics.slotCapacity =
        RVX_RENDER_QUALITY_PERFORMANCE_SLOT_COUNT;
}

RenderQualityPerformanceSession::~RenderQualityPerformanceSession()
{
    Shutdown(true);
}

bool RenderQualityPerformanceSession::Arm(
    const RenderQualityPerformanceHardwareIdentity& hardware,
    uint64 initialCumulativeTrackedUploadBytes) noexcept
{
    if (m_requestIssued ||
        (m_diagnostics.state != RenderQualityPerformanceSessionState::Idle &&
         !m_diagnostics.IsTerminal()))
    {
        return false;
    }

    // A request is one-shot even when admission fails before slot allocation.
    // Otherwise an asynchronous caller could repeatedly re-arm a terminal
    // error and hide the first deterministic diagnostic.
    m_requestIssued = true;

    if (!hardware.IsValid())
    {
        return Reject(
            RenderQualityPerformanceFailure::InvalidHardwareIdentity,
            "Performance evidence requires a backend and capability-report SHA-256.");
    }

    try
    {
        m_hardware = hardware;
    }
    catch (...)
    {
        return Reject(
            RenderQualityPerformanceFailure::SlotInitializationFailed,
            "Performance hardware provenance could not be retained.");
    }

    m_completedEvidence.reset();
    m_recordingSlot = nullptr;
    m_expectedPassCount = 0;
    m_scheduledFrameCount = 0;
    m_completedFrameCount = 0;
    m_completedOrdinals.fill(false);
    m_consumedGraphicsTerminalCommandSpans.fill({});
    m_sessionSurfaceGeneration = 0;
    m_sessionConfigurationRevision = 0;
    m_sessionViewIdentity = {};
    m_sessionReceiptSchemaVersion = 0;
    m_measurementResourceCounts = {};
    m_lastCumulativeTrackedUploadBytes = initialCumulativeTrackedUploadBytes;
    m_lastPresentedSubmissionFrame = 0;
    m_lastCompletedSubmissionFrame = 0;
    m_measurementResourceCountsCaptured = false;
    m_sessionSourceCommit.clear();
    m_sessionPreset.clear();
    m_sessionBackend.clear();
    m_sessionRecipe = RenderPipelineRecipe::ForwardStandard;
    m_sessionQuality = RenderQualityTier::Medium;
    m_sessionProfileHash = 0;
    m_slotsInitialized = false;
    try
    {
        m_measuredSamples.clear();
        m_measuredSamples.resize(
            RVX_RENDER_QUALITY_PERFORMANCE_MEASURED_SAMPLE_COUNT);
    }
    catch (...)
    {
        return Reject(
            RenderQualityPerformanceFailure::SlotInitializationFailed,
            "Performance evidence storage could not be preallocated.");
    }

    for (Slot& slot : m_slots)
    {
        slot.frameIdentity = {};
        slot.viewIdentity = {};
        slot.executionBinding = {};
        slot.recordingBinding = {};
        slot.cpu = {};
        slot.telemetry = {};
        slot.submittedFrame = 0;
        slot.completedFrame = 0;
        slot.graphicsCompletionValue = 0;
        slot.trackedUploadBytes = 0;
        slot.sessionOrdinal = 0;
        slot.graphicsTerminalCommandSpan = {};
        slot.inUse = false;
        slot.receiptBound = false;
        slot.presented = false;
        slot.cpuBound = false;
        slot.telemetryBound = false;
        slot.graphicsTerminalCommandSpanBound = false;
    }

    m_diagnostics = {};
    m_diagnostics.state = RenderQualityPerformanceSessionState::Armed;
    m_diagnostics.failure = RenderQualityPerformanceFailure::None;
    m_diagnostics.slotCapacity =
        RVX_RENDER_QUALITY_PERFORMANCE_SLOT_COUNT;
    return true;
}

bool RenderQualityPerformanceSession::RequiresExactGraphicsFrame() const noexcept
{
    return m_diagnostics.state == RenderQualityPerformanceSessionState::Armed ||
           m_diagnostics.state ==
               RenderQualityPerformanceSessionState::Collecting;
}

IRenderGraphPassTimestampRecorder*
RenderQualityPerformanceSession::PrepareFrame(
    IRHIDevice* device,
    uint32 expectedPassCount,
    const RenderFrameIdentity& frameIdentity,
    const RenderViewIdentity& viewIdentity) noexcept
{
    if (!RequiresExactGraphicsFrame())
        return nullptr;
    if (!frameIdentity.IsValid() || !viewIdentity.IsValid() ||
        expectedPassCount == 0)
    {
        static_cast<void>(Reject(
            RenderQualityPerformanceFailure::InvalidFrameIdentity,
            "Performance frame identity or pass count is invalid."));
        return nullptr;
    }
    if (m_recordingSlot != nullptr)
    {
        static_cast<void>(Reject(
            RenderQualityPerformanceFailure::GraphRecordingFailed,
            "A prior performance frame did not close its recording boundary."));
        return nullptr;
    }
    if (!m_slotsInitialized && !InitializeSlots(device, expectedPassCount))
        return nullptr;
    if (expectedPassCount != m_expectedPassCount)
    {
        static_cast<void>(Reject(
            RenderQualityPerformanceFailure::PassSchemaChanged,
            "The active RenderGraph pass count changed during performance qualification."));
        return nullptr;
    }
    if (m_scheduledFrameCount >=
        RVX_RENDER_QUALITY_PERFORMANCE_TOTAL_FRAME_COUNT)
    {
        m_diagnostics.state = RenderQualityPerformanceSessionState::Draining;
        return nullptr;
    }

    Slot* slot = FindFreeSlot();
    if (slot == nullptr)
    {
        static_cast<void>(Reject(
            RenderQualityPerformanceFailure::SlotSaturated,
            "All preallocated timestamp slots are still completion-owned; no sample was dropped."));
        return nullptr;
    }

    slot->frameIdentity = frameIdentity;
    slot->viewIdentity = viewIdentity;
    slot->executionBinding = {};
    slot->recordingBinding = {};
    slot->cpu = {};
    slot->telemetry = {};
    slot->submittedFrame = 0;
    slot->completedFrame = 0;
    slot->graphicsCompletionValue = 0;
    slot->trackedUploadBytes = 0;
    slot->sessionOrdinal = m_scheduledFrameCount++;
    slot->graphicsTerminalCommandSpan = {};
    slot->inUse = true;
    slot->receiptBound = false;
    slot->presented = false;
    slot->cpuBound = false;
    slot->telemetryBound = false;
    slot->graphicsTerminalCommandSpanBound = false;
    m_recordingSlot = slot;
    m_diagnostics.state =
        m_scheduledFrameCount ==
                RVX_RENDER_QUALITY_PERFORMANCE_TOTAL_FRAME_COUNT
            ? RenderQualityPerformanceSessionState::Draining
            : RenderQualityPerformanceSessionState::Collecting;
    RefreshInFlightDiagnostics();
    return slot->timing.get();
}

bool RenderQualityPerformanceSession::BindExecutionReceipt(
    const RenderExecutionReceipt& receipt) noexcept
{
    // A session failure is immutable.  The Render owner calls the later
    // presentation and CPU-binding steps after an unsuccessful graph-record
    // attempt so they can release their normal frame ownership.  Those
    // follow-up calls must not obscure the first causal failure with a
    // secondary (and much less useful) timing error.
    if (m_diagnostics.IsTerminal())
        return false;
    if (m_recordingSlot == nullptr || !m_recordingSlot->inUse)
    {
        return Reject(
            RenderQualityPerformanceFailure::ReceiptUnavailable,
            "The sampled graph has no active performance recording slot for its execution receipt.");
    }
    if (!receipt.executionSucceeded)
    {
        return Reject(
            RenderQualityPerformanceFailure::ReceiptUnavailable,
            receipt.diagnostic.empty()
                ? "The sampled graph did not complete execution successfully."
                : receipt.diagnostic.c_str());
    }
    if (!receipt.IsStructurallyValid())
    {
        return Reject(
            RenderQualityPerformanceFailure::ReceiptUnavailable,
            "The sampled graph produced an execution-success receipt with invalid structural provenance.");
    }
    RenderQualityPerformanceReceiptBinding binding;
    try
    {
        binding = MakeReceiptBinding(receipt);
    }
    catch (...)
    {
        return Reject(
            RenderQualityPerformanceFailure::SlotInitializationFailed,
            "Performance receipt provenance could not be retained.");
    }
    if (!binding.IsValid())
    {
        return Reject(
            RenderQualityPerformanceFailure::ReceiptMismatch,
            "The execution receipt has invalid performance provenance.");
    }
    if (binding.frameIdentity != m_recordingSlot->frameIdentity)
    {
        return Reject(
            RenderQualityPerformanceFailure::ReceiptMismatch,
            "The execution receipt frame identity does not match the sampled performance frame.");
    }
    if (binding.viewIdentity != m_recordingSlot->viewIdentity)
    {
        return Reject(
            RenderQualityPerformanceFailure::ReceiptMismatch,
            "The execution receipt view identity does not match the sampled performance view.");
    }
    try
    {
        m_recordingSlot->executionBinding = std::move(binding);
    }
    catch (...)
    {
        return Reject(
            RenderQualityPerformanceFailure::SlotInitializationFailed,
            "Performance receipt binding could not be retained.");
    }
    m_recordingSlot->receiptBound = true;
    return true;
}

bool RenderQualityPerformanceSession::MarkFramePresented(
    const RenderExecutionReceipt& receipt,
    const RenderQualityPerformanceFrameTelemetry& telemetry) noexcept
{
    // Preserve the first terminal failure.  Presentation is invoked after
    // graph execution even when BindExecutionReceipt rejected that execution,
    // so replacing its diagnostic here would misidentify the root cause.
    if (m_diagnostics.IsTerminal())
        return false;
    Slot* slot = FindSlot(receipt.frameIdentity);
    if (slot == nullptr)
    {
        return m_diagnostics.state ==
                   RenderQualityPerformanceSessionState::Idle ||
               m_diagnostics.state ==
                   RenderQualityPerformanceSessionState::Draining ||
               m_diagnostics.IsTerminal();
    }
    if (!slot->receiptBound || slot->presented || !receipt.presented ||
        !receipt.succeeded || !receipt.IsStructurallyValid())
    {
        return Reject(
            RenderQualityPerformanceFailure::PresentationRejected,
            "The sampled frame did not produce a presentation-qualified receipt.");
    }
    RenderQualityPerformanceReceiptBinding presentedBinding;
    try
    {
        presentedBinding = MakeReceiptBinding(receipt);
    }
    catch (...)
    {
        return Reject(
            RenderQualityPerformanceFailure::SlotInitializationFailed,
            "Presented performance receipt provenance could not be retained.");
    }
    if (!presentedBinding.IsValid() ||
        !HasMatchingExecutionIdentity(slot->executionBinding, presentedBinding))
    {
        return Reject(
            RenderQualityPerformanceFailure::ReceiptMismatch,
            "The presented receipt does not match the graph-recording receipt identity.");
    }
    if (!telemetry.IsValid() || telemetry.frameIdentity != slot->frameIdentity)
    {
        return Reject(
            RenderQualityPerformanceFailure::TelemetryUnavailable,
            "The presentation-qualified frame is missing exact Render telemetry.");
    }
    if (telemetry.cumulativeTrackedUploadBytes <
        m_lastCumulativeTrackedUploadBytes)
    {
        return Reject(
            RenderQualityPerformanceFailure::UploadCounterDecreased,
            "The monotonic Render upload counter decreased during qualification.");
    }
    if (m_lastPresentedSubmissionFrame ==
        std::numeric_limits<uint64>::max())
    {
        return Reject(
            RenderQualityPerformanceFailure::FrameOrdinalInvalid,
            "The performance submission ordinal cannot advance.");
    }
    try
    {
        slot->recordingBinding = std::move(presentedBinding);
        slot->telemetry = telemetry;
    }
    catch (...)
    {
        return Reject(
            RenderQualityPerformanceFailure::SlotInitializationFailed,
            "Presented performance receipt binding could not be retained.");
    }
    const uint64 cumulativeDelta = telemetry.cumulativeTrackedUploadBytes -
        m_lastCumulativeTrackedUploadBytes;
    uint64 trackedUploadBytes = cumulativeDelta;
    if (!TryAccumulateTrackedUploadBytes(
            trackedUploadBytes, telemetry.frameTrackedUploadBytes))
    {
        return Reject(
            RenderQualityPerformanceFailure::TelemetryUnavailable,
            "The declared per-frame tracked upload bytes overflowed.");
    }
    slot->trackedUploadBytes = trackedUploadBytes;
    m_lastCumulativeTrackedUploadBytes =
        telemetry.cumulativeTrackedUploadBytes;
    slot->submittedFrame = ++m_lastPresentedSubmissionFrame;
    slot->completedFrame = 0;
    slot->presented = true;
    slot->telemetryBound = true;
    return true;
}

bool RenderQualityPerformanceSession::RequiresFrameTelemetry(
    const RenderFrameIdentity& frameIdentity) const noexcept
{
    const auto found = std::find_if(
        m_slots.begin(), m_slots.end(), [&frameIdentity](const Slot& slot)
        {
            return slot.inUse && slot.frameIdentity == frameIdentity &&
                   slot.receiptBound && !slot.presented;
        });
    return found != m_slots.end();
}

bool RenderQualityPerformanceSession::RecordCpuTiming(
    const RenderFrameIdentity& frameIdentity,
    const RenderQualityPerformanceCpuTiming& timing) noexcept
{
    // CPU timing is bound after Present.  A prior receipt/presentation
    // rejection is authoritative; do not overwrite it merely because its
    // ring slot never reached the presented state.
    if (m_diagnostics.IsTerminal())
        return false;
    Slot* slot = FindSlot(frameIdentity);
    if (slot == nullptr)
    {
        return m_diagnostics.state ==
                   RenderQualityPerformanceSessionState::Idle ||
               m_diagnostics.state ==
                   RenderQualityPerformanceSessionState::Draining ||
               m_diagnostics.IsTerminal();
    }
    if (!slot->presented || slot->cpuBound)
    {
        return Reject(
            RenderQualityPerformanceFailure::CpuTimingUnavailable,
            "CPU timing did not match a presentation-qualified sampled frame.");
    }
    slot->cpu = timing;
    slot->cpuBound = true;
    if (m_recordingSlot == slot)
        m_recordingSlot = nullptr;
    return true;
}

bool RenderQualityPerformanceSession::NotifyGraphicsSubmission(
    const GPUCompletionToken& completion) noexcept
{
    if (m_diagnostics.IsTerminal())
        return true;
    if (m_recordingSlot == nullptr || !m_recordingSlot->inUse)
        return true;
    if (completion.count != 1 ||
        completion.points[0].domain != GPUQueueDomain::Graphics ||
        completion.points[0].value == 0 ||
        m_recordingSlot->graphicsCompletionValue != 0)
    {
        return Reject(
            RenderQualityPerformanceFailure::SubmissionRejected,
            "The sampled performance frame did not bind exactly one Graphics completion value.");
    }
    m_recordingSlot->graphicsCompletionValue = completion.points[0].value;
    return true;
}

void RenderQualityPerformanceSession::AbortCurrentFrame(
    RenderQualityPerformanceFailure failure,
    const char* diagnostic) noexcept
{
    if (m_recordingSlot == nullptr)
        return;
    Slot* slot = m_recordingSlot;
    if (slot->timing)
    {
        const RenderGraphPassTimingState state =
            slot->timing->GetDiagnostics().state;
        if (state == RenderGraphPassTimingState::Recording ||
            state == RenderGraphPassTimingState::AwaitingSubmission)
        {
            slot->timing->AbortExactGraphicsExecution(diagnostic);
        }
    }
    static_cast<void>(Reject(failure, diagnostic));
    if (slot->timing)
    {
        const RenderGraphPassTimingState state =
            slot->timing->GetDiagnostics().state;
        if (state == RenderGraphPassTimingState::Unavailable ||
            state == RenderGraphPassTimingState::Idle ||
            state == RenderGraphPassTimingState::Rejected ||
            state == RenderGraphPassTimingState::Completed)
        {
            ReleaseSlot(*slot);
            RefreshInFlightDiagnostics();
        }
    }
}

void RenderQualityPerformanceSession::RejectAdmission(
    RenderQualityPerformanceFailure failure,
    const char* diagnostic) noexcept
{
    if (m_diagnostics.state == RenderQualityPerformanceSessionState::Completed)
    {
        return;
    }
    m_requestIssued = true;
    if (m_diagnostics.state == RenderQualityPerformanceSessionState::Rejected)
        return;
    static_cast<void>(Reject(failure, diagnostic));
    ReleaseTerminalSlots();
    RefreshInFlightDiagnostics();
}

bool RenderQualityPerformanceSession::BindGraphicsTerminalCommandSpans(
    const RenderGpuFrameTimingSampleBatch& samples) noexcept
{
    if (m_diagnostics.state == RenderQualityPerformanceSessionState::Completed)
        return true;
    if (m_diagnostics.state == RenderQualityPerformanceSessionState::Rejected)
        return false;
    if (samples.count > samples.samples.size())
    {
        return Reject(
            RenderQualityPerformanceFailure::QueryReadbackFailed,
            "RenderContext returned an invalid Graphics terminal timing batch size.");
    }

    for (uint32 sampleIndex = 0; sampleIndex < samples.count; ++sampleIndex)
    {
        const RenderGpuFrameTimingSample& sample = samples.samples[sampleIndex];
        Slot* matchingSlot = nullptr;
        for (Slot& slot : m_slots)
        {
            if (slot.inUse &&
                slot.frameIdentity.frameSequence == sample.sourceFrameSequence)
            {
                matchingSlot = &slot;
                break;
            }
        }

        const auto previouslyConsumed = std::find_if(
            m_consumedGraphicsTerminalCommandSpans.begin(),
            m_consumedGraphicsTerminalCommandSpans.end(),
            [&sample](const ConsumedGraphicsTerminalCommandSpan& consumed)
            {
                return consumed.sourceFrameSequence != 0 &&
                       consumed.sourceFrameSequence ==
                           sample.sourceFrameSequence;
            });

        if (matchingSlot == nullptr)
        {
            if (previouslyConsumed !=
                m_consumedGraphicsTerminalCommandSpans.end())
            {
                return Reject(
                    RenderQualityPerformanceFailure::ReceiptMismatch,
                    "A Graphics terminal timing sample was duplicated after its performance slot completed.");
            }
            // RenderContext retains timing for ordinary frames too. Samples
            // with no source frame reserved by this session are irrelevant.
            continue;
        }

        if (matchingSlot->graphicsTerminalCommandSpanBound)
        {
            return Reject(
                RenderQualityPerformanceFailure::ReceiptMismatch,
                "A sampled Graphics frame produced more than one terminal command span.");
        }
        if (!IsGraphicsTerminalCommandSpanValid(sample))
        {
            return Reject(
                RenderQualityPerformanceFailure::QueryReadbackFailed,
                "The sampled Graphics terminal command span is incomplete or inconsistent.");
        }
        if (!matchingSlot->timing || !matchingSlot->receiptBound ||
            !matchingSlot->presented ||
            matchingSlot->sessionOrdinal >=
                RVX_RENDER_QUALITY_PERFORMANCE_TOTAL_FRAME_COUNT)
        {
            return Reject(
                RenderQualityPerformanceFailure::ReceiptUnavailable,
                "A Graphics terminal command span arrived without a presentation-qualified performance slot.");
        }

        if (matchingSlot->graphicsCompletionValue == 0)
        {
            return Reject(
                RenderQualityPerformanceFailure::ReceiptUnavailable,
                "A Graphics terminal command span arrived before its exact RenderGraph submission binding.");
        }
        if (matchingSlot->graphicsCompletionValue != sample.completionValue)
        {
            return Reject(
                RenderQualityPerformanceFailure::ReceiptMismatch,
                "The Graphics terminal command span completion value does not match the sampled submission.");
        }

        try
        {
            matchingSlot->graphicsTerminalCommandSpan = sample;
            matchingSlot->graphicsTerminalCommandSpanBound = true;
            m_consumedGraphicsTerminalCommandSpans[
                matchingSlot->sessionOrdinal] = {
                sample.sourceFrameSequence,
                sample.completionValue};
        }
        catch (...)
        {
            return Reject(
                RenderQualityPerformanceFailure::SlotInitializationFailed,
                "The exact Graphics terminal command span could not be retained.");
        }
    }
    return true;
}

void RenderQualityPerformanceSession::PollCompletion(
    const RenderSubmissionTracker& tracker) noexcept
{
    for (Slot& slot : m_slots)
    {
        if (!slot.inUse || !slot.timing)
            continue;

        RenderGraphPassTimingState state = slot.timing->GetDiagnostics().state;
        if (state == RenderGraphPassTimingState::PendingCompletion)
        {
            static_cast<void>(slot.timing->PollCompletion(tracker));
            state = slot.timing->GetDiagnostics().state;
        }

        if (state == RenderGraphPassTimingState::Rejected)
        {
            const RenderGraphPassTimingFailure timingFailure =
                slot.timing->GetDiagnostics().failure;
            const RenderQualityPerformanceFailure failure =
                MapTimingFailure(timingFailure);
            static_cast<void>(Reject(
                failure,
                GetRenderGraphPassTimingFailureName(timingFailure)));
            ReleaseSlot(slot);
            continue;
        }
        // RenderContext's terminal span and the RenderGraph pass-query owner
        // share the same completion token but can be observed on different
        // completion-only pumps. A completed pass owner without its terminal
        // span remains completion-owned; it is neither mapped into evidence
        // nor released until the matching source-bound span arrives.
    }

    // Slot reuse is deliberately independent from submission order. Drain all
    // completed Graphics slots by their Present-frozen ordinal so a recently
    // reused low-index slot cannot make a later submission appear to complete
    // before an older high-index slot.
    while (m_diagnostics.state != RenderQualityPerformanceSessionState::Rejected)
    {
        Slot* earliestCompletedSlot = nullptr;
        for (Slot& slot : m_slots)
        {
            if (!slot.inUse || !slot.timing ||
                slot.timing->GetDiagnostics().state !=
                    RenderGraphPassTimingState::Completed ||
                !slot.graphicsTerminalCommandSpanBound)
            {
                continue;
            }
            if (earliestCompletedSlot == nullptr ||
                slot.submittedFrame < earliestCompletedSlot->submittedFrame)
            {
                earliestCompletedSlot = &slot;
            }
        }
        if (earliestCompletedSlot == nullptr)
            break;
        if (!FinalizeCompletedSlot(*earliestCompletedSlot))
        {
            ReleaseSlot(*earliestCompletedSlot);
            break;
        }
        ReleaseSlot(*earliestCompletedSlot);
    }
    if (m_diagnostics.state == RenderQualityPerformanceSessionState::Rejected)
        ReleaseTerminalSlots();
    RefreshInFlightDiagnostics();
    if (m_diagnostics.state == RenderQualityPerformanceSessionState::Draining &&
        m_completedFrameCount ==
            RVX_RENDER_QUALITY_PERFORMANCE_TOTAL_FRAME_COUNT &&
        m_diagnostics.inFlightSlots == 0)
    {
        static_cast<void>(PublishCompletedEvidence());
    }
}

void RenderQualityPerformanceSession::NotifyDeviceLost() noexcept
{
    if (m_diagnostics.state == RenderQualityPerformanceSessionState::Idle ||
        m_diagnostics.IsTerminal())
    {
        return;
    }
    for (Slot& slot : m_slots)
    {
        if (slot.inUse && slot.timing)
            slot.timing->OnRenderGraphDeviceLost();
    }
    static_cast<void>(Reject(
        RenderQualityPerformanceFailure::DeviceLost,
        "The render device was lost while performance evidence was completion-owned."));
    ReleaseTerminalSlots();
    RefreshInFlightDiagnostics();
}

void RenderQualityPerformanceSession::Shutdown(bool deviceLost) noexcept
{
    if (deviceLost &&
        m_diagnostics.state != RenderQualityPerformanceSessionState::Idle &&
        !m_diagnostics.IsTerminal())
        NotifyDeviceLost();
    for (Slot& slot : m_slots)
    {
        if (slot.timing)
            slot.timing->Shutdown(deviceLost);
        slot.inUse = false;
    }
    m_recordingSlot = nullptr;
    m_slotsInitialized = false;
    RefreshInFlightDiagnostics();
}

bool RenderQualityPerformanceSession::HasPendingCompletion() const noexcept
{
    return std::any_of(
        m_slots.begin(), m_slots.end(), [](const Slot& slot)
        {
            if (!slot.inUse || !slot.timing)
                return false;
            const RenderGraphPassTimingState state =
                slot.timing->GetDiagnostics().state;
            return state == RenderGraphPassTimingState::AwaitingSubmission ||
                   state == RenderGraphPassTimingState::PendingCompletion ||
                   (state == RenderGraphPassTimingState::Completed &&
                    !slot.graphicsTerminalCommandSpanBound);
        });
}

bool RenderQualityPerformanceSession::InitializeSlots(
    IRHIDevice* device,
    uint32 expectedPassCount) noexcept
{
    if (device == nullptr || expectedPassCount == 0)
    {
        return Reject(
            RenderQualityPerformanceFailure::TimestampQueriesUnavailable,
            "Timestamp queries require a valid Render device and a non-empty graph.");
    }

    for (Slot& slot : m_slots)
    {
        bool initialized = false;
        try
        {
            initialized = slot.timing &&
                slot.timing->Initialize(device, expectedPassCount);
        }
        catch (...)
        {
            initialized = false;
        }
        if (!initialized)
        {
            for (Slot& initializedSlot : m_slots)
            {
                if (initializedSlot.timing)
                    initializedSlot.timing->Shutdown(false);
            }
            return Reject(
                RenderQualityPerformanceFailure::SlotInitializationFailed,
                "The fixed timestamp slot ring could not be initialized.");
        }
    }
    m_expectedPassCount = expectedPassCount;
    m_slotsInitialized = true;
    m_diagnostics.state = RenderQualityPerformanceSessionState::Collecting;
    return true;
}

RenderQualityPerformanceSession::Slot*
RenderQualityPerformanceSession::FindFreeSlot() noexcept
{
    const auto found = std::find_if(
        m_slots.begin(), m_slots.end(), [](const Slot& slot)
        {
            return !slot.inUse && slot.timing &&
                   slot.timing->GetDiagnostics().state ==
                       RenderGraphPassTimingState::Idle;
        });
    return found == m_slots.end() ? nullptr : &*found;
}

RenderQualityPerformanceSession::Slot*
RenderQualityPerformanceSession::FindSlot(
    const RenderFrameIdentity& frameIdentity) noexcept
{
    const auto found = std::find_if(
        m_slots.begin(), m_slots.end(), [&frameIdentity](const Slot& slot)
        {
            return slot.inUse && slot.frameIdentity == frameIdentity;
        });
    return found == m_slots.end() ? nullptr : &*found;
}

bool RenderQualityPerformanceSession::FinalizeCompletedSlot(Slot& slot) noexcept
{
    if (!slot.receiptBound || !slot.presented || !slot.cpuBound ||
        !slot.telemetryBound || !slot.graphicsTerminalCommandSpanBound)
    {
        return Reject(
            RenderQualityPerformanceFailure::ReceiptUnavailable,
            "A completed timestamp slot is missing receipt, presentation, CPU, telemetry, or Graphics terminal timing binding.");
    }
    const RenderGraphPassTimingDiagnostics& timingDiagnostics =
        slot.timing->GetDiagnostics();
    if (slot.graphicsCompletionValue == 0 ||
        timingDiagnostics.completionValue != slot.graphicsCompletionValue ||
        slot.graphicsTerminalCommandSpan.completionValue !=
            slot.graphicsCompletionValue)
    {
        return Reject(
            RenderQualityPerformanceFailure::ReceiptMismatch,
            "The terminal Graphics span and RenderGraph pass timing owner do not share one completion value.");
    }
    if (slot.submittedFrame == 0 ||
        slot.submittedFrame <= m_lastCompletedSubmissionFrame)
    {
        return Reject(
            RenderQualityPerformanceFailure::FrameOrdinalInvalid,
            "Graphics completion order did not advance the submitted performance ordinal.");
    }
    slot.completedFrame = slot.submittedFrame;
    return CommitCompletedSample(
        slot.sessionOrdinal,
        slot.recordingBinding,
        slot.cpu,
        slot.submittedFrame,
        slot.completedFrame,
        slot.trackedUploadBytes,
        slot.telemetry.trackedUploadCoverageMask,
        slot.telemetry.trackedEstimatedFootprint,
        slot.telemetry.resourceCounts,
        timingDiagnostics.completionValue,
        timingDiagnostics.timestampFrequency,
        slot.graphicsTerminalCommandSpan,
        slot.timing->GetCompletedSamples());
}

bool RenderQualityPerformanceSession::CommitCompletedSample(
    uint32 sessionOrdinal,
    const RenderQualityPerformanceReceiptBinding& binding,
    const RenderQualityPerformanceCpuTiming& cpu,
    uint64 submittedFrame,
    uint64 completedFrame,
    uint64 trackedUploadBytes,
    uint32 trackedUploadCoverageMask,
    const RenderQualityPerformanceTrackedEstimatedFootprint&
        trackedEstimatedFootprint,
    const RenderQualityPerformanceResourceCounts& resourceCounts,
    uint64 completionValue,
    uint64 timestampFrequency,
    const RenderGpuFrameTimingSample& graphicsTerminalCommandSpan,
    std::span<const RenderGraphPassTimingSample> timings) noexcept
{
    if (m_diagnostics.state == RenderQualityPerformanceSessionState::Rejected)
        return false;
    if (sessionOrdinal >= RVX_RENDER_QUALITY_PERFORMANCE_TOTAL_FRAME_COUNT ||
        !binding.IsValid() || !trackedEstimatedFootprint.IsValid() ||
        trackedUploadCoverageMask !=
            RVX_RENDER_QUALITY_PERFORMANCE_REQUIRED_UPLOAD_COVERAGE ||
        !resourceCounts.IsValid() ||
        completionValue == 0 || timestampFrequency == 0 ||
        !IsGraphicsTerminalCommandSpanValid(graphicsTerminalCommandSpan) ||
        graphicsTerminalCommandSpan.sourceFrameSequence !=
            binding.frameIdentity.frameSequence ||
        graphicsTerminalCommandSpan.completionValue != completionValue ||
        graphicsTerminalCommandSpan.timestampFrequency != timestampFrequency ||
        timings.empty())
    {
        return Reject(
            RenderQualityPerformanceFailure::SampleCountMismatch,
            "A completed performance sample has invalid identity or query metadata.");
    }
    if (m_completedOrdinals[sessionOrdinal])
    {
        return Reject(
            RenderQualityPerformanceFailure::SampleCountMismatch,
            "A performance session ordinal completed more than once.");
    }
    if (submittedFrame == 0 || completedFrame < submittedFrame ||
        submittedFrame <= m_lastCompletedSubmissionFrame ||
        completedFrame <= m_lastCompletedSubmissionFrame ||
        completedFrame != submittedFrame)
    {
        return Reject(
            RenderQualityPerformanceFailure::FrameOrdinalInvalid,
            "Completed performance frame ordinals are duplicate, retrograde, or mismatched.");
    }
    if (!cpu.extraction.available)
    {
        return Reject(
            RenderQualityPerformanceFailure::ExtractionTimingUnavailable,
            "Update-thread extraction timing is unavailable to the Render owner; required performance evidence cannot be published.");
    }
    if (!cpu.visibilityAndDrawPlanning.available)
    {
        return Reject(
            RenderQualityPerformanceFailure::VisibilityTimingUnavailable,
            "Render visibility and draw-planning timing is unavailable.");
    }
    if (!cpu.HasAllRequiredMeasurements())
    {
        return Reject(
            RenderQualityPerformanceFailure::CpuTimingUnavailable,
            "One or more required Render CPU phases were not measured.");
    }
    if (binding.backend != m_hardware.backend)
    {
        return Reject(
            RenderQualityPerformanceFailure::ReceiptMismatch,
            "The presented receipt backend does not match the armed hardware identity.");
    }

    if (sessionOrdinal >= RVX_RENDER_QUALITY_PERFORMANCE_WARMUP_SAMPLE_COUNT)
    {
        if (!m_measurementResourceCountsCaptured)
        {
            m_measurementResourceCounts = resourceCounts;
            m_measurementResourceCountsCaptured = true;
        }
        else if (resourceCounts != m_measurementResourceCounts)
        {
            const std::string diagnostic = DescribeResourceCountMismatch(
                m_measurementResourceCounts, resourceCounts);
            return Reject(
                RenderQualityPerformanceFailure::ResourceCountsChanged,
                diagnostic.empty()
                    ? "Stable resource counts changed after performance warmup."
                    : diagnostic.c_str());
        }
    }

    if (m_sessionConfigurationRevision == 0)
    {
        m_sessionSurfaceGeneration = binding.frameIdentity.surfaceGeneration;
        m_sessionConfigurationRevision =
            binding.frameIdentity.configurationRevision;
        m_sessionViewIdentity = binding.viewIdentity;
        m_sessionReceiptSchemaVersion = binding.receiptSchemaVersion;
        m_sessionRecipe = binding.recipe;
        m_sessionQuality = binding.quality;
        m_sessionProfileHash = binding.profileHash;
        try
        {
            m_sessionSourceCommit = binding.sourceCommit;
            m_sessionPreset = binding.preset;
            m_sessionBackend = binding.backend;
        }
        catch (...)
        {
            return Reject(
                RenderQualityPerformanceFailure::SlotInitializationFailed,
                "Performance build provenance could not be retained.");
        }
    }
    else if (binding.frameIdentity.surfaceGeneration !=
                 m_sessionSurfaceGeneration ||
             binding.frameIdentity.configurationRevision !=
                 m_sessionConfigurationRevision ||
             binding.viewIdentity != m_sessionViewIdentity ||
             binding.receiptSchemaVersion !=
                 m_sessionReceiptSchemaVersion ||
             binding.recipe != m_sessionRecipe ||
             binding.quality != m_sessionQuality ||
             binding.profileHash != m_sessionProfileHash ||
             binding.sourceCommit != m_sessionSourceCommit ||
             binding.preset != m_sessionPreset ||
             binding.backend != m_sessionBackend)
    {
        return Reject(
            RenderQualityPerformanceFailure::ReceiptMismatch,
            "View, surface, configuration, recipe, build, backend, or receipt schema changed during the performance session.");
    }

    for (uint32 index = 0; index < timings.size(); ++index)
    {
        const RenderGraphPassTimingSample& timing = timings[index];
        const float64 expectedMilliseconds =
            (static_cast<float64>(timing.elapsedTimestampTicks) * 1000.0) /
            static_cast<float64>(timestampFrequency);
        if (timing.executionIndex != index || timing.passName.empty() ||
            timing.endTimestamp < timing.startTimestamp ||
            (timing.startTimestamp == 0 && timing.endTimestamp == 0) ||
            timing.elapsedTimestampTicks !=
                timing.endTimestamp - timing.startTimestamp ||
            timing.durationMilliseconds < 0.0 ||
            std::abs(timing.durationMilliseconds - expectedMilliseconds) >
                std::max(1.0e-9, expectedMilliseconds * 1.0e-9))
        {
            return Reject(
                RenderQualityPerformanceFailure::QueryReadbackFailed,
                "A per-pass timestamp pair is incomplete or out of order.");
        }
    }

    const uint64 passSchemaHash = HashPassSchema(timings);
    if (passSchemaHash == 0)
    {
        return Reject(
            RenderQualityPerformanceFailure::PassSchemaChanged,
            "The RenderGraph pass schema could not be identified.");
    }
    if (m_diagnostics.passSchemaHash == 0)
        m_diagnostics.passSchemaHash = passSchemaHash;
    else if (m_diagnostics.passSchemaHash != passSchemaHash)
    {
        return Reject(
            RenderQualityPerformanceFailure::PassSchemaChanged,
            "The RenderGraph pass names or execution order changed during the session.");
    }

    if (sessionOrdinal < RVX_RENDER_QUALITY_PERFORMANCE_WARMUP_SAMPLE_COUNT)
    {
        ++m_diagnostics.completedWarmupSamples;
    }
    else
    {
        const uint32 measuredIndex = sessionOrdinal -
            RVX_RENDER_QUALITY_PERFORMANCE_WARMUP_SAMPLE_COUNT;
        if (measuredIndex >= m_measuredSamples.size() ||
            m_measuredSamples[measuredIndex].binding.IsValid())
        {
            return Reject(
                RenderQualityPerformanceFailure::SampleCountMismatch,
                "A measured performance ordinal is duplicate or out of range.");
        }

        RenderQualityPerformanceSample sample;
        sample.sampleIndex = measuredIndex;
        sample.binding = binding;
        sample.cpu = cpu;
        sample.submittedFrame = submittedFrame;
        sample.completedFrame = completedFrame;
        sample.trackedUploadBytes = trackedUploadBytes;
        sample.trackedUploadCoverageMask = trackedUploadCoverageMask;
        sample.trackedEstimatedFootprint = trackedEstimatedFootprint;
        sample.resourceCounts = resourceCounts;
        sample.completionValue = completionValue;
        sample.timestampFrequency = timestampFrequency;
        sample.passSchemaHash = passSchemaHash;
        sample.graphicsTerminalCommandSpanStartTimestamp =
            graphicsTerminalCommandSpan.startTimestamp;
        sample.graphicsTerminalCommandSpanEndTimestamp =
            graphicsTerminalCommandSpan.endTimestamp;
        sample.graphicsTerminalCommandSpanElapsedTimestampTicks =
            graphicsTerminalCommandSpan.elapsedTimestampTicks;
        sample.graphicsTerminalCommandSpanTimestampValidBits =
            static_cast<uint8>(graphicsTerminalCommandSpan.timestampValidBits);
        sample.graphicsTerminalCommandSpanMilliseconds =
            graphicsTerminalCommandSpan.terminalCriticalPathMilliseconds;
        sample.graphStartTimestamp = timings.front().startTimestamp;
        sample.graphEndTimestamp = timings.back().endTimestamp;
        if (sample.graphEndTimestamp < sample.graphStartTimestamp)
        {
            return Reject(
                RenderQualityPerformanceFailure::QueryReadbackFailed,
                "The RenderGraph pass timestamp envelope is retrograde.");
        }
        sample.graphSpanMilliseconds =
            (static_cast<float64>(sample.graphEndTimestamp -
                                  sample.graphStartTimestamp) *
             1000.0) /
            static_cast<float64>(timestampFrequency);
        try
        {
            sample.passTimings.reserve(timings.size());
            for (const RenderGraphPassTimingSample& timing : timings)
            {
                sample.passTimings.push_back(
                    RenderQualityPerformancePassTiming{
                        timing.executionIndex,
                        timing.passName,
                        timing.startTimestamp,
                        timing.endTimestamp,
                        timing.elapsedTimestampTicks,
                        timing.durationMilliseconds});
            }
        }
        catch (...)
        {
            return Reject(
                RenderQualityPerformanceFailure::SlotInitializationFailed,
                "Completed pass timing evidence could not be retained.");
        }
        m_measuredSamples[measuredIndex] = std::move(sample);
        ++m_diagnostics.completedMeasuredSamples;
    }
    m_completedOrdinals[sessionOrdinal] = true;
    m_lastCompletedSubmissionFrame = completedFrame;
    ++m_completedFrameCount;
    return true;
}

bool RenderQualityPerformanceSession::PublishCompletedEvidence() noexcept
{
    if (m_diagnostics.state == RenderQualityPerformanceSessionState::Completed)
    {
        return m_completedEvidence != nullptr &&
               m_completedEvidence->IsComplete();
    }
    if (m_diagnostics.completedWarmupSamples !=
            RVX_RENDER_QUALITY_PERFORMANCE_WARMUP_SAMPLE_COUNT ||
        m_diagnostics.completedMeasuredSamples !=
            RVX_RENDER_QUALITY_PERFORMANCE_MEASURED_SAMPLE_COUNT)
    {
        return Reject(
            RenderQualityPerformanceFailure::SampleCountMismatch,
            "The session drained without exactly 60 warmup and 300 measured completions.");
    }

    try
    {
        auto evidence = std::make_shared<RenderQualityPerformanceEvidence>();
        evidence->completedWarmupSamples =
            m_diagnostics.completedWarmupSamples;
        evidence->completedMeasuredSamples =
            m_diagnostics.completedMeasuredSamples;
        evidence->slotCapacity = m_diagnostics.slotCapacity;
        evidence->slotHighWaterMark = m_diagnostics.slotHighWaterMark;
        evidence->hardware = m_hardware;
        evidence->passSchemaHash = m_diagnostics.passSchemaHash;
        evidence->samples = std::move(m_measuredSamples);
        if (!evidence->IsComplete())
        {
            return Reject(
                RenderQualityPerformanceFailure::SampleCountMismatch,
                DescribeIncompleteEvidence(*evidence));
        }
        m_completedEvidence = std::move(evidence);
    }
    catch (...)
    {
        return Reject(
            RenderQualityPerformanceFailure::SlotInitializationFailed,
            "The immutable performance evidence could not be published.");
    }
    m_diagnostics.state = RenderQualityPerformanceSessionState::Completed;
    m_diagnostics.failure = RenderQualityPerformanceFailure::None;
    m_diagnostics.diagnostic.clear();
    return true;
}

bool RenderQualityPerformanceSession::Reject(
    RenderQualityPerformanceFailure failure,
    const char* diagnostic) noexcept
{
    m_diagnostics.state = RenderQualityPerformanceSessionState::Rejected;
    m_diagnostics.failure = failure;
    try
    {
        m_diagnostics.diagnostic = diagnostic != nullptr ? diagnostic : "";
    }
    catch (...)
    {
        m_diagnostics.diagnostic.clear();
    }
    m_completedEvidence.reset();
    m_recordingSlot = nullptr;
    return false;
}

void RenderQualityPerformanceSession::ReleaseSlot(Slot& slot) noexcept
{
    if (slot.timing)
    {
        const RenderGraphPassTimingState state =
            slot.timing->GetDiagnostics().state;
        if (state == RenderGraphPassTimingState::Completed ||
            state == RenderGraphPassTimingState::Rejected)
        {
            slot.timing->ResetCompletedTiming();
        }
    }
    if (m_recordingSlot == &slot)
        m_recordingSlot = nullptr;
    slot.frameIdentity = {};
    slot.viewIdentity = {};
    slot.executionBinding = {};
    slot.recordingBinding = {};
    slot.cpu = {};
    slot.telemetry = {};
    slot.graphicsCompletionValue = 0;
    slot.trackedUploadBytes = 0;
    slot.sessionOrdinal = 0;
    slot.graphicsTerminalCommandSpan = {};
    slot.inUse = false;
    slot.receiptBound = false;
    slot.presented = false;
    slot.cpuBound = false;
    slot.telemetryBound = false;
    slot.graphicsTerminalCommandSpanBound = false;
}

void RenderQualityPerformanceSession::ReleaseTerminalSlots() noexcept
{
    for (Slot& slot : m_slots)
    {
        if (!slot.inUse || !slot.timing)
            continue;
        const RenderGraphPassTimingState state =
            slot.timing->GetDiagnostics().state;
        if (state == RenderGraphPassTimingState::Idle ||
            state == RenderGraphPassTimingState::Completed ||
            state == RenderGraphPassTimingState::Rejected)
        {
            ReleaseSlot(slot);
        }
    }
}

void RenderQualityPerformanceSession::RefreshInFlightDiagnostics() noexcept
{
    m_diagnostics.inFlightSlots = static_cast<uint32>(std::count_if(
        m_slots.begin(), m_slots.end(), [](const Slot& slot)
        {
            return slot.inUse;
        }));
    m_diagnostics.slotHighWaterMark = std::max(
        m_diagnostics.slotHighWaterMark, m_diagnostics.inFlightSlots);
}

bool RenderQualityPerformanceSessionTestAccess::ArmSynthetic(
    RenderQualityPerformanceSession& session,
    uint64 initialCumulativeTrackedUploadBytes) noexcept
{
    RenderQualityPerformanceHardwareIdentity hardware;
    hardware.backend = "dx12";
    hardware.capabilityReportSha256 = std::string(64, 'a');
    if (!session.Arm(hardware, initialCumulativeTrackedUploadBytes))
        return false;
    session.m_slotsInitialized = true;
    session.m_diagnostics.state =
        RenderQualityPerformanceSessionState::Collecting;
    session.m_diagnostics.slotHighWaterMark = 1;
    return true;
}

bool RenderQualityPerformanceSessionTestAccess::CommitCompletedSample(
    RenderQualityPerformanceSession& session,
    uint32 sessionOrdinal,
    const RenderQualityPerformanceReceiptBinding& binding,
    const RenderQualityPerformanceCpuTiming& cpu,
    const RenderQualityPerformanceFrameTelemetry& telemetry,
    uint64 submittedFrame,
    uint64 completedFrame,
    uint64 completionValue,
    uint64 timestampFrequency,
    std::span<const RenderGraphPassTimingSample> timings) noexcept
{
    const RenderGpuFrameTimingSample graphicsTerminalCommandSpan{
        binding.frameIdentity.frameSequence,
        completionValue,
        10'000 + completionValue * 10,
        10'000 + completionValue * 10 + 400,
        400,
        timestampFrequency,
        64,
        (400.0 * 1000.0) / static_cast<float64>(timestampFrequency)};
    return CommitCompletedSampleWithGraphicsTerminal(
        session,
        sessionOrdinal,
        binding,
        cpu,
        telemetry,
        submittedFrame,
        completedFrame,
        completionValue,
        timestampFrequency,
        graphicsTerminalCommandSpan,
        timings);
}

bool RenderQualityPerformanceSessionTestAccess::
    CommitCompletedSampleWithGraphicsTerminal(
        RenderQualityPerformanceSession& session,
        uint32 sessionOrdinal,
        const RenderQualityPerformanceReceiptBinding& binding,
        const RenderQualityPerformanceCpuTiming& cpu,
        const RenderQualityPerformanceFrameTelemetry& telemetry,
        uint64 submittedFrame,
        uint64 completedFrame,
        uint64 completionValue,
        uint64 timestampFrequency,
        const RenderGpuFrameTimingSample& graphicsTerminalCommandSpan,
        std::span<const RenderGraphPassTimingSample> timings) noexcept
{
    if (!telemetry.IsValid() ||
        telemetry.frameIdentity != binding.frameIdentity)
    {
        return session.Reject(
            RenderQualityPerformanceFailure::TelemetryUnavailable,
            "Synthetic performance telemetry did not match the completed frame.");
    }
    if (telemetry.cumulativeTrackedUploadBytes <
        session.m_lastCumulativeTrackedUploadBytes)
    {
        return session.Reject(
            RenderQualityPerformanceFailure::UploadCounterDecreased,
            "Synthetic tracked upload counter decreased.");
    }
    uint64 trackedUploadBytes = telemetry.cumulativeTrackedUploadBytes -
        session.m_lastCumulativeTrackedUploadBytes;
    if (!TryAccumulateTrackedUploadBytes(
            trackedUploadBytes, telemetry.frameTrackedUploadBytes))
    {
        return session.Reject(
            RenderQualityPerformanceFailure::TelemetryUnavailable,
            "Synthetic per-frame tracked upload bytes overflowed.");
    }
    if (submittedFrame == 0 ||
        submittedFrame <= session.m_lastPresentedSubmissionFrame ||
        completedFrame < submittedFrame)
    {
        return session.Reject(
            RenderQualityPerformanceFailure::FrameOrdinalInvalid,
            "Synthetic presentation ordinals are duplicate or retrograde.");
    }
    session.m_lastCumulativeTrackedUploadBytes =
        telemetry.cumulativeTrackedUploadBytes;
    session.m_lastPresentedSubmissionFrame = submittedFrame;
    const bool committed = session.CommitCompletedSample(
        sessionOrdinal,
        binding,
        cpu,
        submittedFrame,
        completedFrame,
        trackedUploadBytes,
        telemetry.trackedUploadCoverageMask,
        telemetry.trackedEstimatedFootprint,
        telemetry.resourceCounts,
        completionValue,
        timestampFrequency,
        graphicsTerminalCommandSpan,
        timings);
    if (!committed)
        return false;
    ++session.m_scheduledFrameCount;
    if (session.m_completedFrameCount ==
        RVX_RENDER_QUALITY_PERFORMANCE_TOTAL_FRAME_COUNT)
    {
        return session.PublishCompletedEvidence();
    }
    return true;
}

void RenderQualityPerformanceSessionTestAccess::SaturateSlots(
    RenderQualityPerformanceSession& session) noexcept
{
    for (RenderQualityPerformanceSession::Slot& slot : session.m_slots)
        slot.inUse = true;
    session.RefreshInFlightDiagnostics();
}

void RenderQualityPerformanceSessionTestAccess::StageSyntheticRecordingSlot(
    RenderQualityPerformanceSession& session) noexcept
{
    RenderQualityPerformanceSession::Slot& slot = session.m_slots.front();
    slot.inUse = true;
    slot.frameIdentity = {1, 1, 1, 1};
    slot.viewIdentity = {1, 1, 0, ViewRole::Main};
    slot.receiptBound = true;
    session.m_recordingSlot = &slot;
    session.RefreshInFlightDiagnostics();
}

void RenderQualityPerformanceSessionTestAccess::StageSyntheticSubmittedSlot(
    RenderQualityPerformanceSession& session,
    uint64 frameSequence,
    uint64 completionValue) noexcept
{
    RenderQualityPerformanceSession::Slot& slot = session.m_slots.front();
    slot.inUse = true;
    slot.frameIdentity = {frameSequence, 1000 + frameSequence, 5, 17};
    slot.viewIdentity = {41, 0x0000000200000003ULL, 0, ViewRole::Main};
    slot.receiptBound = true;
    slot.presented = true;
    slot.sessionOrdinal = 0;
    slot.graphicsCompletionValue = completionValue;
    slot.graphicsTerminalCommandSpan = {};
    slot.graphicsTerminalCommandSpanBound = false;
    session.m_consumedGraphicsTerminalCommandSpans.fill({});
    session.RefreshInFlightDiagnostics();
}

void RenderQualityPerformanceSessionTestAccess::
    StageSyntheticCompletedPassTimingSlot(
        RenderQualityPerformanceSession& session,
        uint64 frameSequence,
        uint64 completionValue) noexcept
{
    StageSyntheticSubmittedSlot(session, frameSequence, completionValue);
    RenderQualityPerformanceSession::Slot& slot = session.m_slots.front();

    RenderQualityPerformanceReceiptBinding binding;
    binding.frameIdentity = slot.frameIdentity;
    binding.viewIdentity = slot.viewIdentity;
    binding.receiptSchemaVersion = RVX_RENDER_EXECUTION_RECEIPT_SCHEMA_VERSION;
    binding.receiptDigest.front() = 1;
    binding.sourceCommit = std::string(40, 'a');
    binding.preset = "win_x64_gfx24_debug";
    binding.backend = "dx12";
    binding.recipe = RenderPipelineRecipe::DeferredQuality;
    binding.quality = RenderQualityTier::High;
    binding.profileHash = ComputeRenderPipelineProfileHash(
        binding.recipe, binding.quality);

    const RenderQualityPerformanceDuration duration =
        RenderQualityPerformanceDuration::Available(1);
    slot.executionBinding = binding;
    slot.recordingBinding = binding;
    slot.cpu.extraction = duration;
    slot.cpu.visibilityAndDrawPlanning = duration;
    slot.cpu.frameApply = duration;
    slot.cpu.frameBeginAcquire = duration;
    slot.cpu.renderPrepare = duration;
    slot.cpu.policy = duration;
    slot.cpu.graphBuild = duration;
    slot.cpu.graphCompile = duration;
    slot.cpu.graphRealizeRecord = duration;
    slot.cpu.frameEndSubmit = duration;
    slot.cpu.present = duration;
    slot.cpu.acceptedFrameTotal = duration;
    slot.telemetry.frameIdentity = slot.frameIdentity;
    slot.telemetry.trackedUploadCoverageMask =
        RVX_RENDER_QUALITY_PERFORMANCE_REQUIRED_UPLOAD_COVERAGE;
    slot.telemetry.cumulativeTrackedUploadBytesAvailable = true;
    slot.telemetry.trackedEstimatedFootprint.coverageMask =
        RVX_RENDER_QUALITY_PERFORMANCE_REQUIRED_FOOTPRINT_COVERAGE;
    slot.telemetry.trackedEstimatedFootprint.available = true;
    slot.telemetry.resourceCounts.available = true;
    slot.submittedFrame = 1;
    slot.completedFrame = 0;
    slot.trackedUploadBytes = 0;
    slot.sessionOrdinal = 0;
    slot.receiptBound = true;
    slot.presented = true;
    slot.cpuBound = true;
    slot.telemetryBound = true;

    slot.timing->m_enabled = true;
    slot.timing->m_samples = {{0, "Depth", 100, 200, 100, 0.1}};
    slot.timing->m_diagnostics.state = RenderGraphPassTimingState::Completed;
    slot.timing->m_diagnostics.failure = RenderGraphPassTimingFailure::None;
    slot.timing->m_diagnostics.expectedPassCount = 1;
    slot.timing->m_diagnostics.recordedPassCount = 1;
    slot.timing->m_diagnostics.completedPassCount = 1;
    slot.timing->m_diagnostics.completionValue = completionValue;
    slot.timing->m_diagnostics.timestampFrequency = 1'000'000;
    slot.timing->m_diagnostics.timestampValidBits = 64;
    session.RefreshInFlightDiagnostics();
}

void RenderQualityPerformanceSessionTestAccess::PollSyntheticCompletion(
    RenderQualityPerformanceSession& session) noexcept
{
    // The staged test slot has already reached the completed state, so this
    // tracker is never queried. It simply exercises the real session polling
    // and finalization ordering without a backend fixture.
    RenderSubmissionTracker tracker;
    session.PollCompletion(tracker);
}

bool RenderQualityPerformanceSessionTestAccess::IsSyntheticSlotReusable(
    const RenderQualityPerformanceSession& session) noexcept
{
    const RenderQualityPerformanceSession::Slot& slot = session.m_slots.front();
    return !slot.inUse && !slot.frameIdentity.IsValid() &&
           !slot.viewIdentity.IsValid() && !slot.receiptBound &&
           !slot.presented && !slot.cpuBound && !slot.telemetryBound &&
           !slot.graphicsTerminalCommandSpanBound &&
           session.m_recordingSlot == nullptr;
}

bool RenderQualityPerformanceSessionTestAccess::RejectSaturatedFrame(
    RenderQualityPerformanceSession& session) noexcept
{
    if (session.FindFreeSlot() != nullptr)
        return true;
    return session.Reject(
        RenderQualityPerformanceFailure::SlotSaturated,
        "Synthetic timestamp slot saturation.");
}

bool RenderQualityPerformanceSessionTestAccess::PublishCompletedEvidence(
    RenderQualityPerformanceSession& session) noexcept
{
    return session.PublishCompletedEvidence();
}
} // namespace RVX
