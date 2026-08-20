#pragma once

/**
 * @file AnimationEcsBridge.h
 * @brief Pure ECS animation evaluation, palette publication, and root-motion bridge.
 */

#include "ECS/Entity.h"
#include "Scene/ECS/AnimationFragments.h"
#include "Scene/ECS/SceneEcsRuntime.h"

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace RVX::AnimationSceneAdapters
{
    /** @brief Explicit registration result; no legacy-object fallback exists. */
    enum class AnimationEcsBridgeRegistrationResult : uint8
    {
        Registered = 0,
        AlreadyRegistered,
        InvalidRuntime,
        ProcessorRegistrationFailed,
    };

    /** @brief Exact result of binding root motion to a physics-owned body value. */
    enum class AnimationRootMotionPhysicsBindingResult : uint8
    {
        Applied = 0,
        ForeignScene,
        InvalidEntity,
        InvalidPhysicsHandle,
        PhysicsValidationUnavailable,
        AnimationEntityNotBound,
        BindingMismatch,
    };

    /** @brief Clock and data supplied to one value-only pose evaluation. */
    struct AnimationEcsEvaluationRequest
    {
        ECS::SceneRuntimeId sceneRuntimeId;
        ECS::EntityHandle entity = ECS::EntityHandle::Invalid();
        SceneECS::AnimationSkeletonBinding skeleton;
        SceneECS::Animator animator;
        float64 deltaSeconds = 0.0;
        uint64 frameSequence = 0;
        uint64 fixedStepSequence = 0;
        uint64 nextPoseSequence = 0;
    };

    /**
     * @brief Fully staged evaluator output.
     *
     * rootMotionSequence is an evaluator-owned source sequence.  The bridge
     * rejects replay and gaps before publishing RootMotionIntent. Zero selects
     * the bridge-owned contiguous root-motion publication stream; it is
     * intentionally independent of pose/palette revisions.
     */
    struct AnimationEcsEvaluatedPose
    {
        std::vector<Mat4> skinningPalette;
        Vec3 rootMotionTranslation{0.0f};
        Quat rootMotionRotation{1.0f, 0.0f, 0.0f, 0.0f};
        uint64 rootMotionSequence = 0;
    };

    /** @brief Stateless value evaluator; it receives no Scene runtime object. */
    using AnimationEcsPoseEvaluator = std::function<std::optional<AnimationEcsEvaluatedPose>(
        const AnimationEcsEvaluationRequest&)>;

    /**
     * @brief Value-only proof that a packed physics handle still belongs to this entity.
     *
     * The ECS animation bridge deliberately does not own a PhysicsWorld.  The
     * owner supplies this proof from the ECS Physics bridge/side table, both
     * when binding and before each RootMotion publication.
     */
    using AnimationEcsPhysicsBindingValidator = std::function<bool(
        ECS::SceneRuntimeId sceneRuntimeId,
        ECS::EntityHandle entity,
        uint64 physicsBodyHandlePacked)>;

    /**
     * @brief Releases evaluator-owned state for one exact Scene/entity binding.
     *
     * The bridge invokes this synchronously before a non-retained binding is
     * erased and, for retained Animation cleanup, before Scene receives the
     * Animation acknowledgement.  Returning false retains the binding and
     * leaves the Scene cleanup gate unacknowledged for a later retry.
     */
    using AnimationEcsCleanupCallback = std::function<bool(
        ECS::SceneRuntimeId sceneRuntimeId,
        ECS::EntityHandle entity)>;

    /** @brief Immutable pose/palette read copied from the bridge side table. */
    struct AnimationEcsPoseSnapshot
    {
        ECS::SceneRuntimeId sceneRuntimeId;
        ECS::EntityHandle entity = ECS::EntityHandle::Invalid();
        uint64 poseSequence = 0;
        uint64 paletteRevision = 0;
        std::vector<Mat4> skinningPalette;
    };

    /** @brief Value diagnostic for one generation-safe bridge binding. */
    struct AnimationEcsBindingDiagnostic
    {
        ECS::EntityHandle entity = ECS::EntityHandle::Invalid();
        SceneECS::AnimationBindingStatus status = SceneECS::AnimationBindingStatus::Unbound;
        uint64 poseSequence = 0;
        uint64 publishedRootMotionSequence = 0;
        uint64 consumedRootMotionSequence = 0;
        uint64 physicsBodyHandlePacked = 0;
        bool awaitingCleanupAcknowledgement = false;
    };

    /** @brief Deterministic bridge counters and bindings ordered by EntityHandle. */
    struct AnimationEcsBridgeDiagnosticsSnapshot
    {
        ECS::SceneRuntimeId sceneRuntimeId;
        AnimationEcsBridgeRegistrationResult registration =
            AnimationEcsBridgeRegistrationResult::InvalidRuntime;
        uint32 activeBindingCount = 0;
        uint32 pendingCleanupCount = 0;
        uint64 structuralContinuityLossCount = 0;
        uint64 cleanupContinuityLossCount = 0;
        uint64 authoritativeReconcileCount = 0;
        uint64 variableEvaluationCount = 0;
        uint64 fixedEvaluationCount = 0;
        uint64 rejectedEvaluationCount = 0;
        uint64 rootMotionReplayCount = 0;
        uint64 rootMotionGapCount = 0;
        /** @brief Pending intents observed from a superseded exact physics-body epoch. */
        uint64 rootMotionSupersededEpochCount = 0;
        uint64 rootMotionPublicationCount = 0;
        std::vector<AnimationEcsBindingDiagnostic> bindings;
    };

    /**
     * @brief Owns pure-ECS animation runtime state for exactly one Scene runtime.
     *
     * The bridge retains only EntityHandle values, value palettes, and a
     * value-only evaluator callback.  It never holds legacy scene-object or
     * resource-object pointers.
     */
    class AnimationEcsBridge
    {
    public:
        explicit AnimationEcsBridge(SceneECS::SceneEcsRuntime& runtime,
                                    AnimationEcsPoseEvaluator evaluator = {},
                                    AnimationEcsPhysicsBindingValidator physicsBindingValidator = {},
                                    AnimationEcsCleanupCallback cleanupCallback = {});
        ~AnimationEcsBridge();

        AnimationEcsBridge(const AnimationEcsBridge&) = delete;
        AnimationEcsBridge& operator=(const AnimationEcsBridge&) = delete;
        AnimationEcsBridge(AnimationEcsBridge&&) = delete;
        AnimationEcsBridge& operator=(AnimationEcsBridge&&) = delete;

        [[nodiscard]] AnimationEcsBridgeRegistrationResult RegisterProcessors();
        [[nodiscard]] bool IsRegistered() const;
        [[nodiscard]] ECS::SceneRuntimeId GetSceneRuntimeId() const;

        /**
         * @brief Bind an already reconciled animation entity to an exact packed physics body handle.
         *
         * The caller obtains this packed value from the ECS Physics bridge.
         * This operation requires the bridge to have received a validator at
         * construction.  The downstream SceneToPhysics consumer repeats the
         * same exact-handle proof before applying an intent.
         */
        [[nodiscard]] AnimationRootMotionPhysicsBindingResult BindRootMotionPhysicsBody(
            ECS::SceneRuntimeId sceneRuntimeId,
            ECS::EntityHandle entity,
            uint64 physicsBodyHandlePacked);
        /** @brief Clear only the exact generation-safe body relation previously bound. */
        [[nodiscard]] AnimationRootMotionPhysicsBindingResult UnbindRootMotionPhysicsBody(
            ECS::SceneRuntimeId sceneRuntimeId,
            ECS::EntityHandle entity,
            uint64 expectedPhysicsBodyHandlePacked);

        /** @brief Copy the latest atomically committed palette only for this Scene identity. */
        [[nodiscard]] std::optional<AnimationEcsPoseSnapshot> GetPoseSnapshot(
            ECS::SceneRuntimeId sceneRuntimeId,
            ECS::EntityHandle entity) const;
        [[nodiscard]] AnimationEcsBridgeDiagnosticsSnapshot GetDiagnosticsSnapshot() const;

    private:
        struct State;
        std::shared_ptr<State> m_state;
    };
} // namespace RVX::AnimationSceneAdapters
