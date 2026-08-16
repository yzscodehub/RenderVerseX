/** @file FrameworkAssessment.cpp @brief Sample assessment contract implementation. */

#include "Samples/FrameworkAssessment.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <locale>
#include <sstream>
#include <tuple>
#include <type_traits>
#include <utility>

namespace RVX
{
    namespace
    {
        bool IsCodeSegmentStart(char value) noexcept
        {
            return value >= 'A' && value <= 'Z';
        }

        bool IsCodeSegmentCharacter(char value) noexcept
        {
            return IsCodeSegmentStart(value) ||
                   (value >= '0' && value <= '9') || value == '_';
        }

        bool IsUnavailableFingerprintValue(std::string_view value) noexcept
        {
            return value == "unavailable" ||
                   value == "driver-unavailable" ||
                   value.find("|driver-unavailable") != std::string_view::npos ||
                   value.find("#content=unavailable") != std::string_view::npos;
        }

        bool HasCompatibleV2Fingerprint(
            const AssessmentFingerprint& current,
            const AssessmentFingerprint& baseline) noexcept
        {
            // engineBuild remains report provenance. It is intentionally not a
            // compatibility dimension in schema v2, so a rebuild of the same
            // selected workload can be compared to its approved baseline.
            return current.sampleId == baseline.sampleId &&
                   current.sampleRevision == baseline.sampleRevision &&
                   current.backend == baseline.backend &&
                   current.device == baseline.device &&
                   current.renderConfiguration ==
                       baseline.renderConfiguration &&
                   current.assetSet == baseline.assetSet &&
                   current.platform == baseline.platform;
        }

        template <typename T>
        bool HasOnlyValidCodes(const std::vector<T>& values) noexcept
        {
            return std::all_of(values.begin(), values.end(),
                               [](const T& value) { return value.IsValid(); });
        }

        bool IsFiniteScalar(const AssessmentScalar& scalar) noexcept
        {
            if (const auto* value = std::get_if<float64>(&scalar))
            {
                return std::isfinite(*value);
            }
            return true;
        }

        std::string ScalarSortKey(const AssessmentScalar& scalar)
        {
            std::ostringstream stream;
            stream.imbue(std::locale::classic());
            stream << scalar.index() << ':';
            std::visit(
                [&stream](const auto& value)
                {
                    using ValueType = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<ValueType, bool>)
                    {
                        stream << (value ? "true" : "false");
                    }
                    else
                    {
                        stream << value;
                    }
                },
                scalar);
            return stream.str();
        }

        std::string MetricValueSortKey(const AssessmentMetricValue& value)
        {
            std::string key = value.metric.code.GetValue();
            key += '\x1f';
            key += value.metric.unit;
            key += '\x1f';
            key += value.value.IsAvailable() ? "1" : "0";
            key += '\x1f';
            key += value.value.GetReason();
            if (value.value.IsAvailable())
            {
                key += '\x1f';
                key += ScalarSortKey(*value.value.GetValue());
            }
            return key;
        }

        std::string CheckpointSortKey(const AssessmentCheckpoint& value)
        {
            return value.code.GetValue() + '\x1f' + value.description;
        }

        std::string FindingSortKey(const Finding& value)
        {
            std::ostringstream stream;
            stream << value.code.GetValue() << '\x1f'
                   << value.subsystemCode.GetValue() << '\x1f'
                   << value.invariantCode.GetValue() << '\x1f'
                   << CheckpointSortKey(value.checkpoint) << '\x1f'
                   << static_cast<uint32>(value.classification) << '\x1f'
                   << static_cast<uint32>(value.severity) << '\x1f'
                   << static_cast<uint32>(value.confidence) << '\x1f'
                   << value.summary << '\x1f' << value.detail << '\x1f'
                   << value.expected << '\x1f' << value.observed << '\x1f'
                   << value.traceCorrelationId << '\x1f'
                   << (value.gating ? "1" : "0") << '\x1f'
                   << value.blockingReason << '\x1f'
                   << (value.capabilityBlocking ? "1" : "0");
            const auto appendDiagnostic = [&stream](
                const DiagnosticValue<uint64>& diagnostic)
            {
                stream << '\x1f' << (diagnostic.IsAvailable() ? "1" : "0")
                       << '\x1f' << diagnostic.GetReason();
                if (diagnostic.IsAvailable())
                {
                    stream << '\x1f' << *diagnostic.GetValue();
                }
            };
            appendDiagnostic(value.frameBegin);
            appendDiagnostic(value.frameEnd);
            appendDiagnostic(value.sceneRevision);
            appendDiagnostic(value.resourceGeneration);
            appendDiagnostic(value.requestId);
            appendDiagnostic(value.completionToken);
            for (const std::filesystem::path& artifactPath : value.artifactPaths)
            {
                stream << '\x1f' << artifactPath.generic_string();
            }
            return stream.str();
        }

        void SortFinding(Finding& finding)
        {
            std::sort(finding.artifactPaths.begin(), finding.artifactPaths.end(),
                      [](const std::filesystem::path& lhs,
                         const std::filesystem::path& rhs)
                      {
                          return lhs.generic_string() < rhs.generic_string();
                      });
            finding.artifactPaths.erase(
                std::unique(finding.artifactPaths.begin(),
                            finding.artifactPaths.end(),
                            [](const std::filesystem::path& lhs,
                               const std::filesystem::path& rhs)
                            {
                                return lhs.generic_string() ==
                                       rhs.generic_string();
                            }),
                finding.artifactPaths.end());
        }

        void SortContract(SampleAssessmentContract& contract)
        {
            const auto sortByCode = [](const auto& lhs, const auto& rhs)
            {
                return std::tie(lhs.code.GetValue(), lhs.description) <
                       std::tie(rhs.code.GetValue(), rhs.description);
            };
            std::sort(contract.checkpoints.begin(), contract.checkpoints.end(),
                      sortByCode);
            std::sort(contract.actions.begin(), contract.actions.end(), sortByCode);
            std::sort(contract.invariants.begin(), contract.invariants.end(),
                      sortByCode);
            std::sort(contract.capabilities.begin(), contract.capabilities.end(),
                      [](const AssessmentCapability& lhs,
                         const AssessmentCapability& rhs)
                      {
                          return std::tie(lhs.code.GetValue(), lhs.description,
                                          lhs.gating, lhs.blockingReason) <
                                 std::tie(rhs.code.GetValue(), rhs.description,
                                          rhs.gating, rhs.blockingReason);
                      });
            std::sort(contract.metrics.begin(), contract.metrics.end(),
                      [](const AssessmentMetric& lhs, const AssessmentMetric& rhs)
                      {
                          return std::tie(lhs.code.GetValue(), lhs.unit,
                                          lhs.description) <
                                 std::tie(rhs.code.GetValue(), rhs.unit,
                                          rhs.description);
                      });
        }

        void SortSnapshot(AssessmentSnapshot& snapshot)
        {
            std::sort(snapshot.metrics.begin(), snapshot.metrics.end(),
                      [](const AssessmentMetricValue& lhs,
                         const AssessmentMetricValue& rhs)
                      {
                          return MetricValueSortKey(lhs) < MetricValueSortKey(rhs);
                      });
        }

        std::string SnapshotSortKey(const AssessmentSnapshot& snapshot)
        {
            std::string key = CheckpointSortKey(snapshot.checkpoint);
            for (const AssessmentMetricValue& metric : snapshot.metrics)
            {
                key += '\x1e';
                key += MetricValueSortKey(metric);
            }
            return key;
        }

        std::string InvariantObservationSortKey(
            const AssessmentInvariantObservation& observation)
        {
            return observation.invariant.code.GetValue() + '\x1f' +
                   observation.invariant.description + '\x1f' +
                   CheckpointSortKey(observation.checkpoint);
        }

        void SortInvariantObservations(
            std::vector<AssessmentInvariantObservation>& observations)
        {
            std::sort(observations.begin(), observations.end(),
                      [](const AssessmentInvariantObservation& lhs,
                         const AssessmentInvariantObservation& rhs)
                      {
                          return InvariantObservationSortKey(lhs) <
                                 InvariantObservationSortKey(rhs);
                      });
            observations.erase(
                std::unique(observations.begin(), observations.end(),
                            [](const AssessmentInvariantObservation& lhs,
                               const AssessmentInvariantObservation& rhs)
                            {
                                return InvariantObservationSortKey(lhs) ==
                                       InvariantObservationSortKey(rhs);
                            }),
                observations.end());
        }

        std::string CapabilityObservationSortKey(
            const AssessmentCapabilityObservation& observation)
        {
            std::string key = observation.capability.code.GetValue();
            key += '\x1f';
            key += observation.capability.description;
            key += '\x1f';
            key += observation.capability.gating ? "1" : "0";
            key += '\x1f';
            key += observation.capability.blockingReason;
            key += '\x1f';
            key += CheckpointSortKey(observation.checkpoint);
            key += '\x1f';
            key += observation.value.IsAvailable() ? "1" : "0";
            key += '\x1f';
            key += observation.value.GetReason();
            if (observation.value.IsAvailable())
            {
                key += '\x1f';
                key += *observation.value.GetValue() ? "1" : "0";
            }
            key += '\x1f';
            key += observation.reason;
            return key;
        }

        void SortCapabilityObservations(
            std::vector<AssessmentCapabilityObservation>& observations)
        {
            std::sort(observations.begin(), observations.end(),
                      [](const AssessmentCapabilityObservation& lhs,
                         const AssessmentCapabilityObservation& rhs)
                      {
                          return CapabilityObservationSortKey(lhs) <
                                 CapabilityObservationSortKey(rhs);
                      });
        }

        SampleAssessmentReport MakeDeterministicReport(
            const SampleAssessmentReport& report)
        {
            SampleAssessmentReport result = report;
            SortContract(result.contract);
            std::sort(result.actions.begin(), result.actions.end(),
                      [](const AssessmentAction& lhs,
                         const AssessmentAction& rhs)
                      {
                          return std::tie(lhs.code.GetValue(), lhs.description) <
                                 std::tie(rhs.code.GetValue(), rhs.description);
                      });
            std::sort(result.checkpoints.begin(), result.checkpoints.end(),
                      [](const AssessmentCheckpoint& lhs,
                         const AssessmentCheckpoint& rhs)
                      {
                          return CheckpointSortKey(lhs) < CheckpointSortKey(rhs);
                      });
            for (AssessmentSnapshot& snapshot : result.snapshots)
            {
                SortSnapshot(snapshot);
            }
            std::sort(result.snapshots.begin(), result.snapshots.end(),
                      [](const AssessmentSnapshot& lhs,
                         const AssessmentSnapshot& rhs)
                      {
                          return SnapshotSortKey(lhs) < SnapshotSortKey(rhs);
                      });
            SortCapabilityObservations(result.capabilityObservations);
            SortInvariantObservations(result.invariantObservations);
            for (Finding& finding : result.findings)
            {
                SortFinding(finding);
            }
            std::sort(result.findings.begin(), result.findings.end(),
                      [](const Finding& lhs, const Finding& rhs)
                      {
                          return FindingSortKey(lhs) < FindingSortKey(rhs);
                      });
            return result;
        }

        void WriteJsonString(std::ostream& stream, std::string_view value)
        {
            stream.put('"');
            for (const unsigned char character : value)
            {
                switch (character)
                {
                    case '"': stream << "\\\""; break;
                    case '\\': stream << "\\\\"; break;
                    case '\b': stream << "\\b"; break;
                    case '\f': stream << "\\f"; break;
                    case '\n': stream << "\\n"; break;
                    case '\r': stream << "\\r"; break;
                    case '\t': stream << "\\t"; break;
                    default:
                    {
                        if (character < 0x20)
                        {
                            constexpr char Hex[] = "0123456789abcdef";
                            stream << "\\u00" << Hex[(character >> 4) & 0x0f]
                                   << Hex[character & 0x0f];
                        }
                        else
                        {
                            stream.put(static_cast<char>(character));
                        }
                        break;
                    }
                }
            }
            stream.put('"');
        }

        void WriteJsonScalar(std::ostream& stream, const AssessmentScalar& value)
        {
            std::visit(
                [&stream](const auto& scalar)
                {
                    using ScalarType = std::decay_t<decltype(scalar)>;
                    if constexpr (std::is_same_v<ScalarType, bool>)
                    {
                        stream << (scalar ? "true" : "false");
                    }
                    else if constexpr (std::is_same_v<ScalarType, std::string>)
                    {
                        WriteJsonString(stream, scalar);
                    }
                    else if constexpr (std::is_same_v<ScalarType, float64>)
                    {
                        if (!std::isfinite(scalar))
                        {
                            stream << "null";
                            return;
                        }
                        char buffer[64] = {};
                        const std::to_chars_result result = std::to_chars(
                            std::begin(buffer), std::end(buffer), scalar,
                            std::chars_format::general,
                            std::numeric_limits<float64>::max_digits10);
                        if (result.ec == std::errc{})
                        {
                            stream.write(buffer, result.ptr - buffer);
                        }
                        else
                        {
                            stream << "null";
                        }
                    }
                    else
                    {
                        stream << scalar;
                    }
                },
                value);
        }

        template <typename T, typename WriteValueFn>
        void WriteDiagnosticValue(std::ostream& stream,
                                  const DiagnosticValue<T>& value,
                                  WriteValueFn&& writeValue)
        {
            stream << "{\"available\":"
                   << (value.IsAvailable() ? "true" : "false")
                   << ",\"reason\":";
            WriteJsonString(stream, value.GetReason());
            if (value.IsAvailable())
            {
                stream << ",\"value\":";
                writeValue(*value.GetValue());
            }
            stream.put('}');
        }

        void WriteCheckpoint(std::ostream& stream,
                             const AssessmentCheckpoint& checkpoint)
        {
            stream << "{\"code\":";
            WriteJsonString(stream, checkpoint.code.GetValue());
            stream << ",\"description\":";
            WriteJsonString(stream, checkpoint.description);
            stream.put('}');
        }

        void WriteAction(std::ostream& stream, const AssessmentAction& action)
        {
            stream << "{\"code\":";
            WriteJsonString(stream, action.code.GetValue());
            stream << ",\"description\":";
            WriteJsonString(stream, action.description);
            stream.put('}');
        }

        void WriteInvariant(std::ostream& stream,
                            const AssessmentInvariant& invariant)
        {
            stream << "{\"code\":";
            WriteJsonString(stream, invariant.code.GetValue());
            stream << ",\"description\":";
            WriteJsonString(stream, invariant.description);
            stream.put('}');
        }

        void WriteMetric(std::ostream& stream, const AssessmentMetric& metric)
        {
            stream << "{\"code\":";
            WriteJsonString(stream, metric.code.GetValue());
            stream << ",\"unit\":";
            WriteJsonString(stream, metric.unit);
            stream << ",\"description\":";
            WriteJsonString(stream, metric.description);
            stream.put('}');
        }

        void WriteCapability(std::ostream& stream,
                             const AssessmentCapability& capability)
        {
            stream << "{\"code\":";
            WriteJsonString(stream, capability.code.GetValue());
            stream << ",\"description\":";
            WriteJsonString(stream, capability.description);
            stream << ",\"gating\":"
                   << (capability.gating ? "true" : "false")
                   << ",\"blockingReason\":";
            WriteJsonString(stream, capability.blockingReason);
            stream.put('}');
        }

        void WriteCapabilityObservation(
            std::ostream& stream,
            const AssessmentCapabilityObservation& observation)
        {
            stream << "{\"capability\":";
            WriteCapability(stream, observation.capability);
            stream << ",\"checkpoint\":";
            WriteCheckpoint(stream, observation.checkpoint);
            stream << ",\"diagnostic\":";
            WriteDiagnosticValue(
                stream, observation.value,
                [&stream](bool value)
                {
                    stream << (value ? "true" : "false");
                });
            stream << ",\"reason\":";
            WriteJsonString(stream, observation.reason);
            stream.put('}');
        }

        void WriteMetricValue(std::ostream& stream,
                              const AssessmentMetricValue& metric)
        {
            stream << "{\"metric\":";
            WriteMetric(stream, metric.metric);
            stream << ",\"diagnostic\":";
            WriteDiagnosticValue(
                stream, metric.value,
                [&stream](const AssessmentScalar& scalar)
                {
                    WriteJsonScalar(stream, scalar);
                });
            stream.put('}');
        }

        void WriteInvariantObservation(
            std::ostream& stream,
            const AssessmentInvariantObservation& observation)
        {
            stream << "{\"invariant\":";
            WriteInvariant(stream, observation.invariant);
            stream << ",\"checkpoint\":";
            WriteCheckpoint(stream, observation.checkpoint);
            stream.put('}');
        }

        void WriteUnsignedDiagnostic(std::ostream& stream,
                                     const DiagnosticValue<uint64>& value)
        {
            WriteDiagnosticValue(
                stream, value,
                [&stream](uint64 scalar)
                {
                    stream << scalar;
                });
        }

        void WriteFinding(std::ostream& stream, const Finding& finding)
        {
            stream << "{\"code\":";
            WriteJsonString(stream, finding.code.GetValue());
            stream << ",\"subsystemCode\":";
            WriteJsonString(stream, finding.subsystemCode.GetValue());
            stream << ",\"invariantCode\":";
            WriteJsonString(stream, finding.invariantCode.GetValue());
            stream << ",\"checkpoint\":";
            WriteCheckpoint(stream, finding.checkpoint);
            stream << ",\"class\":";
            WriteJsonString(stream, GetFindingClassCode(finding.classification));
            stream << ",\"severity\":";
            WriteJsonString(stream, GetFindingSeverityCode(finding.severity));
            stream << ",\"confidence\":";
            WriteJsonString(stream, GetFindingConfidenceCode(finding.confidence));
            stream << ",\"summary\":";
            WriteJsonString(stream, finding.summary);
            stream << ",\"detail\":";
            WriteJsonString(stream, finding.detail);
            stream << ",\"expected\":";
            WriteJsonString(stream, finding.expected);
            stream << ",\"observed\":";
            WriteJsonString(stream, finding.observed);
            stream << ",\"frameBegin\":";
            WriteUnsignedDiagnostic(stream, finding.frameBegin);
            stream << ",\"frameEnd\":";
            WriteUnsignedDiagnostic(stream, finding.frameEnd);
            stream << ",\"sceneRevision\":";
            WriteUnsignedDiagnostic(stream, finding.sceneRevision);
            stream << ",\"resourceGeneration\":";
            WriteUnsignedDiagnostic(stream, finding.resourceGeneration);
            stream << ",\"requestId\":";
            WriteUnsignedDiagnostic(stream, finding.requestId);
            stream << ",\"completionToken\":";
            WriteUnsignedDiagnostic(stream, finding.completionToken);
            stream << ",\"traceCorrelationId\":";
            WriteJsonString(stream, finding.traceCorrelationId);
            stream << ",\"artifactPaths\":[";
            for (size_t index = 0; index < finding.artifactPaths.size(); ++index)
            {
                if (index > 0)
                {
                    stream.put(',');
                }
                WriteJsonString(stream, finding.artifactPaths[index].generic_string());
            }
            stream << "],\"gating\":" << (finding.gating ? "true" : "false")
                   << ",\"blockingReason\":";
            WriteJsonString(stream, finding.blockingReason);
            stream << ",\"capabilityBlocking\":"
                   << (finding.capabilityBlocking ? "true" : "false");
            stream.put('}');
        }

        void WriteFingerprint(std::ostream& stream,
                              const AssessmentFingerprint& fingerprint)
        {
            stream << "{\"sampleId\":";
            WriteJsonString(stream, fingerprint.sampleId);
            stream << ",\"sampleRevision\":";
            WriteJsonString(stream, fingerprint.sampleRevision);
            stream << ",\"engineBuild\":";
            WriteJsonString(stream, fingerprint.engineBuild);
            stream << ",\"backend\":";
            WriteJsonString(stream, fingerprint.backend);
            stream << ",\"device\":";
            WriteJsonString(stream, fingerprint.device);
            stream << ",\"renderConfiguration\":";
            WriteJsonString(stream, fingerprint.renderConfiguration);
            stream << ",\"assetSet\":";
            WriteJsonString(stream, fingerprint.assetSet);
            stream << ",\"platform\":";
            WriteJsonString(stream, fingerprint.platform);
            stream.put('}');
        }

        void WriteContract(std::ostream& stream,
                           const SampleAssessmentContract& contract)
        {
            stream << "{\"code\":";
            WriteJsonString(stream, contract.code.GetValue());
            stream << ",\"revision\":";
            WriteJsonString(stream, contract.revision);

            const auto writeCollection = [&stream](std::string_view name,
                                                    const auto& values,
                                                    auto&& writeValue)
            {
                stream << ",\"" << name << "\":[";
                for (size_t index = 0; index < values.size(); ++index)
                {
                    if (index > 0)
                    {
                        stream.put(',');
                    }
                    writeValue(stream, values[index]);
                }
                stream.put(']');
            };
            writeCollection("checkpoints", contract.checkpoints, WriteCheckpoint);
            writeCollection("actions", contract.actions, WriteAction);
            writeCollection("invariants", contract.invariants, WriteInvariant);
            writeCollection("metrics", contract.metrics, WriteMetric);
            writeCollection("capabilities", contract.capabilities, WriteCapability);
            stream.put('}');
        }

        AssessmentBlockGrade GetBlockGrade(const std::vector<Finding>& findings)
        {
            AssessmentBlockGrade grade = AssessmentBlockGrade::Pass;
            for (const Finding& finding : findings)
            {
                if (finding.IsBlocking())
                {
                    return finding.severity == FindingSeverity::Fatal
                               ? AssessmentBlockGrade::Fatal
                               : AssessmentBlockGrade::Blocked;
                }
                if (finding.severity != FindingSeverity::Info &&
                    grade == AssessmentBlockGrade::Pass)
                {
                    grade = AssessmentBlockGrade::Advisory;
                }
            }
            return grade;
        }

        Finding CreateInvalidContractFinding()
        {
            Finding finding;
            finding.code = AssessmentCode("CONTRACT.INVALID_DEFINITION");
            finding.subsystemCode = AssessmentCode("ASSESSMENT.CONTRACT");
            finding.invariantCode = AssessmentCode("ASSESSMENT.CONTRACT.VALID");
            finding.checkpoint = AssessmentCheckpoints::EngineBaseline;
            finding.classification = FindingClass::ContractViolation;
            finding.severity = FindingSeverity::Error;
            finding.confidence = FindingConfidence::Confirmed;
            finding.summary = "The sample assessment contract contains an invalid stable value.";
            finding.expected = "A valid stable sample assessment contract.";
            finding.observed = "An invalid assessment contract definition.";
            finding.gating = true;
            finding.blockingReason = "Assessment contract validation failed.";
            return finding;
        }

        Finding CreateDroppedEventFinding(uint64 droppedEventCount)
        {
            Finding finding;
            finding.code = AssessmentCode("INSTRUMENTATION.EVENTS_DROPPED");
            finding.subsystemCode = AssessmentCode("ASSESSMENT.CHANNEL");
            finding.invariantCode = AssessmentCode("INSTRUMENTATION.EVENT_DELIVERY");
            finding.checkpoint = AssessmentCheckpoints::ScenarioStable;
            finding.classification = FindingClass::InstrumentationGap;
            finding.severity = FindingSeverity::Error;
            finding.confidence = FindingConfidence::Confirmed;
            finding.summary = "Assessment channel capacity dropped one or more events.";
            finding.expected = "0 dropped assessment events.";
            finding.observed = std::to_string(droppedEventCount) +
                               " dropped assessment events.";
            finding.gating = true;
            finding.blockingReason =
                "Assessment evidence is incomplete because channel events were dropped.";
            return finding;
        }

        Finding CreateCoverageFinding(AssessmentCode code,
                                      AssessmentCode invariantCode,
                                      AssessmentCheckpoint checkpoint,
                                      std::string summary,
                                      std::string expected,
                                      std::string observed)
        {
            Finding finding;
            finding.code = std::move(code);
            finding.subsystemCode = AssessmentCode("ASSESSMENT.CONTRACT");
            finding.invariantCode = std::move(invariantCode);
            finding.checkpoint = std::move(checkpoint);
            finding.classification = FindingClass::InstrumentationGap;
            finding.severity = FindingSeverity::Error;
            finding.confidence = FindingConfidence::Confirmed;
            finding.summary = std::move(summary);
            finding.expected = std::move(expected);
            finding.observed = std::move(observed);
            finding.gating = true;
            finding.blockingReason =
                "A required assessment contract observation is missing or unavailable.";
            return finding;
        }

        Finding CreateCapabilityGapFinding(
            const AssessmentCapability& capability,
            std::string observed)
        {
            Finding finding;
            finding.code = AssessmentCode("CAPABILITY.UNAVAILABLE");
            finding.subsystemCode = AssessmentCode("ASSESSMENT.CONTRACT");
            finding.invariantCode = AssessmentCode("CAPABILITY.COVERAGE");
            finding.checkpoint = AssessmentCheckpoints::EngineShutdownComplete;
            finding.classification = FindingClass::CapabilityGap;
            finding.severity = capability.gating ? FindingSeverity::Error
                                                 : FindingSeverity::Warning;
            finding.confidence = FindingConfidence::Confirmed;
            finding.summary = "A required assessment capability is unavailable.";
            finding.expected = "Capability " + capability.code.GetValue() +
                               " available.";
            finding.observed = std::move(observed);
            finding.gating = capability.gating;
            finding.blockingReason = capability.gating
                                        ? capability.blockingReason
                                        : std::string{};
            finding.capabilityBlocking = capability.gating;
            return finding;
        }

        bool MarkCoverageCode(std::vector<std::string>& observedCodes,
                              const AssessmentCode& code)
        {
            const auto existing = std::find(observedCodes.begin(),
                                            observedCodes.end(),
                                            code.GetValue());
            if (existing != observedCodes.end())
            {
                return false;
            }
            observedCodes.push_back(code.GetValue());
            return true;
        }

        bool HasCheckpointObservation(const SampleAssessmentReport& report,
                                      const AssessmentCode& code)
        {
            const auto checkpointMatches = [&code](
                                               const AssessmentCheckpoint& value)
            {
                return value.code == code;
            };
            return std::any_of(report.checkpoints.begin(),
                               report.checkpoints.end(), checkpointMatches) ||
                   std::any_of(report.snapshots.begin(), report.snapshots.end(),
                               [&checkpointMatches](const AssessmentSnapshot& value)
                               {
                                   return checkpointMatches(value.checkpoint);
                               });
        }

        bool HasActionObservation(const SampleAssessmentReport& report,
                                  const AssessmentCode& code)
        {
            return std::any_of(report.actions.begin(), report.actions.end(),
                               [&code](const AssessmentAction& action)
                               {
                                   return action.code == code;
                               });
        }

        bool HasMetricObservation(const SampleAssessmentReport& report,
                                  const AssessmentCode& code,
                                  bool* outAvailable,
                                  std::vector<std::string>* outReasons)
        {
            bool observed = false;
            bool available = false;
            for (const AssessmentSnapshot& snapshot : report.snapshots)
            {
                for (const AssessmentMetricValue& metric : snapshot.metrics)
                {
                    if (metric.metric.code != code)
                    {
                        continue;
                    }

                    observed = true;
                    if (metric.value.IsAvailable())
                    {
                        available = true;
                        continue;
                    }

                    if (outReasons != nullptr)
                    {
                        const std::string& reason = metric.value.GetReason();
                        outReasons->push_back(
                            reason.empty() ? "<unspecified>" : reason);
                    }
                }
            }

            if (outAvailable != nullptr)
            {
                *outAvailable = available;
            }
            return observed;
        }

        bool HasInvariantObservation(const SampleAssessmentReport& report,
                                     const AssessmentCode& code)
        {
            return std::any_of(
                       report.invariantObservations.begin(),
                       report.invariantObservations.end(),
                       [&code](const AssessmentInvariantObservation& observation)
                       {
                           return observation.invariant.code == code;
                       }) ||
                   std::any_of(report.findings.begin(), report.findings.end(),
                               [&code](const Finding& finding)
                               {
                                   return finding.invariantCode == code;
                               });
        }

        void AppendContractCoverageFindings(SampleAssessmentReport& report)
        {
            std::vector<std::string> inspectedCodes;
            for (const AssessmentCheckpoint& required : report.contract.checkpoints)
            {
                if (!MarkCoverageCode(inspectedCodes, required.code) ||
                    HasCheckpointObservation(report, required.code))
                {
                    continue;
                }

                report.findings.push_back(CreateCoverageFinding(
                    AssessmentCode("INSTRUMENTATION.CHECKPOINT.MISSING"),
                    AssessmentCode("INSTRUMENTATION.CHECKPOINT.COVERAGE"),
                    required,
                    "A required assessment checkpoint was not observed.",
                    "Checkpoint " + required.code.GetValue() + " recorded.",
                    "No checkpoint or snapshot recorded for " +
                        required.code.GetValue() + "."));
            }

            inspectedCodes.clear();
            for (const AssessmentAction& required : report.contract.actions)
            {
                if (!MarkCoverageCode(inspectedCodes, required.code) ||
                    HasActionObservation(report, required.code))
                {
                    continue;
                }

                report.findings.push_back(CreateCoverageFinding(
                    AssessmentCode("INSTRUMENTATION.ACTION.MISSING"),
                    AssessmentCode("INSTRUMENTATION.ACTION.COVERAGE"),
                    AssessmentCheckpoints::EngineShutdownComplete,
                    "A required assessment action was not observed.",
                    "Action " + required.code.GetValue() + " recorded.",
                    "No action recorded for " + required.code.GetValue() +
                        "."));
            }

            inspectedCodes.clear();
            for (const AssessmentMetric& required : report.contract.metrics)
            {
                if (!MarkCoverageCode(inspectedCodes, required.code))
                {
                    continue;
                }

                bool available = false;
                std::vector<std::string> unavailableReasons;
                const bool observed = HasMetricObservation(
                    report, required.code, &available, &unavailableReasons);
                if (!observed)
                {
                    report.findings.push_back(CreateCoverageFinding(
                        AssessmentCode("INSTRUMENTATION.METRIC.MISSING"),
                        AssessmentCode("INSTRUMENTATION.METRIC.COVERAGE"),
                        AssessmentCheckpoints::EngineShutdownComplete,
                        "A required assessment metric was not observed.",
                        "Metric " + required.code.GetValue() + " recorded.",
                        "No diagnostic value recorded for " +
                            required.code.GetValue() + "."));
                    continue;
                }
                if (available)
                {
                    continue;
                }

                std::sort(unavailableReasons.begin(), unavailableReasons.end());
                unavailableReasons.erase(
                    std::unique(unavailableReasons.begin(),
                                unavailableReasons.end()),
                    unavailableReasons.end());
                std::string reasonList;
                for (size_t index = 0; index < unavailableReasons.size(); ++index)
                {
                    if (index > 0)
                    {
                        reasonList += "; ";
                    }
                    reasonList += unavailableReasons[index];
                }
                report.findings.push_back(CreateCoverageFinding(
                    AssessmentCode("INSTRUMENTATION.METRIC.UNAVAILABLE"),
                    AssessmentCode("INSTRUMENTATION.METRIC.AVAILABILITY"),
                    AssessmentCheckpoints::EngineShutdownComplete,
                    "A required assessment metric was captured but unavailable.",
                    "At least one available diagnostic value for " +
                        required.code.GetValue() + ".",
                    "All recorded diagnostic values for " +
                        required.code.GetValue() + " were unavailable: " +
                        reasonList + "."));
            }

            inspectedCodes.clear();
            for (const AssessmentCapability& required : report.contract.capabilities)
            {
                if (!MarkCoverageCode(inspectedCodes, required.code))
                {
                    continue;
                }

                bool observed = false;
                bool available = false;
                bool supported = false;
                std::vector<std::string> unavailableReasons;
                std::vector<std::string> unsupportedReasons;
                for (const AssessmentCapabilityObservation& observation :
                     report.capabilityObservations)
                {
                    if (observation.capability.code != required.code)
                    {
                        continue;
                    }

                    observed = true;
                    if (!observation.value.IsAvailable())
                    {
                        const std::string& diagnosticReason =
                            observation.value.GetReason();
                        unavailableReasons.push_back(
                            diagnosticReason.empty() ? observation.reason
                                                      : diagnosticReason);
                        continue;
                    }

                    available = true;
                    if (*observation.value.GetValue())
                    {
                        supported = true;
                        continue;
                    }
                    unsupportedReasons.push_back(observation.reason);
                }

                if (!observed)
                {
                    report.findings.push_back(CreateCoverageFinding(
                        AssessmentCode("INSTRUMENTATION.CAPABILITY.MISSING"),
                        AssessmentCode("INSTRUMENTATION.CAPABILITY.COVERAGE"),
                        AssessmentCheckpoints::EngineShutdownComplete,
                        "A required assessment capability was not observed.",
                        "Capability " + required.code.GetValue() +
                            " evaluated.",
                        "No capability observation recorded for " +
                            required.code.GetValue() + "."));
                    continue;
                }
                if (supported)
                {
                    continue;
                }
                if (!available)
                {
                    std::sort(unavailableReasons.begin(),
                              unavailableReasons.end());
                    unavailableReasons.erase(
                        std::unique(unavailableReasons.begin(),
                                    unavailableReasons.end()),
                        unavailableReasons.end());
                    std::string reasonList;
                    for (size_t index = 0;
                         index < unavailableReasons.size();
                         ++index)
                    {
                        if (index > 0)
                        {
                            reasonList += "; ";
                        }
                        reasonList += unavailableReasons[index];
                    }
                    report.findings.push_back(CreateCoverageFinding(
                        AssessmentCode("INSTRUMENTATION.CAPABILITY.UNAVAILABLE"),
                        AssessmentCode("INSTRUMENTATION.CAPABILITY.AVAILABILITY"),
                        AssessmentCheckpoints::EngineShutdownComplete,
                        "A required assessment capability could not be evaluated.",
                        "An available diagnostic value for capability " +
                            required.code.GetValue() + ".",
                        "All capability observations for " +
                            required.code.GetValue() + " were unavailable: " +
                            reasonList + "."));
                    continue;
                }

                std::sort(unsupportedReasons.begin(), unsupportedReasons.end());
                unsupportedReasons.erase(
                    std::unique(unsupportedReasons.begin(),
                                unsupportedReasons.end()),
                    unsupportedReasons.end());
                std::string reasonList;
                for (size_t index = 0; index < unsupportedReasons.size(); ++index)
                {
                    if (index > 0)
                    {
                        reasonList += "; ";
                    }
                    reasonList += unsupportedReasons[index];
                }
                report.findings.push_back(CreateCapabilityGapFinding(
                    required,
                    "Capability " + required.code.GetValue() +
                        " evaluated unavailable: " + reasonList + "."));
            }

            inspectedCodes.clear();
            for (const AssessmentInvariant& required : report.contract.invariants)
            {
                if (!MarkCoverageCode(inspectedCodes, required.code) ||
                    HasInvariantObservation(report, required.code))
                {
                    continue;
                }

                report.findings.push_back(CreateCoverageFinding(
                    AssessmentCode("INSTRUMENTATION.INVARIANT.MISSING"),
                    AssessmentCode("INSTRUMENTATION.INVARIANT.COVERAGE"),
                    AssessmentCheckpoints::EngineShutdownComplete,
                    "A required assessment invariant was not evaluated.",
                    "An explicit observation or related finding for " +
                        required.code.GetValue() + ".",
                    "No invariant observation or finding recorded for " +
                        required.code.GetValue() + "."));
            }
        }
    } // namespace

    AssessmentCode::AssessmentCode(std::string value)
        : m_value(std::move(value))
    {
    }

    const std::string& AssessmentCode::GetValue() const noexcept
    {
        return m_value;
    }

    bool AssessmentCode::IsValid() const noexcept
    {
        return IsStable(m_value);
    }

    bool AssessmentCode::IsStable(std::string_view value) noexcept
    {
        if (value.empty() || !IsCodeSegmentStart(value.front()))
        {
            return false;
        }

        bool previousWasDot = false;
        for (size_t index = 0; index < value.size(); ++index)
        {
            const char character = value[index];
            if (character == '.')
            {
                if (previousWasDot || index == 0 || index + 1 == value.size())
                {
                    return false;
                }
                previousWasDot = true;
                continue;
            }
            if ((previousWasDot && !IsCodeSegmentStart(character)) ||
                (!previousWasDot && !IsCodeSegmentCharacter(character)))
            {
                return false;
            }
            previousWasDot = false;
        }
        return true;
    }

    bool operator<(const AssessmentCode& lhs, const AssessmentCode& rhs) noexcept
    {
        return lhs.GetValue() < rhs.GetValue();
    }

    bool AssessmentCheckpoint::IsValid() const noexcept
    {
        return code.IsValid();
    }

    bool AssessmentAction::IsValid() const noexcept
    {
        return code.IsValid();
    }

    bool AssessmentInvariant::IsValid() const noexcept
    {
        return code.IsValid();
    }

    bool AssessmentMetric::IsValid() const noexcept
    {
        return code.IsValid();
    }

    bool AssessmentCapability::IsValid() const noexcept
    {
        return code.IsValid() && (!gating || !blockingReason.empty());
    }

    bool AssessmentCapabilityObservation::IsValid() const noexcept
    {
        return capability.IsValid() && checkpoint.IsValid() && !reason.empty() &&
               (value.IsAvailable() || !value.GetReason().empty());
    }

    bool AssessmentMetricValue::IsValid() const noexcept
    {
        if (!metric.IsValid())
            return false;
        if (!value.IsAvailable())
            return !value.GetReason().empty();
        return value.GetValue().has_value() &&
               IsFiniteScalar(*value.GetValue());
    }

    bool AssessmentSnapshot::IsValid() const noexcept
    {
        return checkpoint.IsValid() && HasOnlyValidCodes(metrics);
    }

    bool AssessmentInvariantObservation::IsValid() const noexcept
    {
        return invariant.IsValid() && checkpoint.IsValid();
    }

    const char* GetFindingClassCode(FindingClass value) noexcept
    {
        switch (value)
        {
            case FindingClass::ContractViolation: return "CONTRACT_VIOLATION";
            case FindingClass::LifetimeLeak: return "LIFETIME_LEAK";
            case FindingClass::StaleIdentity: return "STALE_IDENTITY";
            case FindingClass::RevisionDiscontinuity:
                return "REVISION_DISCONTINUITY";
            case FindingClass::HiddenFallback: return "HIDDEN_FALLBACK";
            case FindingClass::BackendDivergence: return "BACKEND_DIVERGENCE";
            case FindingClass::Nondeterminism: return "NONDETERMINISM";
            case FindingClass::PerformanceRegression:
                return "PERFORMANCE_REGRESSION";
            case FindingClass::CapabilityGap: return "CAPABILITY_GAP";
            case FindingClass::InstrumentationGap: return "INSTRUMENTATION_GAP";
            default: return "INVALID";
        }
    }

    const char* GetFindingSeverityCode(FindingSeverity value) noexcept
    {
        switch (value)
        {
            case FindingSeverity::Info: return "INFO";
            case FindingSeverity::Warning: return "WARNING";
            case FindingSeverity::Error: return "ERROR";
            case FindingSeverity::Fatal: return "FATAL";
            default: return "INVALID";
        }
    }

    const char* GetFindingConfidenceCode(FindingConfidence value) noexcept
    {
        switch (value)
        {
            case FindingConfidence::Suspected: return "SUSPECTED";
            case FindingConfidence::StrongEvidence: return "STRONG_EVIDENCE";
            case FindingConfidence::Confirmed: return "CONFIRMED";
            default: return "INVALID";
        }
    }

    bool Finding::IsValid() const noexcept
    {
        return code.IsValid() && subsystemCode.IsValid() &&
               invariantCode.IsValid() && checkpoint.IsValid() &&
               !summary.empty() && !expected.empty() && !observed.empty() &&
               (!gating || !blockingReason.empty()) &&
               (!capabilityBlocking ||
                (classification == FindingClass::CapabilityGap && gating));
    }

    bool Finding::IsBlocking() const noexcept
    {
        if ((classification == FindingClass::CapabilityGap &&
             !capabilityBlocking) ||
            classification == FindingClass::PerformanceRegression)
        {
            return false;
        }
        return gating && confidence == FindingConfidence::Confirmed &&
               (severity == FindingSeverity::Error ||
                severity == FindingSeverity::Fatal);
    }

    bool AssessmentFingerprint::IsComplete() const noexcept
    {
        return !sampleId.empty() && !sampleRevision.empty() &&
               !engineBuild.empty() && !backend.empty() && !device.empty() &&
               !renderConfiguration.empty() && !assetSet.empty() &&
               !platform.empty() &&
               !IsUnavailableFingerprintValue(sampleId) &&
               !IsUnavailableFingerprintValue(sampleRevision) &&
               !IsUnavailableFingerprintValue(engineBuild) &&
               !IsUnavailableFingerprintValue(backend) &&
               !IsUnavailableFingerprintValue(device) &&
               !IsUnavailableFingerprintValue(renderConfiguration) &&
               !IsUnavailableFingerprintValue(assetSet) &&
               !IsUnavailableFingerprintValue(platform);
    }

    bool SampleAssessmentContract::IsValid() const noexcept
    {
        return code.IsValid() && !revision.empty() &&
               HasOnlyValidCodes(checkpoints) && HasOnlyValidCodes(actions) &&
               HasOnlyValidCodes(invariants) && HasOnlyValidCodes(metrics) &&
               HasOnlyValidCodes(capabilities);
    }

    SampleAssessmentContract MakeDefaultSampleAssessmentContract()
    {
        SampleAssessmentContract contract;
        contract.code = AssessmentCode("SAMPLE.FRAMEWORK");
        contract.revision = "2";
        contract.checkpoints = {
            AssessmentCheckpoints::EngineBaseline,
            AssessmentCheckpoints::ScenarioSetup,
            AssessmentCheckpoints::ActionRequested,
            AssessmentCheckpoints::ActionApplied,
            AssessmentCheckpoints::ScenarioStable,
            AssessmentCheckpoints::TeardownBefore,
            AssessmentCheckpoints::TeardownSceneComplete,
            AssessmentCheckpoints::TeardownRenderDrained,
            AssessmentCheckpoints::EngineShutdownComplete,
        };
        contract.invariants = {
            {AssessmentCode("ENGINE.FRAME.PRESENTED"),
             "At least one fully submitted frame reaches presentation."},
            {AssessmentCode("ENGINE.TEARDOWN.CLEAN"),
             "Scene, render, jobs, and worlds retire before shutdown ends."},
            {AssessmentCode("INSTRUMENTATION.REQUIRED_AVAILABLE"),
             "Required diagnostics explicitly report availability."},
        };
        contract.metrics = {
            {AssessmentCode("SCENE.ENTITY_COUNT"), "count",
             "Live generation-qualified ECS entity count."},
            {AssessmentCode("SCENE.PENDING_DESTROY_COUNT"), "count",
             "Entities hidden from ordinary queries and awaiting cleanup publication."},
            {AssessmentCode("SCENE.CLEANUP_REQUIRED_COUNT"), "count",
             "Entities waiting for one or more side-table cleanup acknowledgements."},
            {AssessmentCode("SCENE.RETIRING_COUNT"), "count",
             "Entities whose external retirement proof is still pending."},
            {AssessmentCode("SCENE.RECYCLABLE_COUNT"), "count",
             "Entities eligible for generation-safe slot recycling."},
            {AssessmentCode("SCENE.SNAPSHOT_REVISION"), "revision",
             "Latest immutable ECS Scene snapshot revision."},
            {AssessmentCode("SCENE.STRUCTURAL_SEQUENCE"), "sequence",
             "Next retained ECS structural-journal sequence."},
            {AssessmentCode("SCENE.COMMAND_BUFFER_QUEUED"), "count",
             "Owner-barrier ECS command buffers awaiting playback."},
            {AssessmentCode("SCENE.COMMAND_BUFFER_APPLIED"), "count",
             "ECS command buffers accepted by an owner playback barrier."},
            {AssessmentCode("SCENE.COMMAND_BUFFER_REJECTED"), "count",
             "ECS command buffers rejected without partial publication."},
            {AssessmentCode("SCENE.CLEANUP_CONTINUITY_LOSS"), "count",
             "Cleanup-journal continuity losses requiring authoritative rebuild."},
            {AssessmentCode("SCENE.LOCAL_TRANSFORM_WRITE_VERSION"), "version",
             "Latest local-transform Fragment write version."},
            {AssessmentCode("RESOURCE.ACTIVE_OPERATIONS"), "count",
             "Active coalesced resource operations."},
            {AssessmentCode("RESOURCE.ACTIVE_SUBSCRIBERS"), "count",
             "Active resource request subscribers."},
            {AssessmentCode("RESOURCE.PENDING_PUBLICATIONS"), "count",
             "Prepared bundles awaiting owner publication."},
            {AssessmentCode("RESOURCE.PENDING_UPLOADS"), "count",
             "Resource uploads not yet terminal."},
            {AssessmentCode("RESOURCE.PENDING_REPLACEMENTS"), "count",
             "Transactional content replacements not yet terminal."},
            {AssessmentCode("RESOURCE.PENDING_RETIREMENTS"), "count",
             "Resource retirements awaiting completion."},
            {AssessmentCode("RESOURCE.ACTIVE_LEASES"), "count",
             "Active asset residency leases."},
            {AssessmentCode("PHYSICS.BODY_COUNT"), "count",
             "Physics body count."},
            {AssessmentCode("PHYSICS.FIXED_STEPS"), "count",
             "Completed fixed physics steps."},
            {AssessmentCode("RENDER.SUBMITTED_SEQUENCE"), "sequence",
             "Latest submitted render frame sequence."},
            {AssessmentCode("RENDER.PRESENTED_SEQUENCE"), "sequence",
             "Latest presented render frame sequence."},
            {AssessmentCode("RENDER.PENDING_UPLOADS"), "count",
             "Render-side pending uploads."},
            {AssessmentCode("ENGINE.WORLD_COUNT"), "count",
             "World count at the checkpoint."},
            {AssessmentCode("ENGINE.SHUTDOWN_CLEAN"), "boolean",
             "Whether Engine shutdown satisfied all lifetime gates."},
            {AssessmentCode("RESOURCE.SHUTDOWN_CLEAN"), "boolean",
             "Whether Resource shutdown retired CPU and completion-owned work."},
        };
        return contract;
    }

    const char* GetAssessmentBlockGradeCode(AssessmentBlockGrade value) noexcept
    {
        switch (value)
        {
            case AssessmentBlockGrade::Pass: return "PASS";
            case AssessmentBlockGrade::Advisory: return "ADVISORY";
            case AssessmentBlockGrade::Blocked: return "BLOCKED";
            case AssessmentBlockGrade::Fatal: return "FATAL";
            default: return "INVALID";
        }
    }

    bool IsBlockingGrade(AssessmentBlockGrade value) noexcept
    {
        return value == AssessmentBlockGrade::Blocked ||
               value == AssessmentBlockGrade::Fatal;
    }

    SampleAssessmentChannel::SampleAssessmentChannel(size_t capacity)
        : m_capacity(std::max<size_t>(capacity, 1))
    {
    }

    bool SampleAssessmentChannel::TryPublish(SampleAssessmentEvent event)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed)
        {
            return false;
        }
        if (m_events.size() >= m_capacity)
        {
            ++m_droppedEventCount;
            return false;
        }
        m_events.push_back(std::move(event));
        return true;
    }

    bool SampleAssessmentChannel::MarkAction(AssessmentAction action)
    {
        if (!action.IsValid())
        {
            return false;
        }
        return TryPublish(std::move(action));
    }

    bool SampleAssessmentChannel::MarkAction(AssessmentCode code,
                                             std::string description)
    {
        return MarkAction(AssessmentAction{std::move(code),
                                           std::move(description)});
    }

    bool SampleAssessmentChannel::MarkCheckpoint(AssessmentCheckpoint checkpoint)
    {
        if (!checkpoint.IsValid())
        {
            return false;
        }
        return TryPublish(std::move(checkpoint));
    }

    bool SampleAssessmentChannel::MarkCheckpoint(AssessmentCode code,
                                                 std::string description)
    {
        return MarkCheckpoint(AssessmentCheckpoint{std::move(code),
                                                    std::move(description)});
    }

    bool SampleAssessmentChannel::MarkCapabilityObservation(
        AssessmentCapabilityObservation observation)
    {
        if (!observation.IsValid())
        {
            return false;
        }
        return TryPublish(std::move(observation));
    }

    void SampleAssessmentChannel::Close()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_closed = true;
    }

    bool SampleAssessmentChannel::IsClosed() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_closed;
    }

    std::vector<SampleAssessmentEvent> SampleAssessmentChannel::Drain()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<SampleAssessmentEvent> events;
        events.reserve(m_events.size());
        std::move(m_events.begin(), m_events.end(), std::back_inserter(events));
        m_events.clear();
        return events;
    }

    size_t SampleAssessmentChannel::GetCapacity() const noexcept
    {
        return m_capacity;
    }

    size_t SampleAssessmentChannel::GetPendingEventCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_events.size();
    }

    uint64 SampleAssessmentChannel::GetDroppedEventCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_droppedEventCount;
    }

    const char* GetBaselineComparisonStatusCode(
        BaselineComparisonStatus value) noexcept
    {
        switch (value)
        {
            case BaselineComparisonStatus::IncompatibleFingerprint:
                return "INCOMPATIBLE_FINGERPRINT";
            case BaselineComparisonStatus::CompatibleNoThresholds:
                return "COMPATIBLE_NO_THRESHOLDS";
            default: return "INVALID";
        }
    }

    bool BaselineComparisonResult::IsComparable() const noexcept
    {
        return status == BaselineComparisonStatus::CompatibleNoThresholds;
    }

    FrameworkAssessmentSession::FrameworkAssessmentSession(
        SampleAssessmentContract contract,
        AssessmentMetadata metadata,
        size_t channelCapacity)
        : m_contract(std::move(contract))
        , m_metadata(std::move(metadata))
        , m_channel(channelCapacity)
    {
    }

    bool FrameworkAssessmentSession::RecordMetadata(std::string key,
                                                    std::string value)
    {
        if (key.empty())
        {
            return false;
        }
        return Publish(AssessmentMetadataEntry{std::move(key), std::move(value)});
    }

    bool FrameworkAssessmentSession::RecordAction(AssessmentAction action)
    {
        if (!action.IsValid())
        {
            return false;
        }
        return Publish(std::move(action));
    }

    bool FrameworkAssessmentSession::RecordCheckpoint(
        AssessmentCheckpoint checkpoint)
    {
        if (!checkpoint.IsValid())
        {
            return false;
        }
        return Publish(std::move(checkpoint));
    }

    bool FrameworkAssessmentSession::RecordSnapshot(AssessmentSnapshot snapshot)
    {
        if (!snapshot.IsValid())
        {
            return false;
        }
        return Publish(std::move(snapshot));
    }

    bool FrameworkAssessmentSession::RecordMetric(AssessmentCheckpoint checkpoint,
                                                   AssessmentMetricValue metric)
    {
        AssessmentSnapshot snapshot;
        snapshot.checkpoint = std::move(checkpoint);
        snapshot.metrics.push_back(std::move(metric));
        return RecordSnapshot(std::move(snapshot));
    }

    bool FrameworkAssessmentSession::RecordCapabilityObservation(
        AssessmentCapabilityObservation observation)
    {
        if (!observation.IsValid())
        {
            return false;
        }
        return Publish(std::move(observation));
    }

    bool FrameworkAssessmentSession::RecordInvariantObservation(
        AssessmentInvariantObservation observation)
    {
        if (!observation.IsValid())
        {
            return false;
        }
        return Publish(std::move(observation));
    }

    bool FrameworkAssessmentSession::RecordFinding(Finding finding)
    {
        if (!finding.IsValid())
        {
            return false;
        }
        return Publish(std::move(finding));
    }

    bool FrameworkAssessmentSession::BindVerifiedAssetSet(std::string assetSet)
    {
        if (assetSet.empty())
        {
            return false;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_finalReport.has_value())
        {
            return false;
        }
        if (m_metadata.fingerprint.assetSet.empty())
        {
            m_metadata.fingerprint.assetSet = std::move(assetSet);
            return true;
        }
        return m_metadata.fingerprint.assetSet == assetSet;
    }

    SampleAssessmentChannel& FrameworkAssessmentSession::GetChannel() noexcept
    {
        return m_channel;
    }

    const SampleAssessmentChannel&
    FrameworkAssessmentSession::GetChannel() const noexcept
    {
        return m_channel;
    }

    SampleAssessmentReport FrameworkAssessmentSession::Finalize()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_finalReport.has_value())
        {
            return *m_finalReport;
        }

        m_channel.Close();

        SampleAssessmentReport report;
        report.contract = m_contract;
        report.metadata = m_metadata;
        SortContract(report.contract);
        if (!report.contract.IsValid())
        {
            report.findings.push_back(CreateInvalidContractFinding());
        }

        for (SampleAssessmentEvent& event : m_channel.Drain())
        {
            std::visit(
                [&report](auto&& payload)
                {
                    using PayloadType = std::decay_t<decltype(payload)>;
                    if constexpr (std::is_same_v<PayloadType,
                                                 AssessmentMetadataEntry>)
                    {
                        if (payload.key.empty())
                        {
                            return;
                        }
                        auto existing = report.metadata.values.find(payload.key);
                        if (existing == report.metadata.values.end() ||
                            payload.value < existing->second)
                        {
                            report.metadata.values[payload.key] =
                                std::move(payload.value);
                        }
                    }
                    else if constexpr (std::is_same_v<PayloadType,
                                                      AssessmentAction>)
                    {
                        report.actions.push_back(std::move(payload));
                    }
                    else if constexpr (std::is_same_v<PayloadType,
                                                      AssessmentCheckpoint>)
                    {
                        report.checkpoints.push_back(std::move(payload));
                    }
                    else if constexpr (std::is_same_v<PayloadType,
                                                      AssessmentSnapshot>)
                    {
                        report.snapshots.push_back(std::move(payload));
                    }
                    else if constexpr (std::is_same_v<
                                           PayloadType,
                                           AssessmentCapabilityObservation>)
                    {
                        report.capabilityObservations.push_back(
                            std::move(payload));
                    }
                    else if constexpr (std::is_same_v<
                                           PayloadType,
                                           AssessmentInvariantObservation>)
                    {
                        report.invariantObservations.push_back(
                            std::move(payload));
                    }
                    else if constexpr (std::is_same_v<PayloadType, Finding>)
                    {
                        report.findings.push_back(std::move(payload));
                    }
                },
                std::move(event));
        }

        report.droppedEventCount = m_channel.GetDroppedEventCount();
        if (report.droppedEventCount > 0)
        {
            report.findings.push_back(CreateDroppedEventFinding(
                report.droppedEventCount));
        }
        if (report.contract.IsValid())
        {
            AppendContractCoverageFindings(report);
        }

        report = MakeDeterministicReport(report);
        report.blockGrade = GetBlockGrade(report.findings);
        m_finalReport = report;
        return report;
    }

    std::optional<SampleAssessmentReport>
    FrameworkAssessmentSession::GetFinalReport() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_finalReport;
    }

    BaselineComparisonResult FrameworkAssessmentSession::CompareBaseline(
        const AssessmentFingerprint& baselineFingerprint) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_metadata.fingerprint.IsComplete() ||
            !baselineFingerprint.IsComplete() ||
            !HasCompatibleV2Fingerprint(m_metadata.fingerprint,
                                        baselineFingerprint))
        {
            return {BaselineComparisonStatus::IncompatibleFingerprint,
                    "Baseline comparison requires a complete schema-v2 fingerprint match."};
        }
        return {BaselineComparisonStatus::CompatibleNoThresholds,
                "Schema-v2 fingerprint matches; no baseline thresholds are defined."};
    }

    uint64 FrameworkAssessmentSession::GetDroppedEventCount() const
    {
        return m_channel.GetDroppedEventCount();
    }

    bool FrameworkAssessmentSession::Publish(SampleAssessmentEvent event)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_finalReport.has_value())
        {
            return false;
        }
        return m_channel.TryPublish(std::move(event));
    }

    std::string FrameworkAssessmentJsonWriter::ToJson(
        const SampleAssessmentReport& report)
    {
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        static_cast<void>(Write(stream, report));
        return stream.str();
    }

    bool FrameworkAssessmentJsonWriter::Write(
        std::ostream& stream,
        const SampleAssessmentReport& report)
    {
        stream.imbue(std::locale::classic());
        const SampleAssessmentReport deterministicReport =
            MakeDeterministicReport(report);
        stream << "{\"schema\":";
        WriteJsonString(stream, SchemaName);
        stream << ",\"schemaVersion\":" << SchemaVersion;
        stream << ",\"contract\":";
        WriteContract(stream, deterministicReport.contract);
        stream << ",\"metadata\":{\"fingerprint\":";
        WriteFingerprint(stream, deterministicReport.metadata.fingerprint);
        stream << ",\"values\":{";
        bool needsSeparator = false;
        for (const auto& [key, value] : deterministicReport.metadata.values)
        {
            if (needsSeparator)
            {
                stream.put(',');
            }
            WriteJsonString(stream, key);
            stream.put(':');
            WriteJsonString(stream, value);
            needsSeparator = true;
        }
        stream << "}}";

        stream << ",\"actions\":[";
        for (size_t index = 0;
             index < deterministicReport.actions.size();
             ++index)
        {
            if (index > 0)
            {
                stream.put(',');
            }
            WriteAction(stream, deterministicReport.actions[index]);
        }
        stream.put(']');

        stream << ",\"checkpoints\":[";
        for (size_t index = 0;
             index < deterministicReport.checkpoints.size();
             ++index)
        {
            if (index > 0)
            {
                stream.put(',');
            }
            WriteCheckpoint(stream, deterministicReport.checkpoints[index]);
        }
        stream.put(']');

        stream << ",\"snapshots\":[";
        for (size_t index = 0;
             index < deterministicReport.snapshots.size();
             ++index)
        {
            if (index > 0)
            {
                stream.put(',');
            }
            const AssessmentSnapshot& snapshot = deterministicReport.snapshots[index];
            stream << "{\"checkpoint\":";
            WriteCheckpoint(stream, snapshot.checkpoint);
            stream << ",\"metrics\":[";
            for (size_t metricIndex = 0;
                 metricIndex < snapshot.metrics.size();
                 ++metricIndex)
            {
                if (metricIndex > 0)
                {
                    stream.put(',');
                }
                WriteMetricValue(stream, snapshot.metrics[metricIndex]);
            }
            stream << "]}";
        }
        stream.put(']');

        stream << ",\"capabilityObservations\":[";
        for (size_t index = 0;
             index < deterministicReport.capabilityObservations.size();
             ++index)
        {
            if (index > 0)
            {
                stream.put(',');
            }
            WriteCapabilityObservation(
                stream, deterministicReport.capabilityObservations[index]);
        }
        stream.put(']');

        stream << ",\"invariantObservations\":[";
        for (size_t index = 0;
             index < deterministicReport.invariantObservations.size();
             ++index)
        {
            if (index > 0)
            {
                stream.put(',');
            }
            WriteInvariantObservation(
                stream, deterministicReport.invariantObservations[index]);
        }
        stream.put(']');

        stream << ",\"findings\":[";
        for (size_t index = 0;
             index < deterministicReport.findings.size();
             ++index)
        {
            if (index > 0)
            {
                stream.put(',');
            }
            WriteFinding(stream, deterministicReport.findings[index]);
        }
        stream.put(']');

        stream << ",\"blockGrade\":";
        WriteJsonString(stream,
                        GetAssessmentBlockGradeCode(deterministicReport.blockGrade));
        stream << ",\"blocked\":"
               << (IsBlockingGrade(deterministicReport.blockGrade) ? "true" : "false");
        stream << ",\"droppedEventCount\":"
               << deterministicReport.droppedEventCount
               << '}';
        return stream.good();
    }

    bool FrameworkAssessmentJsonWriter::WriteFile(
        const std::filesystem::path& path,
        const SampleAssessmentReport& report,
        std::string* outError)
    {
        std::ofstream stream(path, std::ios::out | std::ios::trunc);
        if (!stream)
        {
            if (outError)
            {
                *outError = "Failed to open framework assessment report: " +
                            path.string();
            }
            return false;
        }
        if (!Write(stream, report))
        {
            if (outError)
            {
                *outError = "Failed to write framework assessment report: " +
                            path.string();
            }
            return false;
        }
        return true;
    }
} // namespace RVX
