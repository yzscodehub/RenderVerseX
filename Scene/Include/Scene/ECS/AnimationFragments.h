#pragma once

/**
 * @file AnimationFragments.h
 * @brief Data-only animation and root-motion fragments owned by the Scene ECS.
 *
 * Animation runtime objects, decoded clips, pose evaluators, and physics bodies
 * deliberately stay outside these fragments.  The AnimationSceneAdapters ECS
 * bridge owns those transient concerns and publishes only these value states.
 */

#include "Core/MathTypes.h"
#include "ECS/Entity.h"
#include "ECS/Fragment.h"

namespace RVX::SceneECS
{
    /** @brief Selects the explicit Scene clock used to evaluate an animator. */
    enum class AnimationEvaluationMode : uint8
    {
        VariablePrePhysics = 0,
        Fixed,
    };

    /** @brief Observable bridge outcome for one animation binding. */
    enum class AnimationBindingStatus : uint8
    {
        Unbound = 0,
        Active,
        InvalidConfiguration,
        EvaluatorRejected,
        InvalidPhysicsBinding,
        RootMotionSequenceRejected,
        PendingDestroy,
    };

    /**
     * @brief Authored skeletal source identity and palette cardinality.
     *
     * animationAssetValue remains a plain stable asset identity so Scene does
     * not acquire an Animation/Resource object dependency.  A value of zero is
     * allowed for procedural or test evaluators.
     */
    struct AnimationSkeletonBinding
    {
        uint64 animationAssetValue = 0;
        /**
         * @brief Collision-free zero-based ordinal in the immutable AnimationResource clip map.
         *
         * AnimationResource stores clips in a std::map ordered by their exact
         * authored UTF-8 names.  This ordinal therefore selects one and only
         * one clip in one frozen resource payload without putting a path,
         * string, hash, or resource pointer into ECS storage.  Re-cooking a
         * resource that changes its clip ordering is an authored structural
         * change and must update this value in the same entity transaction.
         */
        uint32 animationClipOrdinal = 0;
        /** @brief Stable source model identity shared with every skinned primitive. */
        uint64 sourceModelAssetValue = 0;
        int32 sourceSkinIndex = -1;
        uint32 boneCount = 0;
    };

    /**
     * @brief Value link from one renderable mesh to its exact pose owner.
     *
     * The link intentionally carries the model and skin identities as values.
     * Scene can therefore validate a palette publication without retaining an
     * Animation evaluator, model resource, or legacy component pointer.
     */
    struct SkinnedMeshBinding
    {
        ECS::EntityHandle poseEntity = ECS::EntityHandle::Invalid();
        uint64 sourceModelAssetValue = 0;
        int32 sourceSkinIndex = -1;
    };

    /** @brief Data-only runtime controls consumed by the ECS animation bridge. */
    struct Animator
    {
        AnimationEvaluationMode evaluationMode = AnimationEvaluationMode::VariablePrePhysics;
        float playbackRate = 1.0f;
        bool playing = true;
        bool rootMotionEnabled = false;
    };

    /** @brief Published pose metadata; palette matrices live in the bridge side table. */
    struct AnimationPoseState
    {
        AnimationBindingStatus status = AnimationBindingStatus::Unbound;
        uint64 poseSequence = 0;
        uint64 paletteRevision = 0;
        uint64 lastVariableFrameSequence = 0;
        uint64 lastFixedStepSequence = 0;
        uint32 paletteBoneCount = 0;
    };

    /**
     * @brief Value root-motion message consumed by the Scene-to-Physics stage.
     *
     * sourcePoseSequence identifies the exact committed Animation pose that
     * produced this delta. rootMotionSequence is the independent contiguous
     * root-motion stream consumed by Physics for baseline, replay, and gap
     * validation. physicsBodyHandlePacked is the exact packed generation-safe
     * physics body handle supplied by PhysicsSceneAdapters. Scene intentionally
     * does not include Physics headers or retain a physics-world pointer.
     */
    struct RootMotionIntent
    {
        uint64 sceneRuntimeIdValue = 0;
        ECS::EntityHandle targetEntity = ECS::EntityHandle::Invalid();
        uint64 physicsBodyHandlePacked = 0;
        uint64 sourcePoseSequence = 0;
        uint64 rootMotionSequence = 0;
        uint64 fixedStepSequence = 0;
        Vec3 translationDelta{0.0f};
        Quat rotationDelta{1.0f, 0.0f, 0.0f, 0.0f};
        bool pending = false;
        bool sequenceRejected = false;
    };

    static_assert(ECS::Fragment<AnimationSkeletonBinding>);
    static_assert(ECS::Fragment<SkinnedMeshBinding>);
    static_assert(ECS::Fragment<Animator>);
    static_assert(ECS::Fragment<AnimationPoseState>);
    static_assert(ECS::Fragment<RootMotionIntent>);
} // namespace RVX::SceneECS
