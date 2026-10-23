#include "AnimationSceneAdapters/ECS/ResourceAnimationEcsEvaluator.h"

#include "Animation/Core/Interpolation.h"
#include "Animation/Runtime/AnimationEvaluator.h"
#include "Animation/Runtime/RootMotion.h"
#include "Animation/Runtime/SkeletonPose.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <utility>

namespace RVX::AnimationSceneAdapters
{
namespace
{
    constexpr uint32 MaximumPaletteBoneCount = 4096;

    struct BindingKey
    {
        ECS::SceneRuntimeId sceneRuntimeId;
        ECS::EntityHandle entity = ECS::EntityHandle::Invalid();

        [[nodiscard]] friend bool operator==(const BindingKey&, const BindingKey&) = default;

        [[nodiscard]] friend bool operator<(const BindingKey& left, const BindingKey& right)
        {
            if (left.sceneRuntimeId.GetValue() != right.sceneRuntimeId.GetValue())
            {
                return left.sceneRuntimeId.GetValue() < right.sceneRuntimeId.GetValue();
            }
            return left.entity < right.entity;
        }
    };

    struct PlaybackState
    {
        uint64 animationAssetValue = 0;
        uint32 animationClipOrdinal = 0;
        int64 unwrappedTimeUs = 0;
        uint64 committedEvaluationCount = 0;
        uint64 lastVariableFrameSequence = 0;
        uint64 lastFixedStepSequence = 0;
        std::shared_ptr<const Resource::AnimationResource> resource;
    };

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

    [[nodiscard]] bool IsValidRequest(const AnimationEcsEvaluationRequest& request)
    {
        return request.sceneRuntimeId.IsValid() && request.entity.IsValid() &&
               request.skeleton.animationAssetValue != 0 &&
               request.skeleton.boneCount != 0 &&
               request.skeleton.boneCount <= MaximumPaletteBoneCount &&
               IsValidEvaluationMode(request.animator.evaluationMode) &&
               std::isfinite(request.animator.playbackRate) &&
               request.animator.playbackRate >= 0.0f &&
               std::isfinite(request.deltaSeconds) && request.deltaSeconds >= 0.0;
    }

    /**
     * @brief Permit the bridge to materialize a paused fixed animator's first palette during
     *        variable gameplay without consuming a fixed simulation step.
     *
     * The bridge emits this exact request only when a paused binding has no coherent palette.
     * It is intentionally narrower than accepting an arbitrary zero fixed-step sequence: the
     * request must be paused, carry no variable time delta, and be anchored to a non-zero
     * variable-frame sequence.  Fixed playback sequencing remains fail-closed everywhere else.
     */
    [[nodiscard]] bool IsPausedVariablePaletteInitialization(
        const AnimationEcsEvaluationRequest& request)
    {
        return request.animator.evaluationMode == SceneECS::AnimationEvaluationMode::Fixed &&
               !request.animator.playing && request.deltaSeconds == 0.0 &&
               request.frameSequence != 0 && request.fixedStepSequence == 0;
    }

    [[nodiscard]] std::optional<Animation::TimeUs> ComputeDeltaTimeUs(
        const AnimationEcsEvaluationRequest& request)
    {
        long double baseDeltaUs = 0.0L;
        if (request.animator.evaluationMode == SceneECS::AnimationEvaluationMode::Fixed)
        {
            if (request.fixedStepSequence == 0)
            {
                if (!IsPausedVariablePaletteInitialization(request))
                {
                    return std::nullopt;
                }
            }
            else
            {
                const Animation::TimeUs fixedDeltaUs =
                    Animation::Fixed60HzStepDeltaUs(request.fixedStepSequence);
                if (fixedDeltaUs <= 0)
                {
                    return std::nullopt;
                }
                baseDeltaUs = static_cast<long double>(fixedDeltaUs);
            }
        }
        else
        {
            baseDeltaUs = static_cast<long double>(request.deltaSeconds) *
                          static_cast<long double>(Animation::kMicrosecondsPerSecond);
        }

        const long double scaledDeltaUs = baseDeltaUs *
                                          static_cast<long double>(request.animator.playbackRate);
        if (!std::isfinite(scaledDeltaUs) || scaledDeltaUs < 0.0L ||
            scaledDeltaUs > static_cast<long double>(std::numeric_limits<Animation::TimeUs>::max()))
        {
            return std::nullopt;
        }
        return static_cast<Animation::TimeUs>(std::llround(scaledDeltaUs));
    }

    [[nodiscard]] Animation::AnimationClip::ConstPtr SelectClip(
        const Resource::AnimationResource& resource,
        uint32 ordinal)
    {
        const Resource::AnimationResource::AnimationClipMap& clips = resource.GetClips();
        if (ordinal >= clips.size())
        {
            return nullptr;
        }

        auto selected = clips.begin();
        std::advance(selected, static_cast<std::ptrdiff_t>(ordinal));
        return selected->second;
    }

    [[nodiscard]] bool IsCompatiblePayload(const Resource::AnimationResource& resource,
                                           const Animation::AnimationClip& clip,
                                           uint32 expectedBoneCount)
    {
        const Animation::Skeleton::ConstPtr skeleton = resource.GetSkeleton();
        if (!skeleton || !skeleton->Validate() ||
            skeleton->GetBoneCount() != expectedBoneCount ||
            clip.skeleton.get() != skeleton.get() || clip.duration < 0)
        {
            return false;
        }

        return clip.ValidateAgainstSkeleton(*skeleton).empty();
    }

    [[nodiscard]] bool HasQualifiedRootMotion(const Animation::AnimationClip& clip,
                                              const Animation::Skeleton& skeleton)
    {
        if (!clip.hasRootMotion || clip.defaultWrapMode != Animation::WrapMode::Loop ||
            clip.duration <= 0 || clip.rootMotionBoneName.empty() ||
            skeleton.FindBoneIndex(clip.rootMotionBoneName) < 0)
        {
            return false;
        }

        return std::any_of(
            clip.transformTracks.begin(), clip.transformTracks.end(),
            [&clip](const Animation::TransformTrack& track)
            {
                return track.targetType == Animation::TrackTargetType::Bone &&
                       track.targetName == clip.rootMotionBoneName && !track.IsEmpty();
            });
    }

    [[nodiscard]] std::optional<Animation::RootMotionDelta> EvaluateRootMotion(
        const Animation::AnimationClip& clip,
        Animation::Skeleton::ConstPtr skeleton,
        Animation::TimeUs previousTimeUs,
        Animation::TimeUs currentTimeUs)
    {
        if (!skeleton || previousTimeUs < 0 || currentTimeUs < previousTimeUs ||
            !HasQualifiedRootMotion(clip, *skeleton))
        {
            return std::nullopt;
        }

        Animation::RootMotionExtractor extractor(std::move(skeleton));
        Animation::RootMotionConfig config;
        config.translationMode = Animation::RootMotionTranslationMode::XYZ;
        config.rotationMode = Animation::RootMotionRotationMode::Full;
        config.rootBoneName = clip.rootMotionBoneName;
        config.zeroRootBone = true;
        extractor.SetConfig(config);

        const Animation::RootMotionDelta delta = extractor.ExtractLoopingDelta(
            clip, previousTimeUs, currentTimeUs);
        if (!delta.valid || !IsFinite(delta.deltaTranslation) || !IsFinite(delta.deltaRotation))
        {
            return std::nullopt;
        }
        return delta;
    }

} // namespace

struct ResourceAnimationEcsEvaluator::State
{
    explicit State(ResourceAnimationEcsResolver resolverIn,
                   ResourceAnimationEcsEntityValidator entityValidatorIn)
        : resolver(std::move(resolverIn))
        , entityValidator(std::move(entityValidatorIn))
    {
    }

    mutable std::mutex mutex;
    ResourceAnimationEcsResolver resolver;
    ResourceAnimationEcsEntityValidator entityValidator;
    std::map<BindingKey, PlaybackState> playbacks;
    uint64 acceptedEvaluationCount = 0;
    uint64 rejectedEvaluationCount = 0;
    bool shutdown = false;
};

ResourceAnimationEcsEvaluator::ResourceAnimationEcsEvaluator(
    ResourceAnimationEcsResolver resolver,
    ResourceAnimationEcsEntityValidator entityValidator)
    : m_state(std::make_shared<State>(std::move(resolver), std::move(entityValidator)))
{
}

ResourceAnimationEcsEvaluator::~ResourceAnimationEcsEvaluator()
{
    Shutdown();
}

std::optional<AnimationEcsEvaluatedPose> ResourceAnimationEcsEvaluator::Evaluate(
    const AnimationEcsEvaluationRequest& request) const
{
    return EvaluateState(m_state, request);
}

AnimationEcsPoseEvaluator ResourceAnimationEcsEvaluator::CreatePoseEvaluator() const
{
    const std::shared_ptr<State> state = m_state;
    return [state](const AnimationEcsEvaluationRequest& request)
    {
        return EvaluateState(state, request);
    };
}

AnimationEcsCleanupCallback ResourceAnimationEcsEvaluator::CreateCleanupCallback() const
{
    const std::shared_ptr<State> state = m_state;
    return [state](ECS::SceneRuntimeId sceneRuntimeId, ECS::EntityHandle entity)
    {
        if (!state || !sceneRuntimeId.IsValid() || !entity.IsValid())
        {
            return false;
        }

        std::lock_guard lock(state->mutex);
        // Absence is a successful, idempotent cleanup.  It covers evaluator
        // rejection before the first committed pose and a retry after Scene
        // accepted a different cleanup domain but rejected Animation.
        state->playbacks.erase({.sceneRuntimeId = sceneRuntimeId, .entity = entity});
        return true;
    };
}

std::optional<AnimationEcsEvaluatedPose> ResourceAnimationEcsEvaluator::EvaluateState(
    const std::shared_ptr<State>& state,
    const AnimationEcsEvaluationRequest& request)
{
    const auto reject = [&state]()
    {
        if (state)
        {
            std::lock_guard lock(state->mutex);
            ++state->rejectedEvaluationCount;
        }
        return std::optional<AnimationEcsEvaluatedPose>{};
    };

    if (!state || !IsValidRequest(request))
    {
        return reject();
    }

    {
        std::lock_guard lock(state->mutex);
        if (state->shutdown)
        {
            ++state->rejectedEvaluationCount;
            return std::nullopt;
        }
    }

    bool isLive = true;
    if (state->entityValidator)
    {
        try
        {
            isLive = state->entityValidator(request.sceneRuntimeId, request.entity);
        }
        catch (...)
        {
            isLive = false;
        }
    }
    if (!isLive || !state->resolver)
    {
        return reject();
    }

    std::shared_ptr<const Resource::AnimationResource> resource;
    try
    {
        resource = state->resolver(request.skeleton.animationAssetValue);
    }
    catch (...)
    {
        return reject();
    }
    if (!resource)
    {
        return reject();
    }

    const Animation::AnimationClip::ConstPtr clip = SelectClip(
        *resource, request.skeleton.animationClipOrdinal);
    if (!clip || !IsCompatiblePayload(*resource, *clip, request.skeleton.boneCount))
    {
        return reject();
    }

    const std::optional<Animation::TimeUs> deltaTimeUs = ComputeDeltaTimeUs(request);
    if (!deltaTimeUs.has_value())
    {
        return reject();
    }

    const BindingKey key{.sceneRuntimeId = request.sceneRuntimeId, .entity = request.entity};
    PlaybackState previousState;
    bool hasPreviousState = false;
    {
        std::lock_guard lock(state->mutex);
        if (state->shutdown)
        {
            ++state->rejectedEvaluationCount;
            return std::nullopt;
        }
        const auto existing = state->playbacks.find(key);
        if (existing != state->playbacks.end())
        {
            previousState = existing->second;
            hasPreviousState = true;
        }
    }

    const bool sameSource = hasPreviousState &&
                            previousState.animationAssetValue ==
                                request.skeleton.animationAssetValue &&
                            previousState.animationClipOrdinal ==
                                request.skeleton.animationClipOrdinal &&
                            previousState.resource.get() == resource.get();
    const bool pausedVariablePaletteInitialization =
        IsPausedVariablePaletteInitialization(request);
    const Animation::TimeUs previousTimeUs = sameSource ? previousState.unwrappedTimeUs : 0;
    if (previousTimeUs < 0 || *deltaTimeUs >
                                  std::numeric_limits<Animation::TimeUs>::max() - previousTimeUs)
    {
        return reject();
    }

    if (sameSource && request.animator.evaluationMode ==
                          SceneECS::AnimationEvaluationMode::Fixed &&
        !pausedVariablePaletteInitialization &&
        previousState.lastFixedStepSequence != 0 &&
        request.fixedStepSequence <= previousState.lastFixedStepSequence)
    {
        return reject();
    }
    if (sameSource && request.animator.evaluationMode ==
                          SceneECS::AnimationEvaluationMode::VariablePrePhysics &&
        previousState.lastVariableFrameSequence != 0 &&
        request.frameSequence <= previousState.lastVariableFrameSequence)
    {
        return reject();
    }

    const Animation::TimeUs currentTimeUs = previousTimeUs + *deltaTimeUs;
    Animation::SkeletonPose pose(resource->GetSkeleton());
    Animation::AnimationEvaluator evaluator;
    evaluator.SetCacheContext(request.entity.GetPackedValue());
    const Animation::EvaluationResult evaluation = evaluator.Evaluate(*clip, currentTimeUs, pose);
    if (!evaluation.success)
    {
        return reject();
    }

    AnimationEcsEvaluatedPose result;
    if (request.animator.rootMotionEnabled)
    {
        const std::optional<Animation::RootMotionDelta> rootMotion = EvaluateRootMotion(
            *clip, resource->GetSkeleton(), previousTimeUs, currentTimeUs);
        if (!rootMotion.has_value())
        {
            return reject();
        }

        Animation::RootMotionExtractor rootRemoval(resource->GetSkeleton());
        if (!rootRemoval.RemoveRootMotionFromPose(*clip, pose, true))
        {
            return reject();
        }
        result.rootMotionTranslation = rootMotion->deltaTranslation;
        result.rootMotionRotation = rootMotion->deltaRotation;
        // Zero delegates transport sequencing to AnimationEcsBridge's
        // contiguous root-motion publication stream. Pose revisions may also
        // advance for paused palette repair and therefore are not a valid
        // root-motion sequence source.
        result.rootMotionSequence = 0;
    }

    pose.ComputeSkinningMatrices();
    result.skinningPalette = pose.GetSkinningMatrices();
    if (result.skinningPalette.size() != request.skeleton.boneCount ||
        !std::all_of(
            result.skinningPalette.begin(), result.skinningPalette.end(),
            [](const Mat4& matrix) { return IsFinite(matrix); }) ||
        (request.animator.rootMotionEnabled &&
         (!IsFinite(result.rootMotionTranslation) || !IsFinite(result.rootMotionRotation))))
    {
        return reject();
    }

    PlaybackState nextState;
    nextState.animationAssetValue = request.skeleton.animationAssetValue;
    nextState.animationClipOrdinal = request.skeleton.animationClipOrdinal;
    nextState.unwrappedTimeUs = currentTimeUs;
    if (sameSource && previousState.committedEvaluationCount == std::numeric_limits<uint64>::max())
    {
        return reject();
    }
    nextState.committedEvaluationCount = sameSource ?
        previousState.committedEvaluationCount + 1u : 1u;
    nextState.lastVariableFrameSequence = sameSource ?
        previousState.lastVariableFrameSequence : 0;
    nextState.lastFixedStepSequence = sameSource ? previousState.lastFixedStepSequence : 0;
    if (request.animator.evaluationMode == SceneECS::AnimationEvaluationMode::Fixed &&
        !pausedVariablePaletteInitialization)
    {
        nextState.lastFixedStepSequence = request.fixedStepSequence;
    }
    else if (request.animator.evaluationMode ==
             SceneECS::AnimationEvaluationMode::VariablePrePhysics)
    {
        nextState.lastVariableFrameSequence = request.frameSequence;
    }
    nextState.resource = resource;

    {
        std::lock_guard lock(state->mutex);
        if (state->shutdown)
        {
            ++state->rejectedEvaluationCount;
            return std::nullopt;
        }

        const auto current = state->playbacks.find(key);
        const bool currentMatchesObserved =
            (!hasPreviousState && current == state->playbacks.end()) ||
            (hasPreviousState && current != state->playbacks.end() &&
             current->second.committedEvaluationCount == previousState.committedEvaluationCount &&
             current->second.resource.get() == previousState.resource.get());
        if (!currentMatchesObserved)
        {
            ++state->rejectedEvaluationCount;
            return std::nullopt;
        }

        for (auto existing = state->playbacks.begin(); existing != state->playbacks.end();)
        {
            if (existing->first.sceneRuntimeId == request.sceneRuntimeId &&
                existing->first.entity.GetIndex() == request.entity.GetIndex() &&
                existing->first.entity.GetGeneration() != request.entity.GetGeneration())
            {
                existing = state->playbacks.erase(existing);
            }
            else
            {
                ++existing;
            }
        }
        state->playbacks.insert_or_assign(key, std::move(nextState));
        ++state->acceptedEvaluationCount;
    }
    return result;
}

bool ResourceAnimationEcsEvaluator::RemoveBinding(ECS::SceneRuntimeId sceneRuntimeId,
                                                   ECS::EntityHandle entity)
{
    if (!m_state || !sceneRuntimeId.IsValid() || !entity.IsValid())
    {
        return false;
    }
    std::lock_guard lock(m_state->mutex);
    return m_state->playbacks.erase({.sceneRuntimeId = sceneRuntimeId, .entity = entity}) != 0;
}

uint32 ResourceAnimationEcsEvaluator::RemoveScene(ECS::SceneRuntimeId sceneRuntimeId)
{
    if (!m_state || !sceneRuntimeId.IsValid())
    {
        return 0;
    }

    std::lock_guard lock(m_state->mutex);
    uint32 removed = 0;
    for (auto entry = m_state->playbacks.begin(); entry != m_state->playbacks.end();)
    {
        if (entry->first.sceneRuntimeId == sceneRuntimeId)
        {
            entry = m_state->playbacks.erase(entry);
            ++removed;
        }
        else
        {
            ++entry;
        }
    }
    return removed;
}

void ResourceAnimationEcsEvaluator::Shutdown()
{
    if (!m_state)
    {
        return;
    }
    std::lock_guard lock(m_state->mutex);
    m_state->shutdown = true;
    m_state->playbacks.clear();
}

bool ResourceAnimationEcsEvaluator::IsShutdown() const
{
    if (!m_state)
    {
        return true;
    }
    std::lock_guard lock(m_state->mutex);
    return m_state->shutdown;
}

std::optional<ResourceAnimationEcsPlaybackSnapshot>
ResourceAnimationEcsEvaluator::GetPlaybackSnapshot(ECS::SceneRuntimeId sceneRuntimeId,
                                                    ECS::EntityHandle entity) const
{
    if (!m_state || !sceneRuntimeId.IsValid() || !entity.IsValid())
    {
        return std::nullopt;
    }

    std::lock_guard lock(m_state->mutex);
    const auto found = m_state->playbacks.find({.sceneRuntimeId = sceneRuntimeId, .entity = entity});
    if (found == m_state->playbacks.end())
    {
        return std::nullopt;
    }
    const PlaybackState& playback = found->second;
    return ResourceAnimationEcsPlaybackSnapshot{
        .sceneRuntimeId = sceneRuntimeId,
        .entity = entity,
        .animationAssetValue = playback.animationAssetValue,
        .animationClipOrdinal = playback.animationClipOrdinal,
        .unwrappedTimeUs = playback.unwrappedTimeUs,
        .committedEvaluationCount = playback.committedEvaluationCount,
        .lastVariableFrameSequence = playback.lastVariableFrameSequence,
        .lastFixedStepSequence = playback.lastFixedStepSequence,
    };
}

ResourceAnimationEcsEvaluatorDiagnosticsSnapshot
ResourceAnimationEcsEvaluator::GetDiagnosticsSnapshot() const
{
    ResourceAnimationEcsEvaluatorDiagnosticsSnapshot snapshot;
    if (!m_state)
    {
        snapshot.shutdown = true;
        return snapshot;
    }

    std::lock_guard lock(m_state->mutex);
    snapshot.activePlaybackCount = static_cast<uint32>(m_state->playbacks.size());
    snapshot.acceptedEvaluationCount = m_state->acceptedEvaluationCount;
    snapshot.rejectedEvaluationCount = m_state->rejectedEvaluationCount;
    snapshot.shutdown = m_state->shutdown;
    return snapshot;
}

ResourceAnimationEcsEvaluatorSceneDiagnosticsSnapshot
ResourceAnimationEcsEvaluator::GetSceneDiagnosticsSnapshot(
    ECS::SceneRuntimeId sceneRuntimeId) const
{
    ResourceAnimationEcsEvaluatorSceneDiagnosticsSnapshot snapshot;
    snapshot.sceneRuntimeId = sceneRuntimeId;
    if (!m_state)
    {
        snapshot.shutdown = true;
        return snapshot;
    }

    std::lock_guard lock(m_state->mutex);
    snapshot.shutdown = m_state->shutdown;
    if (!sceneRuntimeId.IsValid())
    {
        return snapshot;
    }

    for (const auto& [key, playback] : m_state->playbacks)
    {
        static_cast<void>(playback);
        if (key.sceneRuntimeId == sceneRuntimeId)
        {
            ++snapshot.activePlaybackCount;
        }
    }
    return snapshot;
}
} // namespace RVX::AnimationSceneAdapters
