/** @file SampleLifetimeQualification.cpp @brief Sustained sample lifetime gate. */

#include "Samples/SampleLifetimeQualification.h"

#include "Core/Diagnostics/JsonWriter.h"
#include "Samples/SampleCLI.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <numeric>
#include <ostream>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <Windows.h>
#include <Psapi.h>
#endif

namespace RVX
{
    namespace
    {
        constexpr uint64 ProcessGrowthAllowanceBytes = 32ULL * 1024ULL * 1024ULL;
        constexpr uint64 GPUProcessGrowthAllowanceBytes = 16ULL * 1024ULL * 1024ULL;
        constexpr float64 GrowthSlopeAllowanceBytesPerFrame = 4096.0;

        SampleLifetimeMetricSnapshot BuildMetrics(
            const RenderDiagnosticsSnapshot& diagnostics,
            const SampleProcessMemorySnapshot& processMemory)
        {
            const RenderGraphLifetimeDiagnostics& graph =
                diagnostics.renderGraphLifetime;
            SampleLifetimeMetricSnapshot metrics;
            metrics.frameSequence = graph.frameSequence;
            metrics.planHash = graph.planHash;
            metrics.physicalRealizationCount =
                graph.physicalRealizationCount;
            metrics.physicalTextureAllocationCount =
                graph.physicalTextureAllocationCount;
            metrics.physicalBufferAllocationCount =
                graph.physicalBufferAllocationCount;
            metrics.totalPooledMemoryBytes = graph.totalPooledMemoryBytes;
            metrics.transientViewCount = graph.transientViewCount;
            metrics.surfaceIncompatibleFrameDrops =
                diagnostics.frameTransport.surfaceIncompatibleDrops;
            metrics.inFlightTextureLeases = graph.inFlightTextureLeases;
            metrics.inFlightBufferLeases = graph.inFlightBufferLeases;
            metrics.leaseCommitCount = graph.leaseCommitCount;
            metrics.leaseAbortCount = graph.leaseAbortCount;
            metrics.completionRetirementCount =
                graph.completionRetirementCount;
            metrics.renderTargetActiveDescriptors =
                graph.descriptors.renderTargets.activeDescriptors;
            metrics.renderTargetPeakDescriptors =
                graph.descriptors.renderTargets.peakActiveDescriptors;
            metrics.depthStencilActiveDescriptors =
                graph.descriptors.depthStencils.activeDescriptors;
            metrics.depthStencilPeakDescriptors =
                graph.descriptors.depthStencils.peakActiveDescriptors;
            metrics.nativeValidationAvailable =
                diagnostics.nativeValidation.available;
            metrics.nativeValidationEnabled =
                diagnostics.nativeValidation.enabled;
            metrics.nativeValidationReadComplete =
                diagnostics.nativeValidation.readComplete;
            metrics.nativeValidationWarningCount =
                diagnostics.nativeValidation.warningCount;
            metrics.nativeValidationErrorCount =
                diagnostics.nativeValidation.errorCount;
            metrics.nativeValidationCorruptionCount =
                diagnostics.nativeValidation.corruptionCount;
            metrics.processPrivateBytes = processMemory.available
                                              ? processMemory.privateBytes
                                              : 0;
            metrics.gpuUsedMemoryBytes =
                diagnostics.frameFeatures.gpuUsedMemory;
            return metrics;
        }

        void UpdatePeak(SampleLifetimeMetricSnapshot& peak,
                        const SampleLifetimeMetricSnapshot& metrics)
        {
            peak.frameSequence = std::max(peak.frameSequence,
                                          metrics.frameSequence);
            peak.planHash = metrics.planHash;
            peak.physicalRealizationCount = std::max(
                peak.physicalRealizationCount,
                metrics.physicalRealizationCount);
            peak.physicalTextureAllocationCount = std::max(
                peak.physicalTextureAllocationCount,
                metrics.physicalTextureAllocationCount);
            peak.physicalBufferAllocationCount = std::max(
                peak.physicalBufferAllocationCount,
                metrics.physicalBufferAllocationCount);
            peak.totalPooledMemoryBytes = std::max(
                peak.totalPooledMemoryBytes,
                metrics.totalPooledMemoryBytes);
            peak.transientViewCount = std::max(
                peak.transientViewCount,
                metrics.transientViewCount);
            peak.surfaceIncompatibleFrameDrops = std::max(
                peak.surfaceIncompatibleFrameDrops,
                metrics.surfaceIncompatibleFrameDrops);
            peak.inFlightTextureLeases = std::max(
                peak.inFlightTextureLeases,
                metrics.inFlightTextureLeases);
            peak.inFlightBufferLeases = std::max(
                peak.inFlightBufferLeases,
                metrics.inFlightBufferLeases);
            peak.leaseCommitCount = std::max(peak.leaseCommitCount,
                                             metrics.leaseCommitCount);
            peak.leaseAbortCount = std::max(peak.leaseAbortCount,
                                            metrics.leaseAbortCount);
            peak.completionRetirementCount = std::max(
                peak.completionRetirementCount,
                metrics.completionRetirementCount);
            peak.renderTargetActiveDescriptors = std::max(
                peak.renderTargetActiveDescriptors,
                metrics.renderTargetActiveDescriptors);
            peak.renderTargetPeakDescriptors = std::max(
                peak.renderTargetPeakDescriptors,
                metrics.renderTargetPeakDescriptors);
            peak.depthStencilActiveDescriptors = std::max(
                peak.depthStencilActiveDescriptors,
                metrics.depthStencilActiveDescriptors);
            peak.depthStencilPeakDescriptors = std::max(
                peak.depthStencilPeakDescriptors,
                metrics.depthStencilPeakDescriptors);
            peak.nativeValidationAvailable =
                peak.nativeValidationAvailable ||
                metrics.nativeValidationAvailable;
            peak.nativeValidationEnabled =
                peak.nativeValidationEnabled ||
                metrics.nativeValidationEnabled;
            peak.nativeValidationReadComplete =
                peak.nativeValidationReadComplete &&
                metrics.nativeValidationReadComplete;
            peak.nativeValidationWarningCount = std::max(
                peak.nativeValidationWarningCount,
                metrics.nativeValidationWarningCount);
            peak.nativeValidationErrorCount = std::max(
                peak.nativeValidationErrorCount,
                metrics.nativeValidationErrorCount);
            peak.nativeValidationCorruptionCount = std::max(
                peak.nativeValidationCorruptionCount,
                metrics.nativeValidationCorruptionCount);
            peak.processPrivateBytes = std::max(peak.processPrivateBytes,
                                                metrics.processPrivateBytes);
            peak.gpuUsedMemoryBytes = std::max(peak.gpuUsedMemoryBytes,
                                              metrics.gpuUsedMemoryBytes);
        }

        uint64 AverageWindow(const std::array<uint64, 32>& values,
                             uint32 count)
        {
            if (count == 0)
            {
                return 0;
            }
            uint64 sum = 0;
            for (uint32 index = 0; index < count; ++index)
            {
                sum += values[index];
            }
            return sum / count;
        }

        void WriteMetrics(std::ostream& stream,
                          const char* name,
                          const SampleLifetimeMetricSnapshot& metrics,
                          const char* suffix)
        {
            stream << "  \"" << name << "\": {\n";
            stream << "    \"frameSequence\": " << metrics.frameSequence << ",\n";
            stream << "    \"planHash\": " << metrics.planHash << ",\n";
            stream << "    \"physicalRealizationCount\": "
                   << metrics.physicalRealizationCount << ",\n";
            stream << "    \"physicalTextureAllocationCount\": "
                   << metrics.physicalTextureAllocationCount << ",\n";
            stream << "    \"physicalBufferAllocationCount\": "
                   << metrics.physicalBufferAllocationCount << ",\n";
            stream << "    \"totalPooledMemoryBytes\": "
                   << metrics.totalPooledMemoryBytes << ",\n";
            stream << "    \"transientViewCount\": "
                   << metrics.transientViewCount << ",\n";
            stream << "    \"surfaceIncompatibleFrameDrops\": "
                   << metrics.surfaceIncompatibleFrameDrops << ",\n";
            stream << "    \"inFlightTextureLeases\": "
                   << metrics.inFlightTextureLeases << ",\n";
            stream << "    \"inFlightBufferLeases\": "
                   << metrics.inFlightBufferLeases << ",\n";
            stream << "    \"leaseCommitCount\": "
                   << metrics.leaseCommitCount << ",\n";
            stream << "    \"leaseAbortCount\": "
                   << metrics.leaseAbortCount << ",\n";
            stream << "    \"completionRetirementCount\": "
                   << metrics.completionRetirementCount << ",\n";
            stream << "    \"renderTargetActiveDescriptors\": "
                   << metrics.renderTargetActiveDescriptors << ",\n";
            stream << "    \"renderTargetPeakDescriptors\": "
                   << metrics.renderTargetPeakDescriptors << ",\n";
            stream << "    \"depthStencilActiveDescriptors\": "
                   << metrics.depthStencilActiveDescriptors << ",\n";
            stream << "    \"depthStencilPeakDescriptors\": "
                   << metrics.depthStencilPeakDescriptors << ",\n";
            stream << "    \"nativeValidationAvailable\": "
                   << Diagnostics::JsonBool(
                          metrics.nativeValidationAvailable) << ",\n";
            stream << "    \"nativeValidationEnabled\": "
                   << Diagnostics::JsonBool(
                          metrics.nativeValidationEnabled) << ",\n";
            stream << "    \"nativeValidationReadComplete\": "
                   << Diagnostics::JsonBool(
                          metrics.nativeValidationReadComplete) << ",\n";
            stream << "    \"nativeValidationWarningCount\": "
                   << metrics.nativeValidationWarningCount << ",\n";
            stream << "    \"nativeValidationErrorCount\": "
                   << metrics.nativeValidationErrorCount << ",\n";
            stream << "    \"nativeValidationCorruptionCount\": "
                   << metrics.nativeValidationCorruptionCount << ",\n";
            stream << "    \"processPrivateBytes\": "
                   << metrics.processPrivateBytes << ",\n";
            stream << "    \"gpuUsedMemoryBytes\": "
                   << metrics.gpuUsedMemoryBytes << "\n";
            stream << "  }" << suffix << "\n";
        }

        void WriteTrend(std::ostream& stream,
                        const char* name,
                        const SampleLifetimeTrend& trend,
                        const char* suffix)
        {
            stream << "  \"" << name << "\": {\n";
            stream << "    \"available\": "
                   << Diagnostics::JsonBool(trend.available) << ",\n";
            stream << "    \"sampleCount\": " << trend.sampleCount << ",\n";
            stream << "    \"firstWindowAverageBytes\": "
                   << trend.firstWindowAverageBytes << ",\n";
            stream << "    \"lastWindowAverageBytes\": "
                   << trend.lastWindowAverageBytes << ",\n";
            stream << "    \"peakBytes\": " << trend.peakBytes << ",\n";
            stream << "    \"netGrowthBytes\": "
                   << trend.netGrowthBytes << ",\n";
            stream << "    \"slopeBytesPerFrame\": "
                   << trend.slopeBytesPerFrame << "\n";
            stream << "  }" << suffix << "\n";
        }
    } // namespace

    void SampleLifetimeQualification::TrendState::Add(uint64 value)
    {
        const float64 x = static_cast<float64>(count);
        const float64 y = static_cast<float64>(value);
        sumX += x;
        sumY += y;
        sumXX += x * x;
        sumXY += x * y;
        peak = std::max(peak, value);
        if (firstCount < WindowSize)
        {
            firstWindow[firstCount++] = value;
        }
        lastWindow[lastIndex] = value;
        lastIndex = (lastIndex + 1U) % WindowSize;
        lastCount = std::min(lastCount + 1U, WindowSize);
        ++count;
    }

    SampleLifetimeTrend
        SampleLifetimeQualification::TrendState::Build() const
    {
        SampleLifetimeTrend trend;
        trend.available = count != 0;
        trend.sampleCount = count;
        trend.firstWindowAverageBytes =
            AverageWindow(firstWindow, firstCount);
        trend.lastWindowAverageBytes =
            AverageWindow(lastWindow, lastCount);
        trend.peakBytes = peak;
        trend.netGrowthBytes =
            static_cast<int64>(trend.lastWindowAverageBytes) -
            static_cast<int64>(trend.firstWindowAverageBytes);
        const float64 denominator =
            static_cast<float64>(count) * sumXX - sumX * sumX;
        if (count > 1 && denominator > 0.0)
        {
            trend.slopeBytesPerFrame =
                (static_cast<float64>(count) * sumXY - sumX * sumY) /
                denominator;
        }
        return trend;
    }

    SampleLifetimeQualification::SampleLifetimeQualification(
        const SampleLifetimeQualificationConfig& config,
        SampleLifetimeQualificationMetadata metadata)
    {
        m_report.config = config;
        m_report.metadata = std::move(metadata);
        if (config.warmupFrames == 0 || config.observationFrames == 0)
        {
            Fail("Lifetime qualification requires non-zero warmup and observation frames");
        }
    }

    void SampleLifetimeQualification::Observe(
        const RenderDiagnosticsSnapshot& diagnostics,
        const SampleProcessMemorySnapshot& processMemory,
        uint64 elapsedMs)
    {
        if (m_report.complete)
        {
            return;
        }

        ++m_report.readyFrames;
        m_report.elapsedMs = elapsedMs;
        const RenderGraphLifetimeDiagnostics& graph =
            diagnostics.renderGraphLifetime;
        if (!graph.available || graph.planHash == 0)
        {
            Fail("RenderGraph lifetime telemetry is unavailable after RenderReady");
            return;
        }

        if (!diagnostics.nativeValidation.available ||
            !diagnostics.nativeValidation.enabled ||
            !diagnostics.nativeValidation.readComplete)
        {
            Fail("Native graphics validation telemetry is unavailable, disabled, or incomplete");
            return;
        }
        if (diagnostics.nativeValidation.errorCount != 0 ||
            diagnostics.nativeValidation.corruptionCount != 0)
        {
            Fail("Native graphics validation reported an error or corruption message");
            return;
        }

        const SampleLifetimeMetricSnapshot metrics =
            BuildMetrics(diagnostics, processMemory);
        if (m_report.final.planHash != 0 &&
            m_report.final.planHash != metrics.planHash)
        {
            ++m_report.planHashChangeCount;
        }
        m_report.final = metrics;
        UpdatePeak(m_report.peak, metrics);
        if (m_report.warmupFramesObserved < m_report.config.warmupFrames)
        {
            m_warmupMaxInFlightTextures = std::max(
                m_warmupMaxInFlightTextures,
                metrics.inFlightTextureLeases);
            m_warmupMaxInFlightBuffers = std::max(
                m_warmupMaxInFlightBuffers,
                metrics.inFlightBufferLeases);
            ++m_report.warmupFramesObserved;
            if (m_report.warmupFramesObserved ==
                m_report.config.warmupFrames)
            {
                CapturePlateau(metrics);
            }
            return;
        }

        if (m_settleFramesRemaining > 0)
        {
            m_warmupMaxInFlightTextures = std::max(
                m_warmupMaxInFlightTextures,
                metrics.inFlightTextureLeases);
            m_warmupMaxInFlightBuffers = std::max(
                m_warmupMaxInFlightBuffers,
                metrics.inFlightBufferLeases);
            --m_settleFramesRemaining;
            if (m_settleFramesRemaining == 0)
            {
                CapturePlateau(metrics);
            }
            return;
        }

        ValidateObservation(graph, metrics);
        if (m_report.complete)
        {
            return;
        }

        ++m_report.observationFramesObserved;
        ++m_framesSinceResize;
        if (processMemory.available)
        {
            m_processTrend.Add(processMemory.privateBytes);
        }
        if (metrics.gpuUsedMemoryBytes != 0)
        {
            m_gpuTrend.Add(metrics.gpuUsedMemoryBytes);
        }
        CompleteIfSatisfied(elapsedMs);
    }

    void SampleLifetimeQualification::NotifyResize()
    {
        if (m_report.complete ||
            m_report.warmupFramesObserved < m_report.config.warmupFrames)
        {
            return;
        }
        ++m_report.resizeCount;
        m_framesSinceResize = 0;
        m_settleFramesRemaining = m_report.config.resizeSettleFrames;
        m_plateauReady = false;
        m_processTrend = {};
        m_gpuTrend = {};
        if (m_settleFramesRemaining == 0)
        {
            CapturePlateau(m_report.final);
        }
    }

    bool SampleLifetimeQualification::IsComplete() const noexcept
    {
        return m_report.complete;
    }

    bool SampleLifetimeQualification::HasFailed() const noexcept
    {
        return m_report.complete && !m_report.pass;
    }

    bool SampleLifetimeQualification::ShouldRequestResize() const noexcept
    {
        return !m_report.complete && m_plateauReady &&
               m_report.config.resizeIntervalFrames != 0 &&
               m_framesSinceResize >=
                   m_report.config.resizeIntervalFrames;
    }

    const SampleLifetimeQualificationReport&
        SampleLifetimeQualification::GetReport() const noexcept
    {
        return m_report;
    }

    void SampleLifetimeQualification::CapturePlateau(
        const SampleLifetimeMetricSnapshot& metrics)
    {
        m_plateauBaseline = metrics;
        if (m_report.plateauCount == 0)
        {
            m_report.baseline = metrics;
        }
        ++m_report.plateauCount;
        m_framesSinceResize = 0;
        m_plateauReady = true;
    }

    void SampleLifetimeQualification::ValidateObservation(
        const RenderGraphLifetimeDiagnostics& graph,
        const SampleLifetimeMetricSnapshot& metrics)
    {
        if (!m_plateauReady)
        {
            Fail("Lifetime observation has no completed allocation plateau");
            return;
        }
        const auto requireNoGrowth = [this](uint64 current,
                                            uint64 baseline,
                                            const char* label)
        {
            if (current > baseline)
            {
                Fail(std::string(label) + " grew after the allocation plateau");
            }
        };
        requireNoGrowth(metrics.physicalTextureAllocationCount,
                        m_plateauBaseline.physicalTextureAllocationCount,
                        "Physical texture allocation count");
        requireNoGrowth(metrics.physicalBufferAllocationCount,
                        m_plateauBaseline.physicalBufferAllocationCount,
                        "Physical buffer allocation count");
        requireNoGrowth(metrics.totalPooledMemoryBytes,
                        m_plateauBaseline.totalPooledMemoryBytes,
                        "Transient pooled memory");
        requireNoGrowth(metrics.transientViewCount,
                        m_plateauBaseline.transientViewCount,
                        "Transient texture view count");
        requireNoGrowth(metrics.surfaceIncompatibleFrameDrops,
                        m_plateauBaseline.surfaceIncompatibleFrameDrops,
                        "Surface-incompatible frame drop count");
        requireNoGrowth(metrics.renderTargetActiveDescriptors,
                        m_plateauBaseline.renderTargetActiveDescriptors,
                        "RTV active descriptor count");
        requireNoGrowth(metrics.renderTargetPeakDescriptors,
                        m_plateauBaseline.renderTargetPeakDescriptors,
                        "RTV descriptor high-water mark");
        requireNoGrowth(metrics.depthStencilActiveDescriptors,
                        m_plateauBaseline.depthStencilActiveDescriptors,
                        "DSV active descriptor count");
        requireNoGrowth(metrics.depthStencilPeakDescriptors,
                        m_plateauBaseline.depthStencilPeakDescriptors,
                        "DSV descriptor high-water mark");
        if (graph.texturePoolMissCount != 0 ||
            graph.bufferPoolMissCount != 0 ||
            graph.transientViewMissCount != 0)
        {
            Fail("Transient pool or graph view cache missed during observation");
        }
        if (graph.recordingTextureLeases != 0 ||
            graph.recordingBufferLeases != 0)
        {
            Fail("Recording leases remained after frame submission");
        }
        if (metrics.inFlightTextureLeases >
                m_warmupMaxInFlightTextures ||
            metrics.inFlightBufferLeases >
                m_warmupMaxInFlightBuffers)
        {
            Fail("In-flight lease count exceeded the warmup frames-in-flight bound");
        }
        const RenderDescriptorLifetimeDiagnostics& descriptors =
            graph.descriptors;
        const auto hasDescriptorFailure = [](
            const RenderDescriptorAllocatorDiagnostics& allocator)
        {
            return allocator.allocationFailures != 0 ||
                   allocator.validationFailures != 0;
        };
        if (hasDescriptorFailure(descriptors.resourceViews) ||
            hasDescriptorFailure(descriptors.samplers) ||
            hasDescriptorFailure(descriptors.renderTargets) ||
            hasDescriptorFailure(descriptors.depthStencils))
        {
            Fail("RHI descriptor allocation or validation failure was reported");
        }
        if (graph.partialRealizationRollbackCount != 0 ||
            graph.transientViewCreationFailureCount != 0 ||
            graph.leaseDeviceLostCount != 0 ||
            graph.leaseValidationFailureCount != 0)
        {
            Fail("RenderGraph realization, view, lease, or device lifetime failure was reported");
        }
        if (graph.leaseAbortCount > m_plateauBaseline.leaseAbortCount)
        {
            Fail("Unexpected transient lease abort growth was reported");
        }
    }

    void SampleLifetimeQualification::CompleteIfSatisfied(uint64 elapsedMs)
    {
        if (m_report.observationFramesObserved <
                m_report.config.observationFrames ||
            elapsedMs < m_report.config.minimumDurationMs)
        {
            return;
        }

        m_report.processPrivateMemory = m_processTrend.Build();
        m_report.gpuMemory = m_gpuTrend.Build();
        const auto validateTrend = [this](const SampleLifetimeTrend& trend,
                                          uint64 growthAllowance,
                                          const char* label)
        {
            if (trend.available &&
                trend.netGrowthBytes > static_cast<int64>(growthAllowance) &&
                trend.slopeBytesPerFrame >
                    GrowthSlopeAllowanceBytesPerFrame)
            {
                Fail(std::string(label) +
                     " shows sustained linear growth after warmup");
            }
        };
        validateTrend(m_report.processPrivateMemory,
                      ProcessGrowthAllowanceBytes,
                      "Process private memory");
        validateTrend(m_report.gpuMemory,
                      GPUProcessGrowthAllowanceBytes,
                      "GPU memory");
        if (m_report.complete)
        {
            return;
        }
        if (m_report.final.leaseCommitCount <=
                m_report.baseline.leaseCommitCount ||
            m_report.final.completionRetirementCount <=
                m_report.baseline.completionRetirementCount)
        {
            Fail("Completion-owned transient leases did not commit and retire during observation");
            return;
        }
        m_report.complete = true;
        m_report.pass = true;
    }

    void SampleLifetimeQualification::Fail(std::string reason)
    {
        if (m_report.failures.size() < 16)
        {
            m_report.failures.push_back(std::move(reason));
        }
        m_report.processPrivateMemory = m_processTrend.Build();
        m_report.gpuMemory = m_gpuTrend.Build();
        m_report.complete = true;
        m_report.pass = false;
    }

    SampleProcessMemorySnapshot CaptureSampleProcessMemory()
    {
        SampleProcessMemorySnapshot snapshot;
#if defined(_WIN32)
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof(counters);
        if (GetProcessMemoryInfo(
                GetCurrentProcess(),
                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                sizeof(counters)))
        {
            snapshot.available = true;
            snapshot.privateBytes =
                static_cast<uint64>(counters.PrivateUsage);
        }
#endif
        return snapshot;
    }

    bool WriteSampleLifetimeQualificationReport(
        const SampleLifetimeQualificationReport& report,
        const std::filesystem::path& path,
        std::string* outError)
    {
        std::error_code error;
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(path.parent_path(), error);
        }
        if (error)
        {
            if (outError)
            {
                *outError = "Failed to create lifetime report directory: " +
                            error.message();
            }
            return false;
        }
        std::ofstream stream(path, std::ios::out | std::ios::trunc);
        if (!stream)
        {
            if (outError)
            {
                *outError = "Failed to open lifetime report: " + path.string();
            }
            return false;
        }

        stream << "{\n";
        stream << "  \"schemaId\": "
               << Diagnostics::JsonString(report.schemaId) << ",\n";
        stream << "  \"schemaVersion\": " << report.schemaVersion << ",\n";
        stream << "  \"sampleName\": "
               << Diagnostics::JsonString(report.metadata.sampleName) << ",\n";
        stream << "  \"assetId\": "
               << Diagnostics::JsonString(report.metadata.assetId) << ",\n";
        stream << "  \"backend\": "
               << Diagnostics::JsonString(
                      GetSampleBackendName(report.metadata.backend)) << ",\n";
        stream << "  \"renderPath\": "
               << Diagnostics::JsonString(report.metadata.renderPath) << ",\n";
        stream << "  \"width\": " << report.metadata.width << ",\n";
        stream << "  \"height\": " << report.metadata.height << ",\n";
        stream << "  \"deterministicOrbit\": "
               << Diagnostics::JsonBool(report.metadata.deterministicOrbit)
               << ",\n";
        stream << "  \"warmupFrames\": " << report.config.warmupFrames << ",\n";
        stream << "  \"observationFrames\": "
               << report.config.observationFrames << ",\n";
        stream << "  \"minimumDurationMs\": "
               << report.config.minimumDurationMs << ",\n";
        stream << "  \"resizeIntervalFrames\": "
               << report.config.resizeIntervalFrames << ",\n";
        stream << "  \"resizeSettleFrames\": "
               << report.config.resizeSettleFrames << ",\n";
        stream << "  \"readyFrames\": " << report.readyFrames << ",\n";
        stream << "  \"warmupFramesObserved\": "
               << report.warmupFramesObserved << ",\n";
        stream << "  \"observationFramesObserved\": "
               << report.observationFramesObserved << ",\n";
        stream << "  \"resizeCount\": " << report.resizeCount << ",\n";
        stream << "  \"plateauCount\": " << report.plateauCount << ",\n";
        stream << "  \"planHashChangeCount\": "
               << report.planHashChangeCount << ",\n";
        stream << "  \"elapsedMs\": " << report.elapsedMs << ",\n";
        WriteMetrics(stream, "baseline", report.baseline, ",");
        WriteMetrics(stream, "final", report.final, ",");
        WriteMetrics(stream, "peak", report.peak, ",");
        WriteTrend(stream,
                   "processPrivateMemory",
                   report.processPrivateMemory,
                   ",");
        WriteTrend(stream, "gpuMemory", report.gpuMemory, ",");
        stream << "  \"failures\": [";
        for (size_t index = 0; index < report.failures.size(); ++index)
        {
            if (index != 0)
            {
                stream << ", ";
            }
            stream << Diagnostics::JsonString(report.failures[index]);
        }
        stream << "],\n";
        stream << "  \"complete\": "
               << Diagnostics::JsonBool(report.complete) << ",\n";
        stream << "  \"pass\": "
               << Diagnostics::JsonBool(report.pass) << "\n";
        stream << "}\n";
        return static_cast<bool>(stream);
    }
} // namespace RVX
