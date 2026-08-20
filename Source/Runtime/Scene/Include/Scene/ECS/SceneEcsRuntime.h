#pragma once

/**
 * @file SceneEcsRuntime.h
 * @brief Pure runtime owner for Scene-domain ECS fragments and deferred cleanup.
 */

#include "ECS/Commands.h"
#include "ECS/ProcessorScheduler.h"
#include "ECS/Registry.h"
#include "Scene/ECS/AnimationFragments.h"
#include "Scene/ECS/FrozenSceneSnapshot.h"
#include "Scene/ECS/Fragments.h"
#include "Scene/ECS/SceneFeatureSnapshotStore.h"
#include "Scene/ECS/SceneSkinningSnapshotStore.h"
#include "Scene/ECS/SceneSpatialIndex.h"
#include "Scene/ECS/TransformHierarchy.h"

#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace RVX::SceneECS
{
    class SceneEcsRuntime;
    class SceneSpawnTransaction;
}

namespace RVX::ResourceSceneAdapters
{
    struct EcsEnvironmentAdoptionBatch;
    struct EcsEnvironmentAdoptionOptions;
    struct EcsEnvironmentAdoptionReceipt;

    [[nodiscard]] EcsEnvironmentAdoptionReceipt AdoptEcsEnvironmentBatch(
        SceneECS::SceneEcsRuntime& runtime,
        const EcsEnvironmentAdoptionBatch& batch,
        ECS::SceneRuntimeId expectedSceneRuntimeId,
        const EcsEnvironmentAdoptionOptions& options);

    struct PreparedModelBatch;
    struct PreparedModelAdoptionOptions;
    struct PreparedModelAdoptionReceipt;

    [[nodiscard]] PreparedModelAdoptionReceipt AdoptPreparedModelBatch(
        SceneECS::SceneEcsRuntime& runtime,
        const PreparedModelBatch& batch,
        ECS::SceneRuntimeId expectedSceneRuntimeId,
        const PreparedModelAdoptionOptions& options);
} // namespace RVX::ResourceSceneAdapters

namespace RVX::SceneECS
{
    /** @brief Value parameters used to construct one baseline Scene ECS entity. */
    struct RuntimeEntityDesc
    {
        LocalTransform localTransform;
        Bounds bounds;
        Active active;
        Layer layer;
    };

    /** @brief Outcome of asking the runtime to begin deferred entity retirement. */
    enum class DestroyRequestResult : uint8
    {
        Accepted = 0,
        InvalidEntity,
        AlreadyPending,
        MissingLifecycleState,
        MutationRejected,
    };

    /** @brief Atomic result of requesting every currently Alive entity for destruction. */
    struct DestroyAllRequestResult
    {
        DestroyRequestResult result = DestroyRequestResult::MutationRejected;
        uint64 requestedEntityCount = 0;

        [[nodiscard]] bool IsAccepted() const noexcept
        {
            return result == DestroyRequestResult::Accepted;
        }
    };

    /**
     * @brief Scene-owned token for a sequential, owner-thread processor registration batch.
     *
     * A token identifies registrations rather than a vector position, so its
     * removal cannot truncate processors registered by another owner later.
     * Tokens are issued, closed, and retired only by their originating Scene.
     */
    class ProcessorRegistrationScope final
    {
    public:
        [[nodiscard]] bool IsValid() const noexcept
        {
            return m_sceneRuntimeId.IsValid() && m_scopeId != 0;
        }

    private:
        friend class SceneEcsRuntime;

        ProcessorRegistrationScope(ECS::SceneRuntimeId sceneRuntimeId, uint64 scopeId) noexcept
            : m_sceneRuntimeId(sceneRuntimeId)
            , m_scopeId(scopeId)
        {
        }

        ECS::SceneRuntimeId m_sceneRuntimeId{};
        uint64 m_scopeId = 0;
    };

    /** @brief Value-only cursor over runtime cleanup records. */
    struct CleanupRecordCursor
    {
        uint64 nextSequence = 1;
    };

    enum class CleanupRecordContinuity : uint8
    {
        Continuous = 0,
        Lost,
    };

    /** @brief Stable cleanup-record read suitable for independent consumers. */
    struct CleanupRecordRead
    {
        CleanupRecordContinuity continuity = CleanupRecordContinuity::Continuous;
        std::vector<CleanupRecord> records;
        uint64 nextSequence = 1;
    };

    /** @brief Owner-thread boundaries at which deferred structural work may play. */
    enum class SceneCommandBarrier : uint8
    {
        BeginSimulation = 0,
        BeforeFixedStep,
        EndFixedStep,
        PrePresentation,
    };

    inline constexpr uint32 RVX_SCENE_COMMAND_BARRIER_COUNT = 4;

    /** @brief Aggregate result of one submitted owner-thread command buffer. */
    enum class SceneCommandBufferStatus : uint8
    {
        Queued = 0,
        Applied,
        Rejected,
        Discarded,
    };

    namespace Detail
    {
        template<typename T>
        inline constexpr bool IsSceneAuthorityFragment =
            std::is_same_v<T, ParentRelation> ||
            std::is_same_v<T, EntityLifecycleState> ||
            std::is_same_v<T, LocalTransform> ||
            std::is_same_v<T, SimulationWorldTransform> ||
            std::is_same_v<T, PreviousSimulationWorldTransform> ||
            std::is_same_v<T, RenderWorldTransform> ||
            std::is_same_v<T, Bounds> ||
            std::is_same_v<T, Active> ||
            std::is_same_v<T, Layer>;

        struct SceneCommandReceiptState
        {
            [[nodiscard]] ECS::CommandStatus GetStatus() const
            {
                std::lock_guard lock(m_mutex);
                return status;
            }

            [[nodiscard]] ECS::CommandError GetError() const
            {
                std::lock_guard lock(m_mutex);
                return error;
            }

            void SetOutcome(ECS::CommandStatus newStatus, ECS::CommandError newError)
            {
                std::lock_guard lock(m_mutex);
                status = newStatus;
                error = newError;
            }

        private:
            mutable std::mutex m_mutex;
            ECS::CommandStatus status = ECS::CommandStatus::Queued;
            ECS::CommandError error = ECS::CommandError::None;
        };

        struct SceneEntityReceiptState
        {
            [[nodiscard]] std::optional<ECS::EntityHandle> GetEntity() const
            {
                std::lock_guard lock(m_mutex);
                return entity;
            }

            void SetEntity(ECS::EntityHandle newEntity)
            {
                std::lock_guard lock(m_mutex);
                entity = newEntity;
            }

            std::shared_ptr<SceneCommandReceiptState> command =
                std::make_shared<SceneCommandReceiptState>();

        private:
            mutable std::mutex m_mutex;
            std::optional<ECS::EntityHandle> entity;
        };

        struct SceneCommandBufferReceiptState
        {
            [[nodiscard]] SceneCommandBufferStatus GetStatus() const
            {
                std::lock_guard lock(m_mutex);
                return status;
            }

            [[nodiscard]] SceneCommandBarrier GetBarrier() const
            {
                std::lock_guard lock(m_mutex);
                return barrier;
            }

            [[nodiscard]] uint64 GetSubmissionSequence() const
            {
                std::lock_guard lock(m_mutex);
                return submissionSequence;
            }

            void SetQueued(SceneCommandBarrier newBarrier, uint64 newSubmissionSequence)
            {
                std::lock_guard lock(m_mutex);
                status = SceneCommandBufferStatus::Queued;
                barrier = newBarrier;
                submissionSequence = newSubmissionSequence;
            }

            void SetStatus(SceneCommandBufferStatus newStatus)
            {
                std::lock_guard lock(m_mutex);
                status = newStatus;
            }

        private:
            mutable std::mutex m_mutex;
            SceneCommandBufferStatus status = SceneCommandBufferStatus::Queued;
            SceneCommandBarrier barrier = SceneCommandBarrier::BeginSimulation;
            uint64 submissionSequence = 0;
        };

        struct SceneCommandSubmissionState;
    } // namespace Detail

    /**
     * @brief Opaque local identifier for one entity recorded in a Scene spawn transaction.
     *
     * This identity only selects a pending entity while the transaction is
     * being built. It never aliases an ECS handle and cannot expose a Registry
     * entity before the transaction commits.
     */
    class SceneSpawnEntityId final
    {
    public:
        constexpr SceneSpawnEntityId() = default;

        [[nodiscard]] constexpr bool IsValid() const
        {
            return m_index != RVX_INVALID_INDEX;
        }

        friend constexpr bool operator==(SceneSpawnEntityId, SceneSpawnEntityId) = default;

    private:
        friend class SceneSpawnTransaction;
        friend class SceneSpawnCommitResult;

        explicit constexpr SceneSpawnEntityId(uint32 index)
            : m_index(index)
        {
        }

        uint32 m_index = RVX_INVALID_INDEX;
    };

    /**
     * @brief Pure Scene-owned identity for one generation-qualified ECS entity.
     *
     * Unlike ECS::EntityRef this value contains no Registry pointer and exposes
     * no fragment or structural mutation API. A Scene runtime only produces it
     * for an alive entity it owns at the instant of lookup or spawn commit.
     */
    struct SceneEntityRef
    {
        ECS::SceneRuntimeId sceneRuntimeId;
        ECS::EntityHandle entity = ECS::EntityHandle::Invalid();

        [[nodiscard]] constexpr bool IsValid() const
        {
            return sceneRuntimeId.IsValid() && entity.IsValid();
        }

        friend constexpr bool operator==(const SceneEntityRef&, const SceneEntityRef&) = default;
    };

    /**
     * @brief Value-only outcome of one atomic Scene spawn transaction.
     *
     * Entity handle and SceneEntityRef mappings remain empty until the underlying
     * ECS transaction has committed. The contained SceneEntityRef values carry
     * the owning scene runtime and entity generation, but never expose a
     * Registry or fragment reference.
     */
    class SceneSpawnCommitResult final
    {
    public:
        [[nodiscard]] ECS::CommandStatus GetStatus() const { return m_status; }
        [[nodiscard]] ECS::CommandError GetError() const { return m_error; }
        [[nodiscard]] bool IsApplied() const { return m_status == ECS::CommandStatus::Applied; }
        [[nodiscard]] bool IsRejected() const { return m_status == ECS::CommandStatus::Rejected; }
        explicit operator bool() const { return IsApplied(); }

        [[nodiscard]] ECS::EntityHandle GetEntity(SceneSpawnEntityId entity) const
        {
            return IsApplied() && entity.IsValid() && entity.m_index < m_entities.size()
                ? m_entities[entity.m_index]
                : ECS::EntityHandle::Invalid();
        }

        [[nodiscard]] SceneEntityRef GetEntityRef(SceneSpawnEntityId entity) const
        {
            return IsApplied() && entity.IsValid() && entity.m_index < m_entityRefs.size()
                ? m_entityRefs[entity.m_index]
                : SceneEntityRef{};
        }

    private:
        friend class SceneSpawnTransaction;

        void SetRejected(ECS::CommandError error)
        {
            m_status = ECS::CommandStatus::Rejected;
            m_error = error;
            m_entities.clear();
            m_entityRefs.clear();
        }

        void SetApplied()
        {
            m_status = ECS::CommandStatus::Applied;
            m_error = ECS::CommandError::None;
        }

        ECS::CommandStatus m_status = ECS::CommandStatus::Rejected;
        ECS::CommandError m_error = ECS::CommandError::InvalidTarget;
        std::vector<ECS::EntityHandle> m_entities;
        std::vector<SceneEntityRef> m_entityRefs;
    };

    /** @brief Value receipt for one Scene-owned structural command. */
    class SceneCommandReceipt
    {
    public:
        SceneCommandReceipt() = default;

        [[nodiscard]] ECS::CommandStatus GetStatus() const
        {
            return m_state != nullptr ? m_state->GetStatus() : ECS::CommandStatus::Rejected;
        }

        [[nodiscard]] ECS::CommandError GetError() const
        {
            return m_state != nullptr ? m_state->GetError() : ECS::CommandError::InvalidTarget;
        }

        [[nodiscard]] bool IsQueued() const { return GetStatus() == ECS::CommandStatus::Queued; }
        [[nodiscard]] bool IsApplied() const { return GetStatus() == ECS::CommandStatus::Applied; }
        [[nodiscard]] bool IsRejected() const { return GetStatus() == ECS::CommandStatus::Rejected; }
        [[nodiscard]] bool IsDiscarded() const { return GetStatus() == ECS::CommandStatus::Discarded; }

    private:
        friend class SceneCommandBuffer;
        friend class SceneEcsRuntime;

        explicit SceneCommandReceipt(std::shared_ptr<Detail::SceneCommandReceiptState> state)
            : m_state(std::move(state))
        {
        }

        std::shared_ptr<Detail::SceneCommandReceiptState> m_state;
    };

    /** @brief Deferred Scene entity creation receipt, resolved only at playback. */
    class SceneEntityReceipt
    {
    public:
        SceneEntityReceipt() = default;

        [[nodiscard]] ECS::CommandStatus GetStatus() const
        {
            return m_state != nullptr ? m_state->command->GetStatus() :
                                        ECS::CommandStatus::Rejected;
        }

        [[nodiscard]] ECS::CommandError GetError() const
        {
            return m_state != nullptr ? m_state->command->GetError() :
                                        ECS::CommandError::InvalidTarget;
        }

        [[nodiscard]] bool IsQueued() const { return GetStatus() == ECS::CommandStatus::Queued; }
        [[nodiscard]] bool IsApplied() const { return GetStatus() == ECS::CommandStatus::Applied; }
        [[nodiscard]] bool IsRejected() const { return GetStatus() == ECS::CommandStatus::Rejected; }
        [[nodiscard]] bool IsDiscarded() const { return GetStatus() == ECS::CommandStatus::Discarded; }
        [[nodiscard]] bool IsResolved() const
        {
            return IsApplied() && m_state != nullptr && m_state->GetEntity().has_value();
        }
        [[nodiscard]] ECS::EntityHandle GetEntity() const
        {
            if (!IsApplied() || m_state == nullptr)
            {
                return ECS::EntityHandle::Invalid();
            }

            const std::optional<ECS::EntityHandle> entity = m_state->GetEntity();
            return entity.has_value() ? *entity : ECS::EntityHandle::Invalid();
        }

    private:
        friend class SceneCommandBuffer;
        friend class SceneEcsRuntime;

        explicit SceneEntityReceipt(std::shared_ptr<Detail::SceneEntityReceiptState> state)
            : m_state(std::move(state))
        {
        }

        std::shared_ptr<Detail::SceneEntityReceiptState> m_state;
    };

    class SceneCommandBuffer;

    /**
     * @brief Value handle to the aggregate outcome of one submitted command buffer.
     *
     * A rejected aggregate means the underlying non-transactional command buffer
     * contained a rejected command. Inspect its ECS command receipts for the exact
     * command outcome; earlier commands may already have applied by design.
     */
    class SceneCommandBufferReceipt
    {
    public:
        SceneCommandBufferReceipt() = default;

        [[nodiscard]] SceneCommandBufferStatus GetStatus() const
        {
            return m_state != nullptr ? m_state->GetStatus() :
                                        SceneCommandBufferStatus::Rejected;
        }

        [[nodiscard]] SceneCommandBarrier GetBarrier() const
        {
            return m_state != nullptr ? m_state->GetBarrier() :
                                        SceneCommandBarrier::BeginSimulation;
        }

        [[nodiscard]] uint64 GetSubmissionSequence() const
        {
            return m_state != nullptr ? m_state->GetSubmissionSequence() : 0;
        }

        [[nodiscard]] bool IsQueued() const { return GetStatus() == SceneCommandBufferStatus::Queued; }
        [[nodiscard]] bool IsApplied() const { return GetStatus() == SceneCommandBufferStatus::Applied; }
        [[nodiscard]] bool IsRejected() const { return GetStatus() == SceneCommandBufferStatus::Rejected; }
        [[nodiscard]] bool IsDiscarded() const { return GetStatus() == SceneCommandBufferStatus::Discarded; }

    private:
        friend class SceneEcsRuntime;
        friend class SceneCommandSubmissionPort;

        explicit SceneCommandBufferReceipt(std::shared_ptr<Detail::SceneCommandBufferReceiptState> state)
            : m_state(std::move(state))
        {
        }

        std::shared_ptr<Detail::SceneCommandBufferReceiptState> m_state;
    };

    /**
     * @brief Shared-lifetime MPSC submission endpoint for Scene command buffers.
     *
     * Acquire this value while its SceneEcsRuntime is alive, then retain and use
     * it from producer threads without retaining or dereferencing the runtime.
     * Closing the runtime makes retained ports reject new submissions safely.
     */
    class SceneCommandSubmissionPort
    {
    public:
        SceneCommandSubmissionPort() = default;

        /** @brief True while the owning runtime has not closed this endpoint. */
        [[nodiscard]] bool IsOpen() const;
        /** @brief Create a recorder bound to the caller's recording thread. */
        [[nodiscard]] SceneCommandBuffer CreateCommandBuffer() const;
        /** @brief MPSC-safe submission that never dereferences SceneEcsRuntime. */
        [[nodiscard]] SceneCommandBufferReceipt SubmitCommandBuffer(
            SceneCommandBuffer&& commandBuffer,
            SceneCommandBarrier barrier = SceneCommandBarrier::BeginSimulation) const;

    private:
        friend class SceneEcsRuntime;

        explicit SceneCommandSubmissionPort(
            std::shared_ptr<Detail::SceneCommandSubmissionState> state)
            : m_state(std::move(state))
        {
        }

        std::shared_ptr<Detail::SceneCommandSubmissionState> m_state;
    };

    /** @brief Value counters for one command-playback boundary. */
    struct SceneCommandBarrierDiagnostics
    {
        SceneCommandBarrier barrier = SceneCommandBarrier::BeginSimulation;
        uint32 queuedBufferCount = 0;
        uint64 submittedBufferCount = 0;
        uint64 appliedBufferCount = 0;
        uint64 rejectedBufferCount = 0;
        uint64 discardedBufferCount = 0;
    };

    /** @brief Immutable aggregate diagnostics for all runtime command boundaries. */
    struct SceneCommandDiagnosticsSnapshot
    {
        uint64 nextSubmissionSequence = 1;
        std::array<SceneCommandBarrierDiagnostics, RVX_SCENE_COMMAND_BARRIER_COUNT> barriers{};
    };

    /** @brief Per-frame input. Fixed steps are supplied explicitly by the owner clock. */
    struct SceneEcsTickRequest
    {
        float64 variableDeltaSeconds = 0.0;
        float64 fixedDeltaSeconds = 1.0 / 60.0;
        uint32 fixedStepCount = 0;
    };

    /** @brief Per-boundary outcome captured during a single deterministic tick. */
    struct SceneCommandBarrierExecution
    {
        SceneCommandBarrier barrier = SceneCommandBarrier::BeginSimulation;
        uint32 attemptedBufferCount = 0;
        uint32 appliedBufferCount = 0;
        uint32 rejectedBufferCount = 0;
    };

    /** @brief Value summary of one complete Scene ECS frame. */
    struct SceneEcsTickResult
    {
        bool succeeded = false;
        uint64 frameSequence = 0;
        uint64 lastFixedStepSequence = 0;
        uint32 fixedStepsExecuted = 0;
        std::array<SceneCommandBarrierExecution, RVX_SCENE_COMMAND_BARRIER_COUNT> commandBarriers{};
        TransformResolveStats beginSimulationTransforms;
        /** @brief Resolve after root motion and before SceneToPhysics. */
        TransformResolveStats prePhysicsFixedTransforms;
        /** @brief Resolve after PhysicsToScene and before FixedTransformResolve processors. */
        TransformResolveStats fixedTransforms;
        TransformResolveStats presentationTransforms;
        uint32 synchronizedRenderTransformCount = 0;
        SpatialSynchronizeStats spatial;
        uint32 publishedCleanupRecordCount = 0;
        uint32 advancedRetirementCount = 0;
        uint32 recycledEntityCount = 0;
        uint64 sceneSnapshotRevision = 0;
        /** @brief Exact first processor fault that aborted this frame, if any. */
        std::optional<ECS::ProcessorExecutionFailure> processorFailure;
    };

    /** @brief Point-in-time runtime counters and version diagnostics. */
    struct SceneEcsDiagnosticsSnapshot
    {
        ECS::SceneRuntimeId sceneRuntimeId;
        uint32 entityCount = 0;
        uint32 pendingDestroyCount = 0;
        uint32 cleanupRequiredCount = 0;
        uint32 retiringCount = 0;
        uint32 recyclableCount = 0;
        uint64 nextCleanupSequence = 1;
        uint64 firstCleanupSequence = 1;
        uint64 cleanupContinuityLossCount = 0;
        uint64 nextStructuralSequence = 1;
        uint64 localTransformWriteVersion = 0;
        uint64 simulationWorldTransformWriteVersion = 0;
        uint64 renderWorldTransformWriteVersion = 0;
        uint64 frameSequence = 0;
        uint64 fixedStepSequence = 0;
        uint64 sceneSnapshotRevision = 0;
        uint32 spatialEntryCount = 0;
        uint32 queuedCommandBufferCount = 0;
        uint64 appliedCommandBufferCount = 0;
        uint64 rejectedCommandBufferCount = 0;
    };

    /** @brief Terminal outcome of one owner-thread skinned-mesh pose-owner rebind. */
    enum class SkinnedMeshPoseRebindCode : uint8
    {
        Applied = 0,
        OwnerThreadRequired,
        InvalidExpectedSceneRuntime,
        SceneRuntimeMismatch,
        EmptyMeshSet,
        InvalidPoseOwner,
        DuplicateMeshMember,
        InvalidMeshMember,
        MeshBindingUnavailable,
        MeshBindingMismatch,
        PoseBindingMismatch,
        RecordingRejected,
        TransactionRejected,
    };

    /**
     * @brief Value-only exact identity set for an atomic model skinning rebind.
     *
     * Both pose values and every mesh value must carry this runtime's current
     * generation.  sourceModelAssetValue/sourceSkinIndex make the operation
     * fail closed if a caller tries to cross-link a different imported skin.
     */
    struct SkinnedMeshPoseRebindRequest
    {
        ECS::SceneRuntimeId expectedSceneRuntimeId;
        std::span<const SceneEntityRef> meshMembers;
        SceneEntityRef expectedPoseOwner;
        SceneEntityRef newPoseOwner;
        uint64 sourceModelAssetValue = 0;
        int32 sourceSkinIndex = -1;
    };

    /** @brief Receipt for an all-or-nothing skinned-mesh pose-owner rebind. */
    struct SkinnedMeshPoseRebindReceipt
    {
        SkinnedMeshPoseRebindCode code = SkinnedMeshPoseRebindCode::TransactionRejected;
        ECS::CommandStatus transactionStatus = ECS::CommandStatus::Rejected;
        ECS::CommandError transactionError = ECS::CommandError::None;
        ECS::SceneRuntimeId sceneRuntimeId;
        uint32 meshCount = 0;

        [[nodiscard]] bool IsApplied() const noexcept
        {
            return code == SkinnedMeshPoseRebindCode::Applied &&
                   transactionStatus == ECS::CommandStatus::Applied;
        }
    };

    /**
     * @brief Runtime-only Scene ECS owner with cleanup-gated handle recycling.
     *
     * The Registry remains the sole ECS authority. This class only composes
     * Scene fragments, hierarchy policy, diagnostics, and value-owned cleanup
     * records; it contains no object-model compatibility path.
     */
    class SceneEcsRuntime
    {
    public:
        explicit SceneEcsRuntime(uint32 cleanupRecordCapacity = 8192,
                                 uint32 structuralJournalCapacity = 8192);
        ~SceneEcsRuntime();

        SceneEcsRuntime(const SceneEcsRuntime&) = delete;
        SceneEcsRuntime& operator=(const SceneEcsRuntime&) = delete;
        SceneEcsRuntime(SceneEcsRuntime&&) = delete;
        SceneEcsRuntime& operator=(SceneEcsRuntime&&) = delete;

        [[nodiscard]] ECS::SceneRuntimeId GetSceneRuntimeId() const;
        /**
         * @brief Return a live, generation-qualified identity owned by this Scene runtime.
         *
         * A stale or non-matching-generation handle returns an invalid SceneEntityRef.
         * This value is pointer-free and cannot mutate ECS state.
         */
        [[nodiscard]] SceneEntityRef GetEntityRef(ECS::EntityHandle entity) const;
        [[nodiscard]] const ECS::Registry& GetRegistry() const { return m_registry; }
        [[nodiscard]] const TransformHierarchy& GetTransformHierarchy() const
        {
            return m_transformHierarchy;
        }
        [[nodiscard]] const SceneSpatialIndex& GetSpatialIndex() const { return m_spatialIndex; }

        // =====================================================================
        // Deferred structural work and processor registration
        // =====================================================================
        /**
         * @brief Acquire a retainable producer endpoint while this runtime is alive.
         *
         * The returned value remains safe to use after runtime destruction, but
         * then rejects submission rather than extending runtime lifetime.
        */
        [[nodiscard]] SceneCommandSubmissionPort AcquireCommandSubmissionPort() const;
        /**
         * @brief Close producer submission while retaining the runtime for drain ticks.
         *
         * Queued buffers receive terminal Discarded receipts. Owner-thread
         * cleanup, processors, and retirement advancement remain available.
         */
        void BeginShutdown() noexcept;
        /** @brief True until BeginShutdown or destruction closes submission. */
        [[nodiscard]] bool IsAcceptingCommandSubmissions() const;
        /**
         * @brief Create a value-only command buffer bound to its recording thread.
         *
         * Buffers may be created and recorded by producers, then handed to any
         * producer thread for concurrent submission. Playback remains owner-thread-only.
         * This convenience member requires the SceneEcsRuntime object to remain alive;
         * retained producers should use AcquireCommandSubmissionPort instead.
         */
        [[nodiscard]] SceneCommandBuffer CreateCommandBuffer();
        /**
         * @brief Queue a command buffer for the selected playback boundary.
         *
         * Submission is MPSC-safe. A buffer submitted while its target barrier
         * is already playing is intentionally deferred to that barrier in the
         * following eligible frame/substep. This convenience member requires the
         * SceneEcsRuntime object to remain alive; retained producers should submit
         * through SceneCommandSubmissionPort instead.
         */
        [[nodiscard]] SceneCommandBufferReceipt SubmitCommandBuffer(
            SceneCommandBuffer&& commandBuffer,
            SceneCommandBarrier barrier = SceneCommandBarrier::BeginSimulation);

        /**
         * @brief Register an ECS processor into its explicit phase and clock.
         *
         * RenderExtraction is intentionally excluded because render extraction
         * may only observe FrozenSceneSnapshot values; use the dedicated method.
         */
        [[nodiscard]] bool RegisterProcessor(ECS::ProcessorDescriptor descriptor);
        /**
         * @brief Atomically register a related processor set.
         *
         * Every descriptor is validated, names are checked against the runtime
         * and within the batch, and storage is reserved before publication. A
         * rejected batch leaves no partially registered processors behind.
         */
        [[nodiscard]] bool RegisterProcessors(
            std::vector<ECS::ProcessorDescriptor> descriptors);
        /**
         * @brief Begin one exclusive owner-thread registration scope.
         *
         * Every processor registered until CloseProcessorRegistrationScope is
         * tagged with this token. Scopes are deliberately not nestable.
         */
        [[nodiscard]] std::optional<ProcessorRegistrationScope>
        BeginProcessorRegistrationScope();
        /** @brief Close a successfully compiled registration scope without retiring its token. */
        [[nodiscard]] bool CloseProcessorRegistrationScope(
            const ProcessorRegistrationScope& scope);
        /**
         * @brief Atomically remove and recompile exactly the processors tagged by one scope.
         *
         * On compilation or allocation failure the complete pre-removal
         * registration and scheduler state remains in place.
         */
        [[nodiscard]] bool RemoveProcessorRegistrationScope(
            ProcessorRegistrationScope& scope);
        /**
         * @brief Register value-only render extraction work after Freeze.
         *
         * The callback deliberately receives no Registry or fragment reference.
         */
        [[nodiscard]] bool RegisterRenderExtractionProcessor(
            std::string name,
            uint32 order,
            std::function<void(const FrozenSceneSnapshot&)> run);
        void ClearProcessors();
        [[nodiscard]] bool CompileProcessors();
        [[nodiscard]] const std::string& GetLastProcessorCompileError() const
        {
            return m_processorCompileError;
        }
        [[nodiscard]] const std::vector<ECS::ProcessorConflict>& GetProcessorConflictDiagnostics() const
        {
            return m_processorConflicts;
        }

        /** @brief Drive one complete owner-thread simulation-to-presentation frame. */
        [[nodiscard]] SceneEcsTickResult Tick(const SceneEcsTickRequest& request = {});
        /**
         * @brief Retain the immutable snapshot published by the latest Tick.
         *
         * A caller-held shared snapshot is never overwritten by later frames.
         */
        [[nodiscard]] std::shared_ptr<const FrozenSceneSnapshot> GetLatestFrozenSnapshot() const
        {
            return m_latestFrozenSnapshot;
        }
        /** @brief Publish a complete Particle value only from the Feature processor phase. */
        [[nodiscard]] bool PublishParticleFeatureSnapshot(
            ECS::EntityHandle entity,
            ParticleRenderSnapshotItem state,
            uint64 payloadRevision = 0);
        /** @brief Publish a complete Water value only from the Feature processor phase. */
        [[nodiscard]] bool PublishWaterFeatureSnapshot(
            ECS::EntityHandle entity,
            WaterRenderSnapshotItem state,
            uint64 payloadRevision = 0);
        /** @brief Publish a complete Terrain value only from the Feature processor phase. */
        [[nodiscard]] bool PublishTerrainFeatureSnapshot(
            ECS::EntityHandle entity,
            TerrainRenderSnapshotItem state,
            uint64 payloadRevision = 0);
        /** @brief Remove every feature value for an exact entity during cleanup. */
        [[nodiscard]] bool RemoveFeatureSnapshots(ECS::EntityHandle entity);
        /** @brief Remove only the particle value for an exact entity during cleanup. */
        [[nodiscard]] bool RemoveParticleFeatureSnapshot(ECS::EntityHandle entity);
        /** @brief Remove only the water value for an exact entity during cleanup. */
        [[nodiscard]] bool RemoveWaterFeatureSnapshot(ECS::EntityHandle entity);
        /** @brief Remove only the terrain value for an exact entity during cleanup. */
        [[nodiscard]] bool RemoveTerrainFeatureSnapshot(ECS::EntityHandle entity);
        [[nodiscard]] SceneFeatureSnapshotStoreCounts GetFeatureSnapshotCounts() const
        {
            return m_featureSnapshotStore.GetCounts();
        }
        /** @brief Publish one complete skinning palette only from the Feature processor phase. */
        [[nodiscard]] bool PublishSkinningPaletteSnapshot(
            ECS::EntityHandle meshEntity,
            SceneSkinningPaletteSnapshot snapshot);
        /** @brief Invalidate one stale mesh palette only from the Feature processor phase. */
        [[nodiscard]] bool InvalidateSkinningPaletteSnapshot(ECS::EntityHandle meshEntity);
        /** @brief Remove skinning values only during EndFrameCleanup. */
        [[nodiscard]] bool RemoveSkinningPaletteSnapshots(ECS::EntityHandle entity);
        [[nodiscard]] SceneSkinningSnapshotStoreCounts GetSkinningSnapshotCounts() const
        {
            return m_skinningSnapshotStore.GetCounts();
        }
        [[nodiscard]] SceneCommandDiagnosticsSnapshot GetCommandDiagnosticsSnapshot() const;

        /**
         * @brief Begin an owner-thread-only atomic pure-ECS Scene spawn batch.
         *
         * The returned move-only builder is bound to this runtime and must be
         * committed or discarded before this runtime is destroyed. It offers no
         * Registry access and publishes no EntityHandle or EntityRef before a
         * successful commit.
         */
        [[nodiscard]] SceneSpawnTransaction BeginSpawnTransaction();
        [[nodiscard]] ECS::EntityHandle CreateEntity(const RuntimeEntityDesc& desc = {});
        [[nodiscard]] bool SetActive(ECS::EntityHandle entity, bool active);
        [[nodiscard]] bool SetLocalTransform(ECS::EntityHandle entity,
                                             const LocalTransform& transform);
        /**
         * @brief Write a physics-authoritative world pose through hierarchy-local state.
         *
         * This intentionally exposes only a value operation; callers cannot
         * obtain a mutable Registry or TransformHierarchy from the runtime.
         */
        [[nodiscard]] SetWorldPoseResult SetSimulationWorldPose(
            ECS::EntityHandle entity,
            const Vec3& worldTranslation,
            const Quat& worldRotation);
        [[nodiscard]] ReparentResult Reparent(ECS::EntityHandle child,
                                              ECS::EntityHandle parent,
                                              ReparentMode mode = ReparentMode::KeepLocal);
        [[nodiscard]] ReparentResult Detach(ECS::EntityHandle child,
                                            ReparentMode mode = ReparentMode::KeepLocal);

        /**
         * @brief Atomically retarget an exact model skin's mesh bindings to one pose owner.
         *
         * The owner thread validates the complete value request before it
         * records any mutation. Existing fragment enablement and the exact
         * source model/skin identities are retained; a rejected transaction
         * leaves every mesh binding untouched.
         */
        [[nodiscard]] SkinnedMeshPoseRebindReceipt RebindSkinnedMeshPoseOwner(
            const SkinnedMeshPoseRebindRequest& request);

        /** @brief Attach a non-authority fragment during setup or through a Scene command. */
        template<ECS::Fragment T>
        [[nodiscard]] bool AddFragment(ECS::EntityHandle entity, T value = {})
        {
            if constexpr (Detail::IsSceneAuthorityFragment<T>)
            {
                return false;
            }
            else
            {
                const EntityLifecycleState* lifecycle =
                    m_registry.TryGet<EntityLifecycleState>(entity);
                return lifecycle != nullptr && lifecycle->phase == EntityLifecyclePhase::Alive &&
                       m_registry.Add<T>(entity, value);
            }
        }

        /** @brief Remove a non-authority fragment during setup or through a Scene command. */
        template<ECS::Fragment T>
        [[nodiscard]] bool RemoveFragment(ECS::EntityHandle entity)
        {
            if constexpr (Detail::IsSceneAuthorityFragment<T>)
            {
                return false;
            }
            else
            {
                const EntityLifecycleState* lifecycle =
                    m_registry.TryGet<EntityLifecycleState>(entity);
                return lifecycle != nullptr && lifecycle->phase == EntityLifecyclePhase::Alive &&
                       m_registry.Remove<T>(entity);
            }
        }

        /** @brief Replace a non-authority fragment value without a structural mutation. */
        template<ECS::Fragment T>
        [[nodiscard]] bool SetFragment(ECS::EntityHandle entity, const T& value)
        {
            if constexpr (Detail::IsSceneAuthorityFragment<T>)
            {
                return false;
            }
            else
            {
                const EntityLifecycleState* lifecycle =
                    m_registry.TryGet<EntityLifecycleState>(entity);
                return lifecycle != nullptr && lifecycle->phase == EntityLifecyclePhase::Alive &&
                       m_registry.Write<T>(entity, [&value](T& target) { target = value; });
            }
        }

        template<ECS::Fragment T>
        [[nodiscard]] bool SetFragmentEnabled(ECS::EntityHandle entity, bool enabled)
        {
            if constexpr (Detail::IsSceneAuthorityFragment<T>)
            {
                return false;
            }
            else
            {
                const EntityLifecycleState* lifecycle =
                    m_registry.TryGet<EntityLifecycleState>(entity);
                return lifecycle != nullptr && lifecycle->phase == EntityLifecyclePhase::Alive &&
                       m_registry.SetFragmentEnabled<T>(entity, enabled);
            }
        }

        [[nodiscard]] DestroyRequestResult RequestDestroy(
            ECS::EntityHandle entity,
            CleanupDomainMask requiredCleanupDomains =
                ToCleanupDomainMask(CleanupDomain::All));
        /**
         * @brief Atomically mark an exact entity set pending destroy.
         *
         * Validation and storage reservation happen before the first lifecycle
         * write, so asset-instance teardown cannot publish only a prefix.
         */
        [[nodiscard]] DestroyRequestResult RequestDestroyBatch(
            std::span<const ECS::EntityHandle> entities,
            CleanupDomainMask requiredCleanupDomains =
                ToCleanupDomainMask(CleanupDomain::All));
        /**
         * @brief Atomically mark every currently Alive entity pending destroy.
         *
         * This is the owner-thread World shutdown boundary. It keeps Registry
         * enumeration private, includes disabled entities, and lets the Scene
         * infer each entity's exact subsystem cleanup domains. Passing an
         * additional mask can only add gates; it cannot remove inferred ones.
         */
        [[nodiscard]] DestroyAllRequestResult RequestDestroyAll(
            CleanupDomainMask additionalCleanupDomains =
                ToCleanupDomainMask(CleanupDomain::None));
        /**
         * @brief Atomically mark every Alive entity except an exact retained set.
         *
         * The exclusions are generation-qualified handles in this Scene. Every
         * exclusion must still be Alive; a stale, duplicate-scene, pending, or
         * missing lifecycle target rejects the request without publishing any
         * lifecycle change. This is the shutdown seam used to retain a
         * presentation camera while all render-owned entities retire.
         */
        [[nodiscard]] DestroyAllRequestResult RequestDestroyAllExcept(
            std::span<const ECS::EntityHandle> exclusions,
            CleanupDomainMask additionalCleanupDomains =
                ToCleanupDomainMask(CleanupDomain::None));
        /** @brief Publish value cleanup records for every pending-destroy entity. */
        uint32 PublishPendingDestroyCleanup(CleanupReason reason = CleanupReason::RequestedDestroy);
        /** @brief Acknowledge one or more required cleanup domains for an entity. */
        [[nodiscard]] bool AcknowledgeCleanup(ECS::EntityHandle entity,
                                              CleanupDomainMask acknowledgedDomains);
        /** @brief Advance fully acknowledged cleanup entries to Recyclable. */
        uint32 AdvanceRetirements();
        /** @brief Destroy only entities already marked Recyclable, allowing slot reuse. */
        uint32 RecycleRecyclableEntities();

        [[nodiscard]] CleanupRecordRead ReadCleanupRecords(CleanupRecordCursor& cursor) const;
        [[nodiscard]] SceneEcsDiagnosticsSnapshot GetDiagnosticsSnapshot() const;

        TransformResolveStats BeginSimulationFrame();
        TransformResolveStats ResolveSimulationTransforms();
        uint32 SynchronizeRenderWorldTransforms();

    private:
        friend class SceneCommandBuffer;
        friend class SceneCommandSubmissionPort;
        friend class SceneSpawnTransaction;
        friend ResourceSceneAdapters::EcsEnvironmentAdoptionReceipt
        ResourceSceneAdapters::AdoptEcsEnvironmentBatch(
            SceneEcsRuntime& runtime,
            const ResourceSceneAdapters::EcsEnvironmentAdoptionBatch& batch,
            ECS::SceneRuntimeId expectedSceneRuntimeId,
            const ResourceSceneAdapters::EcsEnvironmentAdoptionOptions& options);
        friend ResourceSceneAdapters::PreparedModelAdoptionReceipt
        ResourceSceneAdapters::AdoptPreparedModelBatch(
            SceneEcsRuntime& runtime,
            const ResourceSceneAdapters::PreparedModelBatch& batch,
            ECS::SceneRuntimeId expectedSceneRuntimeId,
            const ResourceSceneAdapters::PreparedModelAdoptionOptions& options);

        static constexpr uint32 ProcessorPhaseCount =
            static_cast<uint32>(ECS::ProcessorPhase::EndFrameCleanup) + 1u;

        struct RegisteredProcessor
        {
            ECS::ProcessorDescriptor descriptor;
            uint64 registrationScopeId = 0;
        };

        [[nodiscard]] static uint32 ToBarrierIndex(SceneCommandBarrier barrier);
        [[nodiscard]] static uint32 ToProcessorPhaseIndex(ECS::ProcessorPhase phase);
        [[nodiscard]] bool IsOwnerThread() const;
        [[nodiscard]] CleanupDomainMask InferRequiredCleanupDomains(
            ECS::EntityHandle entity,
            CleanupDomainMask requestedDomains) const;
        [[nodiscard]] SceneCommandBarrierExecution ExecuteCommandBarrier(SceneCommandBarrier barrier);
        void DiscardQueuedCommandBuffers();
        [[nodiscard]] bool RunProcessorPhase(ECS::ProcessorPhase phase,
                                             ECS::ProcessorStepMode stepMode,
                                             float64 deltaSeconds,
                                             uint64 frameSequence,
                                             uint64 fixedStepSequence,
                                             std::optional<ECS::ProcessorExecutionFailure>& outFailure);
        [[nodiscard]] bool ValidateAndBuildProcessorSchedulers();
        [[nodiscard]] bool BuildProcessorSchedulers(
            const std::vector<RegisteredProcessor>& processors,
            std::array<ECS::ProcessorScheduler, ProcessorPhaseCount>& outVariableSchedulers,
            std::array<ECS::ProcessorScheduler, ProcessorPhaseCount>& outFixedSchedulers,
            std::array<uint32, ProcessorPhaseCount>& outVariableSchedulerSizes,
            std::array<uint32, ProcessorPhaseCount>& outFixedSchedulerSizes,
            std::vector<ECS::ProcessorConflict>& outConflicts,
            std::string& outError) const;
        [[nodiscard]] bool RegisterProcessorInternal(ECS::ProcessorDescriptor descriptor);
        void ResetCommandBarrierExecution(SceneEcsTickResult& result) const;
        [[nodiscard]] std::shared_ptr<const FrozenSceneSnapshot> FreezeAtPresentationBoundary();
        /** @brief Narrow friend-only seam for atomic prepared-model adoption. */
        [[nodiscard]] ECS::Registry& GetMutableRegistryForPreparedModelAdoption()
        {
            return m_registry;
        }
        /** @brief Narrow friend-only seam for atomic environment Skybox adoption. */
        [[nodiscard]] ECS::Registry& GetMutableRegistryForEcsEnvironmentAdoption()
        {
            return m_registry;
        }
        void PruneDeadLifecycleHandles();
        void TrimCleanupRecordsToCapacity();

        ECS::Registry m_registry;
        TransformHierarchy m_transformHierarchy;
        SceneSpatialIndex m_spatialIndex;
        std::array<ECS::ProcessorScheduler, ProcessorPhaseCount> m_variableSchedulers;
        std::array<ECS::ProcessorScheduler, ProcessorPhaseCount> m_fixedSchedulers;
        std::array<uint32, ProcessorPhaseCount> m_variableSchedulerSizes{};
        std::array<uint32, ProcessorPhaseCount> m_fixedSchedulerSizes{};
        std::vector<RegisteredProcessor> m_registeredProcessors;
        std::unordered_set<uint64> m_processorRegistrationScopes;
        std::optional<uint64> m_activeProcessorRegistrationScope;
        uint64 m_nextProcessorRegistrationScopeId = 1;
        std::vector<ECS::ProcessorConflict> m_processorConflicts;
        std::string m_processorCompileError;
        bool m_processorsNeedCompile = false;
        bool m_processorsCompiled = true;
        std::shared_ptr<Detail::SceneCommandSubmissionState> m_commandSubmissionState;
        SceneSnapshotBuilder m_sceneSnapshotBuilder;
        SceneFeatureSnapshotStore m_featureSnapshotStore;
        SceneSkinningSnapshotStore m_skinningSnapshotStore;
        std::shared_ptr<const FrozenSceneSnapshot> m_latestFrozenSnapshot;
        std::optional<ECS::ProcessorPhase> m_executingProcessorPhase;
        uint64 m_frameSequence = 0;
        uint64 m_fixedStepSequence = 0;
        std::vector<ECS::EntityHandle> m_pendingDestroyEntities;
        std::vector<ECS::EntityHandle> m_cleanupRequiredEntities;
        std::vector<ECS::EntityHandle> m_retiringEntities;
        std::vector<ECS::EntityHandle> m_recyclableEntities;
        std::vector<CleanupRecord> m_cleanupRecords;
        uint32 m_cleanupRecordCapacity = 8192;
        uint64 m_firstCleanupSequence = 1;
        uint64 m_nextCleanupSequence = 1;
        mutable uint64 m_cleanupContinuityLossCount = 0;
    };

    /**
     * @brief Move-only, value-owning builder for one atomic pure-ECS Scene spawn.
     *
     * Each entity starts with the RuntimeEntityDesc baseline. Generic Add only
     * accepts non-authority Fragment values and stores copy-owned recorder
     * commands; no fragment pointer or reference is retained after Commit.
     * ParentRelation remains deliberately unavailable to generic Add: use one
     * of the explicit SetParent overloads instead.
     */
    class SceneSpawnTransaction final
    {
    public:
        SceneSpawnTransaction() = default;
        SceneSpawnTransaction(const SceneSpawnTransaction&) = delete;
        SceneSpawnTransaction& operator=(const SceneSpawnTransaction&) = delete;
        SceneSpawnTransaction(SceneSpawnTransaction&& other) noexcept
            : m_runtime(std::exchange(other.m_runtime, nullptr))
            , m_originRuntimeId(std::exchange(other.m_originRuntimeId, {}))
            , m_entities(std::move(other.m_entities))
            , m_fragmentRecorders(std::move(other.m_fragmentRecorders))
            , m_recordingError(other.m_recordingError)
            , m_committed(std::exchange(other.m_committed, true))
        {
            other.m_recordingError = ECS::CommandError::InvalidTarget;
        }

        SceneSpawnTransaction& operator=(SceneSpawnTransaction&& other) noexcept
        {
            if (this != &other)
            {
                DiscardRecordedValues();
                m_runtime = std::exchange(other.m_runtime, nullptr);
                m_originRuntimeId = std::exchange(other.m_originRuntimeId, {});
                m_entities = std::move(other.m_entities);
                m_fragmentRecorders = std::move(other.m_fragmentRecorders);
                m_recordingError = other.m_recordingError;
                m_committed = std::exchange(other.m_committed, true);
                other.m_recordingError = ECS::CommandError::InvalidTarget;
            }
            return *this;
        }
        ~SceneSpawnTransaction() = default;

        /** @brief Record one baseline entity and return its transaction-local identity. */
        [[nodiscard]] SceneSpawnEntityId Create(const RuntimeEntityDesc& desc = {});
        /** @brief Parent a pending child to another pending entity in this batch. */
        [[nodiscard]] bool SetParent(SceneSpawnEntityId child, SceneSpawnEntityId parent);
        /** @brief Parent a pending child to a generation-qualified identity from this Scene. */
        [[nodiscard]] bool SetParent(SceneSpawnEntityId child, SceneEntityRef parent);
        /** @brief Parent a pending child to an already-alive entity in this Scene runtime. */
        [[nodiscard]] bool SetParent(SceneSpawnEntityId child, ECS::EntityHandle parent);

        /**
         * @brief Add one copy-owned non-authority fragment to a pending entity.
         *
         * The exact value is copied into a type-erased recorder while building
         * the transaction. The recorder is discarded during Commit, before any
         * SceneEntityRef mapping is returned.
         */
        template<ECS::Fragment T>
        [[nodiscard]] bool Add(SceneSpawnEntityId entity, T value = {})
        {
            if constexpr (Detail::IsSceneAuthorityFragment<T>)
            {
                return RejectRecording(ECS::CommandError::OperationRejected);
            }
            else
            {
                if (!CanRecord() || !Contains(entity))
                {
                    return RejectRecording(ECS::CommandError::InvalidTarget);
                }

                try
                {
                    m_fragmentRecorders.emplace_back(
                        [entityIndex = entity.m_index, value](
                            ECS::EntityTransaction& transaction,
                            const std::vector<ECS::EntityReceipt>& entities)
                        {
                            return transaction.Add<T>(entities[entityIndex], value).IsQueued();
                        });
                    return true;
                }
                catch (...)
                {
                    return RejectRecording(ECS::CommandError::RecordingFailed);
                }
            }
        }

        /** @brief Publish the entire recorded batch, or publish nothing. */
        [[nodiscard]] SceneSpawnCommitResult Commit();
        [[nodiscard]] bool IsCommitted() const { return m_committed; }

    private:
        friend class SceneEcsRuntime;

        enum class ParentKind : uint8
        {
            None = 0,
            Local,
            External,
        };

        struct ParentTarget
        {
            ParentKind kind = ParentKind::None;
            SceneSpawnEntityId localParent;
            ECS::EntityHandle externalParent = ECS::EntityHandle::Invalid();
        };

        struct PendingEntity
        {
            RuntimeEntityDesc desc;
            ParentTarget parent;
        };

        using FragmentRecorder = std::function<bool(
            ECS::EntityTransaction&,
            const std::vector<ECS::EntityReceipt>&)>;

        SceneSpawnTransaction(SceneEcsRuntime& runtime, ECS::SceneRuntimeId originRuntimeId)
            : m_runtime(&runtime)
            , m_originRuntimeId(originRuntimeId)
        {
        }

        [[nodiscard]] bool CanRecord() const
        {
            return m_runtime != nullptr && !m_committed &&
                   m_recordingError == ECS::CommandError::None;
        }

        [[nodiscard]] bool Contains(SceneSpawnEntityId entity) const
        {
            return entity.IsValid() && entity.m_index < m_entities.size();
        }

        [[nodiscard]] bool RejectRecording(ECS::CommandError error)
        {
            if (m_recordingError == ECS::CommandError::None)
            {
                m_recordingError = error;
            }
            return false;
        }

        [[nodiscard]] bool ValidateParentGraph() const;
        [[nodiscard]] bool ValidateExternalParentChain(ECS::EntityHandle parent) const;
        void DiscardRecordedValues() noexcept;

        SceneEcsRuntime* m_runtime = nullptr;
        ECS::SceneRuntimeId m_originRuntimeId;
        std::vector<PendingEntity> m_entities;
        std::vector<FragmentRecorder> m_fragmentRecorders;
        ECS::CommandError m_recordingError = ECS::CommandError::None;
        bool m_committed = false;
    };

    /**
     * @brief Scene-owned, value-only deferred structural command recorder.
     *
     * Unlike the generic ECS command buffer, this surface cannot physically
     * destroy a Registry slot or mutate Scene hierarchy, lifecycle, and
     * transform-authority fragments.  It records Scene semantic operations and
     * resolves them at a fixed Scene playback barrier. Non-atomic playback is
     * intentionally ordered: commands before a rejection stay applied.
     */
    class SceneCommandBuffer
    {
    public:
        SceneCommandBuffer() = default;
        ~SceneCommandBuffer() noexcept { DiscardOutstanding(); }
        SceneCommandBuffer(const SceneCommandBuffer&) = delete;
        SceneCommandBuffer& operator=(const SceneCommandBuffer&) = delete;
        SceneCommandBuffer(SceneCommandBuffer&& other) noexcept
            : m_commands(std::move(other.m_commands))
            , m_originRuntimeId(other.m_originRuntimeId)
            , m_ownerThread(other.m_ownerThread)
            , m_submitted(std::exchange(other.m_submitted, true))
        {
        }

        SceneCommandBuffer& operator=(SceneCommandBuffer&& other) noexcept
        {
            if (this != &other)
            {
                DiscardOutstanding();
                m_commands = std::move(other.m_commands);
                m_originRuntimeId = other.m_originRuntimeId;
                m_ownerThread = other.m_ownerThread;
                m_submitted = std::exchange(other.m_submitted, true);
            }
            return *this;
        }

        [[nodiscard]] SceneEntityReceipt Create(const RuntimeEntityDesc& desc = {})
        {
            if (!CanRecord())
            {
                return MakeRejectedEntityReceipt(ECS::CommandError::AlreadyCommitted);
            }

            try
            {
                auto state = std::make_shared<Detail::SceneEntityReceiptState>();
                m_commands.push_back({
                    .receipt = state->command,
                    .apply = [desc, state](SceneEcsRuntime& runtime, ECS::CommandError& error)
                    {
                        const ECS::EntityHandle entity = runtime.CreateEntity(desc);
                        if (!entity.IsValid())
                        {
                            error = ECS::CommandError::OperationRejected;
                            return false;
                        }
                        state->SetEntity(entity);
                        return true;
                    },
                });
                return SceneEntityReceipt(std::move(state));
            }
            catch (...)
            {
                return MakeRejectedEntityReceipt(ECS::CommandError::RecordingFailed);
            }
        }

        /** @brief Begin cleanup-gated retirement rather than destroying an ECS slot. */
        [[nodiscard]] SceneCommandReceipt RequestDestroy(
            ECS::EntityHandle entity,
            CleanupDomainMask requiredCleanupDomains = ToCleanupDomainMask(CleanupDomain::All))
        {
            return Record([entity, requiredCleanupDomains](SceneEcsRuntime& runtime,
                                                            ECS::CommandError& error)
            {
                if (runtime.RequestDestroy(entity, requiredCleanupDomains) != DestroyRequestResult::Accepted)
                {
                    error = ECS::CommandError::OperationRejected;
                    return false;
                }
                return true;
            });
        }

        /** @brief Route hierarchy edits through cycle and KeepWorld validation. */
        [[nodiscard]] SceneCommandReceipt Reparent(
            ECS::EntityHandle child,
            ECS::EntityHandle parent,
            ReparentMode mode = ReparentMode::KeepLocal)
        {
            return Record([child, parent, mode](SceneEcsRuntime& runtime, ECS::CommandError& error)
            {
                if (runtime.Reparent(child, parent, mode) != ReparentResult::Applied)
                {
                    error = ECS::CommandError::OperationRejected;
                    return false;
                }
                return true;
            });
        }

        [[nodiscard]] SceneCommandReceipt SetActive(ECS::EntityHandle entity, bool active)
        {
            return Record([entity, active](SceneEcsRuntime& runtime, ECS::CommandError& error)
            {
                if (!runtime.SetActive(entity, active))
                {
                    error = ECS::CommandError::OperationRejected;
                    return false;
                }
                return true;
            });
        }

        template<ECS::Fragment T>
        [[nodiscard]] SceneCommandReceipt Add(ECS::EntityHandle entity, T value = {})
        {
            if constexpr (Detail::IsSceneAuthorityFragment<T>)
            {
                return MakeRejectedReceipt(ECS::CommandError::OperationRejected);
            }
            else
            {
                return Record([entity, value](SceneEcsRuntime& runtime, ECS::CommandError& error)
                {
                    if (!runtime.AddFragment<T>(entity, value))
                    {
                        error = ECS::CommandError::OperationRejected;
                        return false;
                    }
                    return true;
                });
            }
        }

        template<ECS::Fragment T>
        [[nodiscard]] SceneCommandReceipt Add(const SceneEntityReceipt& entity, T value = {})
        {
            if constexpr (Detail::IsSceneAuthorityFragment<T>)
            {
                return MakeRejectedReceipt(ECS::CommandError::OperationRejected);
            }
            else if (entity.m_state == nullptr)
            {
                return MakeRejectedReceipt(ECS::CommandError::InvalidTarget);
            }
            else
            {
                const std::shared_ptr<Detail::SceneEntityReceiptState> state = entity.m_state;
                return Record([state, value](SceneEcsRuntime& runtime, ECS::CommandError& error)
                {
                    const std::optional<ECS::EntityHandle> resolvedEntity = state->GetEntity();
                    if (!resolvedEntity.has_value() ||
                        !runtime.AddFragment<T>(*resolvedEntity, value))
                    {
                        error = ECS::CommandError::InvalidTarget;
                        return false;
                    }
                    return true;
                });
            }
        }

        template<ECS::Fragment T>
        [[nodiscard]] SceneCommandReceipt Remove(ECS::EntityHandle entity)
        {
            if constexpr (Detail::IsSceneAuthorityFragment<T>)
            {
                return MakeRejectedReceipt(ECS::CommandError::OperationRejected);
            }
            else
            {
                return Record([entity](SceneEcsRuntime& runtime, ECS::CommandError& error)
                {
                    if (!runtime.RemoveFragment<T>(entity))
                    {
                        error = ECS::CommandError::OperationRejected;
                        return false;
                    }
                    return true;
                });
            }
        }


        template<ECS::Fragment T>
        [[nodiscard]] SceneCommandReceipt Set(ECS::EntityHandle entity, T value)
        {
            if constexpr (Detail::IsSceneAuthorityFragment<T>)
            {
                return MakeRejectedReceipt(ECS::CommandError::OperationRejected);
            }
            else
            {
                return Record([entity, value](SceneEcsRuntime& runtime, ECS::CommandError& error)
                {
                    if (!runtime.SetFragment<T>(entity, value))
                    {
                        error = ECS::CommandError::OperationRejected;
                        return false;
                    }
                    return true;
                });
            }
        }

        template<ECS::Fragment T>
        [[nodiscard]] SceneCommandReceipt SetFragmentEnabled(ECS::EntityHandle entity, bool enabled)
        {
            if constexpr (Detail::IsSceneAuthorityFragment<T>)
            {
                return MakeRejectedReceipt(ECS::CommandError::OperationRejected);
            }
            else
            {
                return Record([entity, enabled](SceneEcsRuntime& runtime, ECS::CommandError& error)
                {
                    if (!runtime.SetFragmentEnabled<T>(entity, enabled))
                    {
                        error = ECS::CommandError::OperationRejected;
                        return false;
                    }
                    return true;
                });
            }
        }

        [[nodiscard]] bool IsEmpty() const { return m_commands.empty(); }
        [[nodiscard]] bool IsSubmitted() const { return m_submitted; }

    private:
        friend class SceneEcsRuntime;
        friend class SceneCommandSubmissionPort;

        using Operation = std::function<bool(SceneEcsRuntime&, ECS::CommandError&)>;

        struct RecordedCommand
        {
            std::shared_ptr<Detail::SceneCommandReceiptState> receipt;
            Operation apply;
        };

        SceneCommandBuffer(ECS::SceneRuntimeId originRuntimeId, std::thread::id ownerThread)
            : m_originRuntimeId(originRuntimeId)
            , m_ownerThread(ownerThread)
        {
        }

        [[nodiscard]] bool CanRecord() const
        {
            return !m_submitted && m_originRuntimeId.IsValid() &&
                   m_ownerThread == std::this_thread::get_id();
        }

        [[nodiscard]] SceneCommandReceipt Record(Operation operation)
        {
            if (!CanRecord())
            {
                return MakeRejectedReceipt(ECS::CommandError::AlreadyCommitted);
            }
            try
            {
                auto receipt = std::make_shared<Detail::SceneCommandReceiptState>();
                m_commands.push_back({.receipt = receipt, .apply = std::move(operation)});
                return SceneCommandReceipt(std::move(receipt));
            }
            catch (...)
            {
                return MakeRejectedReceipt(ECS::CommandError::RecordingFailed);
            }
        }

        [[nodiscard]] bool IsBoundTo(ECS::SceneRuntimeId runtimeId) const
        {
            return m_originRuntimeId.IsValid() &&
                   m_originRuntimeId == runtimeId;
        }

        [[nodiscard]] bool Commit(SceneEcsRuntime& runtime)
        {
            if (m_submitted || !IsBoundTo(runtime.GetSceneRuntimeId()))
            {
                RejectOutstanding(ECS::CommandError::InvalidTarget);
                m_submitted = true;
                return false;
            }

            m_submitted = true;
            for (size_t index = 0; index < m_commands.size(); ++index)
            {
                RecordedCommand& command = m_commands[index];
                ECS::CommandError error = ECS::CommandError::None;
                bool applied = false;
                try
                {
                    applied = command.apply(runtime, error);
                }
                catch (...)
                {
                    error = ECS::CommandError::OperationRejected;
                }
                if (!applied)
                {
                    command.receipt->SetOutcome(
                        ECS::CommandStatus::Rejected,
                        error == ECS::CommandError::None ?
                            ECS::CommandError::OperationRejected : error);
                    RejectOutstandingFrom(index + 1u, ECS::CommandError::OperationRejected);
                    return false;
                }
                command.receipt->SetOutcome(ECS::CommandStatus::Applied,
                                            ECS::CommandError::None);
            }
            return true;
        }

        [[nodiscard]] static SceneCommandReceipt MakeRejectedReceipt(ECS::CommandError error)
        {
            auto state = std::make_shared<Detail::SceneCommandReceiptState>();
            state->SetOutcome(ECS::CommandStatus::Rejected, error);
            return SceneCommandReceipt(std::move(state));
        }

        [[nodiscard]] static SceneEntityReceipt MakeRejectedEntityReceipt(ECS::CommandError error)
        {
            auto state = std::make_shared<Detail::SceneEntityReceiptState>();
            state->command->SetOutcome(ECS::CommandStatus::Rejected, error);
            return SceneEntityReceipt(std::move(state));
        }

        void RejectOutstanding(ECS::CommandError error) { RejectOutstandingFrom(0, error); }

        void DiscardOutstanding() noexcept
        {
            if (m_submitted)
            {
                return;
            }
            for (RecordedCommand& command : m_commands)
            {
                if (command.receipt != nullptr &&
                    command.receipt->GetStatus() == ECS::CommandStatus::Queued)
                {
                    command.receipt->SetOutcome(ECS::CommandStatus::Discarded,
                                                ECS::CommandError::Discarded);
                }
            }
            m_submitted = true;
        }

        void RejectOutstandingFrom(size_t firstIndex, ECS::CommandError error)
        {
            for (size_t index = firstIndex; index < m_commands.size(); ++index)
            {
                if (m_commands[index].receipt->GetStatus() == ECS::CommandStatus::Queued)
                {
                    m_commands[index].receipt->SetOutcome(ECS::CommandStatus::Rejected, error);
                }
            }
        }

        std::vector<RecordedCommand> m_commands;
        ECS::SceneRuntimeId m_originRuntimeId;
        std::thread::id m_ownerThread;
        bool m_submitted = false;
    };

    namespace Detail
    {
        struct SceneCommandSubmissionState
        {
            struct QueuedCommandBuffer
            {
                std::shared_ptr<SceneCommandBuffer> commandBuffer;
                std::shared_ptr<SceneCommandBufferReceiptState> receipt;
                uint64 submissionSequence = 0;
            };

            explicit SceneCommandSubmissionState(ECS::SceneRuntimeId newRuntimeId)
                : runtimeId(newRuntimeId)
            {
                for (uint32 index = 0; index < RVX_SCENE_COMMAND_BARRIER_COUNT; ++index)
                {
                    diagnostics.barriers[index].barrier =
                        static_cast<SceneCommandBarrier>(index);
                }
            }

            mutable std::mutex mutex;
            bool closed = false;
            ECS::SceneRuntimeId runtimeId;
            std::array<std::vector<QueuedCommandBuffer>, RVX_SCENE_COMMAND_BARRIER_COUNT>
                commandBuffers;
            SceneCommandDiagnosticsSnapshot diagnostics;
        };
    } // namespace Detail
} // namespace RVX::SceneECS
