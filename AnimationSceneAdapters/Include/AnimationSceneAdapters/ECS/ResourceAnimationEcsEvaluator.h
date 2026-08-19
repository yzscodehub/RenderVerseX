#pragma once

/**
 * @file ResourceAnimationEcsEvaluator.h
 * @brief Resource-backed, value-only pose evaluation for the pure ECS animation bridge.
 */

#include "AnimationSceneAdapters/ECS/AnimationEcsBridge.h"
#include "Resource/Types/AnimationResource.h"

#include <functional>
#include <memory>
#include <optional>

namespace RVX::AnimationSceneAdapters
{
    /**
     * @brief Resolves one stable Scene asset value to an immutable animation payload.
     *
     * The resolver is called synchronously for each requested evaluation.  It
     * must return a published immutable resource snapshot; it never receives
     * an Entity, Scene, Actor, Component, or mutable ECS fragment.
     */
    using ResourceAnimationEcsResolver = std::function<
        std::shared_ptr<const Resource::AnimationResource>(uint64 animationAssetValue)>;

    /**
     * @brief Optional exact liveness proof for direct evaluator users.
     *
     * AnimationEcsBridge already proves this before invoking its evaluator.
     * Supplying this callback additionally makes standalone users reject stale
     * scene/entity pairs before resource resolution.  The callback must not
     * retain raw Scene objects beyond the synchronous call.
     */
    using ResourceAnimationEcsEntityValidator = std::function<bool(
        ECS::SceneRuntimeId sceneRuntimeId,
        ECS::EntityHandle entity)>;

    /** @brief Read-only playback state copied from the generation-safe side table. */
    struct ResourceAnimationEcsPlaybackSnapshot
    {
        ECS::SceneRuntimeId sceneRuntimeId;
        ECS::EntityHandle entity = ECS::EntityHandle::Invalid();
        uint64 animationAssetValue = 0;
        uint32 animationClipOrdinal = 0;
        int64 unwrappedTimeUs = 0;
        uint64 committedEvaluationCount = 0;
        uint64 lastVariableFrameSequence = 0;
        uint64 lastFixedStepSequence = 0;
    };

    /** @brief Aggregate value diagnostics for deterministic tests and host shutdown checks. */
    struct ResourceAnimationEcsEvaluatorDiagnosticsSnapshot
    {
        uint32 activePlaybackCount = 0;
        uint64 acceptedEvaluationCount = 0;
        uint64 rejectedEvaluationCount = 0;
        bool shutdown = false;
    };

    /** @brief Narrow Scene-qualified evaluator state for World shutdown ownership. */
    struct ResourceAnimationEcsEvaluatorSceneDiagnosticsSnapshot
    {
        ECS::SceneRuntimeId sceneRuntimeId{};
        uint32 activePlaybackCount = 0;
        bool shutdown = false;
    };

    /**
     * @brief Evaluates immutable AnimationResource clips without legacy object ownership.
     *
     * The evaluator is intentionally a side-table service: ECS keeps only the
     * authored asset identity, collision-free clip ordinal, and animator
     * controls.  Playback time and the retained immutable resource snapshot
     * are keyed by {SceneRuntimeId, EntityHandle(index,generation)}.  Failed
     * evaluations publish no pose and leave that state untouched.
     */
    class ResourceAnimationEcsEvaluator
    {
    public:
        explicit ResourceAnimationEcsEvaluator(
            ResourceAnimationEcsResolver resolver,
            ResourceAnimationEcsEntityValidator entityValidator = {});
        ~ResourceAnimationEcsEvaluator();

        ResourceAnimationEcsEvaluator(const ResourceAnimationEcsEvaluator&) = delete;
        ResourceAnimationEcsEvaluator& operator=(const ResourceAnimationEcsEvaluator&) = delete;
        ResourceAnimationEcsEvaluator(ResourceAnimationEcsEvaluator&&) = delete;
        ResourceAnimationEcsEvaluator& operator=(ResourceAnimationEcsEvaluator&&) = delete;

        /** @brief Evaluate one bridge request. Failure leaves the side table unchanged. */
        [[nodiscard]] std::optional<AnimationEcsEvaluatedPose> Evaluate(
            const AnimationEcsEvaluationRequest& request) const;

        /**
         * @brief Create the copyable callback injected into AnimationEcsBridge.
         *
         * The callback retains only shared evaluator state.  Explicit
         * Shutdown() turns outstanding callbacks into fail-closed evaluators.
         */
        [[nodiscard]] AnimationEcsPoseEvaluator CreatePoseEvaluator() const;

        /**
         * @brief Create the bridge cleanup callback for this evaluator's side table.
         *
         * A missing record is already clean and returns true.  The returned
         * callback owns shared evaluator state, so it remains fail-closed only
         * after Shutdown() has drained every immutable resource lease.
         */
        [[nodiscard]] AnimationEcsCleanupCallback CreateCleanupCallback() const;

        /** @brief Erase exactly one generation-safe playback record. */
        [[nodiscard]] bool RemoveBinding(ECS::SceneRuntimeId sceneRuntimeId,
                                         ECS::EntityHandle entity);
        /** @brief Erase all records belonging to one Scene identity. */
        [[nodiscard]] uint32 RemoveScene(ECS::SceneRuntimeId sceneRuntimeId);

        /** @brief Make future evaluations fail closed and release all immutable resource leases. */
        void Shutdown();
        [[nodiscard]] bool IsShutdown() const;

        [[nodiscard]] std::optional<ResourceAnimationEcsPlaybackSnapshot> GetPlaybackSnapshot(
            ECS::SceneRuntimeId sceneRuntimeId,
            ECS::EntityHandle entity) const;
        [[nodiscard]] ResourceAnimationEcsEvaluatorDiagnosticsSnapshot
        GetDiagnosticsSnapshot() const;
        /**
         * @brief Return playback state owned by one exact Scene identity only.
         *
         * A World must not infer its drain status from another World's active
         * animation records when the evaluator is Engine-owned and shared.
         */
        [[nodiscard]] ResourceAnimationEcsEvaluatorSceneDiagnosticsSnapshot
        GetSceneDiagnosticsSnapshot(ECS::SceneRuntimeId sceneRuntimeId) const;

    private:
        struct State;

        [[nodiscard]] static std::optional<AnimationEcsEvaluatedPose> EvaluateState(
            const std::shared_ptr<State>& state,
            const AnimationEcsEvaluationRequest& request);

        std::shared_ptr<State> m_state;
    };
} // namespace RVX::AnimationSceneAdapters
