#pragma once

#include "Core/Types.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <typeindex>
#include <vector>

namespace RVX::ECS
{
    class Registry;

    /** @brief Fixed frame-barrier phases used by the runtime ECS scheduler. */
    enum class ProcessorPhase : uint8
    {
        BeginSimulation = 0,
        Gameplay,
        BeforeFixedStep,
        FixedAnimation,
        RootMotion,
        SceneToPhysics,
        PhysicsSimulation,
        PhysicsToScene,
        FixedTransformResolve,
        EndFixedStep,
        PostSimulation,
        PrePresentation,
        Transform,
        Bounds,
        Spatial,
        Feature,
        RenderExtraction,
        EndFrameCleanup,

        // Compatibility aliases for the initial P1 scheduler API.
        PreUpdate = BeginSimulation,
        Update = Gameplay,
        PostUpdate = PostSimulation,
    };

    /** @brief Selects the clock on which a processor may execute. */
    enum class ProcessorStepMode : uint8
    {
        Variable = 0,
        Fixed,
    };

    /** @brief Distinguishes fragment from external-subsystem access diagnostics. */
    enum class ProcessorAccessDomain : uint8
    {
        Fragment = 0,
        Resource,
    };

    /** @brief Conservative access conflict reported before future parallel execution. */
    enum class ProcessorConflictKind : uint8
    {
        ReadWrite = 0,
        WriteWrite,
    };

    /** @brief Value-only diagnostic for two processors that cannot safely run in parallel. */
    struct ProcessorConflict
    {
        std::string firstProcessor;
        std::string secondProcessor;
        std::string accessName;
        ProcessorAccessDomain domain = ProcessorAccessDomain::Fragment;
        ProcessorConflictKind kind = ProcessorConflictKind::ReadWrite;
    };

    /**
     * @brief Declared data access for scheduler validation and future parallelization.
     *
     * The existing reads/writes lists denote Fragment types. Resource lists use
     * neutral type keys so scheduler metadata remains independent of Registry
     * and any subsystem-specific resource implementation.
     */
    struct ProcessorAccess
    {
        std::vector<std::type_index> reads;
        std::vector<std::type_index> writes;
        std::vector<std::type_index> resourceReads;
        std::vector<std::type_index> resourceWrites;
    };

    /** @brief Per-invocation context for processors that require step metadata. */
    struct ProcessorExecutionContext
    {
        ProcessorExecutionContext(Registry& registry,
            ProcessorPhase phase,
            std::string_view group,
            ProcessorStepMode stepMode,
            float64 deltaSeconds,
            uint64 frameSequence,
            uint64 fixedStepSequence)
            : registry(registry)
            , phase(phase)
            , group(group)
            , stepMode(stepMode)
            , deltaSeconds(deltaSeconds)
            , frameSequence(frameSequence)
            , fixedStepSequence(fixedStepSequence)
        {
        }

        Registry& registry;
        ProcessorPhase phase;
        std::string_view group;
        ProcessorStepMode stepMode;
        float64 deltaSeconds = 0.0;
        uint64 frameSequence = 0;
        uint64 fixedStepSequence = 0;

        /** @brief Fail the current scheduler step without using an exception. */
        void ReportFailure(std::string_view reason)
        {
            if (m_hasReportedFailure)
            {
                return;
            }

            m_hasReportedFailure = true;
            m_failureReason = reason;
        }

        [[nodiscard]] bool HasReportedFailure() const { return m_hasReportedFailure; }

    private:
        [[nodiscard]] const std::string& GetFailureReason() const { return m_failureReason; }

        std::string m_failureReason;
        bool m_hasReportedFailure = false;

        friend class ProcessorScheduler;
    };

    /** @brief Immutable diagnostic captured when a processor fails its current step. */
    struct ProcessorExecutionFailure
    {
        std::string processorName;
        ProcessorPhase phase = ProcessorPhase::Update;
        std::string group;
        ProcessorStepMode stepMode = ProcessorStepMode::Variable;
        float64 deltaSeconds = 0.0;
        uint64 frameSequence = 0;
        uint64 fixedStepSequence = 0;
        std::string reason;
    };

    struct ProcessorDescriptor
    {
        std::string name;
        ProcessorPhase phase = ProcessorPhase::Update;
        /** Named sub-group within a phase; empty selects that phase's default group. */
        std::string group;
        /** Stable sub-group ordering before the group name is used as a tie breaker. */
        uint32 groupOrder = 0;
        uint32 order = 0;
        std::vector<std::string> before;
        std::vector<std::string> after;
        ProcessorAccess access;
        /** P1 remains serial; this is only a future parallelization declaration. */
        bool allowsParallel = false;
        ProcessorStepMode stepMode = ProcessorStepMode::Variable;
        /** Compatibility callback for processors that do not need clock metadata. */
        std::function<void(Registry&)> run;
        /** Preferred callback for processors that consume the current execution clock. */
        std::function<void(ProcessorExecutionContext&)> runWithContext;
    };

    /**
     * @brief Deterministic serial processor scheduler.
     *
     * Compile validates dependency targets and cycles. Access conflicts are
     * diagnostic only in P1 because execution remains serial.
     */
    class ProcessorScheduler
    {
    public:
        bool Register(ProcessorDescriptor descriptor);
        void Clear();

        /** Compile deterministic phase/group/order/dependency execution order. */
        [[nodiscard]] bool Compile();

        /** Compatibility variable-step entry point. */
        void Run(Registry& registry);
        [[nodiscard]] bool RunVariable(Registry& registry, float64 deltaSeconds = 0.0, uint64 frameSequence = 0);
        [[nodiscard]] bool RunFixed(Registry& registry, float64 deltaSeconds, uint64 fixedStepSequence);

        [[nodiscard]] uint32 GetProcessorCount() const
        {
            return static_cast<uint32>(m_processors.size());
        }

        [[nodiscard]] bool IsCompiled() const { return m_compileSucceeded && !m_needsCompile; }
        [[nodiscard]] const std::string& GetLastCompileError() const { return m_lastCompileError; }
        [[nodiscard]] const std::optional<ProcessorExecutionFailure>& GetLastExecutionFailure() const
        {
            return m_lastExecutionFailure;
        }
        [[nodiscard]] const std::vector<ProcessorConflict>& GetConflictDiagnostics() const { return m_conflicts; }
        [[nodiscard]] const std::vector<std::string>& GetCompiledProcessorNames() const { return m_compiledProcessorNames; }

    private:
        struct RegisteredProcessor
        {
            ProcessorDescriptor descriptor;
            uint64 registrationSequence = 0;
        };

        [[nodiscard]] bool IsScheduledBefore(uint32 lhsIndex, uint32 rhsIndex) const;
        [[nodiscard]] std::string_view GetEffectiveGroup(const RegisteredProcessor& processor) const;
        [[nodiscard]] bool RunStep(Registry& registry,
            ProcessorStepMode stepMode,
            float64 deltaSeconds,
            uint64 frameSequence,
            uint64 fixedStepSequence);
        void BuildConflictDiagnostics();

        std::vector<RegisteredProcessor> m_processors;
        std::vector<uint32> m_compiledOrder;
        std::vector<std::string> m_compiledProcessorNames;
        std::vector<ProcessorConflict> m_conflicts;
        std::string m_lastCompileError;
        std::optional<ProcessorExecutionFailure> m_lastExecutionFailure;
        uint64 m_nextRegistrationSequence = 0;
        bool m_needsCompile = false;
        bool m_compileSucceeded = false;
    };
} // namespace RVX::ECS
