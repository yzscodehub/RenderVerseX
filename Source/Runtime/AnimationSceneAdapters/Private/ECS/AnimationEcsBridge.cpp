#include "AnimationSceneAdapters/ECS/AnimationEcsBridge.h"

#include "ECS/Query.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <typeindex>
#include <utility>
#include <vector>

namespace RVX::AnimationSceneAdapters
{
namespace
{
    constexpr uint32 MaximumPaletteBoneCount = 4096;

    [[nodiscard]] bool IsFinite(const Vec3& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    [[nodiscard]] bool IsFinite(const Quat& value)
    {
        return std::isfinite(value.w) && std::isfinite(value.x) &&
               std::isfinite(value.y) && std::isfinite(value.z) &&
               glm::dot(value, value) > 0.000001f;
    }

    [[nodiscard]] bool IsFinite(const Mat4& matrix)
    {
        for (uint32 column = 0; column < 4u; ++column)
        {
            for (uint32 row = 0; row < 4u; ++row)
            {
                if (!std::isfinite(matrix[column][row]))
                {
                    return false;
                }
            }
        }
        return true;
    }

    [[nodiscard]] bool IsValidEvaluationMode(SceneECS::AnimationEvaluationMode mode)
    {
        switch (mode)
        {
        case SceneECS::AnimationEvaluationMode::VariablePrePhysics:
        case SceneECS::AnimationEvaluationMode::Fixed:
            return true;
        }
        return false;
    }

    [[nodiscard]] bool HasAnimationCleanupRequirement(
        const SceneECS::EntityLifecycleState* lifecycle)
    {
        return lifecycle != nullptr &&
               (lifecycle->requiredCleanupDomains &
                SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Animation)) != 0;
    }
} // namespace

struct AnimationEcsBridge::State
{
    struct Binding
    {
        ECS::EntityHandle entity = ECS::EntityHandle::Invalid();
        SceneECS::AnimationBindingStatus status = SceneECS::AnimationBindingStatus::Unbound;
        uint64 poseSequence = 0;
        uint64 paletteRevision = 0;
        uint64 skeletonWriteVersion = 0;
        uint64 lastVariableFrameSequence = 0;
        uint64 lastFixedStepSequence = 0;
        uint64 latestRootMotionPoseSequence = 0;
        uint64 latestRootMotionSequence = 0;
        uint64 lastProcessedRootMotionPoseSequence = 0;
        uint64 consumedRootMotionSequence = 0;
        uint64 physicsBodyHandlePacked = 0;
        Vec3 rootMotionTranslation{0.0f};
        Quat rootMotionRotation{1.0f, 0.0f, 0.0f, 0.0f};
        std::vector<Mat4> palette;
        // A paused binding still needs one renderable baseline pose. The bit
        // is cleared atomically with the first committed side-table palette.
        bool initialPalettePending = true;
        // The evaluator owns root-motion sequence values. A new exact Physics
        // binding establishes its sequence epoch on the first valid publish.
        bool rootMotionSequenceBaselinePending = false;
        bool awaitingCleanupAcknowledgement = false;
    };

    explicit State(SceneECS::SceneEcsRuntime& runtimeIn,
                   AnimationEcsPoseEvaluator evaluatorIn,
                   AnimationEcsPhysicsBindingValidator physicsBindingValidatorIn,
                   AnimationEcsCleanupCallback cleanupCallbackIn)
        : runtime(&runtimeIn)
        , sceneRuntimeId(runtimeIn.GetSceneRuntimeId())
        , evaluator(std::move(evaluatorIn))
        , physicsBindingValidator(std::move(physicsBindingValidatorIn))
        , cleanupCallback(std::move(cleanupCallbackIn))
    {
    }

    [[nodiscard]] uint32 FindBindingIndex(ECS::EntityHandle entity) const
    {
        const auto found = std::lower_bound(
            bindings.begin(), bindings.end(), entity,
            [](const Binding& binding, ECS::EntityHandle value)
            {
                return binding.entity < value;
            });
        return found != bindings.end() && found->entity == entity ?
                   static_cast<uint32>(std::distance(bindings.begin(), found)) :
                   RVX_INVALID_INDEX;
    }

    [[nodiscard]] bool AddBinding(ECS::EntityHandle entity)
    {
        if (FindBindingIndex(entity) != RVX_INVALID_INDEX)
        {
            return true;
        }

        try
        {
            if (bindings.size() == bindings.capacity())
            {
                bindings.reserve(bindings.size() + 1u);
            }
            const auto insertion = std::lower_bound(
                bindings.begin(), bindings.end(), entity,
                [](const Binding& binding, ECS::EntityHandle value)
                {
                    return binding.entity < value;
                });
            bindings.insert(insertion, {.entity = entity});
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    void EraseBinding(uint32 index)
    {
        if (index < bindings.size())
        {
            bindings.erase(bindings.begin() + static_cast<std::ptrdiff_t>(index));
        }
    }

    void WritePoseStatus(ECS::Registry& registry,
                         ECS::EntityHandle entity,
                         SceneECS::AnimationBindingStatus status)
    {
        if (!registry.Has<SceneECS::AnimationPoseState>(entity))
        {
            return;
        }

        static_cast<void>(registry.Write<SceneECS::AnimationPoseState>(
            entity,
            [status](SceneECS::AnimationPoseState& state)
            {
                state.status = status;
            }));
    }

    [[nodiscard]] bool IsEligible(const ECS::Registry& registry,
                                  ECS::EntityHandle entity,
                                  const SceneECS::AnimationSkeletonBinding*& skeleton,
                                  const SceneECS::Animator*& animator,
                                  const SceneECS::AnimationPoseState*& poseState,
                                  const SceneECS::EntityLifecycleState*& lifecycle,
                                  SceneECS::AnimationBindingStatus& failure) const
    {
        skeleton = nullptr;
        animator = nullptr;
        poseState = nullptr;
        lifecycle = registry.TryGet<SceneECS::EntityLifecycleState>(entity);
        failure = SceneECS::AnimationBindingStatus::Unbound;
        if (!registry.IsAlive(entity) || lifecycle == nullptr ||
            lifecycle->phase != SceneECS::EntityLifecyclePhase::Alive ||
            !registry.IsEnabled(entity) ||
            !registry.Has<SceneECS::AnimationSkeletonBinding>(entity) ||
            !registry.Has<SceneECS::Animator>(entity) ||
            !registry.Has<SceneECS::AnimationPoseState>(entity) ||
            !registry.IsFragmentEnabled<SceneECS::AnimationSkeletonBinding>(entity) ||
            !registry.IsFragmentEnabled<SceneECS::Animator>(entity) ||
            !registry.IsFragmentEnabled<SceneECS::AnimationPoseState>(entity))
        {
            return false;
        }

        skeleton = registry.TryGet<SceneECS::AnimationSkeletonBinding>(entity);
        animator = registry.TryGet<SceneECS::Animator>(entity);
        poseState = registry.TryGet<SceneECS::AnimationPoseState>(entity);
        if (skeleton == nullptr || animator == nullptr || poseState == nullptr ||
            skeleton->boneCount == 0 || skeleton->boneCount > MaximumPaletteBoneCount ||
            skeleton->sourceSkinIndex < -1 || !IsValidEvaluationMode(animator->evaluationMode) ||
            !std::isfinite(animator->playbackRate) || animator->playbackRate < 0.0f)
        {
            failure = SceneECS::AnimationBindingStatus::InvalidConfiguration;
            return false;
        }
        return true;
    }

    [[nodiscard]] bool RunCleanupCallback(ECS::ProcessorExecutionContext& context,
                                          ECS::EntityHandle entity)
    {
        if (!cleanupCallback)
        {
            return true;
        }
        try
        {
            if (cleanupCallback(sceneRuntimeId, entity))
            {
                return true;
            }
        }
        catch (...)
        {
        }
        context.ReportFailure("Animation evaluator cleanup callback rejected a binding.");
        return false;
    }

    [[nodiscard]] bool Detach(ECS::Registry& registry,
                              ECS::EntityHandle entity,
                              bool retainCleanupEvidence,
                              ECS::ProcessorExecutionContext& context)
    {
        const uint32 index = FindBindingIndex(entity);
        if (index == RVX_INVALID_INDEX)
        {
            return true;
        }

        Binding& binding = bindings[index];
        if (retainCleanupEvidence)
        {
            binding.palette.clear();
            binding.physicsBodyHandlePacked = 0;
            binding.latestRootMotionPoseSequence = 0;
            binding.latestRootMotionSequence = 0;
            binding.rootMotionTranslation = Vec3(0.0f);
            binding.rootMotionRotation = Quat(1.0f, 0.0f, 0.0f, 0.0f);
            binding.awaitingCleanupAcknowledgement = true;
            binding.status = SceneECS::AnimationBindingStatus::PendingDestroy;
            WritePoseStatus(registry, entity, binding.status);
            return true;
        }

        if (!RunCleanupCallback(context, entity))
        {
            return false;
        }

        binding.palette.clear();
        binding.physicsBodyHandlePacked = 0;
        binding.latestRootMotionPoseSequence = 0;
        binding.latestRootMotionSequence = 0;
        binding.rootMotionTranslation = Vec3(0.0f);
        binding.rootMotionRotation = Quat(1.0f, 0.0f, 0.0f, 0.0f);
        WritePoseStatus(registry, entity, SceneECS::AnimationBindingStatus::Unbound);
        EraseBinding(index);
        return true;
    }

    void Reconcile(ECS::ProcessorExecutionContext& context)
    {
        ECS::Registry& registry = context.registry;
        const ECS::StructuralJournalRead read = registry.ReadStructuralChanges(structuralCursor);
        const bool continuityLost = read.continuity == ECS::StructuralJournalContinuity::Lost;
        if (continuityLost)
        {
            ++structuralContinuityLossCount;
        }
        if (!initialReconcileComplete || continuityLost)
        {
            ++authoritativeReconcileCount;
        }
        initialReconcileComplete = true;

        std::vector<ECS::EntityHandle> existing;
        try
        {
            existing.reserve(bindings.size());
            for (const Binding& binding : bindings)
            {
                existing.push_back(binding.entity);
            }
        }
        catch (...)
        {
            context.ReportFailure("Animation ECS reconciliation could not stage existing bindings.");
            return;
        }

        for (const ECS::EntityHandle entity : existing)
        {
            const SceneECS::AnimationSkeletonBinding* skeleton = nullptr;
            const SceneECS::Animator* animator = nullptr;
            const SceneECS::AnimationPoseState* poseState = nullptr;
            const SceneECS::EntityLifecycleState* lifecycle = nullptr;
            SceneECS::AnimationBindingStatus failure;
            if (!IsEligible(registry, entity, skeleton, animator, poseState, lifecycle, failure))
            {
                const bool retain = HasAnimationCleanupRequirement(lifecycle) &&
                                    lifecycle->phase != SceneECS::EntityLifecyclePhase::Alive;
                if (!Detach(registry, entity, retain, context))
                {
                    return;
                }
                if (!retain && registry.IsAlive(entity))
                {
                    WritePoseStatus(registry, entity, failure);
                }
            }
        }

        std::vector<ECS::EntityHandle> candidates;
        registry.Query<ECS::Read<SceneECS::Animator>>().EachIncludingAllDisabled(
            [&candidates](ECS::EntityHandle entity, const SceneECS::Animator&)
            {
                candidates.push_back(entity);
            });
        std::sort(candidates.begin(), candidates.end());
        for (const ECS::EntityHandle entity : candidates)
        {
            const SceneECS::AnimationSkeletonBinding* skeleton = nullptr;
            const SceneECS::Animator* animator = nullptr;
            const SceneECS::AnimationPoseState* poseState = nullptr;
            const SceneECS::EntityLifecycleState* lifecycle = nullptr;
            SceneECS::AnimationBindingStatus failure;
            if (!IsEligible(registry, entity, skeleton, animator, poseState, lifecycle, failure))
            {
                if (registry.IsAlive(entity))
                {
                    WritePoseStatus(registry, entity, failure);
                }
                continue;
            }

            const bool isNewBinding = FindBindingIndex(entity) == RVX_INVALID_INDEX;
            if (!AddBinding(entity))
            {
                context.ReportFailure("Animation ECS bridge could not allocate a binding side-table entry.");
                return;
            }
            const uint32 index = FindBindingIndex(entity);
            if (isNewBinding && index != RVX_INVALID_INDEX)
            {
                bindings[index].status = SceneECS::AnimationBindingStatus::Active;
                WritePoseStatus(registry, entity, bindings[index].status);
            }
        }
    }

    [[nodiscard]] std::optional<AnimationEcsEvaluatedPose> EvaluatePose(
        const AnimationEcsEvaluationRequest& request) const
    {
        try
        {
            if (evaluator)
            {
                return evaluator(request);
            }

            AnimationEcsEvaluatedPose result;
            result.skinningPalette.assign(request.skeleton.boneCount, Mat4(1.0f));
            return result;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    [[nodiscard]] bool HasValidPhysicsBinding(const Binding& binding) const
    {
        if (binding.physicsBodyHandlePacked == 0 || !physicsBindingValidator)
        {
            return false;
        }
        try
        {
            return physicsBindingValidator(sceneRuntimeId, binding.entity,
                                           binding.physicsBodyHandlePacked);
        }
        catch (...)
        {
            return false;
        }
    }

    [[nodiscard]] bool IsValidEvaluatedPose(const AnimationEcsEvaluatedPose& evaluated,
                                             const SceneECS::AnimationSkeletonBinding& skeleton,
                                             bool requiresRootMotion) const
    {
        if (evaluated.skinningPalette.size() != skeleton.boneCount)
        {
            return false;
        }
        for (const Mat4& matrix : evaluated.skinningPalette)
        {
            if (!IsFinite(matrix))
            {
                return false;
            }
        }
        return !requiresRootMotion ||
               (IsFinite(evaluated.rootMotionTranslation) && IsFinite(evaluated.rootMotionRotation));
    }

    [[nodiscard]] bool CommitPose(ECS::Registry& registry,
                                  Binding& binding,
                                  AnimationEcsEvaluatedPose evaluated,
                                  const ECS::ProcessorExecutionContext& context,
                                  uint64 skeletonWriteVersion,
                                  bool fixedEvaluation,
                                  bool publishRootMotion)
    {
        const SceneECS::AnimationPoseState* previous =
            registry.TryGet<SceneECS::AnimationPoseState>(binding.entity);
        if (previous == nullptr || previous->poseSequence == std::numeric_limits<uint64>::max() ||
            previous->paletteRevision == std::numeric_limits<uint64>::max())
        {
            return false;
        }

        const uint64 poseSequence = previous->poseSequence + 1u;
        const uint64 paletteRevision = previous->paletteRevision + 1u;
        uint64 rootMotionSequence = 0;
        if (publishRootMotion)
        {
            if (evaluated.rootMotionSequence != 0)
            {
                rootMotionSequence = evaluated.rootMotionSequence;
            }
            else
            {
                if (binding.consumedRootMotionSequence == std::numeric_limits<uint64>::max())
                {
                    return false;
                }
                rootMotionSequence = binding.consumedRootMotionSequence + 1u;
            }
        }
        std::vector<Mat4> stagedPalette = std::move(evaluated.skinningPalette);
        const bool committed = registry.Write<SceneECS::AnimationPoseState>(
            binding.entity,
            [&binding, &stagedPalette, &evaluated, poseSequence, paletteRevision,
             rootMotionSequence, skeletonWriteVersion, publishRootMotion,
             fixedEvaluation, &context](
                SceneECS::AnimationPoseState& state)
            {
                // This lambda performs only no-throw scalar writes and a
                // vector swap.  Therefore metadata and palette publication
                // become visible together, or neither changes at all.
                state.status = SceneECS::AnimationBindingStatus::Active;
                state.poseSequence = poseSequence;
                state.paletteRevision = paletteRevision;
                state.paletteBoneCount = static_cast<uint32>(stagedPalette.size());
                if (fixedEvaluation)
                {
                    state.lastFixedStepSequence = context.fixedStepSequence;
                }
                else
                {
                    state.lastVariableFrameSequence = context.frameSequence;
                }
                binding.palette.swap(stagedPalette);
                binding.status = SceneECS::AnimationBindingStatus::Active;
                binding.poseSequence = poseSequence;
                binding.paletteRevision = paletteRevision;
                binding.skeletonWriteVersion = skeletonWriteVersion;
                binding.initialPalettePending = false;
                if (fixedEvaluation)
                {
                    binding.lastFixedStepSequence = context.fixedStepSequence;
                }
                else
                {
                    binding.lastVariableFrameSequence = context.frameSequence;
                }
                if (publishRootMotion)
                {
                    binding.latestRootMotionPoseSequence = poseSequence;
                    binding.latestRootMotionSequence = rootMotionSequence;
                    binding.rootMotionTranslation = evaluated.rootMotionTranslation;
                    binding.rootMotionRotation = evaluated.rootMotionRotation;
                }
            });
        return committed;
    }

    void Evaluate(ECS::ProcessorExecutionContext& context, bool fixedEvaluation)
    {
        ECS::Registry& registry = context.registry;
        std::vector<ECS::EntityHandle> entities;
        try
        {
            entities.reserve(bindings.size());
            for (const Binding& binding : bindings)
            {
                entities.push_back(binding.entity);
            }
        }
        catch (...)
        {
            context.ReportFailure("Animation ECS evaluation could not stage bindings.");
            return;
        }

        for (const ECS::EntityHandle entity : entities)
        {
            const uint32 index = FindBindingIndex(entity);
            if (index == RVX_INVALID_INDEX)
            {
                continue;
            }
            Binding& binding = bindings[index];
            const SceneECS::AnimationSkeletonBinding* skeleton = nullptr;
            const SceneECS::Animator* animator = nullptr;
            const SceneECS::AnimationPoseState* poseState = nullptr;
            const SceneECS::EntityLifecycleState* lifecycle = nullptr;
            SceneECS::AnimationBindingStatus failure;
            if (!IsEligible(registry, entity, skeleton, animator, poseState, lifecycle, failure))
            {
                continue;
            }

            const bool wantsFixed = animator->evaluationMode ==
                                    SceneECS::AnimationEvaluationMode::Fixed;
            const uint64 skeletonWriteVersion =
                registry.GetFragmentWriteVersion<SceneECS::AnimationSkeletonBinding>(entity);
            const bool hasCoherentCommittedPalette =
                binding.poseSequence != 0 && binding.paletteRevision != 0 &&
                binding.skeletonWriteVersion == skeletonWriteVersion &&
                binding.palette.size() == skeleton->boneCount &&
                poseState->poseSequence == binding.poseSequence &&
                poseState->paletteRevision == binding.paletteRevision &&
                poseState->paletteBoneCount == skeleton->boneCount;
            const bool initializePausedPose =
                !fixedEvaluation && !animator->playing &&
                (binding.initialPalettePending || !hasCoherentCommittedPalette);
            if (!initializePausedPose &&
                (wantsFixed != fixedEvaluation || !animator->playing))
            {
                continue;
            }
            if ((fixedEvaluation && binding.lastFixedStepSequence == context.fixedStepSequence) ||
                (!fixedEvaluation && binding.lastVariableFrameSequence == context.frameSequence) ||
                poseState->poseSequence == std::numeric_limits<uint64>::max())
            {
                continue;
            }

            AnimationEcsEvaluationRequest request;
            request.sceneRuntimeId = sceneRuntimeId;
            request.entity = entity;
            request.skeleton = *skeleton;
            request.animator = *animator;
            request.deltaSeconds = initializePausedPose ? 0.0 : context.deltaSeconds;
            request.frameSequence = context.frameSequence;
            request.fixedStepSequence = context.fixedStepSequence;
            request.nextPoseSequence = poseState->poseSequence + 1u;
            const bool publishRootMotion =
                fixedEvaluation && animator->playing && animator->rootMotionEnabled;
            if (publishRootMotion)
            {
                const SceneECS::RootMotionIntent* pendingIntent =
                    registry.TryGet<SceneECS::RootMotionIntent>(entity);
                // Scene-to-Physics owns retry of an already published value.
                // Do not advance the evaluator or overwrite that exact intent
                // until the consumer has cleared it.
                if (pendingIntent != nullptr && pendingIntent->pending &&
                    pendingIntent->sceneRuntimeIdValue == sceneRuntimeId.GetValue() &&
                    pendingIntent->targetEntity == binding.entity &&
                    pendingIntent->physicsBodyHandlePacked == binding.physicsBodyHandlePacked)
                {
                    continue;
                }
            }
            std::optional<AnimationEcsEvaluatedPose> evaluated = EvaluatePose(request);
            if (!evaluated.has_value() ||
                !IsValidEvaluatedPose(*evaluated, *skeleton, publishRootMotion) ||
                !CommitPose(registry, binding, std::move(*evaluated), context,
                            skeletonWriteVersion, fixedEvaluation, publishRootMotion))
            {
                binding.status = SceneECS::AnimationBindingStatus::EvaluatorRejected;
                WritePoseStatus(registry, entity, binding.status);
                ++rejectedEvaluationCount;
                continue;
            }

            if (fixedEvaluation)
            {
                ++fixedEvaluationCount;
            }
            else
            {
                ++variableEvaluationCount;
            }
        }
    }

    void RejectRootMotion(ECS::Registry& registry,
                          Binding& binding,
                          SceneECS::AnimationBindingStatus status,
                          bool sequenceRejected,
                          uint64 fixedStepSequence)
    {
        binding.status = status;
        WritePoseStatus(registry, binding.entity, status);
        const uint64 sceneRuntimeIdValue = sceneRuntimeId.GetValue();
        if (registry.Has<SceneECS::RootMotionIntent>(binding.entity))
        {
            static_cast<void>(registry.Write<SceneECS::RootMotionIntent>(
                binding.entity,
                [&binding, sequenceRejected, sceneRuntimeIdValue, fixedStepSequence](
                    SceneECS::RootMotionIntent& intent)
                {
                    intent.sceneRuntimeIdValue = sceneRuntimeIdValue;
                    intent.targetEntity = binding.entity;
                    intent.physicsBodyHandlePacked = binding.physicsBodyHandlePacked;
                    intent.sourcePoseSequence = binding.latestRootMotionPoseSequence;
                    intent.rootMotionSequence = binding.latestRootMotionSequence;
                    intent.fixedStepSequence = fixedStepSequence;
                    intent.translationDelta = Vec3(0.0f);
                    intent.rotationDelta = Quat(1.0f, 0.0f, 0.0f, 0.0f);
                    intent.pending = false;
                    intent.sequenceRejected = sequenceRejected;
                }));
        }
    }

    void PublishRootMotion(ECS::ProcessorExecutionContext& context)
    {
        ECS::Registry& registry = context.registry;
        std::vector<ECS::EntityHandle> entities;
        try
        {
            entities.reserve(bindings.size());
            for (const Binding& binding : bindings)
            {
                entities.push_back(binding.entity);
            }
        }
        catch (...)
        {
            context.ReportFailure("Animation ECS root-motion stage could not stage bindings.");
            return;
        }

        for (const ECS::EntityHandle entity : entities)
        {
            const uint32 index = FindBindingIndex(entity);
            if (index == RVX_INVALID_INDEX)
            {
                continue;
            }
            Binding& binding = bindings[index];
            const SceneECS::AnimationSkeletonBinding* skeleton = nullptr;
            const SceneECS::Animator* animator = nullptr;
            const SceneECS::AnimationPoseState* poseState = nullptr;
            const SceneECS::EntityLifecycleState* lifecycle = nullptr;
            SceneECS::AnimationBindingStatus failure;
            if (!IsEligible(registry, entity, skeleton, animator, poseState, lifecycle, failure) ||
                animator->evaluationMode != SceneECS::AnimationEvaluationMode::Fixed ||
                !animator->rootMotionEnabled ||
                binding.latestRootMotionPoseSequence == 0 ||
                binding.latestRootMotionPoseSequence == binding.lastProcessedRootMotionPoseSequence)
            {
                continue;
            }

            binding.lastProcessedRootMotionPoseSequence = binding.latestRootMotionPoseSequence;
            // The first successfully published sequence for a new exact body
            // is the binding epoch baseline. Idempotent binds leave this false,
            // so all later replay/gap checks remain strict.
            if (!binding.rootMotionSequenceBaselinePending &&
                binding.latestRootMotionSequence <= binding.consumedRootMotionSequence)
            {
                ++rootMotionReplayCount;
                RejectRootMotion(registry, binding,
                                 SceneECS::AnimationBindingStatus::RootMotionSequenceRejected,
                                 true, context.fixedStepSequence);
                continue;
            }
            if (!binding.rootMotionSequenceBaselinePending &&
                (binding.consumedRootMotionSequence == std::numeric_limits<uint64>::max() ||
                 binding.latestRootMotionSequence != binding.consumedRootMotionSequence + 1u))
            {
                ++rootMotionGapCount;
                RejectRootMotion(registry, binding,
                                 SceneECS::AnimationBindingStatus::RootMotionSequenceRejected,
                                 true, context.fixedStepSequence);
                continue;
            }
            if (!HasValidPhysicsBinding(binding) ||
                !registry.Has<SceneECS::RootMotionIntent>(entity) ||
                !registry.IsFragmentEnabled<SceneECS::RootMotionIntent>(entity))
            {
                RejectRootMotion(registry, binding,
                                 SceneECS::AnimationBindingStatus::InvalidPhysicsBinding,
                                 false, context.fixedStepSequence);
                continue;
            }

            const uint64 sceneRuntimeIdValue = sceneRuntimeId.GetValue();
            const uint64 fixedStepSequence = context.fixedStepSequence;
            const bool published = registry.Write<SceneECS::RootMotionIntent>(
                entity,
                [&binding, sceneRuntimeIdValue, fixedStepSequence](SceneECS::RootMotionIntent& intent)
                {
                    intent.sceneRuntimeIdValue = sceneRuntimeIdValue;
                    intent.targetEntity = binding.entity;
                    intent.physicsBodyHandlePacked = binding.physicsBodyHandlePacked;
                    intent.sourcePoseSequence = binding.latestRootMotionPoseSequence;
                    intent.rootMotionSequence = binding.latestRootMotionSequence;
                    intent.fixedStepSequence = fixedStepSequence;
                    intent.translationDelta = binding.rootMotionTranslation;
                    intent.rotationDelta = binding.rootMotionRotation;
                    intent.pending = true;
                    intent.sequenceRejected = false;
                });
            if (!published)
            {
                RejectRootMotion(registry, binding,
                                 SceneECS::AnimationBindingStatus::InvalidPhysicsBinding,
                                 false, context.fixedStepSequence);
                continue;
            }

            binding.status = SceneECS::AnimationBindingStatus::Active;
            binding.consumedRootMotionSequence = binding.latestRootMotionSequence;
            binding.rootMotionSequenceBaselinePending = false;
            WritePoseStatus(registry, entity, binding.status);
            ++rootMotionPublicationCount;
        }
    }

    void PublishSkinningSnapshots(ECS::ProcessorExecutionContext& context)
    {
        struct MeshCandidate
        {
            ECS::EntityHandle entity = ECS::EntityHandle::Invalid();
            SceneECS::SkinnedMeshBinding binding;
        };

        ECS::Registry& registry = context.registry;
        std::vector<MeshCandidate> candidates;
        registry.Query<ECS::Read<SceneECS::Mesh>,
                       ECS::Read<SceneECS::SkinnedMeshBinding>,
                       ECS::Read<SceneECS::EntityLifecycleState>>()
            .Each([&candidates](ECS::EntityHandle entity,
                                const SceneECS::Mesh&,
                                const SceneECS::SkinnedMeshBinding& binding,
                                const SceneECS::EntityLifecycleState& lifecycle)
            {
                if (lifecycle.phase == SceneECS::EntityLifecyclePhase::Alive)
                {
                    candidates.push_back({.entity = entity, .binding = binding});
                }
            });
        std::sort(candidates.begin(), candidates.end(), [](const MeshCandidate& left,
                                                            const MeshCandidate& right)
        {
            return left.entity < right.entity;
        });

        for (const MeshCandidate& candidate : candidates)
        {
            const auto invalidateCandidate = [this, &context, &candidate]()
            {
                if (runtime->InvalidateSkinningPaletteSnapshot(candidate.entity))
                {
                    return true;
                }
                context.ReportFailure(
                    "Animation ECS could not invalidate an incoherent skinning snapshot.");
                return false;
            };
            const uint32 poseIndex = FindBindingIndex(candidate.binding.poseEntity);
            if (poseIndex == RVX_INVALID_INDEX)
            {
                if (!invalidateCandidate())
                {
                    return;
                }
                continue;
            }

            const SceneECS::AnimationSkeletonBinding* skeleton = nullptr;
            const SceneECS::Animator* animator = nullptr;
            const SceneECS::AnimationPoseState* poseState = nullptr;
            const SceneECS::EntityLifecycleState* lifecycle = nullptr;
            SceneECS::AnimationBindingStatus failure;
            if (!IsEligible(registry,
                            candidate.binding.poseEntity,
                            skeleton,
                            animator,
                            poseState,
                            lifecycle,
                            failure))
            {
                if (!invalidateCandidate())
                {
                    return;
                }
                continue;
            }

            const Binding& pose = bindings[poseIndex];
            if (candidate.binding.sourceModelAssetValue == 0 ||
                candidate.binding.sourceSkinIndex < 0 ||
                skeleton->sourceModelAssetValue != candidate.binding.sourceModelAssetValue ||
                skeleton->sourceSkinIndex != candidate.binding.sourceSkinIndex ||
                pose.poseSequence == 0 || pose.paletteRevision == 0 ||
                poseState->poseSequence != pose.poseSequence ||
                poseState->paletteRevision != pose.paletteRevision ||
                poseState->paletteBoneCount != skeleton->boneCount ||
                pose.skeletonWriteVersion !=
                    registry.GetFragmentWriteVersion<SceneECS::AnimationSkeletonBinding>(
                        candidate.binding.poseEntity) ||
                pose.palette.size() != skeleton->boneCount)
            {
                if (!invalidateCandidate())
                {
                    return;
                }
                continue;
            }

            SceneECS::SceneSkinningPaletteSnapshot snapshot;
            snapshot.poseEntity = candidate.binding.poseEntity;
            snapshot.sourceModelAssetValue = candidate.binding.sourceModelAssetValue;
            snapshot.sourceSkinIndex = candidate.binding.sourceSkinIndex;
            snapshot.poseSequence = pose.poseSequence;
            snapshot.paletteRevision = pose.paletteRevision;
            snapshot.paletteBoneCount = static_cast<uint32>(pose.palette.size());
            snapshot.matrices = pose.palette;
            if (!runtime->PublishSkinningPaletteSnapshot(candidate.entity, std::move(snapshot)))
            {
                context.ReportFailure("Animation ECS could not publish a skinning snapshot.");
                return;
            }
        }
    }

    void AcknowledgeRetainedCleanup(ECS::ProcessorExecutionContext& context)
    {
        ECS::Registry& registry = context.registry;
        std::vector<ECS::EntityHandle> entities;
        registry.Query<ECS::Read<SceneECS::EntityLifecycleState>>().EachIncludingAllDisabled(
            [&entities](ECS::EntityHandle entity, const SceneECS::EntityLifecycleState& lifecycle)
            {
                if (lifecycle.phase == SceneECS::EntityLifecyclePhase::CleanupRequired &&
                    (lifecycle.requiredCleanupDomains &
                     SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Animation)) != 0)
                {
                    entities.push_back(entity);
                }
            });
        std::sort(entities.begin(), entities.end());
        for (const ECS::EntityHandle entity : entities)
        {
            const SceneECS::EntityLifecycleState* lifecycle =
                registry.TryGet<SceneECS::EntityLifecycleState>(entity);
            if (!HasAnimationCleanupRequirement(lifecycle) ||
                lifecycle->phase != SceneECS::EntityLifecyclePhase::CleanupRequired)
            {
                continue;
            }

            if (!runtime->RemoveSkinningPaletteSnapshots(entity))
            {
                context.ReportFailure("Animation cleanup could not remove a skinning snapshot.");
                return;
            }
            const uint32 index = FindBindingIndex(entity);
            if (index != RVX_INVALID_INDEX)
            {
                if (!Detach(registry, entity, true, context))
                {
                    return;
                }
            }
            if (!RunCleanupCallback(context, entity))
            {
                return;
            }
            if (!runtime->AcknowledgeCleanup(
                    entity, SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Animation)))
            {
                context.ReportFailure("Animation cleanup acknowledgement was rejected.");
                return;
            }
            const uint32 retained = FindBindingIndex(entity);
            if (retained != RVX_INVALID_INDEX)
            {
                EraseBinding(retained);
            }
        }
    }

    void Cleanup(ECS::ProcessorExecutionContext& context)
    {
        // Lifecycle is durable evidence, so retry it before and after the
        // bounded cleanup journal.  A cursor loss therefore cannot strand an
        // animation palette or delay entity generation retirement.
        AcknowledgeRetainedCleanup(context);
        if (context.HasReportedFailure())
        {
            return;
        }

        const SceneECS::CleanupRecordRead read = runtime->ReadCleanupRecords(cleanupCursor);
        if (read.continuity == SceneECS::CleanupRecordContinuity::Lost)
        {
            ++cleanupContinuityLossCount;
            Reconcile(context);
            if (context.HasReportedFailure())
            {
                return;
            }
            AcknowledgeRetainedCleanup(context);
            return;
        }

        for (const SceneECS::CleanupRecord& record : read.records)
        {
            if (record.sceneRuntimeId != sceneRuntimeId ||
                (record.requiredCleanupDomains &
                 SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Animation)) == 0)
            {
                continue;
            }
            if (!Detach(context.registry, record.entity, true, context))
            {
                return;
            }
        }
        AcknowledgeRetainedCleanup(context);
    }

    SceneECS::SceneEcsRuntime* runtime = nullptr;
    ECS::SceneRuntimeId sceneRuntimeId;
    AnimationEcsPoseEvaluator evaluator;
    AnimationEcsPhysicsBindingValidator physicsBindingValidator;
    AnimationEcsCleanupCallback cleanupCallback;
    ECS::StructuralJournalCursor structuralCursor;
    SceneECS::CleanupRecordCursor cleanupCursor;
    std::vector<Binding> bindings;
    AnimationEcsBridgeRegistrationResult registration =
        AnimationEcsBridgeRegistrationResult::InvalidRuntime;
    uint64 structuralContinuityLossCount = 0;
    uint64 cleanupContinuityLossCount = 0;
    uint64 authoritativeReconcileCount = 0;
    uint64 variableEvaluationCount = 0;
    uint64 fixedEvaluationCount = 0;
    uint64 rejectedEvaluationCount = 0;
    uint64 rootMotionReplayCount = 0;
    uint64 rootMotionGapCount = 0;
    uint64 rootMotionSupersededEpochCount = 0;
    uint64 rootMotionPublicationCount = 0;
    bool initialReconcileComplete = false;
    bool processorsEnabled = false;
};

AnimationEcsBridge::AnimationEcsBridge(SceneECS::SceneEcsRuntime& runtime,
                                       AnimationEcsPoseEvaluator evaluator,
                                       AnimationEcsPhysicsBindingValidator physicsBindingValidator,
                                       AnimationEcsCleanupCallback cleanupCallback)
    : m_state(std::make_shared<State>(runtime, std::move(evaluator),
                                      std::move(physicsBindingValidator),
                                      std::move(cleanupCallback)))
{
}

AnimationEcsBridge::~AnimationEcsBridge() = default;

AnimationEcsBridgeRegistrationResult AnimationEcsBridge::RegisterProcessors()
{
    if (m_state == nullptr || m_state->runtime == nullptr || !m_state->sceneRuntimeId.IsValid())
    {
        return AnimationEcsBridgeRegistrationResult::InvalidRuntime;
    }
    if (m_state->registration == AnimationEcsBridgeRegistrationResult::Registered)
    {
        return AnimationEcsBridgeRegistrationResult::AlreadyRegistered;
    }

    const std::string prefix = "AnimationEcsBridge." +
                               std::to_string(m_state->sceneRuntimeId.GetValue()) + ".";
    const std::weak_ptr<State> weakState = m_state;
    const auto makeDescriptor = [weakState, prefix](
                                    std::string suffix,
                                    ECS::ProcessorPhase phase,
                                    ECS::ProcessorStepMode stepMode,
                                    std::vector<std::type_index> reads,
                                    std::vector<std::type_index> writes,
                                    std::function<void(State&, ECS::ProcessorExecutionContext&)> run)
    {
        ECS::ProcessorDescriptor descriptor;
        descriptor.name = prefix + std::move(suffix);
        descriptor.phase = phase;
        descriptor.group = phase == ECS::ProcessorPhase::Gameplay ? "AnimationPrePhysics" : "";
        descriptor.groupOrder = phase == ECS::ProcessorPhase::Gameplay ? 100u : 0u;
        descriptor.stepMode = stepMode;
        descriptor.access.reads = std::move(reads);
        descriptor.access.writes = std::move(writes);
        descriptor.access.resourceWrites = {std::type_index(typeid(AnimationEcsBridge))};
        descriptor.runWithContext = [weakState, run = std::move(run)](
                                        ECS::ProcessorExecutionContext& context)
        {
            if (const std::shared_ptr<State> state = weakState.lock())
            {
                if (state->processorsEnabled)
                {
                    run(*state, context);
                }
            }
        };
        return descriptor;
    };

    const std::vector<std::type_index> bindingReads = {
        std::type_index(typeid(SceneECS::AnimationSkeletonBinding)),
        std::type_index(typeid(SceneECS::Animator)),
        std::type_index(typeid(SceneECS::AnimationPoseState)),
        std::type_index(typeid(SceneECS::EntityLifecycleState)),
    };
    const std::vector<std::type_index> bindingWrites = {
        std::type_index(typeid(SceneECS::AnimationPoseState)),
    };

    std::vector<ECS::ProcessorDescriptor> descriptors;
    try
    {
        descriptors.reserve(7u);
        descriptors.push_back(makeDescriptor(
            "BeginSimulationReconcile", ECS::ProcessorPhase::BeginSimulation,
            ECS::ProcessorStepMode::Variable, bindingReads, bindingWrites,
            [](State& state, ECS::ProcessorExecutionContext& context)
            {
                state.Reconcile(context);
            }));
        descriptors.push_back(makeDescriptor(
            "BeforeFixedStepReconcile", ECS::ProcessorPhase::BeforeFixedStep,
            ECS::ProcessorStepMode::Fixed, bindingReads, bindingWrites,
            [](State& state, ECS::ProcessorExecutionContext& context)
            {
                state.Reconcile(context);
            }));
        descriptors.push_back(makeDescriptor(
            "AnimationPrePhysics", ECS::ProcessorPhase::Gameplay,
            ECS::ProcessorStepMode::Variable, bindingReads, bindingWrites,
            [](State& state, ECS::ProcessorExecutionContext& context)
            {
                state.Evaluate(context, false);
            }));
        descriptors.push_back(makeDescriptor(
            "FixedAnimation", ECS::ProcessorPhase::FixedAnimation,
            ECS::ProcessorStepMode::Fixed, bindingReads, bindingWrites,
            [](State& state, ECS::ProcessorExecutionContext& context)
            {
                state.Evaluate(context, true);
            }));
        descriptors.push_back(makeDescriptor(
            "SkinningSnapshot", ECS::ProcessorPhase::Feature,
            ECS::ProcessorStepMode::Variable,
            {std::type_index(typeid(SceneECS::Mesh)),
             std::type_index(typeid(SceneECS::SkinnedMeshBinding)),
             std::type_index(typeid(SceneECS::AnimationSkeletonBinding)),
             std::type_index(typeid(SceneECS::Animator)),
             std::type_index(typeid(SceneECS::AnimationPoseState)),
             std::type_index(typeid(SceneECS::EntityLifecycleState))},
            {},
            [](State& state, ECS::ProcessorExecutionContext& context)
            {
                state.PublishSkinningSnapshots(context);
            }));
        descriptors.push_back(makeDescriptor(
            "RootMotion", ECS::ProcessorPhase::RootMotion,
            ECS::ProcessorStepMode::Fixed,
            {std::type_index(typeid(SceneECS::AnimationSkeletonBinding)),
             std::type_index(typeid(SceneECS::Animator)),
             std::type_index(typeid(SceneECS::AnimationPoseState)),
             std::type_index(typeid(SceneECS::RootMotionIntent)),
             std::type_index(typeid(SceneECS::EntityLifecycleState))},
            {std::type_index(typeid(SceneECS::AnimationPoseState)),
             std::type_index(typeid(SceneECS::RootMotionIntent))},
            [](State& state, ECS::ProcessorExecutionContext& context)
            {
                state.PublishRootMotion(context);
            }));
        descriptors.push_back(makeDescriptor(
            "EndFrameCleanup", ECS::ProcessorPhase::EndFrameCleanup,
            ECS::ProcessorStepMode::Variable,
            {std::type_index(typeid(SceneECS::EntityLifecycleState)),
             std::type_index(typeid(SceneECS::SkinnedMeshBinding))},
            {std::type_index(typeid(SceneECS::AnimationPoseState))},
            [](State& state, ECS::ProcessorExecutionContext& context)
            {
                state.Cleanup(context);
            }));
    }
    catch (...)
    {
        m_state->registration = AnimationEcsBridgeRegistrationResult::ProcessorRegistrationFailed;
        return m_state->registration;
    }

    const bool registered = m_state->runtime->RegisterProcessors(std::move(descriptors));
    m_state->processorsEnabled = registered;
    m_state->registration = registered ? AnimationEcsBridgeRegistrationResult::Registered :
                                         AnimationEcsBridgeRegistrationResult::ProcessorRegistrationFailed;
    return m_state->registration;
}

bool AnimationEcsBridge::IsRegistered() const
{
    return m_state != nullptr &&
           m_state->registration == AnimationEcsBridgeRegistrationResult::Registered;
}

ECS::SceneRuntimeId AnimationEcsBridge::GetSceneRuntimeId() const
{
    return m_state != nullptr ? m_state->sceneRuntimeId : ECS::SceneRuntimeId{};
}

AnimationRootMotionPhysicsBindingResult AnimationEcsBridge::BindRootMotionPhysicsBody(
    ECS::SceneRuntimeId sceneRuntimeId,
    ECS::EntityHandle entity,
    uint64 physicsBodyHandlePacked)
{
    if (m_state == nullptr || sceneRuntimeId != m_state->sceneRuntimeId)
    {
        return AnimationRootMotionPhysicsBindingResult::ForeignScene;
    }
    const ECS::Registry& registry = m_state->runtime->GetRegistry();
    const SceneECS::EntityLifecycleState* lifecycle =
        entity.IsValid() ? registry.TryGet<SceneECS::EntityLifecycleState>(entity) : nullptr;
    if (!entity.IsValid() || !registry.IsAlive(entity) || lifecycle == nullptr ||
        lifecycle->phase != SceneECS::EntityLifecyclePhase::Alive ||
        !registry.IsEnabled(entity))
    {
        return AnimationRootMotionPhysicsBindingResult::InvalidEntity;
    }
    if (physicsBodyHandlePacked == 0)
    {
        return AnimationRootMotionPhysicsBindingResult::InvalidPhysicsHandle;
    }
    const SceneECS::AnimationSkeletonBinding* skeleton = nullptr;
    const SceneECS::Animator* animator = nullptr;
    const SceneECS::AnimationPoseState* poseState = nullptr;
    const SceneECS::EntityLifecycleState* eligibleLifecycle = nullptr;
    SceneECS::AnimationBindingStatus eligibilityFailure;
    if (!m_state->IsEligible(registry, entity, skeleton, animator, poseState,
                             eligibleLifecycle, eligibilityFailure))
    {
        return AnimationRootMotionPhysicsBindingResult::AnimationEntityNotBound;
    }
    if (!m_state->physicsBindingValidator)
    {
        return AnimationRootMotionPhysicsBindingResult::PhysicsValidationUnavailable;
    }
    try
    {
        if (!m_state->physicsBindingValidator(sceneRuntimeId, entity, physicsBodyHandlePacked))
        {
            return AnimationRootMotionPhysicsBindingResult::InvalidPhysicsHandle;
        }
    }
    catch (...)
    {
        return AnimationRootMotionPhysicsBindingResult::InvalidPhysicsHandle;
    }
    const uint32 index = m_state->FindBindingIndex(entity);
    if (index == RVX_INVALID_INDEX ||
        m_state->bindings[index].awaitingCleanupAcknowledgement)
    {
        return AnimationRootMotionPhysicsBindingResult::AnimationEntityNotBound;
    }
    State::Binding& binding = m_state->bindings[index];
    if (binding.physicsBodyHandlePacked != physicsBodyHandlePacked)
    {
        const SceneECS::RootMotionIntent* pendingIntent =
            registry.TryGet<SceneECS::RootMotionIntent>(entity);
        if (binding.physicsBodyHandlePacked != 0 && pendingIntent != nullptr &&
            pendingIntent->pending &&
            pendingIntent->sceneRuntimeIdValue == sceneRuntimeId.GetValue() &&
            pendingIntent->targetEntity == entity &&
            pendingIntent->physicsBodyHandlePacked == binding.physicsBodyHandlePacked)
        {
            // The new exact body establishes a distinct source-sequence epoch.
            // Preserve the old fragment until the next RootMotion phase replaces
            // it, but do not let that superseded epoch backpressure B.
            ++m_state->rootMotionSupersededEpochCount;
        }
        binding.physicsBodyHandlePacked = physicsBodyHandlePacked;
        binding.rootMotionSequenceBaselinePending = true;
    }
    return AnimationRootMotionPhysicsBindingResult::Applied;
}

AnimationRootMotionPhysicsBindingResult AnimationEcsBridge::UnbindRootMotionPhysicsBody(
    ECS::SceneRuntimeId sceneRuntimeId,
    ECS::EntityHandle entity,
    uint64 expectedPhysicsBodyHandlePacked)
{
    if (m_state == nullptr || sceneRuntimeId != m_state->sceneRuntimeId)
    {
        return AnimationRootMotionPhysicsBindingResult::ForeignScene;
    }
    if (!entity.IsValid() || !m_state->runtime->GetRegistry().IsAlive(entity))
    {
        return AnimationRootMotionPhysicsBindingResult::InvalidEntity;
    }
    if (expectedPhysicsBodyHandlePacked == 0)
    {
        return AnimationRootMotionPhysicsBindingResult::InvalidPhysicsHandle;
    }
    const uint32 index = m_state->FindBindingIndex(entity);
    if (index == RVX_INVALID_INDEX)
    {
        return AnimationRootMotionPhysicsBindingResult::AnimationEntityNotBound;
    }
    if (m_state->bindings[index].physicsBodyHandlePacked != expectedPhysicsBodyHandlePacked)
    {
        return AnimationRootMotionPhysicsBindingResult::BindingMismatch;
    }
    m_state->bindings[index].physicsBodyHandlePacked = 0;
    m_state->bindings[index].rootMotionSequenceBaselinePending = false;
    return AnimationRootMotionPhysicsBindingResult::Applied;
}

std::optional<AnimationEcsPoseSnapshot> AnimationEcsBridge::GetPoseSnapshot(
    ECS::SceneRuntimeId sceneRuntimeId,
    ECS::EntityHandle entity) const
{
    if (m_state == nullptr || sceneRuntimeId != m_state->sceneRuntimeId || !entity.IsValid())
    {
        return std::nullopt;
    }
    const ECS::Registry& registry = m_state->runtime->GetRegistry();
    const SceneECS::EntityLifecycleState* lifecycle =
        registry.TryGet<SceneECS::EntityLifecycleState>(entity);
    const SceneECS::AnimationSkeletonBinding* skeleton =
        registry.TryGet<SceneECS::AnimationSkeletonBinding>(entity);
    const SceneECS::AnimationPoseState* poseState =
        registry.TryGet<SceneECS::AnimationPoseState>(entity);
    if (!registry.IsAlive(entity) || lifecycle == nullptr || skeleton == nullptr ||
        poseState == nullptr ||
        lifecycle->phase != SceneECS::EntityLifecyclePhase::Alive ||
        !registry.IsEnabled(entity) ||
        !registry.IsFragmentEnabled<SceneECS::AnimationSkeletonBinding>(entity) ||
        !registry.IsFragmentEnabled<SceneECS::Animator>(entity) ||
        !registry.IsFragmentEnabled<SceneECS::AnimationPoseState>(entity))
    {
        return std::nullopt;
    }
    const uint32 index = m_state->FindBindingIndex(entity);
    if (index == RVX_INVALID_INDEX)
    {
        return std::nullopt;
    }
    const State::Binding& binding = m_state->bindings[index];
    // A failed new evaluation must not hide the last atomically committed
    // palette.  Pending destruction and an unbound entry are the two states
    // in which no render/consumer snapshot may escape this bridge.
    if (binding.poseSequence == 0 || binding.paletteRevision == 0 ||
        binding.skeletonWriteVersion !=
            registry.GetFragmentWriteVersion<SceneECS::AnimationSkeletonBinding>(entity) ||
        binding.palette.size() != skeleton->boneCount ||
        poseState->poseSequence != binding.poseSequence ||
        poseState->paletteRevision != binding.paletteRevision ||
        poseState->paletteBoneCount != skeleton->boneCount ||
        binding.status == SceneECS::AnimationBindingStatus::Unbound ||
        binding.status == SceneECS::AnimationBindingStatus::PendingDestroy)
    {
        return std::nullopt;
    }
    try
    {
        return AnimationEcsPoseSnapshot{
            .sceneRuntimeId = m_state->sceneRuntimeId,
            .entity = binding.entity,
            .poseSequence = binding.poseSequence,
            .paletteRevision = binding.paletteRevision,
            .skinningPalette = binding.palette,
        };
    }
    catch (...)
    {
        return std::nullopt;
    }
}

AnimationEcsBridgeDiagnosticsSnapshot AnimationEcsBridge::GetDiagnosticsSnapshot() const
{
    AnimationEcsBridgeDiagnosticsSnapshot snapshot;
    if (m_state == nullptr)
    {
        return snapshot;
    }

    snapshot.sceneRuntimeId = m_state->sceneRuntimeId;
    snapshot.registration = m_state->registration;
    snapshot.structuralContinuityLossCount = m_state->structuralContinuityLossCount;
    snapshot.cleanupContinuityLossCount = m_state->cleanupContinuityLossCount;
    snapshot.authoritativeReconcileCount = m_state->authoritativeReconcileCount;
    snapshot.variableEvaluationCount = m_state->variableEvaluationCount;
    snapshot.fixedEvaluationCount = m_state->fixedEvaluationCount;
    snapshot.rejectedEvaluationCount = m_state->rejectedEvaluationCount;
    snapshot.rootMotionReplayCount = m_state->rootMotionReplayCount;
    snapshot.rootMotionGapCount = m_state->rootMotionGapCount;
    snapshot.rootMotionSupersededEpochCount = m_state->rootMotionSupersededEpochCount;
    snapshot.rootMotionPublicationCount = m_state->rootMotionPublicationCount;
    snapshot.bindings.reserve(m_state->bindings.size());
    for (const State::Binding& binding : m_state->bindings)
    {
        snapshot.bindings.push_back({
            .entity = binding.entity,
            .status = binding.status,
            .poseSequence = binding.poseSequence,
            .publishedRootMotionSequence = binding.latestRootMotionSequence,
            .consumedRootMotionSequence = binding.consumedRootMotionSequence,
            .physicsBodyHandlePacked = binding.physicsBodyHandlePacked,
            .awaitingCleanupAcknowledgement = binding.awaitingCleanupAcknowledgement,
        });
        if (binding.status == SceneECS::AnimationBindingStatus::Active)
        {
            ++snapshot.activeBindingCount;
        }
        if (binding.awaitingCleanupAcknowledgement)
        {
            ++snapshot.pendingCleanupCount;
        }
    }
    return snapshot;
}
} // namespace RVX::AnimationSceneAdapters
