#pragma once

/**
 * @file FrameworkAssessment.h
 * @brief Stable, backend-neutral assessment records for sample qualification.
 *
 * Assessment codes are machine-facing identifiers.  They intentionally use
 * upper-case dot-separated segments, for example
 * SCENE.HIERARCHY.AUTHORITY_MISMATCH.  Display text and diagnostic detail are
 * kept separate so report consumers never need to infer meaning from prose.
 */

#include "Core/Diagnostics/DiagnosticValue.h"
#include "Core/Types.h"

#include <cstddef>
#include <deque>
#include <filesystem>
#include <iosfwd>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace RVX
{
    // =========================================================================
    // Stable Codes and Diagnostic Values
    // =========================================================================

    /** @brief Stable upper-case dot-separated identifier used by assessments. */
    class AssessmentCode
    {
    public:
        AssessmentCode() = default;
        explicit AssessmentCode(std::string value);

        [[nodiscard]] const std::string& GetValue() const noexcept;
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] static bool IsStable(std::string_view value) noexcept;

        friend bool operator==(const AssessmentCode& lhs,
                               const AssessmentCode& rhs) noexcept = default;
        friend bool operator<(const AssessmentCode& lhs,
                              const AssessmentCode& rhs) noexcept;

    private:
        std::string m_value;
    };

    /** @brief One named lifecycle point in a sample assessment. */
    struct AssessmentCheckpoint
    {
        AssessmentCode code;
        std::string description;

        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief One user- or runner-driven action covered by an assessment. */
    struct AssessmentAction
    {
        AssessmentCode code;
        std::string description;

        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief One condition that an assessed sample must preserve. */
    struct AssessmentInvariant
    {
        AssessmentCode code;
        std::string description;

        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief One scalar measurement described by a stable code and unit. */
    struct AssessmentMetric
    {
        AssessmentCode code;
        std::string unit;
        std::string description;

        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief One capability whose availability can affect assessment coverage. */
    struct AssessmentCapability
    {
        AssessmentCode code;
        std::string description;
        bool gating = false;
        std::string blockingReason;

        [[nodiscard]] bool IsValid() const noexcept;
    };

    /**
     * @brief Value-semantic evidence of one capability state at a checkpoint.
     *
     * A false value means the capability was evaluated and is unavailable. An
     * unavailable diagnostic means the capability itself could not be evaluated;
     * both cases are distinct from a missing observation.
     */
    struct AssessmentCapabilityObservation
    {
        AssessmentCapability capability;
        AssessmentCheckpoint checkpoint;
        DiagnosticValue<bool> value;
        std::string reason;

        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Scalar telemetry types that can be represented without coercion. */
    using AssessmentScalar =
        std::variant<bool, int64, uint64, float64, std::string>;

    /** @brief One typed scalar metric value captured at a checkpoint. */
    struct AssessmentMetricValue
    {
        AssessmentMetric metric;
        DiagnosticValue<AssessmentScalar> value;

        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief A snapshot of generic typed metrics captured at one checkpoint. */
    struct AssessmentSnapshot
    {
        AssessmentCheckpoint checkpoint;
        std::vector<AssessmentMetricValue> metrics;

        [[nodiscard]] bool IsValid() const noexcept;
    };

    /**
     * @brief Explicit positive evidence that a contract invariant was evaluated.
     *
     * Findings already provide negative invariant evidence.  A successful
     * invariant must still be recorded explicitly: absence of a finding is not
     * evidence that an invariant was evaluated.
     */
    struct AssessmentInvariantObservation
    {
        AssessmentInvariant invariant;
        AssessmentCheckpoint checkpoint;

        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Fixed lifecycle checkpoints used by every framework assessment. */
    namespace AssessmentCheckpoints
    {
        inline const AssessmentCheckpoint EngineBaseline{
            AssessmentCode("ENGINE.BASELINE"), "Engine baseline established."};
        inline const AssessmentCheckpoint ScenarioSetup{
            AssessmentCode("SCENARIO.SETUP"), "Scenario setup completed."};
        inline const AssessmentCheckpoint ActionRequested{
            AssessmentCode("ACTION.REQUESTED"), "Qualified action requested."};
        inline const AssessmentCheckpoint ActionApplied{
            AssessmentCode("ACTION.APPLIED"), "Qualified action applied."};
        inline const AssessmentCheckpoint ScenarioStable{
            AssessmentCode("SCENARIO.STABLE"), "Scenario reached a stable state."};
        inline const AssessmentCheckpoint TeardownBefore{
            AssessmentCode("TEARDOWN.BEFORE"), "Teardown started."};
        inline const AssessmentCheckpoint TeardownSceneComplete{
            AssessmentCode("TEARDOWN.SCENE_COMPLETE"), "Scene teardown completed."};
        inline const AssessmentCheckpoint TeardownRenderDrained{
            AssessmentCode("TEARDOWN.RENDER_DRAINED"), "Render work drained."};
        inline const AssessmentCheckpoint EngineShutdownComplete{
            AssessmentCode("ENGINE.SHUTDOWN_COMPLETE"),
            "Engine shutdown completed."};
    } // namespace AssessmentCheckpoints

    // =========================================================================
    // Findings and Contract
    // =========================================================================

    enum class FindingClass : uint8
    {
        ContractViolation = 0,
        LifetimeLeak,
        StaleIdentity,
        RevisionDiscontinuity,
        HiddenFallback,
        BackendDivergence,
        Nondeterminism,
        PerformanceRegression,
        CapabilityGap,
        InstrumentationGap
    };

    enum class FindingSeverity : uint8
    {
        Info = 0,
        Warning,
        Error,
        Fatal
    };

    enum class FindingConfidence : uint8
    {
        Suspected = 0,
        StrongEvidence,
        Confirmed
    };

    [[nodiscard]] const char* GetFindingClassCode(FindingClass value) noexcept;
    [[nodiscard]] const char* GetFindingSeverityCode(
        FindingSeverity value) noexcept;
    [[nodiscard]] const char* GetFindingConfidenceCode(
        FindingConfidence value) noexcept;

    /** @brief A self-contained machine-classified assessment result. */
    struct Finding
    {
        AssessmentCode code;
        AssessmentCode subsystemCode;
        AssessmentCode invariantCode;
        AssessmentCheckpoint checkpoint;
        FindingClass classification = FindingClass::ContractViolation;
        FindingSeverity severity = FindingSeverity::Info;
        FindingConfidence confidence = FindingConfidence::Suspected;
        std::string summary;
        std::string detail;
        std::string expected;
        std::string observed;
        DiagnosticValue<uint64> frameBegin;
        DiagnosticValue<uint64> frameEnd;
        DiagnosticValue<uint64> sceneRevision;
        DiagnosticValue<uint64> resourceGeneration;
        DiagnosticValue<uint64> requestId;
        DiagnosticValue<uint64> completionToken;
        std::string traceCorrelationId;
        std::vector<std::filesystem::path> artifactPaths;
        bool gating = false;
        std::string blockingReason;
        bool capabilityBlocking = false;

        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] bool IsBlocking() const noexcept;
    };

    /**
     * @brief Fixed fingerprint used to establish whether a baseline is comparable.
     *
     * Every field must be supplied; comparing a partial environment is forbidden
     * because it could attribute a platform, asset, or configuration change to a
     * sample regression.
     */
    struct AssessmentFingerprint
    {
        std::string sampleId;
        std::string sampleRevision;
        std::string engineBuild;
        std::string backend;
        std::string device;
        std::string renderConfiguration;
        std::string assetSet;
        std::string platform;

        [[nodiscard]] bool IsComplete() const noexcept;

        friend bool operator==(const AssessmentFingerprint& lhs,
                               const AssessmentFingerprint& rhs) noexcept = default;
    };

    /** @brief Fixed metadata and caller-defined, scalar report annotations. */
    struct AssessmentMetadata
    {
        AssessmentFingerprint fingerprint;
        std::map<std::string, std::string, std::less<>> values;
    };

    /** @brief Static contract that describes the coverage of one sample. */
    struct SampleAssessmentContract
    {
        AssessmentCode code;
        std::string revision;
        std::vector<AssessmentCheckpoint> checkpoints;
        std::vector<AssessmentAction> actions;
        std::vector<AssessmentInvariant> invariants;
        std::vector<AssessmentMetric> metrics;
        std::vector<AssessmentCapability> capabilities;

        [[nodiscard]] bool IsValid() const noexcept;
    };

    /**
     * @brief Build the minimum host-owned assessment contract used by samples
     *        that have not yet declared scenario-specific probes.
     *
     * The contract still requires the fixed lifecycle checkpoints and the
     * cross-module lifetime/revision metrics collected by SampleRunner. New
     * diagnostic samples should extend or replace it with their own stable
     * actions, invariants, metrics, and capabilities.
     */
    [[nodiscard]] SampleAssessmentContract
        MakeDefaultSampleAssessmentContract();

    enum class AssessmentBlockGrade : uint8
    {
        Pass = 0,
        Advisory,
        Blocked,
        Fatal
    };

    [[nodiscard]] const char* GetAssessmentBlockGradeCode(
        AssessmentBlockGrade value) noexcept;
    [[nodiscard]] bool IsBlockingGrade(AssessmentBlockGrade value) noexcept;

    // =========================================================================
    // Thread-safe Channel and Session
    // =========================================================================

    struct AssessmentMetadataEntry
    {
        std::string key;
        std::string value;
    };

    /** @brief A payload accepted by the bounded assessment channel. */
    using SampleAssessmentEvent = std::variant<AssessmentMetadataEntry,
                                               AssessmentAction,
                                               AssessmentCheckpoint,
                                               AssessmentSnapshot,
                                               AssessmentCapabilityObservation,
                                               AssessmentInvariantObservation,
                                               Finding>;

    /**
     * @brief Mutex-protected bounded channel for concurrent assessment producers.
     *
     * Publishing is non-blocking with respect to capacity: TryPublish returns
     * false and increments GetDroppedEventCount() once the queue is full.
     */
    class SampleAssessmentChannel
    {
    public:
        static constexpr size_t DefaultCapacity = 1024;

        explicit SampleAssessmentChannel(size_t capacity = DefaultCapacity);

        SampleAssessmentChannel(const SampleAssessmentChannel&) = delete;
        SampleAssessmentChannel& operator=(const SampleAssessmentChannel&) = delete;

        [[nodiscard]] bool TryPublish(SampleAssessmentEvent event);
        [[nodiscard]] bool MarkAction(AssessmentAction action);
        [[nodiscard]] bool MarkAction(AssessmentCode code,
                                      std::string description);
        [[nodiscard]] bool MarkCheckpoint(AssessmentCheckpoint checkpoint);
        [[nodiscard]] bool MarkCheckpoint(AssessmentCode code,
                                          std::string description);
        [[nodiscard]] bool MarkCapabilityObservation(
            AssessmentCapabilityObservation observation);
        void Close();
        [[nodiscard]] bool IsClosed() const;
        [[nodiscard]] std::vector<SampleAssessmentEvent> Drain();
        [[nodiscard]] size_t GetCapacity() const noexcept;
        [[nodiscard]] size_t GetPendingEventCount() const;
        [[nodiscard]] uint64 GetDroppedEventCount() const;

    private:
        size_t m_capacity = DefaultCapacity;
        std::deque<SampleAssessmentEvent> m_events;
        uint64 m_droppedEventCount = 0;
        bool m_closed = false;
        mutable std::mutex m_mutex;
    };

    enum class BaselineComparisonStatus : uint8
    {
        IncompatibleFingerprint = 0,
        CompatibleNoThresholds
    };

    [[nodiscard]] const char* GetBaselineComparisonStatusCode(
        BaselineComparisonStatus value) noexcept;

    /** @brief Explicit baseline-comparison outcome for this contract revision. */
    struct BaselineComparisonResult
    {
        BaselineComparisonStatus status =
            BaselineComparisonStatus::IncompatibleFingerprint;
        std::string reason;

        [[nodiscard]] bool IsComparable() const noexcept;
    };

    /** @brief Immutable assessment report ready for deterministic JSON writing. */
    struct SampleAssessmentReport
    {
        SampleAssessmentContract contract;
        AssessmentMetadata metadata;
        std::vector<AssessmentAction> actions;
        std::vector<AssessmentCheckpoint> checkpoints;
        std::vector<AssessmentSnapshot> snapshots;
        std::vector<AssessmentCapabilityObservation> capabilityObservations;
        std::vector<AssessmentInvariantObservation> invariantObservations;
        std::vector<Finding> findings;
        AssessmentBlockGrade blockGrade = AssessmentBlockGrade::Pass;
        uint64 droppedEventCount = 0;
    };

    /**
     * @brief Collects assessment events and emits a deterministic immutable report.
     *
     * Record methods may be called concurrently.  Finalize() closes the session,
     * drains all accepted events, and sorts all report collections by stable
     * value.  Duplicate metadata keys resolve to the lexicographically smallest
     * value so concurrent publication cannot make report bytes depend on timing.
     */
    class FrameworkAssessmentSession
    {
    public:
        explicit FrameworkAssessmentSession(
            SampleAssessmentContract contract,
            AssessmentMetadata metadata = {},
            size_t channelCapacity = SampleAssessmentChannel::DefaultCapacity);

        FrameworkAssessmentSession(const FrameworkAssessmentSession&) = delete;
        FrameworkAssessmentSession& operator=(const FrameworkAssessmentSession&) = delete;

        [[nodiscard]] bool RecordMetadata(std::string key, std::string value);
        [[nodiscard]] bool RecordAction(AssessmentAction action);
        [[nodiscard]] bool RecordCheckpoint(AssessmentCheckpoint checkpoint);
        [[nodiscard]] bool RecordSnapshot(AssessmentSnapshot snapshot);
        [[nodiscard]] bool RecordMetric(AssessmentCheckpoint checkpoint,
                                        AssessmentMetricValue metric);
        [[nodiscard]] bool RecordCapabilityObservation(
            AssessmentCapabilityObservation observation);
        [[nodiscard]] bool RecordInvariantObservation(
            AssessmentInvariantObservation observation);
        [[nodiscard]] bool RecordFinding(Finding finding);

        /**
         * @brief Bind the verified portable asset-set fingerprint once.
         *
         * Before finalization the fingerprint may move only from empty to a
         * non-empty value, or be repeated byte-for-byte. This keeps late model
         * receipts race-safe without allowing a caller to rewrite identity.
         */
        [[nodiscard]] bool BindVerifiedAssetSet(std::string assetSet);

        [[nodiscard]] SampleAssessmentChannel& GetChannel() noexcept;
        [[nodiscard]] const SampleAssessmentChannel& GetChannel() const noexcept;
        [[nodiscard]] SampleAssessmentReport Finalize();
        [[nodiscard]] std::optional<SampleAssessmentReport> GetFinalReport() const;
        [[nodiscard]] BaselineComparisonResult CompareBaseline(
            const AssessmentFingerprint& baselineFingerprint) const;
        [[nodiscard]] uint64 GetDroppedEventCount() const;

    private:
        [[nodiscard]] bool Publish(SampleAssessmentEvent event);

        SampleAssessmentContract m_contract;
        AssessmentMetadata m_metadata;
        SampleAssessmentChannel m_channel;
        mutable std::mutex m_mutex;
        std::optional<SampleAssessmentReport> m_finalReport;
    };

    // =========================================================================
    // JSON Writer
    // =========================================================================

    /** @brief Deterministic schema-v2 JSON writer for sample assessment reports. */
    class FrameworkAssessmentJsonWriter
    {
    public:
        static constexpr std::string_view SchemaName =
            "RVX.FrameworkAssessmentReport";
        static constexpr uint32 SchemaVersion = 2;

        [[nodiscard]] static std::string ToJson(
            const SampleAssessmentReport& report);
        [[nodiscard]] static bool Write(std::ostream& stream,
                                        const SampleAssessmentReport& report);
        [[nodiscard]] static bool WriteFile(
            const std::filesystem::path& path,
            const SampleAssessmentReport& report,
            std::string* outError = nullptr);
    };
} // namespace RVX
