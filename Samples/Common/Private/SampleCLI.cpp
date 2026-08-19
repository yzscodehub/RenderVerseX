/**
 * @file SampleCLI.cpp
 * @brief Shared command-line and JSON report helpers for sample applications.
 */

#include "Samples/SampleCLI.h"

#include "Core/Diagnostics/JsonWriter.h"
#include "RHI/RHI.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <iostream>
#include <ostream>
#include <string>
#include <system_error>
#include <utility>

namespace RVX
{
    namespace
    {
        std::string ToLower(std::string text)
        {
            std::transform(text.begin(),
                           text.end(),
                           text.begin(),
                           [](unsigned char ch)
                           {
                               return static_cast<char>(std::tolower(ch));
                           });
            return text;
        }

        bool ParsePositiveUInt(const char* text, uint32& outValue)
        {
            if (!text || text[0] == '\0')
            {
                return false;
            }

            uint32 value = 0;
            const char* end = text + std::char_traits<char>::length(text);
            const std::from_chars_result result = std::from_chars(text, end, value);
            if (result.ec != std::errc{} || result.ptr != end)
            {
                return false;
            }

            outValue = value;
            return true;
        }

        void SetError(std::string* outError, const std::string& error)
        {
            if (outError)
            {
                *outError = error;
            }
        }

        void WriteStringArray(std::ostream& stream,
                              const char* name,
                              const std::vector<std::string>& values,
                              const char* suffix)
        {
            stream << "  \"" << name << "\": [";
            for (size_t i = 0; i < values.size(); ++i)
            {
                if (i > 0)
                {
                    stream << ", ";
                }
                stream << Diagnostics::JsonString(values[i]);
            }
            stream << "]" << suffix << "\n";
        }

        void WriteDiagnosticUInt64(
            std::ostream& stream,
            const DiagnosticValue<uint64>& diagnostic)
        {
            stream << "{\"available\":"
                   << Diagnostics::JsonBool(diagnostic.IsAvailable())
                   << ",\"reason\":"
                   << Diagnostics::JsonString(diagnostic.GetReason());
            if (diagnostic.IsAvailable())
            {
                stream << ",\"value\":" << *diagnostic.GetValue();
            }
            stream << "}";
        }

        std::string FormatRawBits(uint32 value, uint32 digits)
        {
            static constexpr char HexDigits[] = "0123456789ABCDEF";
            std::string result(digits + 2U, '0');
            result[0] = '0';
            result[1] = 'x';
            for (uint32 index = 0; index < digits; ++index)
            {
                const uint32 shift = (digits - 1U - index) * 4U;
                result[index + 2U] = HexDigits[(value >> shift) & 0xFU];
            }
            return result;
        }

        void WriteRawBits(std::ostream& stream,
                          const std::array<uint16, 4>& values)
        {
            stream << "[";
            for (size_t index = 0; index < values.size(); ++index)
            {
                if (index != 0)
                {
                    stream << ", ";
                }
                stream << Diagnostics::JsonString(FormatRawBits(values[index], 4));
            }
            stream << "]";
        }

        void WriteRawBits(std::ostream& stream,
                          const std::array<uint8, 4>& values)
        {
            stream << "[";
            for (size_t index = 0; index < values.size(); ++index)
            {
                if (index != 0)
                {
                    stream << ", ";
                }
                stream << Diagnostics::JsonString(FormatRawBits(values[index], 2));
            }
            stream << "]";
        }

        void WriteContentIdentity(
            std::ostream& stream,
            const Resource::ResourceContentIdentity& identity)
        {
            stream << "{\"schemaVersion\":" << identity.schemaVersion
                   << ",\"domain\":" << Diagnostics::JsonString(
                          Resource::GetResourceContentIdentityDomainName(
                              identity.domain))
                   << ",\"scope\":" << Diagnostics::JsonString(
                          Resource::GetResourceContentIdentityScopeName(
                              identity.scope))
                   << ",\"algorithm\":" << Diagnostics::JsonString(
                          Resource::GetResourceContentHashAlgorithmName(
                              identity.algorithm))
                   << ",\"digest\":" << Diagnostics::JsonString(identity.digest)
                   << ",\"byteCount\":" << identity.byteCount
                   << ",\"fileCount\":" << identity.fileCount << "}";
        }

        void WriteUploadWorkDiagnostics(
            std::ostream& stream,
            const char* name,
            const SampleRenderUploadWorkDiagnostics& diagnostics)
        {
            stream << "    \"" << name << "\": {\n";
            stream << "      \"cpuCopiedPayloadBytes\": "
                   << diagnostics.cpuCopiedPayloadBytes << ",\n";
            stream << "      \"committedPayloadBytes\": "
                   << diagnostics.committedPayloadBytes << ",\n";
            stream << "      \"mappedRangeCount\": "
                   << diagnostics.mappedRangeCount << ",\n";
            stream << "      \"committedRangeCount\": "
                   << diagnostics.committedRangeCount << ",\n";
            stream << "      \"hostVisibilitySynchronizedBytes\": ";
            WriteDiagnosticUInt64(
                stream, diagnostics.hostVisibilitySynchronizedBytes);
            stream << ",\n";
            stream << "      \"hostVisibilitySynchronizationScopeRangeCounts\": [";
            for (size_t index = 0;
                 index < diagnostics.hostVisibilitySynchronizationScopeRangeCounts.size();
                 ++index)
            {
                if (index != 0)
                {
                    stream << ", ";
                }
                stream << diagnostics
                              .hostVisibilitySynchronizationScopeRangeCounts[index];
            }
            stream << "],\n";
            stream << "      \"hostVisibilitySynchronizationScopeBytes\": [";
            for (size_t index = 0;
                 index < diagnostics.hostVisibilitySynchronizationScopeBytes.size();
                 ++index)
            {
                if (index != 0)
                {
                    stream << ", ";
                }
                stream << diagnostics.hostVisibilitySynchronizationScopeBytes[index];
            }
            stream << "],\n";
            stream << "      \"gpuCopyBytes\": "
                   << diagnostics.gpuCopyBytes << ",\n";
            stream << "      \"gpuCopyRangeCount\": "
                   << diagnostics.gpuCopyRangeCount << "\n";
            stream << "    },\n";
        }
    } // namespace

    SampleFeatureReporter::SampleFeatureReporter(SampleReport& report)
        : m_report(&report)
    {
    }

    void SampleFeatureReporter::Enable(std::string feature)
    {
        if (m_report)
        {
            m_report->enabledFeatures.push_back(std::move(feature));
        }
    }

    void SampleFeatureReporter::Unsupported(std::string feature)
    {
        if (m_report)
        {
            m_report->unsupportedFeatures.push_back(std::move(feature));
        }
    }

    void SampleFeatureReporter::Fallback(std::string reason)
    {
        if (m_report)
        {
            m_report->fallbackReasons.push_back(std::move(reason));
        }
    }

    void SampleFeatureReporter::ResourceDiagnostic(std::string diagnostic)
    {
        if (m_report)
        {
            m_report->resourceDiagnostics.push_back(std::move(diagnostic));
        }
    }

    void SampleFeatureReporter::SetScenarioContractRevision(
        uint32 contractRevision)
    {
        if (m_report)
        {
            m_report->scenario.contractRevision = contractRevision;
        }
    }

    void SampleFeatureReporter::SetScenarioPhase(std::string phase)
    {
        if (!m_report)
        {
            return;
        }

        if (phase.empty())
        {
            m_report->pass = false;
            m_report->resourceDiagnostics.push_back(
                "scenario contract violation: phase must not be empty");
            return;
        }

        m_report->scenario.phase = std::move(phase);
    }

    bool SampleFeatureReporter::AppendScenarioAction(
        SampleReportScenarioAction action)
    {
        if (!m_report)
        {
            return false;
        }

        const auto duplicate = std::find_if(
            m_report->scenario.actions.begin(),
            m_report->scenario.actions.end(),
            [&action](const SampleReportScenarioAction& existing)
            {
                return existing.name == action.name;
            });
        if (action.name.empty() || duplicate != m_report->scenario.actions.end())
        {
            m_report->pass = false;
            m_report->resourceDiagnostics.push_back(
                "scenario contract violation: duplicate or empty action name '" +
                action.name + "'");
            return false;
        }

        m_report->scenario.actions.push_back(std::move(action));
        return true;
    }

    bool SampleFeatureReporter::AppendScenarioInvariant(
        SampleReportScenarioInvariant invariant)
    {
        if (!m_report)
        {
            return false;
        }

        const auto duplicate = std::find_if(
            m_report->scenario.invariants.begin(),
            m_report->scenario.invariants.end(),
            [&invariant](const SampleReportScenarioInvariant& existing)
            {
                return existing.name == invariant.name;
            });
        if (invariant.name.empty() ||
            duplicate != m_report->scenario.invariants.end())
        {
            m_report->pass = false;
            m_report->resourceDiagnostics.push_back(
                "scenario contract violation: duplicate or empty invariant name '" +
                invariant.name + "'");
            return false;
        }

        m_report->scenario.invariants.push_back(std::move(invariant));
        return true;
    }

    bool SampleFeatureReporter::AppendScenarioMetric(
        SampleReportScenarioMetric metric)
    {
        if (!m_report)
        {
            return false;
        }

        const auto duplicate = std::find_if(
            m_report->scenario.metrics.begin(),
            m_report->scenario.metrics.end(),
            [&metric](const SampleReportScenarioMetric& existing)
            {
                return existing.name == metric.name;
            });
        if (metric.name.empty() || duplicate != m_report->scenario.metrics.end())
        {
            m_report->pass = false;
            m_report->resourceDiagnostics.push_back(
                "scenario contract violation: duplicate or empty metric name '" +
                metric.name + "'");
            return false;
        }

        m_report->scenario.metrics.push_back(std::move(metric));
        return true;
    }

    bool ParseSampleBackend(const std::string& text, RHIBackendType& outBackend)
    {
        const std::string value = ToLower(text);
        if (value == "auto")
        {
            outBackend = RHIBackendType::Auto;
            return true;
        }
        if (value == "dx11" || value == "d3d11" || value == "directx11")
        {
            outBackend = RHIBackendType::DX11;
            return true;
        }
        if (value == "dx12" || value == "d3d12" || value == "directx12")
        {
            outBackend = RHIBackendType::DX12;
            return true;
        }
        if (value == "vulkan" || value == "vk")
        {
            outBackend = RHIBackendType::Vulkan;
            return true;
        }
        if (value == "metal" || value == "mtl")
        {
            outBackend = RHIBackendType::Metal;
            return true;
        }
        if (value == "opengl" || value == "gl")
        {
            outBackend = RHIBackendType::OpenGL;
            return true;
        }

        return false;
    }

    const char* GetSampleBackendName(RHIBackendType backend)
    {
        switch (backend)
        {
            case RHIBackendType::Auto:
                return "auto";
            case RHIBackendType::DX11:
                return "dx11";
            case RHIBackendType::DX12:
                return "dx12";
            case RHIBackendType::Vulkan:
                return "vulkan";
            case RHIBackendType::Metal:
                return "metal";
            case RHIBackendType::OpenGL:
                return "opengl";
            case RHIBackendType::None:
                return "none";
            default:
                return "unknown";
        }
    }

    bool ParseSampleCLI(int argc,
                        const char* const* argv,
                        SampleCLIOptions& options,
                        std::string* outError)
    {
        bool framesSpecified = false;

        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i] ? argv[i] : "";
            const auto requireValue = [&](const char* name) -> const char*
            {
                if (i + 1 >= argc)
                {
                    SetError(outError, std::string("Missing value for ") + name);
                    return nullptr;
                }
                return argv[++i];
            };

            if (arg == "--help" || arg == "-h")
            {
                options.showHelp = true;
            }
            else if (arg == "--backend")
            {
                const char* value = requireValue("--backend");
                if (!value)
                {
                    return false;
                }
                if (!ParseSampleBackend(value, options.backend))
                {
                    SetError(outError, std::string("Invalid --backend value: ") + value);
                    return false;
                }
            }
            else if (arg == "--dx11" || arg == "-d11")
            {
                options.backend = RHIBackendType::DX11;
            }
            else if (arg == "--dx12" || arg == "-d12")
            {
                options.backend = RHIBackendType::DX12;
            }
            else if (arg == "--vulkan" || arg == "-vk")
            {
                options.backend = RHIBackendType::Vulkan;
            }
            else if (arg == "--metal" || arg == "-mtl")
            {
                options.backend = RHIBackendType::Metal;
            }
            else if (arg == "--opengl" || arg == "-gl")
            {
                options.backend = RHIBackendType::OpenGL;
            }
            else if (arg == "--smoke")
            {
                options.smoke = true;
            }
            else if (arg == "--frames")
            {
                const char* value = requireValue("--frames");
                if (!value)
                {
                    return false;
                }
                if (!ParsePositiveUInt(value, options.frames))
                {
                    SetError(outError, std::string("Invalid --frames value: ") + value);
                    return false;
                }
                framesSpecified = true;
            }
            else if (arg == "--screenshot")
            {
                const char* value = requireValue("--screenshot");
                if (!value)
                {
                    return false;
                }
                options.screenshotPath = value;
            }
            else if (arg == "--pixel-probe")
            {
                const char* x = requireValue("--pixel-probe");
                if (!x)
                {
                    return false;
                }
                const char* y = requireValue("--pixel-probe");
                if (!y)
                {
                    return false;
                }
                if (!ParsePositiveUInt(x, options.pixelProbeX) ||
                    !ParsePositiveUInt(y, options.pixelProbeY))
                {
                    SetError(outError,
                             std::string("Invalid --pixel-probe coordinates: ") +
                                 x + " " + y);
                    return false;
                }
                options.pixelProbeEnabled = true;
            }
            else if (arg == "--gpu-scene-culling-qualification")
            {
                options.gpuSceneCullingQualificationEnabled = true;
            }
            else if (arg == "--direct-opaque-raster-readback-qualification")
            {
                options.directOpaqueRasterReadbackQualificationEnabled = true;
            }
            else if (arg == "--report")
            {
                const char* value = requireValue("--report");
                if (!value)
                {
                    return false;
                }
                options.reportPath = value;
            }
            else if (arg == "--width")
            {
                const char* value = requireValue("--width");
                if (!value)
                {
                    return false;
                }
                if (!ParsePositiveUInt(value, options.width) || options.width == 0)
                {
                    SetError(outError, std::string("Invalid --width value: ") + value);
                    return false;
                }
            }
            else if (arg == "--height")
            {
                const char* value = requireValue("--height");
                if (!value)
                {
                    return false;
                }
                if (!ParsePositiveUInt(value, options.height) || options.height == 0)
                {
                    SetError(outError, std::string("Invalid --height value: ") + value);
                    return false;
                }
            }
            else if (arg == "--quality")
            {
                const char* value = requireValue("--quality");
                if (!value)
                {
                    return false;
                }
                options.quality = value;
            }
            else if (arg == "--diagnostics")
            {
                options.diagnostics = true;
            }
            else if (arg == "--validation")
            {
                options.enableValidation = true;
            }
            else if (arg == "--no-validation")
            {
                options.enableValidation = false;
            }
            else
            {
                SetError(outError, std::string("Unknown argument: ") + arg);
                return false;
            }
        }

        if (options.smoke && !framesSpecified && options.frames == 0)
        {
            options.frames = 8;
        }

        return true;
    }

    void PrintSampleCLIUsage(std::ostream& stream, const char* executableName)
    {
        stream
            << executableName << "\n"
            << "  --backend <auto|dx11|dx12|vulkan|metal|opengl>\n"
            << "  --smoke\n"
            << "  --frames <count>\n"
            << "  --screenshot <path.ppm>\n"
            << "  --pixel-probe <x> <y>\n"
            << "  --gpu-scene-culling-qualification\n"
            << "  --direct-opaque-raster-readback-qualification\n"
            << "  --report <path.json>\n"
            << "  --width <pixels>\n"
            << "  --height <pixels>\n"
            << "  --quality <name>\n"
            << "  --diagnostics\n"
            << "  --validation | --no-validation\n";
    }

    void WriteSampleReportJson(std::ostream& stream, const SampleReport& report)
    {
        const auto writeRasterTranscript = [&stream](
                                              const char* name,
                                              const SampleRasterTranscript& value,
                                              const char* indent,
                                              bool trailingComma = true)
        {
            stream << indent << "\"" << name << "\": {\n";
            stream << indent << "  \"available\": "
                   << Diagnostics::JsonBool(value.available) << ",\n";
            stream << indent << "  \"entryCount\": " << value.entryCount
                   << ",\n";
            stream << indent << "  \"orderedIdentityHash\": "
                   << value.orderedIdentityHash << ",\n";
            stream << indent << "  \"consumedPayloadHash\": "
                   << value.consumedPayloadHash << ",\n";
            stream << indent << "  \"unorderedIdentityHash\": "
                   << value.unorderedIdentityHash << ",\n";
            stream << indent << "  \"unorderedIdentityHashSecondary\": "
                   << value.unorderedIdentityHashSecondary << ",\n";
            stream << indent << "  \"unorderedConsumedPayloadHash\": "
                   << value.unorderedConsumedPayloadHash << ",\n";
            stream << indent << "  \"unorderedConsumedPayloadHashSecondary\": "
                   << value.unorderedConsumedPayloadHashSecondary << "\n";
            stream << indent << "}" << (trailingComma ? ",\n" : "\n");
        };
        const auto writeGPUSceneQualification =
            [&stream, &writeRasterTranscript](
                const char* name,
                const SampleGPUSceneCullingQualification& value)
        {
            stream << "    \"" << name << "\": {\n";
            stream << "      \"requested\": "
                   << Diagnostics::JsonBool(value.requested) << ",\n";
            stream << "      \"required\": "
                   << Diagnostics::JsonBool(value.required) << ",\n";
            stream << "      \"readbackAllocated\": "
                   << Diagnostics::JsonBool(value.readbackAllocated) << ",\n";
            stream << "      \"copyRecorded\": "
                   << Diagnostics::JsonBool(value.copyRecorded) << ",\n";
            stream << "      \"submissionAccepted\": "
                   << Diagnostics::JsonBool(value.submissionAccepted) << ",\n";
            stream << "      \"completionObserved\": "
                   << Diagnostics::JsonBool(value.completionObserved) << ",\n";
            stream << "      \"compared\": "
                   << Diagnostics::JsonBool(value.compared) << ",\n";
            stream << "      \"matched\": "
                   << Diagnostics::JsonBool(value.matched) << ",\n";
            stream << "      \"inputCoverageCompared\": "
                   << Diagnostics::JsonBool(value.inputCoverageCompared)
                   << ",\n";
            stream << "      \"inputCoverageMatched\": "
                   << Diagnostics::JsonBool(value.inputCoverageMatched)
                   << ",\n";
            stream << "      \"directVisibilityCoverageCompared\": "
                   << Diagnostics::JsonBool(value.directVisibilityCoverageCompared)
                   << ",\n";
            stream << "      \"directVisibilityCoverageMatched\": "
                   << Diagnostics::JsonBool(value.directVisibilityCoverageMatched)
                   << ",\n";
            stream << "      \"cullOutputsCompared\": "
                   << Diagnostics::JsonBool(value.cullOutputsCompared)
                   << ",\n";
            stream << "      \"cullOutputsMatched\": "
                   << Diagnostics::JsonBool(value.cullOutputsMatched)
                   << ",\n";
            stream << "      \"indirectArgumentsCompared\": "
                   << Diagnostics::JsonBool(value.indirectArgumentsCompared)
                   << ",\n";
            stream << "      \"indirectArgumentsMatched\": "
                   << Diagnostics::JsonBool(value.indirectArgumentsMatched)
                   << ",\n";
            stream << "      \"rasterPayloadCompared\": "
                   << Diagnostics::JsonBool(value.rasterPayloadCompared)
                   << ",\n";
            stream << "      \"rasterPayloadMatched\": "
                   << Diagnostics::JsonBool(value.rasterPayloadMatched)
                   << ",\n";
            stream << "      \"mismatch\": "
                   << Diagnostics::JsonString(value.mismatch) << ",\n";
            stream << "      \"activeRowCount\": " << value.activeRowCount
                   << ",\n";
            stream << "      \"drawGroupCount\": " << value.drawGroupCount
                   << ",\n";
            stream << "      \"expectedInputPacketCount\": "
                   << value.expectedInputPacketCount << ",\n";
            stream << "      \"expectedGPUInputPacketCount\": "
                   << value.expectedGPUInputPacketCount << ",\n";
            stream << "      \"observedGPUInputPacketCount\": "
                   << value.observedGPUInputPacketCount << ",\n";
            stream << "      \"expectedDirectVisiblePacketCount\": "
                   << value.expectedDirectVisiblePacketCount << ",\n";
            stream << "      \"observedGPUVisiblePacketCount\": "
                   << value.observedGPUVisiblePacketCount << ",\n";
            stream << "      \"missingDirectVisiblePacketCount\": "
                   << value.missingDirectVisiblePacketCount << ",\n";
            stream << "      \"gpuOnlyVisiblePacketCount\": "
                   << value.gpuOnlyVisiblePacketCount << ",\n";
            stream << "      \"directInputPacketCount\": "
                   << value.directInputPacketCount << ",\n";
            stream << "      \"skippedInputPacketCount\": "
                   << value.skippedInputPacketCount << ",\n";
            stream << "      \"expectedVisibleInstanceCount\": "
                   << value.expectedVisibleInstanceCount << ",\n";
            stream << "      \"observedVisibleInstanceCount\": "
                   << value.observedVisibleInstanceCount << ",\n";
            stream << "      \"expectedSubmittedDrawCount\": "
                   << value.expectedSubmittedDrawCount << ",\n";
            stream << "      \"observedSubmittedDrawCount\": "
                   << value.observedSubmittedDrawCount << ",\n";
            stream << "      \"firstMismatchActiveRow\": "
                   << value.firstMismatchActiveRow << ",\n";
            stream << "      \"firstMismatchDrawGroup\": "
                   << value.firstMismatchDrawGroup << ",\n";
            stream << "      \"firstMismatchResidentRow\": "
                   << value.firstMismatchResidentRow << ",\n";
            stream << "      \"expectedValue\": " << value.expectedValue
                   << ",\n";
            stream << "      \"observedValue\": " << value.observedValue
                   << ",\n";
            stream << "      \"expectedGPUInputIdentityHash\": "
                   << value.expectedGPUInputIdentityHash << ",\n";
            stream << "      \"observedGPUInputIdentityHash\": "
                   << value.observedGPUInputIdentityHash << ",\n";
            stream << "      \"planPacketIdentityHash\": "
                   << value.planPacketIdentityHash << ",\n";
            stream << "      \"expectedDirectVisibleIdentityHash\": "
                   << value.expectedDirectVisibleIdentityHash << ",\n";
            stream << "      \"observedGPUVisibleIdentityHash\": "
                   << value.observedGPUVisibleIdentityHash << ",\n";
            stream << "      \"expectedRasterPayloadHash\": "
                   << value.expectedRasterPayloadHash << ",\n";
            stream << "      \"observedRasterPayloadHash\": "
                   << value.observedRasterPayloadHash << ",\n";
            stream << "      \"expectedIndirectArgumentsHash\": "
                   << value.expectedIndirectArgumentsHash << ",\n";
            stream << "      \"observedIndirectArgumentsHash\": "
                   << value.observedIndirectArgumentsHash << ",\n";
            writeRasterTranscript("tierOneRasterTranscript",
                                  value.tierOneRasterTranscript,
                                  "      ");
            writeRasterTranscript("tierOneRasterTranscriptReference",
                                  value.tierOneRasterTranscriptReference,
                                  "      ");
            stream << "      \"tierOneRasterTranscriptCompared\": "
                   << Diagnostics::JsonBool(
                          value.tierOneRasterTranscriptCompared)
                   << ",\n";
            stream << "      \"tierOneRasterTranscriptMatched\": "
                   << Diagnostics::JsonBool(
                          value.tierOneRasterTranscriptMatched)
                   << ",\n";
            stream << "      \"firstRasterTranscriptMismatchEntry\": "
                   << value.firstRasterTranscriptMismatchEntry << ",\n";
            stream << "      \"expectedRasterTranscriptIdentityHash\": "
                   << value.expectedRasterTranscriptIdentityHash << ",\n";
            stream << "      \"observedRasterTranscriptIdentityHash\": "
                   << value.observedRasterTranscriptIdentityHash << ",\n";
            stream << "      \"expectedRasterTranscriptPayloadHash\": "
                   << value.expectedRasterTranscriptPayloadHash << ",\n";
            stream << "      \"observedRasterTranscriptPayloadHash\": "
                   << value.observedRasterTranscriptPayloadHash << ",\n";
            stream << "      \"cpuPayloadBytes\": "
                   << value.cpuPayloadBytes << ",\n";
            stream << "      \"capturedTier\": "
                   << Diagnostics::JsonString(value.capturedTier) << ",\n";
            stream << "      \"frameSequence\": " << value.frameSequence
                   << ",\n";
            stream << "      \"recordEpoch\": " << value.recordEpoch
                   << ",\n";
            stream << "      \"gpuSceneLeaseVersion\": "
                   << value.gpuSceneLeaseVersion << ",\n";
            stream << "      \"candidateVersion\": "
                   << value.candidateVersion << ",\n";
            stream << "      \"activeRowVersion\": "
                   << value.activeRowVersion << ",\n";
            stream << "      \"completionValue\": "
                   << value.completionValue << "\n";
            stream << "    },\n";
        };
        const auto writeDirectRasterReadbackQualification =
            [&stream, &writeRasterTranscript](
                const SampleDirectRasterReadbackQualification& value)
        {
            stream << "    \"directOpaqueRasterReadbackQualification\": {\n";
            stream << "      \"requested\": "
                   << Diagnostics::JsonBool(value.requested) << ",\n";
            stream << "      \"required\": "
                   << Diagnostics::JsonBool(value.required) << ",\n";
            stream << "      \"readbackAllocated\": "
                   << Diagnostics::JsonBool(value.readbackAllocated) << ",\n";
            stream << "      \"copyRecorded\": "
                   << Diagnostics::JsonBool(value.copyRecorded) << ",\n";
            stream << "      \"submissionAccepted\": "
                   << Diagnostics::JsonBool(value.submissionAccepted) << ",\n";
            stream << "      \"completionObserved\": "
                   << Diagnostics::JsonBool(value.completionObserved) << ",\n";
            stream << "      \"compared\": "
                   << Diagnostics::JsonBool(value.compared) << ",\n";
            stream << "      \"matched\": "
                   << Diagnostics::JsonBool(value.matched) << ",\n";
            stream << "      \"identity\": "
                   << Diagnostics::JsonBool(value.identity) << ",\n";
            stream << "      \"allDirectDrawsInstanced\": "
                   << Diagnostics::JsonBool(value.allDirectDrawsInstanced) << ",\n";
            stream << "      \"mismatch\": "
                   << Diagnostics::JsonString(value.mismatch) << ",\n";
            stream << "      \"frameSequence\": " << value.frameSequence
                   << ",\n";
            stream << "      \"recordEpoch\": " << value.recordEpoch
                   << ",\n";
            stream << "      \"sourceFrameSlot\": " << value.sourceFrameSlot
                   << ",\n";
            stream << "      \"firstMismatchIndex\": "
                   << value.firstMismatchIndex << ",\n";
            stream << "      \"firstMismatchRow\": "
                   << value.firstMismatchRow << ",\n";
            stream << "      \"cpuPayloadBytes\": " << value.cpuPayloadBytes
                   << ",\n";
            stream << "      \"completionValue\": " << value.completionValue
                   << ",\n";
            writeRasterTranscript("expectedTranscript", value.expectedTranscript,
                                  "      ");
            writeRasterTranscript("observedTranscript", value.observedTranscript,
                                  "      ", false);
            stream << "    },\n";
        };
        stream << "{\n";
        stream << "  \"schemaId\": " << Diagnostics::JsonString(report.schemaId) << ",\n";
        stream << "  \"schemaVersion\": " << report.schemaVersion << ",\n";
        stream << "  \"sampleName\": " << Diagnostics::JsonString(report.sampleName) << ",\n";
        stream << "  \"category\": " << Diagnostics::JsonString(report.category) << ",\n";
        stream << "  \"requestedBackend\": "
               << Diagnostics::JsonString(GetSampleBackendName(report.requestedBackend)) << ",\n";
        stream << "  \"backend\": " << Diagnostics::JsonString(GetSampleBackendName(report.backend)) << ",\n";
        stream << "  \"frameCount\": " << report.frameCount << ",\n";
        stream << "  \"submittedFrameSequence\": " << report.submittedFrameSequence << ",\n";
        stream << "  \"presentedFrameSequence\": " << report.presentedFrameSequence << ",\n";
        stream << "  \"width\": " << report.width << ",\n";
        stream << "  \"height\": " << report.height << ",\n";
        stream << "  \"quality\": " << Diagnostics::JsonString(report.quality) << ",\n";
        stream << "  \"renderPath\": "
               << Diagnostics::JsonString(report.renderPath) << ",\n";
        stream << "  \"requestedRenderPath\": "
               << Diagnostics::JsonString(report.requestedRenderPath) << ",\n";
        stream << "  \"actualRenderPath\": "
               << Diagnostics::JsonString(report.actualRenderPath) << ",\n";
        stream << "  \"requestedPhysicsBackend\": "
               << Diagnostics::JsonString(report.requestedPhysicsBackend) << ",\n";
        stream << "  \"actualPhysicsBackend\": "
               << Diagnostics::JsonString(report.actualPhysicsBackend) << ",\n";
        stream << "  \"physicsBackendFallbackActive\": "
               << Diagnostics::JsonBool(report.physicsBackendFallbackActive) << ",\n";
        stream << "  \"physicsDiagnosticsAvailable\": "
               << Diagnostics::JsonBool(report.physicsDiagnosticsAvailable) << ",\n";
        stream << "  \"diagnostics\": " << Diagnostics::JsonBool(report.diagnostics) << ",\n";
        stream << "  \"screenshotPath\": " << Diagnostics::JsonString(report.screenshotPath.string()) << ",\n";
        stream << "  \"assetId\": " << Diagnostics::JsonString(report.assetId) << ",\n";
        stream << "  \"assetPath\": " << Diagnostics::JsonString(report.assetPath.string()) << ",\n";
        stream << "  \"assetKind\": " << Diagnostics::JsonString(report.assetKind) << ",\n";
        stream << "  \"catalogPath\": " << Diagnostics::JsonString(report.catalogPath.string()) << ",\n";
        stream << "  \"assetRoot\": " << Diagnostics::JsonString(report.assetRoot.string()) << ",\n";
        stream << "  \"assetLicenseSpdx\": " << Diagnostics::JsonString(report.assetLicenseSpdx) << ",\n";
        stream << "  \"assetLicenseFile\": " << Diagnostics::JsonString(report.assetLicenseFile.string()) << ",\n";
        stream << "  \"assetSourceName\": " << Diagnostics::JsonString(report.assetSourceName) << ",\n";
        stream << "  \"assetSourceUri\": " << Diagnostics::JsonString(report.assetSourceUri) << ",\n";
        stream << "  \"assetAuthor\": " << Diagnostics::JsonString(report.assetAuthor) << ",\n";
        stream << "  \"assetAttribution\": " << Diagnostics::JsonString(report.assetAttribution) << ",\n";
        stream << "  \"assetRedistributable\": "
               << Diagnostics::JsonBool(report.assetRedistributable) << ",\n";
        stream << "  \"assetLoaded\": " << Diagnostics::JsonBool(report.assetLoaded) << ",\n";
        WriteStringArray(stream, "enabledFeatures", report.enabledFeatures, ",");
        WriteStringArray(stream, "unsupportedFeatures", report.unsupportedFeatures, ",");
        WriteStringArray(stream, "fallbackReasons", report.fallbackReasons, ",");
        WriteStringArray(stream, "resourceDiagnostics", report.resourceDiagnostics, ",");
        stream << "  \"assets\": [\n";
        for (size_t i = 0; i < report.assets.size(); ++i)
        {
            const SampleReportAsset& asset = report.assets[i];
            stream << "    {\"role\": "
                   << Diagnostics::JsonString(asset.role)
                   << ", \"id\": " << Diagnostics::JsonString(asset.id)
                   << ", \"path\": "
                   << Diagnostics::JsonString(asset.path.string())
                   << ", \"kind\": " << Diagnostics::JsonString(asset.kind)
                   << ", \"catalogPath\": "
                   << Diagnostics::JsonString(asset.catalogPath.string())
                   << ", \"assetRoot\": "
                   << Diagnostics::JsonString(asset.assetRoot.string())
                   << ", \"licenseSpdx\": "
                   << Diagnostics::JsonString(asset.licenseSpdx)
                   << ", \"licenseFile\": "
                   << Diagnostics::JsonString(asset.licenseFile.string())
                   << ", \"sourceName\": "
                   << Diagnostics::JsonString(asset.sourceName)
                   << ", \"sourceUri\": "
                   << Diagnostics::JsonString(asset.sourceUri)
                   << ", \"author\": "
                   << Diagnostics::JsonString(asset.author)
                   << ", \"attribution\": "
                   << Diagnostics::JsonString(asset.attribution)
                   << ", \"redistributable\": "
                   << Diagnostics::JsonBool(asset.redistributable)
                   << ", \"loaded\": "
                   << Diagnostics::JsonBool(asset.loaded)
                   << ", \"sourceContentId\": {\"schemaVersion\": "
                   << asset.sourceContentId.schemaVersion
                   << ", \"algorithm\": "
                   << Diagnostics::JsonString(asset.sourceContentId.algorithm)
                   << ", \"digest\": "
                   << Diagnostics::JsonString(asset.sourceContentId.digest)
                   << ", \"byteCount\": " << asset.sourceContentId.byteCount
                   << ", \"fileCount\": " << asset.sourceContentId.fileCount
                   << "}";
            if (report.schemaVersion >= 11)
            {
                stream << ", \"expectedContentIdentity\": ";
                WriteContentIdentity(stream, asset.expectedContentIdentity);
                stream << ", \"observedContentIdentity\": ";
                WriteContentIdentity(stream, asset.observedContentIdentity);
                stream << ", \"verificationStatus\": "
                       << Diagnostics::JsonString(
                              Resource::GetResourceContentVerificationStatusName(
                                  asset.verificationStatus))
                       << ", \"verified\": "
                       << Diagnostics::JsonBool(asset.verified);
            }
            if (report.schemaVersion >= 12)
            {
                stream << ", \"packageContentId\": {\"schemaVersion\": "
                       << asset.packageContentId.schemaVersion
                       << ", \"algorithm\": "
                       << Diagnostics::JsonString(
                              asset.packageContentId.algorithm)
                       << ", \"digest\": "
                       << Diagnostics::JsonString(
                              asset.packageContentId.digest)
                       << ", \"byteCount\": "
                       << asset.packageContentId.byteCount
                       << ", \"fileCount\": "
                       << asset.packageContentId.fileCount << "}"
                       << ", \"cook\": {\"required\": "
                       << Diagnostics::JsonBool(
                              asset.cookedAdmissionRequired)
                       << ", \"accepted\": "
                       << Diagnostics::JsonBool(
                              asset.cookedAdmissionAccepted)
                       << ", \"code\": "
                       << Diagnostics::JsonString(
                              asset.cookedAdmissionCode)
                       << ", \"detail\": "
                       << Diagnostics::JsonString(
                              asset.cookedAdmissionDetail)
                       << ", \"manifestPath\": "
                       << Diagnostics::JsonString(
                              asset.cookManifestPath.string())
                       << ", \"cookedRoot\": "
                       << Diagnostics::JsonString(asset.cookedRoot.string())
                       << ", \"declaredSourceContentIdentity\": ";
                WriteContentIdentity(
                    stream, asset.declaredCookSourceContentIdentity);
                stream << ", \"declaredCookedContentIdentity\": ";
                WriteContentIdentity(
                    stream, asset.declaredCookedContentIdentity);
                stream << ", \"declaredManifestContentIdentity\": ";
                WriteContentIdentity(
                    stream, asset.declaredCookManifestContentIdentity);
                stream << ", \"observedSourceContentIdentity\": ";
                WriteContentIdentity(
                    stream, asset.observedCookSourceContentIdentity);
                stream << ", \"observedCookedContentIdentity\": ";
                WriteContentIdentity(
                    stream, asset.observedCookedContentIdentity);
                stream << ", \"observedManifestContentIdentity\": ";
                WriteContentIdentity(
                    stream, asset.observedCookManifestContentIdentity);
                stream << ", \"cookSettingsHash\": "
                       << Diagnostics::JsonString(asset.cookSettingsHash)
                       << ", \"recipeHash\": "
                       << Diagnostics::JsonString(asset.cookRecipeHash)
                       << ", \"toolName\": "
                       << Diagnostics::JsonString(asset.cookToolName)
                       << ", \"toolVersion\": "
                       << Diagnostics::JsonString(asset.cookToolVersion)
                       << "}";
            }
            stream << "}";
            stream << (i + 1 < report.assets.size() ? "," : "") << "\n";
        }
        stream << "  ],\n";
        stream << "  \"readiness\": {\n";
        stream << "    \"waitRequested\": "
               << Diagnostics::JsonBool(report.readiness.waitRequested)
               << ",\n";
        stream << "    \"ready\": "
               << Diagnostics::JsonBool(report.readiness.ready) << ",\n";
        stream << "    \"minimumFrames\": "
               << report.readiness.minimumFrames << ",\n";
        stream << "    \"maximumFrames\": "
               << report.readiness.maximumFrames << ",\n";
        stream << "    \"timeoutMs\": "
               << report.readiness.timeoutMs << ",\n";
        stream << "    \"reason\": "
               << Diagnostics::JsonString(report.readiness.reason) << "\n";
        stream << "  },\n";
        if (report.schemaVersion >= RVX_SAMPLE_REPORT_SCENARIO_SCHEMA_VERSION)
        {
            std::vector<SampleReportScenarioAction> actions =
                report.scenario.actions;
            std::vector<SampleReportScenarioInvariant> invariants =
                report.scenario.invariants;
            std::vector<SampleReportScenarioMetric> metrics =
                report.scenario.metrics;
            const auto sortByName = [](const auto& left, const auto& right)
            {
                return left.name < right.name;
            };
            std::sort(actions.begin(), actions.end(), sortByName);
            std::sort(invariants.begin(), invariants.end(), sortByName);
            std::sort(metrics.begin(), metrics.end(), sortByName);

            stream << "  \"scenario\": {\n";
            stream << "    \"contractRevision\": "
                   << report.scenario.contractRevision << ",\n";
            stream << "    \"phase\": "
                   << Diagnostics::JsonString(report.scenario.phase) << ",\n";
            stream << "    \"actions\": [\n";
            for (size_t index = 0; index < actions.size(); ++index)
            {
                const SampleReportScenarioAction& action = actions[index];
                stream << "      {\"name\": "
                       << Diagnostics::JsonString(action.name)
                       << ", \"targetSceneRevision\": "
                       << action.targetSceneRevision
                       << ", \"completedPresentationSequence\": "
                       << action.completedPresentationSequence
                       << ", \"appliedSceneRevision\": "
                       << action.appliedSceneRevision
                       << ", \"passed\": "
                       << Diagnostics::JsonBool(action.passed) << "}"
                       << (index + 1 < actions.size() ? "," : "") << "\n";
            }
            stream << "    ],\n";
            stream << "    \"invariants\": [\n";
            for (size_t index = 0; index < invariants.size(); ++index)
            {
                const SampleReportScenarioInvariant& invariant =
                    invariants[index];
                stream << "      {\"name\": "
                       << Diagnostics::JsonString(invariant.name)
                       << ", \"passed\": "
                       << Diagnostics::JsonBool(invariant.passed)
                       << ", \"evidence\": "
                       << Diagnostics::JsonString(invariant.evidence) << "}"
                       << (index + 1 < invariants.size() ? "," : "") << "\n";
            }
            stream << "    ],\n";
            stream << "    \"metrics\": [\n";
            for (size_t index = 0; index < metrics.size(); ++index)
            {
                const SampleReportScenarioMetric& metric = metrics[index];
                stream << "      {\"name\": "
                       << Diagnostics::JsonString(metric.name)
                       << ", \"value\": " << metric.value << "}"
                       << (index + 1 < metrics.size() ? "," : "") << "\n";
            }
            stream << "    ]\n";
            stream << "  },\n";
        }
        if (report.schemaVersion >= 3)
        {
            const SampleAssessmentSummary& assessment = report.assessment;
            stream << "  \"assessment\": {\n";
            stream << "    \"enabled\": "
                   << Diagnostics::JsonBool(assessment.enabled) << ",\n";
            stream << "    \"reportPath\": "
                   << Diagnostics::JsonString(assessment.reportPath.string())
                   << ",\n";
            stream << "    \"blockGrade\": "
                   << Diagnostics::JsonString(assessment.blockGrade) << ",\n";
            stream << "    \"findingCount\": " << assessment.findingCount
                   << ",\n";
            stream << "    \"advisoryCount\": " << assessment.advisoryCount
                   << ",\n";
            stream << "    \"blockingCount\": " << assessment.blockingCount
                   << ",\n";
            stream << "    \"droppedEventCount\": "
                   << assessment.droppedEventCount << ",\n";
            stream << "    \"pass\": "
                   << Diagnostics::JsonBool(assessment.pass) << "\n";
            stream << "  },\n";
        }
        if (report.schemaVersion >=
            RVX_SAMPLE_REPORT_ECS_DIAGNOSTICS_SCHEMA_VERSION)
        {
            const SampleEcsRuntimeDiagnostics& ecs = report.ecsDiagnostics;
            stream << "  \"ecsDiagnostics\": {\n";
            stream << "    \"available\": "
                   << Diagnostics::JsonBool(ecs.available) << ",\n";
            stream << "    \"sceneRuntimeId\": " << ecs.sceneRuntimeId
                   << ",\n";
            stream << "    \"sceneFrameSequence\": "
                   << ecs.sceneFrameSequence << ",\n";
            stream << "    \"sceneFixedStepSequence\": "
                   << ecs.sceneFixedStepSequence << ",\n";
            stream << "    \"sceneSnapshotRevision\": "
                   << ecs.sceneSnapshotRevision << ",\n";
            stream << "    \"entityCount\": " << ecs.entityCount << ",\n";
            stream << "    \"pendingDestroyEntityCount\": "
                   << ecs.pendingDestroyEntityCount << ",\n";
            stream << "    \"cleanupRequiredEntityCount\": "
                   << ecs.cleanupRequiredEntityCount << ",\n";
            stream << "    \"retiringEntityCount\": "
                   << ecs.retiringEntityCount << ",\n";
            stream << "    \"recyclableEntityCount\": "
                   << ecs.recyclableEntityCount << ",\n";
            stream << "    \"spatialEntryCount\": "
                   << ecs.spatialEntryCount << ",\n";
            stream << "    \"nextStructuralJournalSequence\": "
                   << ecs.nextStructuralJournalSequence << ",\n";
            stream << "    \"nextCleanupJournalSequence\": "
                   << ecs.nextCleanupJournalSequence << ",\n";
            stream << "    \"cleanupJournalContinuityLossCount\": "
                   << ecs.cleanupJournalContinuityLossCount << ",\n";
            stream << "    \"queuedCommandBufferCount\": "
                   << ecs.queuedCommandBufferCount << ",\n";
            stream << "    \"appliedCommandBufferCount\": "
                   << ecs.appliedCommandBufferCount << ",\n";
            stream << "    \"rejectedCommandBufferCount\": "
                   << ecs.rejectedCommandBufferCount << ",\n";
            stream << "    \"localTransformWriteVersion\": "
                   << ecs.localTransformWriteVersion << ",\n";
            stream << "    \"renderWorldTransformWriteVersion\": "
                   << ecs.renderWorldTransformWriteVersion << ",\n";
            stream << "    \"physicsBodySideTableEntryCount\": "
                   << ecs.physicsBodySideTableEntryCount << ",\n";
            stream << "    \"animationBindingSideTableEntryCount\": "
                   << ecs.animationBindingSideTableEntryCount << ",\n";
            stream << "    \"resourceAnimationPlaybackSideTableEntryCount\": "
                   << ecs.resourceAnimationPlaybackSideTableEntryCount
                   << ",\n";
            stream << "    \"audioPlaybackSideTableEntryCount\": "
                   << ecs.audioPlaybackSideTableEntryCount << ",\n";
            stream << "    \"scriptInstanceSideTableEntryCount\": "
                   << ecs.scriptInstanceSideTableEntryCount << ",\n";
            stream << "    \"particleFeatureSnapshotCount\": "
                   << ecs.particleFeatureSnapshotCount << ",\n";
            stream << "    \"waterFeatureSnapshotCount\": "
                   << ecs.waterFeatureSnapshotCount << ",\n";
            stream << "    \"terrainFeatureSnapshotCount\": "
                   << ecs.terrainFeatureSnapshotCount << ",\n";
            stream << "    \"trackedModelRequestCount\": "
                   << ecs.trackedModelRequestCount << ",\n";
            stream << "    \"trackedEnvironmentRequestCount\": "
                   << ecs.trackedEnvironmentRequestCount << ",\n";
            stream << "    \"trackedAnimationRequestCount\": "
                   << ecs.trackedAnimationRequestCount << ",\n";
            stream << "    \"legacyFallbackCount\": "
                   << ecs.legacyFallbackCount << "\n";
            stream << "  },\n";
        }
        const SampleRenderDiagnostics& renderDiagnostics = report.renderDiagnostics;
        stream << "  \"renderDiagnostics\": {\n";
        stream << "    \"available\": " << Diagnostics::JsonBool(renderDiagnostics.available) << ",\n";
        stream << "    \"renderAttempted\": " << Diagnostics::JsonBool(renderDiagnostics.renderAttempted) << ",\n";
        stream << "    \"rendered\": " << Diagnostics::JsonBool(renderDiagnostics.rendered) << ",\n";
        stream << "    \"graphBuilt\": " << Diagnostics::JsonBool(renderDiagnostics.graphBuilt) << ",\n";
        stream << "    \"graphCompiled\": " << Diagnostics::JsonBool(renderDiagnostics.graphCompiled) << ",\n";
        stream << "    \"renderGraphTotalPasses\": " << renderDiagnostics.renderGraphTotalPasses << ",\n";
        stream << "    \"visibleObjectCount\": " << renderDiagnostics.visibleObjectCount << ",\n";
        stream << "    \"renderSceneLightCount\": " << renderDiagnostics.renderSceneLightCount << ",\n";
        if (report.schemaVersion >= 3)
        {
            stream << "    \"renderSceneValuesAvailable\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.renderSceneValuesAvailable)
                   << ",\n";
            stream << "    \"renderSceneFrameSequence\": "
                   << renderDiagnostics.renderSceneFrameSequence << ",\n";
            stream << "    \"renderSceneAppliedRevision\": "
                   << renderDiagnostics.renderSceneAppliedRevision << ",\n";
            stream << "    \"renderSceneRequiredRevision\": "
                   << renderDiagnostics.renderSceneRequiredRevision << ",\n";
            stream << "    \"renderSceneObjectCount\": "
                   << renderDiagnostics.renderSceneObjectCount << ",\n";
            stream << "    \"renderSceneValueLightCount\": "
                   << renderDiagnostics.renderSceneValueLightCount << ",\n";
            stream << "    \"renderSceneLightStateHash\": "
                   << renderDiagnostics.renderSceneLightStateHash << ",\n";
            stream << "    \"engineRenderRuntimeAvailable\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.engineRenderRuntimeAvailable)
                   << ",\n";
            stream << "    \"engineRequiredSceneFrameSequence\": "
                   << renderDiagnostics.engineRequiredSceneFrameSequence
                   << ",\n";
            stream << "    \"engineRequiredSceneRevision\": "
                   << renderDiagnostics.engineRequiredSceneRevision << ",\n";
            stream << "    \"temporalEpoch\": "
                   << renderDiagnostics.temporalEpoch << ",\n";
            stream << "    \"activeCameraIdentity\": "
                   << renderDiagnostics.activeCameraIdentity << ",\n";
            stream << "    \"activeCameraCutRevision\": "
                   << renderDiagnostics.activeCameraCutRevision << ",\n";
            stream << "    \"temporalResetCount\": "
                   << renderDiagnostics.temporalResetCount << ",\n";
        }
        if (report.schemaVersion >= 4)
        {
            stream << "    \"lastSubmittedFrameSequence\": ";
            WriteDiagnosticUInt64(
                stream, renderDiagnostics.lastSubmittedFrameSequence);
            stream << ",\n";
            stream << "    \"lastPresentedFrameSequence\": ";
            WriteDiagnosticUInt64(
                stream, renderDiagnostics.lastPresentedFrameSequence);
            stream << ",\n";
            stream << "    \"engineUpdateTickCount\": ";
            WriteDiagnosticUInt64(
                stream, renderDiagnostics.engineUpdateTickCount);
            stream << ",\n";
        }
        if (report.schemaVersion >= RVX_SAMPLE_REPORT_SCENARIO_SCHEMA_VERSION)
        {
            stream << "    \"nativeValidationAvailable\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.nativeValidationAvailable)
                   << ",\n";
            stream << "    \"nativeValidationEnabled\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.nativeValidationEnabled)
                   << ",\n";
            stream << "    \"nativeValidationReadComplete\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.nativeValidationReadComplete)
                   << ",\n";
            stream << "    \"nativeValidationWarningCount\": "
                   << renderDiagnostics.nativeValidationWarningCount
                   << ",\n";
            stream << "    \"nativeValidationErrorCount\": "
                   << renderDiagnostics.nativeValidationErrorCount
                   << ",\n";
            stream << "    \"nativeValidationCorruptionCount\": "
                   << renderDiagnostics.nativeValidationCorruptionCount
                   << ",\n";
        }
        stream << "    \"requestedPostProcessEffectCount\": "
               << renderDiagnostics.requestedPostProcessEffectCount << ",\n";
        stream << "    \"enabledPostProcessEffectCount\": "
               << renderDiagnostics.enabledPostProcessEffectCount << ",\n";
        stream << "    \"unsupportedPostProcessSkippedCount\": "
               << renderDiagnostics.unsupportedPostProcessSkippedCount << ",\n";
        stream << "    \"postProcessGraphPassCount\": " << renderDiagnostics.postProcessGraphPassCount << ",\n";
        stream << "    \"clusteredLightingInitialized\": "
               << Diagnostics::JsonBool(renderDiagnostics.clusteredLightingInitialized) << ",\n";
        stream << "    \"clusteredLightingActiveClusters\": "
               << renderDiagnostics.clusteredLightingActiveClusters << ",\n";
        stream << "    \"textureIBLEnabled\": " << Diagnostics::JsonBool(renderDiagnostics.textureIBLEnabled) << ",\n";
        stream << "    \"directionalShadowSamplingEnabled\": "
               << Diagnostics::JsonBool(
                      renderDiagnostics.directionalShadowSamplingEnabled)
               << ",\n";
        stream << "    \"directionalShadowReason\": "
               << Diagnostics::JsonString(
                      renderDiagnostics.directionalShadowReason)
               << ",\n";
        if (report.schemaVersion >= 5)
        {
            stream << "    \"directionalShadowAvailable\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.directionalShadowAvailable)
                   << ",\n";
            stream << "    \"directionalShadowRequested\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.directionalShadowRequested)
                   << ",\n";
            stream << "    \"directionalShadowSupported\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.directionalShadowSupported)
                   << ",\n";
            stream << "    \"directionalShadowOutputReady\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.directionalShadowOutputReady)
                   << ",\n";
            stream << "    \"directionalShadowRequestedCascadeCount\": "
                   << renderDiagnostics.directionalShadowRequestedCascadeCount
                   << ",\n";
            stream << "    \"directionalShadowProducedCascadeCount\": "
                   << renderDiagnostics.directionalShadowProducedCascadeCount
                   << ",\n";
            stream << "    \"directionalShadowResolvedCascadeCount\": "
                   << renderDiagnostics.directionalShadowResolvedCascadeCount
                   << ",\n";
            stream << "    \"directionalShadowMapSize\": "
                   << renderDiagnostics.directionalShadowMapSize << ",\n";
            stream << "    \"directionalShadowCasterCount\": "
                   << renderDiagnostics.directionalShadowCasterCount << ",\n";
            stream << "    \"directionalShadowDrawCount\": "
                   << renderDiagnostics.directionalShadowDrawCount << ",\n";
            stream << "    \"localLightingAvailable\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.localLightingAvailable)
                   << ",\n";
            stream << "    \"pointLightRequestedCount\": "
                   << renderDiagnostics.pointLightRequestedCount << ",\n";
            stream << "    \"pointLightAdmittedCount\": "
                   << renderDiagnostics.pointLightAdmittedCount << ",\n";
            stream << "    \"pointLightCapacity\": "
                   << renderDiagnostics.pointLightCapacity << ",\n";
            stream << "    \"pointLightOverflowCount\": "
                   << renderDiagnostics.pointLightOverflowCount << ",\n";
            stream << "    \"spotLightRequestedCount\": "
                   << renderDiagnostics.spotLightRequestedCount << ",\n";
            stream << "    \"spotLightAdmittedCount\": "
                   << renderDiagnostics.spotLightAdmittedCount << ",\n";
            stream << "    \"spotLightCapacity\": "
                   << renderDiagnostics.spotLightCapacity << ",\n";
            stream << "    \"spotLightOverflowCount\": "
                   << renderDiagnostics.spotLightOverflowCount << ",\n";
            stream << "    \"pointShadowRequestedCount\": "
                   << renderDiagnostics.pointShadowRequestedCount << ",\n";
            stream << "    \"pointShadowSupported\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.pointShadowSupported)
                   << ",\n";
            stream << "    \"pointShadowReason\": "
                   << Diagnostics::JsonString(
                          renderDiagnostics.pointShadowReason)
                   << ",\n";
            stream << "    \"spotShadowRequestedCount\": "
                   << renderDiagnostics.spotShadowRequestedCount << ",\n";
            stream << "    \"spotShadowSupported\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.spotShadowSupported)
                   << ",\n";
            stream << "    \"spotShadowReason\": "
                   << Diagnostics::JsonString(
                          renderDiagnostics.spotShadowReason)
                   << ",\n";
            stream << "    \"hzbRequested\": "
                   << Diagnostics::JsonBool(renderDiagnostics.hzbRequested)
                   << ",\n";
            stream << "    \"hzbSupported\": "
                   << Diagnostics::JsonBool(renderDiagnostics.hzbSupported)
                   << ",\n";
            stream << "    \"hzbEnabled\": "
                   << Diagnostics::JsonBool(renderDiagnostics.hzbEnabled)
                   << ",\n";
            stream << "    \"hzbReason\": "
                   << Diagnostics::JsonString(renderDiagnostics.hzbReason)
                   << ",\n";
            stream << "    \"transparentAvailable\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.transparentAvailable)
                   << ",\n";
            stream << "    \"transparentOrderValid\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.transparentOrderValid)
                   << ",\n";
            stream << "    \"transparentOrderHash\": "
                   << renderDiagnostics.transparentOrderHash << ",\n";
            stream << "    \"transparentRejectedNonFiniteDepthCount\": "
                   << renderDiagnostics.transparentRejectedNonFiniteDepthCount
                   << ",\n";
            stream << "    \"transparentCandidateDrawItemCount\": "
                   << renderDiagnostics.transparentCandidateDrawItemCount
                   << ",\n";
            stream << "    \"transparentPreparedDrawItemCount\": "
                   << renderDiagnostics.transparentPreparedDrawItemCount
                   << ",\n";
            stream << "    \"transparentExecutedPacketCount\": "
                   << renderDiagnostics.transparentExecutedPacketCount
                   << ",\n";
            stream << "    \"transparentExecutedDrawCount\": "
                   << renderDiagnostics.transparentExecutedDrawCount
                   << ",\n";
            stream << "    \"transparentSkippedMaterialBindingCount\": "
                   << renderDiagnostics.transparentSkippedMaterialBindingCount
                   << ",\n";
            stream << "    \"transparentSkippedResourceCount\": "
                   << renderDiagnostics.transparentSkippedResourceCount
                   << ",\n";
            stream << "    \"transparentSkippedExecutionDrawCount\": "
                   << renderDiagnostics.transparentSkippedExecutionDrawCount
                   << ",\n";
            stream << "    \"transparentMaterialBindingCount\": "
                   << renderDiagnostics.transparentMaterialBindingCount
                   << ",\n";
            stream << "    \"transparentMaterialFallbackBindingCount\": "
                   << renderDiagnostics.transparentMaterialFallbackBindingCount
                   << ",\n";
            stream << "    \"transparentMaterialTextureFlags\": "
                   << renderDiagnostics.transparentMaterialTextureFlags
                   << ",\n";
            stream << "    \"transparentMaterialFallbackTextureFlags\": "
                   << renderDiagnostics.transparentMaterialFallbackTextureFlags
                   << ",\n";
            stream << "    \"transparentNoWork\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.transparentNoWork)
                   << ",\n";
            stream << "    \"transparentPreflightFailed\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.transparentPreflightFailed)
                   << ",\n";
            stream << "    \"transparentExecutionFailed\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.transparentExecutionFailed)
                   << ",\n";
        }
        stream << "    \"materialReady\": "
               << Diagnostics::JsonBool(renderDiagnostics.materialReady) << ",\n";
        stream << "    \"materialUsedFallback\": "
               << Diagnostics::JsonBool(renderDiagnostics.materialUsedFallback) << ",\n";
        stream << "    \"materialConstantsUpdated\": "
               << Diagnostics::JsonBool(renderDiagnostics.materialConstantsUpdated) << ",\n";
        stream << "    \"materialDescriptorSetAvailable\": "
               << Diagnostics::JsonBool(renderDiagnostics.materialDescriptorSetAvailable) << ",\n";
        stream << "    \"materialTextureFlags\": "
               << renderDiagnostics.materialTextureFlags << ",\n";
        if (report.schemaVersion >= RVX_SAMPLE_REPORT_SCENARIO_SCHEMA_VERSION)
        {
            stream << "    \"presentedSkinningPalettesAvailable\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.presentedSkinningPalettesAvailable)
                   << ",\n";
            stream << "    \"presentedSkinningPalettesOverflow\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.presentedSkinningPalettesOverflow)
                   << ",\n";
            stream << "    \"presentedSkinningPalettes\": [";
            for (size_t index = 0;
                 index < renderDiagnostics.presentedSkinningPalettes.size();
                 ++index)
            {
                const SamplePresentedSkinningPaletteReceipt& receipt =
                    renderDiagnostics.presentedSkinningPalettes[index];
                if (index != 0)
                {
                    stream << ", ";
                }
                stream << "{\"providerComponentId\":"
                       << receipt.providerComponentId
                       << ",\"sourceModelResourceId\":"
                       << receipt.sourceModelResourceId
                       << ",\"poseSequence\":" << receipt.poseSequence
                       << ",\"paletteHash\":" << receipt.paletteHash
                       << ",\"paletteCount\":" << receipt.paletteCount
                       << ",\"lane\":" << Diagnostics::JsonString(
                              GetRenderSkinningPaletteExecutionLaneName(
                                  receipt.lane))
                       << ",\"frameSequence\":" << receipt.frameSequence
                       << ",\"presentationSequence\":"
                       << receipt.presentationSequence << "}";
            }
            stream << "],\n";
        }
        stream << "    \"opaqueExecutionCompleted\": "
               << Diagnostics::JsonBool(
                      renderDiagnostics.opaqueExecutionCompleted)
               << ",\n";
        stream << "    \"opaqueExecutedDrawCountAvailable\": "
               << Diagnostics::JsonBool(
                      renderDiagnostics.opaqueExecutedDrawCountAvailable)
               << ",\n";
        stream << "    \"opaqueExecutedDrawCount\": "
               << renderDiagnostics.opaqueExecutedDrawCount << ",\n";
        stream << "    \"instancingRequestedMode\": "
               << Diagnostics::JsonString(
                      renderDiagnostics.instancingRequestedMode)
               << ",\n";
        stream << "    \"opaqueInstancingPlanAvailable\": "
               << Diagnostics::JsonBool(
                      renderDiagnostics.opaqueInstancingPlanAvailable)
               << ",\n";
        stream << "    \"opaqueInstancingPreflightSucceeded\": "
               << Diagnostics::JsonBool(
                      renderDiagnostics.opaqueInstancingPreflightSucceeded)
               << ",\n";
        stream << "    \"opaqueInstancingPlannedPacketCount\": "
               << renderDiagnostics.opaqueInstancingPlannedPacketCount
               << ",\n";
        stream << "    \"opaqueInstancingPlannedDrawCount\": "
               << renderDiagnostics.opaqueInstancingPlannedDrawCount
               << ",\n";
        stream << "    \"opaqueInstancingPlannedInstanceCount\": "
               << renderDiagnostics.opaqueInstancingPlannedInstanceCount
               << ",\n";
        stream << "    \"opaqueInstancingPlannedBatchCount\": "
               << renderDiagnostics.opaqueInstancingPlannedBatchCount
               << ",\n";
        stream << "    \"opaqueInstancingExecutedPacketCount\": "
               << renderDiagnostics.opaqueInstancingExecutedPacketCount
               << ",\n";
        stream << "    \"opaqueInstancingSubmittedDrawCount\": "
               << renderDiagnostics.opaqueInstancingSubmittedDrawCount
               << ",\n";
        stream << "    \"opaqueInstancingSubmittedInstanceCount\": "
               << renderDiagnostics.opaqueInstancingSubmittedInstanceCount
               << ",\n";
        stream << "    \"opaqueInstancingBatchCount\": "
               << renderDiagnostics.opaqueInstancingBatchCount << ",\n";
        stream << "    \"opaqueInstancingFallbackBatchCount\": "
               << renderDiagnostics.opaqueInstancingFallbackBatchCount
               << ",\n";
        stream << "    \"opaqueMaterialBindingsAvailable\": "
               << Diagnostics::JsonBool(
                      renderDiagnostics.opaqueMaterialBindingsAvailable)
               << ",\n";
        stream << "    \"opaqueMaterialBindingCount\": "
               << renderDiagnostics.opaqueMaterialBindingCount << ",\n";
        stream << "    \"opaqueMaterialFallbackBindingCount\": "
               << renderDiagnostics.opaqueMaterialFallbackBindingCount
               << ",\n";
        stream << "    \"opaqueMaterialTextureFlags\": "
               << renderDiagnostics.opaqueMaterialTextureFlags << ",\n";
        stream << "    \"opaqueMaterialFallbackTextureFlags\": "
               << renderDiagnostics.opaqueMaterialFallbackTextureFlags
               << ",\n";
        stream << "    \"gpuDrivenPolicyDecisionAvailable\": "
               << Diagnostics::JsonBool(
                      renderDiagnostics.gpuDrivenPolicyDecisionAvailable)
               << ",\n";
        stream << "    \"gpuDrivenRequestedMode\": "
               << Diagnostics::JsonString(
                      renderDiagnostics.gpuDrivenRequestedMode)
               << ",\n";
        stream << "    \"gpuDrivenPolicyReason\": "
               << Diagnostics::JsonString(
                      renderDiagnostics.gpuDrivenPolicyReason)
               << ",\n";
        stream << "    \"gpuDrivenQualification\": "
               << Diagnostics::JsonString(
                      renderDiagnostics.gpuDrivenQualification)
               << ",\n";
        stream << "    \"gpuDrivenBackendQualified\": "
               << Diagnostics::JsonBool(
                      renderDiagnostics.gpuDrivenBackendQualified)
               << ",\n";
        stream << "    \"gpuDrivenCapabilitiesReady\": "
               << Diagnostics::JsonBool(
                      renderDiagnostics.gpuDrivenCapabilitiesReady)
               << ",\n";
        stream << "    \"gpuDrivenPipelineReady\": "
               << Diagnostics::JsonBool(
                      renderDiagnostics.gpuDrivenPipelineReady)
               << ",\n";
        stream << "    \"gpuDrivenEnabled\": "
               << Diagnostics::JsonBool(renderDiagnostics.gpuDrivenEnabled)
               << ",\n";
        stream << "    \"gpuDrivenGraphPassAdded\": "
               << Diagnostics::JsonBool(
                      renderDiagnostics.gpuDrivenGraphPassAdded)
               << ",\n";
        stream << "    \"gpuDrivenGraphPassRecorded\": "
               << Diagnostics::JsonBool(
                      renderDiagnostics.gpuDrivenGraphPassRecorded)
               << ",\n";
        stream << "    \"gpuDrivenExecutionRecorded\": "
               << Diagnostics::JsonBool(
                      renderDiagnostics.gpuDrivenExecutionRecorded)
               << ",\n";
        stream << "    \"gpuDrivenGraphInputDrawItemCount\": "
               << renderDiagnostics.gpuDrivenGraphInputDrawItemCount
               << ",\n";
        stream << "    \"gpuDrivenVisibilityCandidateCount\": "
               << renderDiagnostics.gpuDrivenVisibilityCandidateCount
               << ",\n";
        stream << "    \"gpuDrivenVisibleCullableDrawItemCount\": "
               << renderDiagnostics.gpuDrivenVisibleCullableDrawItemCount
               << ",\n";
        stream << "    \"gpuDrivenCpuReferenceVisibleCount\": "
               << renderDiagnostics.gpuDrivenCpuReferenceVisibleCount
               << ",\n";
        stream << "    \"gpuDrivenCpuReferenceCulledCount\": "
               << renderDiagnostics.gpuDrivenCpuReferenceCulledCount
               << ",\n";
        stream << "    \"gpuDrivenOpaqueIndirectRequested\": "
               << Diagnostics::JsonBool(
                      renderDiagnostics.gpuDrivenOpaqueIndirectRequested)
               << ",\n";
        stream << "    \"gpuDrivenOpaqueIndirectEligible\": "
               << Diagnostics::JsonBool(
                      renderDiagnostics.gpuDrivenOpaqueIndirectEligible)
               << ",\n";
        stream << "    \"gpuDrivenOpaqueIndirectSubmitted\": "
               << Diagnostics::JsonBool(
                      renderDiagnostics.gpuDrivenOpaqueIndirectSubmitted)
               << ",\n";
        stream << "    \"gpuDrivenOpaqueDirectDrawCount\": "
               << renderDiagnostics.gpuDrivenOpaqueDirectDrawCount << ",\n";
        stream << "    \"gpuDrivenOpaqueIndirectBatchCount\": "
               << renderDiagnostics.gpuDrivenOpaqueIndirectBatchCount
               << ",\n";
        stream << "    \"gpuDrivenOpaqueIndirectDrawUpperBound\": "
               << renderDiagnostics.gpuDrivenOpaqueIndirectDrawUpperBound
               << ",\n";
        stream << "    \"gpuDrivenOpaqueFallbackReason\": "
               << Diagnostics::JsonString(
                      renderDiagnostics.gpuDrivenOpaqueFallbackReason)
               << ",\n";
        writeRasterTranscript("directOpaqueRasterTranscript",
                              renderDiagnostics.directOpaqueRasterTranscript,
                              "    ");
        if (report.schemaVersion >= 19)
        {
            writeDirectRasterReadbackQualification(
                renderDiagnostics.directOpaqueRasterReadbackQualification);
            stream << "    \"directOpaqueRasterReadbackQualificationTargetFrameSequence\": "
                   << renderDiagnostics
                          .directOpaqueRasterReadbackQualificationTargetFrameSequence
                   << ",\n";
            stream << "    \"directOpaqueRasterReadbackQualificationTargetPublishedSequence\": "
                   << renderDiagnostics
                          .directOpaqueRasterReadbackQualificationTargetPublishedSequence
                   << ",\n";
            stream << "    \"directOpaqueRasterReadbackQualificationTargetSubmittedSequence\": "
                   << renderDiagnostics
                          .directOpaqueRasterReadbackQualificationTargetSubmittedSequence
                   << ",\n";
            stream << "    \"directOpaqueRasterReadbackQualificationTargetPresentedSequence\": "
                   << renderDiagnostics
                          .directOpaqueRasterReadbackQualificationTargetPresentedSequence
                   << ",\n";
        }
        writeGPUSceneQualification(
            "gpuSceneDepthQualification",
            renderDiagnostics.gpuSceneDepthQualification);
        writeGPUSceneQualification(
            "gpuSceneOpaqueQualification",
            renderDiagnostics.gpuSceneOpaqueQualification);
        if (report.schemaVersion >= 16)
        {
            stream << "    \"gpuSceneQualificationTargetFrameSequence\": "
                   << renderDiagnostics.gpuSceneQualificationTargetFrameSequence
                   << ",\n";
            stream << "    \"gpuSceneQualificationTargetPublishedSequence\": "
                   << renderDiagnostics.gpuSceneQualificationTargetPublishedSequence
                   << ",\n";
            stream << "    \"gpuSceneQualificationTargetSubmittedSequence\": "
                   << renderDiagnostics.gpuSceneQualificationTargetSubmittedSequence
                   << ",\n";
            stream << "    \"gpuSceneQualificationTargetPresentedSequence\": "
                   << renderDiagnostics.gpuSceneQualificationTargetPresentedSequence
                   << ",\n";
        }
        if (report.schemaVersion >= 6)
        {
            stream << "    \"gpuDrivenInstanceUploadBytes\": "
                   << renderDiagnostics.gpuDrivenInstanceUploadBytes << ",\n";
            stream << "    \"gpuDrivenCandidateUploadBytes\": "
                   << renderDiagnostics.gpuDrivenCandidateUploadBytes << ",\n";
            if (report.schemaVersion >= 7)
            {
                stream << "    \"gpuDrivenActiveRowUploadBytes\": "
                       << renderDiagnostics.gpuDrivenActiveRowUploadBytes
                       << ",\n";
                stream << "    \"gpuDrivenActiveRowCount\": "
                       << renderDiagnostics.gpuDrivenActiveRowCount << ",\n";
                stream << "    \"gpuDrivenActiveRowHighWatermark\": "
                       << renderDiagnostics.gpuDrivenActiveRowHighWatermark
                       << ",\n";
                stream << "    \"gpuDrivenInstancePatchedRowCount\": "
                       << renderDiagnostics.gpuDrivenInstancePatchedRowCount
                       << ",\n";
                stream << "    \"gpuDrivenCandidatePatchedRowCount\": "
                       << renderDiagnostics.gpuDrivenCandidatePatchedRowCount
                       << ",\n";
                stream << "    \"gpuDrivenActiveRowPatchedRowCount\": "
                       << renderDiagnostics.gpuDrivenActiveRowPatchedRowCount
                       << ",\n";
                stream << "    \"gpuDrivenInstanceFullMaterializationCount\": "
                       << renderDiagnostics
                              .gpuDrivenInstanceFullMaterializationCount
                       << ",\n";
                stream << "    \"gpuDrivenCandidateFullMaterializationCount\": "
                       << renderDiagnostics
                              .gpuDrivenCandidateFullMaterializationCount
                       << ",\n";
                stream << "    \"gpuDrivenActiveRowFullMaterializationCount\": "
                       << renderDiagnostics
                              .gpuDrivenActiveRowFullMaterializationCount
                       << ",\n";
                stream << "    \"gpuDrivenContinuityFullMaterializationCount\": "
                       << renderDiagnostics
                              .gpuDrivenContinuityFullMaterializationCount
                       << ",\n";
                stream << "    \"gpuDrivenCapacityFullMaterializationCount\": "
                       << renderDiagnostics
                              .gpuDrivenCapacityFullMaterializationCount
                       << ",\n";
            }
            stream << "    \"directRasterInstanceUploadBytes\": "
                   << renderDiagnostics.directRasterInstanceUploadBytes
                   << ",\n";
            stream << "    \"directRasterInstanceIndexUploadBytes\": "
                   << renderDiagnostics.directRasterInstanceIndexUploadBytes
                   << ",\n";
            if (report.schemaVersion >= 8)
            {
                stream << "    \"directRasterInstancePatchedRowCount\": "
                       << renderDiagnostics.directRasterInstancePatchedRowCount
                       << ",\n";
                stream << "    \"directRasterIndexPatchedRowCount\": "
                       << renderDiagnostics.directRasterIndexPatchedRowCount
                       << ",\n";
                stream << "    \"directRasterActiveInstanceCount\": "
                       << renderDiagnostics.directRasterActiveInstanceCount
                       << ",\n";
                stream << "    \"directRasterActiveInstanceCapacity\": "
                       << renderDiagnostics.directRasterActiveInstanceCapacity
                       << ",\n";
                stream << "    \"directRasterInstanceFullMaterializationCount\": "
                       << renderDiagnostics
                              .directRasterInstanceFullMaterializationCount
                       << ",\n";
                stream << "    \"directRasterIndexFullMaterializationCount\": "
                       << renderDiagnostics
                              .directRasterIndexFullMaterializationCount
                       << ",\n";
            }
            if (report.schemaVersion >= 9)
            {
                WriteUploadWorkDiagnostics(
                    stream,
                    "gpuDrivenCanonicalInstanceUploadWork",
                    renderDiagnostics.gpuDrivenCanonicalInstanceUploadWork);
                WriteUploadWorkDiagnostics(
                    stream,
                    "gpuDrivenCanonicalCandidateUploadWork",
                    renderDiagnostics.gpuDrivenCanonicalCandidateUploadWork);
                WriteUploadWorkDiagnostics(
                    stream,
                    "gpuDrivenCanonicalActiveRowUploadWork",
                    renderDiagnostics.gpuDrivenCanonicalActiveRowUploadWork);
                WriteUploadWorkDiagnostics(
                    stream,
                    "directRasterInstanceUploadWork",
                    renderDiagnostics.directRasterInstanceUploadWork);
                WriteUploadWorkDiagnostics(
                    stream,
                    "directRasterIndexUploadWork",
                    renderDiagnostics.directRasterIndexUploadWork);
            }
            stream << "    \"extractionAvailable\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.extractionAvailable)
                   << ",\n";
            stream << "    \"extractionFullScanCount\": "
                   << renderDiagnostics.extractionFullScanCount << ",\n";
            stream << "    \"extractionChangeFeedChangeCount\": "
                   << renderDiagnostics.extractionChangeFeedChangeCount
                   << ",\n";
            stream << "    \"extractionActorRebuildCount\": "
                   << renderDiagnostics.extractionActorRebuildCount << ",\n";
            stream << "    \"extractionProxyVisitCount\": "
                   << renderDiagnostics.extractionProxyVisitCount << ",\n";
            stream << "    \"extractionComponentVisitCount\": "
                   << renderDiagnostics.extractionComponentVisitCount
                   << ",\n";
            stream << "    \"extractionFeatureProviderVisitCount\": "
                   << renderDiagnostics.extractionFeatureProviderVisitCount
                   << ",\n";
            stream << "    \"extractionContinuityLost\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.extractionContinuityLost)
                   << ",\n";
            if (report.schemaVersion >= 10)
            {
                stream << "    \"acceptedExtractionDiagnosticsAvailable\": "
                       << Diagnostics::JsonBool(
                              renderDiagnostics
                                  .acceptedExtractionDiagnosticsAvailable)
                       << ",\n";
                stream << "    \"acceptedExtractionPublicationCount\": "
                       << renderDiagnostics.acceptedExtractionPublicationCount
                       << ",\n";
                stream << "    \"acceptedExtractionLastSourceFrameSequence\": "
                       << renderDiagnostics
                              .acceptedExtractionLastSourceFrameSequence
                       << ",\n";
                stream << "    \"acceptedExtractionLastSceneRevision\": "
                       << renderDiagnostics.acceptedExtractionLastSceneRevision
                       << ",\n";
                stream << "    \"acceptedExtractionCumulativeFullScanCount\": "
                       << renderDiagnostics
                              .acceptedExtractionCumulativeFullScanCount
                       << ",\n";
                stream << "    \"acceptedExtractionCumulativeChangeFeedChangeCount\": "
                       << renderDiagnostics
                              .acceptedExtractionCumulativeChangeFeedChangeCount
                       << ",\n";
                stream << "    \"acceptedExtractionCumulativeActorRebuildCount\": "
                       << renderDiagnostics
                              .acceptedExtractionCumulativeActorRebuildCount
                       << ",\n";
                stream << "    \"acceptedExtractionCumulativeProxyVisitCount\": "
                       << renderDiagnostics
                              .acceptedExtractionCumulativeProxyVisitCount
                       << ",\n";
                stream << "    \"acceptedExtractionCumulativeComponentVisitCount\": "
                       << renderDiagnostics
                              .acceptedExtractionCumulativeComponentVisitCount
                       << ",\n";
                stream << "    \"acceptedExtractionCumulativeFeatureProviderVisitCount\": "
                       << renderDiagnostics
                              .acceptedExtractionCumulativeFeatureProviderVisitCount
                       << ",\n";
                stream << "    \"acceptedExtractionContinuityLossCount\": "
                       << renderDiagnostics
                              .acceptedExtractionContinuityLossCount
                       << ",\n";
            }
            stream << "    \"sceneWorkAvailable\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.sceneWorkAvailable)
                   << ",\n";
            stream << "    \"sceneFullRebuildCount\": "
                   << renderDiagnostics.sceneFullRebuildCount << ",\n";
            stream << "    \"sceneIncrementalUpdateCount\": "
                   << renderDiagnostics.sceneIncrementalUpdateCount << ",\n";
            stream << "    \"sceneStaticReuseCount\": "
                   << renderDiagnostics.sceneStaticReuseCount << ",\n";
            stream << "    \"sceneAppliedRevision\": "
                   << renderDiagnostics.sceneAppliedRevision << ",\n";
            stream << "    \"sceneLastRebuiltObjectCount\": "
                   << renderDiagnostics.sceneLastRebuiltObjectCount << ",\n";
            stream << "    \"sceneLastRemovedObjectCount\": "
                   << renderDiagnostics.sceneLastRemovedObjectCount << ",\n";
            stream << "    \"drawPacketResolveCount\": "
                   << renderDiagnostics.drawPacketResolveCount << ",\n";
            stream << "    \"drawPacketHitCount\": "
                   << renderDiagnostics.drawPacketHitCount << ",\n";
            stream << "    \"drawPacketMissCount\": "
                   << renderDiagnostics.drawPacketMissCount << ",\n";
            stream << "    \"drawPacketDynamicBypassCount\": "
                   << renderDiagnostics.drawPacketDynamicBypassCount << ",\n";
            stream << "    \"drawPacketBuildCount\": "
                   << renderDiagnostics.drawPacketBuildCount << ",\n";
            stream << "    \"drawPacketEntryCreationCount\": "
                   << renderDiagnostics.drawPacketEntryCreationCount << ",\n";
            stream << "    \"drawPacketInvalidationCount\": "
                   << renderDiagnostics.drawPacketInvalidationCount << ",\n";
            stream << "    \"drawPacketObjectRevisionInvalidationCount\": "
                   << renderDiagnostics.drawPacketObjectRevisionInvalidationCount
                   << ",\n";
            stream << "    \"drawPacketClearCount\": "
                   << renderDiagnostics.drawPacketClearCount << ",\n";
            stream << "    \"drawPacketEntryCount\": "
                   << renderDiagnostics.drawPacketEntryCount << ",\n";
            stream << "    \"gpuSceneAvailable\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.gpuSceneAvailable)
                   << ",\n";
            stream << "    \"gpuScenePublicationAttempted\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.gpuScenePublicationAttempted)
                   << ",\n";
            stream << "    \"gpuScenePublicationPublished\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.gpuScenePublicationPublished)
                   << ",\n";
            stream << "    \"gpuScenePublicationFailed\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.gpuScenePublicationFailed)
                   << ",\n";
            stream << "    \"gpuScenePublicationComplete\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.gpuScenePublicationComplete)
                   << ",\n";
            stream << "    \"gpuSceneAttemptedObjectCount\": "
                   << renderDiagnostics.gpuSceneAttemptedObjectCount << ",\n";
            stream << "    \"gpuScenePublishedObjectCount\": "
                   << renderDiagnostics.gpuScenePublishedObjectCount << ",\n";
            stream << "    \"gpuSceneAddCount\": "
                   << renderDiagnostics.gpuSceneAddCount << ",\n";
            stream << "    \"gpuSceneUpdateCount\": "
                   << renderDiagnostics.gpuSceneUpdateCount << ",\n";
            stream << "    \"gpuSceneRemoveCount\": "
                   << renderDiagnostics.gpuSceneRemoveCount << ",\n";
            stream << "    \"gpuSceneNoOpCount\": "
                   << renderDiagnostics.gpuSceneNoOpCount << ",\n";
            stream << "    \"gpuSceneCommittedVersion\": "
                   << renderDiagnostics.gpuSceneCommittedVersion << ",\n";
            stream << "    \"gpuSceneResidentVersion\": "
                   << renderDiagnostics.gpuSceneResidentVersion << ",\n";
            stream << "    \"gpuSceneCpuPayloadBytes\": "
                   << renderDiagnostics.gpuSceneCpuPayloadBytes << ",\n";
            stream << "    \"gpuSceneCpuReservedBytes\": "
                   << renderDiagnostics.gpuSceneCpuReservedBytes << ",\n";
            stream << "    \"gpuSceneAllocationBytes\": "
                   << renderDiagnostics.gpuSceneAllocationBytes << ",\n";
            stream << "    \"gpuSceneFrameUploadBytes\": "
                   << renderDiagnostics.gpuSceneFrameUploadBytes << ",\n";
            stream << "    \"gpuSceneCumulativeUploadBytes\": "
                   << renderDiagnostics.gpuSceneCumulativeUploadBytes << ",\n";
            stream << "    \"gpuSceneFrameUploadRangeCount\": "
                   << renderDiagnostics.gpuSceneFrameUploadRangeCount << ",\n";
            stream << "    \"gpuSceneCurrentBufferSetCount\": "
                   << renderDiagnostics.gpuSceneCurrentBufferSetCount << ",\n";
            stream << "    \"gpuScenePendingBufferSetCount\": "
                   << renderDiagnostics.gpuScenePendingBufferSetCount << ",\n";
            stream << "    \"gpuSceneInFlightBufferSetCount\": "
                   << renderDiagnostics.gpuSceneInFlightBufferSetCount << ",\n";
            stream << "    \"gpuSceneFullUpload\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.gpuSceneFullUpload)
                   << ",\n";
            if (report.schemaVersion >= 9)
            {
                WriteUploadWorkDiagnostics(
                    stream,
                    "gpuSceneUploadWork",
                    renderDiagnostics.gpuSceneUploadWork);
                static constexpr std::array<const char*,
                                             GPU_SCENE_DIAGNOSTICS_TABLE_COUNT>
                    GPUSceneUploadWorkNames{{
                        "gpuScenePrimitivesUploadWork",
                        "gpuSceneBoundsUploadWork",
                        "gpuSceneTransformsUploadWork",
                        "gpuSceneMaterialsUploadWork",
                        "gpuSceneGeometriesUploadWork",
                        "gpuSceneDrawsUploadWork"}};
                for (uint32 tableIndex = 0;
                     tableIndex < GPU_SCENE_DIAGNOSTICS_TABLE_COUNT;
                     ++tableIndex)
                {
                    WriteUploadWorkDiagnostics(
                        stream,
                        GPUSceneUploadWorkNames[tableIndex],
                        renderDiagnostics.gpuSceneTableUploadWork[tableIndex]);
                }
            }
        }
        stream << "    \"renderPolicyRequestAvailable\": "
               << Diagnostics::JsonBool(
                      renderDiagnostics.renderPolicyRequestAvailable)
               << ",\n";
        stream << "    \"renderPolicyPlanAvailable\": "
               << Diagnostics::JsonBool(
                      renderDiagnostics.renderPolicyPlanAvailable)
               << ",\n";
        stream << "    \"renderPolicyReportAvailable\": "
               << Diagnostics::JsonBool(
                      renderDiagnostics.renderPolicyReportAvailable)
               << ",\n";
        stream << "    \"renderPolicyRequestFrameSequence\": "
               << renderDiagnostics.renderPolicyRequestFrameSequence << ",\n";
        stream << "    \"renderPolicyPlanFrameSequence\": "
               << renderDiagnostics.renderPolicyPlanFrameSequence << ",\n";
        stream << "    \"renderPolicyReportFrameSequence\": "
               << renderDiagnostics.renderPolicyReportFrameSequence << ",\n";
        stream << "    \"completedPresentedFrameSequence\": "
               << renderDiagnostics.completedPresentedFrameSequence << ",\n";
        stream << "    \"renderPolicyRequestedMode\": "
               << Diagnostics::JsonString(
                      renderDiagnostics.renderPolicyRequestedMode)
               << ",\n";
        stream << "    \"renderPolicyExecutionStatus\": "
               << Diagnostics::JsonString(
                      renderDiagnostics.renderPolicyExecutionStatus)
               << ",\n";
        stream << "    \"renderPolicySelectedTier\": "
               << Diagnostics::JsonString(
                      renderDiagnostics.renderPolicySelectedTier)
               << ",\n";
        stream << "    \"renderPolicyExecutedTier\": "
               << Diagnostics::JsonString(
                      renderDiagnostics.renderPolicyExecutedTier)
               << ",\n";
        stream << "    \"renderPolicyTierFallbackReason\": "
               << Diagnostics::JsonString(
                      renderDiagnostics.renderPolicyTierFallbackReason)
               << "\n";
        stream << "  },\n";
        if (report.pixelProbe.has_value())
        {
            const SamplePixelProbeReport& probe = *report.pixelProbe;
            stream << "  \"pixelProbe\": {\n";
            stream << "    \"complete\": "
                   << Diagnostics::JsonBool(probe.complete) << ",\n";
            stream << "    \"resultCode\": " << probe.resultCode << ",\n";
            stream << "    \"requestId\": " << probe.requestId << ",\n";
            stream << "    \"frameSequence\": " << probe.frameSequence << ",\n";
            stream << "    \"requiredSceneRevision\": "
                   << probe.requiredSceneRevision << ",\n";
            stream << "    \"runtimeSurfaceGeneration\": "
                   << probe.runtimeSurfaceGeneration << ",\n";
            stream << "    \"x\": " << probe.x << ",\n";
            stream << "    \"y\": " << probe.y;
            if (probe.complete)
            {
                stream << ",\n    \"preToneFormat\": \"RGBA16_FLOAT\",\n";
                stream << "    \"preToneRGBA16FloatBits\": ";
                WriteRawBits(stream, probe.preToneRGBA16FloatBits);
                stream << ",\n    \"finalBGRA8Bits\": ";
                WriteRawBits(stream, probe.finalBGRA8Bits);
            }
            stream << ",\n    \"message\": "
                   << Diagnostics::JsonString(probe.message) << "\n";
            stream << "  },\n";
        }
        stream << "  \"pass\": " << Diagnostics::JsonBool(report.pass) << "\n";
        stream << "}\n";
    }

    bool WriteSampleReportJson(const SampleReport& report,
                               const std::filesystem::path& path,
                               std::string* outError)
    {
        std::error_code error;
        const std::filesystem::path parent = path.parent_path();
        if (!parent.empty())
        {
            std::filesystem::create_directories(parent, error);
            if (error)
            {
                SetError(outError, "Failed to create report directory: " + error.message());
                return false;
            }
        }

        std::ofstream stream(path, std::ios::out | std::ios::trunc);
        if (!stream)
        {
            SetError(outError, "Failed to open sample report: " + path.string());
            return false;
        }

        WriteSampleReportJson(stream, report);
        return true;
    }

    SampleReport BuildSampleReport(const SampleAppDesc& desc, const SampleRunContext& context)
    {
        SampleReport report;
        report.schemaVersion = RVX_SAMPLE_REPORT_SCHEMA_VERSION;
        report.sampleName = desc.sampleName;
        report.category = desc.category;
        report.requestedBackend = context.options.backend;
        report.backend = context.resolvedBackend;
        report.frameCount = context.frameCount;
        report.width = context.options.width;
        report.height = context.options.height;
        report.quality = context.options.quality;
        // Report-only samples have no render execution provenance. Preserve
        // their configured/default request while making that absence explicit.
        report.requestedRenderPath = report.renderPath;
        report.actualRenderPath = "unavailable";
        report.diagnostics = context.options.diagnostics;
        report.screenshotPath = context.options.screenshotPath;
        report.enabledFeatures = desc.enabledFeatures;
        report.unsupportedFeatures = desc.unsupportedFeatures;
        report.fallbackReasons = desc.fallbackReasons;
        report.resourceDiagnostics = desc.resourceDiagnostics;
        report.renderDiagnostics = desc.renderDiagnostics;
        report.pass = true;

        SampleFeatureReporter reporter(report);
        if (!context.options.screenshotPath.empty() && !desc.supportsScreenshot)
        {
            reporter.Unsupported("ScreenshotCapture");
            reporter.Fallback(desc.sampleName + " is a report-driven sample and does not capture screenshots yet");
        }
        if (context.options.quality != "default" && !desc.supportsQualityProfiles)
        {
            reporter.Unsupported("QualityProfile");
            reporter.Fallback(desc.sampleName + " reports fixed capability diagnostics for all quality presets");
        }

        return report;
    }

    int RunReportOnlySample(int argc, char* argv[], const SampleAppDesc& desc)
    {
        SampleCLIOptions options;
        std::string parseError;
        if (!ParseSampleCLI(argc, argv, options, &parseError))
        {
            if (!parseError.empty())
            {
                std::cerr << parseError << "\n";
            }
            PrintSampleCLIUsage(std::cerr, desc.sampleName.c_str());
            return 2;
        }

        if (options.showHelp)
        {
            PrintSampleCLIUsage(std::cout, desc.sampleName.c_str());
            return 0;
        }

        SampleRunContext context;
        context.options = options;
        context.resolvedBackend =
            options.backend == RHIBackendType::Auto ? SelectBestBackend() : options.backend;
        context.frameCount = options.frames > 0 ? options.frames : 1;

        SampleReport report = BuildSampleReport(desc, context);
        if (!options.reportPath.empty())
        {
            std::string reportError;
            if (!WriteSampleReportJson(report, options.reportPath, &reportError))
            {
                std::cerr << reportError << "\n";
                return 1;
            }
        }
        else if (options.diagnostics)
        {
            WriteSampleReportJson(std::cout, report);
        }

        return report.pass ? 0 : 1;
    }
} // namespace RVX
