#include "AnimationSceneAdapters/ECS/EcsAnimationAssetService.h"

#include "AnimationSceneAdapters/ECS/ResourceAnimationEcsEvaluator.h"
#include "Resource/ResourceSubsystem.h"
#include "Resource/Types/AnimationResource.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <utility>

namespace RVX::AnimationSceneAdapters
{
namespace
{
    [[nodiscard]] EcsAnimationAssetLoadState ToServiceState(Resource::ResourceLoadState state)
    {
        switch (state)
        {
        case Resource::ResourceLoadState::Queued:
        case Resource::ResourceLoadState::Loading:
        case Resource::ResourceLoadState::AwaitingPublish:
            return EcsAnimationAssetLoadState::Loading;
        case Resource::ResourceLoadState::Ready:
            return EcsAnimationAssetLoadState::Ready;
        case Resource::ResourceLoadState::Failed:
            return EcsAnimationAssetLoadState::Failed;
        case Resource::ResourceLoadState::Cancelled:
            return EcsAnimationAssetLoadState::Cancelled;
        }
        return EcsAnimationAssetLoadState::Failed;
    }

    [[nodiscard]] bool IsValidTopology(const EcsAnimationSkeletonTopology& topology)
    {
        if (topology.bones.empty())
        {
            return false;
        }

        std::unordered_set<std::string> names;
        names.reserve(topology.bones.size());
        for (size_t index = 0; index < topology.bones.size(); ++index)
        {
            const EcsAnimationBoneTopology& bone = topology.bones[index];
            if (bone.name.empty() || bone.parentIndex < -1 ||
                bone.parentIndex >= static_cast<int32>(index) || !names.insert(bone.name).second)
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool HasExactTopology(const Animation::Skeleton& source,
                                        const EcsAnimationSkeletonTopology& target)
    {
        if (source.bones.size() != target.bones.size())
        {
            return false;
        }

        for (size_t index = 0; index < source.bones.size(); ++index)
        {
            if (source.bones[index].name != target.bones[index].name ||
                source.bones[index].parentIndex != target.bones[index].parentIndex)
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool HasExactTransform(const Animation::TransformSample& left,
                                         const Animation::TransformSample& right)
    {
        return left.translation.x == right.translation.x &&
               left.translation.y == right.translation.y &&
               left.translation.z == right.translation.z && left.rotation.w == right.rotation.w &&
               left.rotation.x == right.rotation.x && left.rotation.y == right.rotation.y &&
               left.rotation.z == right.rotation.z && left.scale.x == right.scale.x &&
               left.scale.y == right.scale.y && left.scale.z == right.scale.z;
    }

    [[nodiscard]] bool HasExactMatrix(const Mat4& left, const Mat4& right)
    {
        for (uint32 column = 0; column < 4; ++column)
        {
            for (uint32 row = 0; row < 4; ++row)
            {
                if (left[column][row] != right[column][row])
                {
                    return false;
                }
            }
        }
        return true;
    }

    [[nodiscard]] bool HasExactBindPose(const Animation::Skeleton& source,
                                        const EcsAnimationSkeletonTopology& target)
    {
        for (size_t index = 0; index < source.bones.size(); ++index)
        {
            if (!HasExactTransform(source.bones[index].localBindPose,
                                   target.bones[index].localBindPose) ||
                !HasExactMatrix(source.bones[index].inverseBindPose,
                                target.bones[index].inverseBindPose))
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool HasExactTopology(const Animation::Skeleton& source,
                                        const Animation::Skeleton& target)
    {
        if (source.bones.size() != target.bones.size())
        {
            return false;
        }
        for (size_t index = 0; index < source.bones.size(); ++index)
        {
            if (source.bones[index].name != target.bones[index].name ||
                source.bones[index].parentIndex != target.bones[index].parentIndex)
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool HasExactBindPose(const Animation::Skeleton& source,
                                        const Animation::Skeleton& target)
    {
        if (source.bones.size() != target.bones.size())
        {
            return false;
        }
        for (size_t index = 0; index < source.bones.size(); ++index)
        {
            if (!HasExactTransform(source.bones[index].localBindPose,
                                   target.bones[index].localBindPose) ||
                !HasExactMatrix(source.bones[index].inverseBindPose,
                                target.bones[index].inverseBindPose))
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] std::shared_ptr<const Resource::AnimationResource>
    BridgeIntrusiveAnimation(Resource::ResourceHandle<Resource::AnimationResource> resource)
    {
        if (!resource || !resource->IsLoaded())
        {
            return {};
        }

        struct IntrusiveAnimationLease final
        {
            explicit IntrusiveAnimationLease(Resource::ResourceHandle<Resource::AnimationResource> value)
                : resource(std::move(value))
            {
            }

            Resource::ResourceHandle<Resource::AnimationResource> resource;
        };

        try
        {
            auto lease = std::make_shared<IntrusiveAnimationLease>(std::move(resource));
            return std::shared_ptr<const Resource::AnimationResource>(lease, lease->resource.Get());
        }
        catch (...)
        {
            return {};
        }
    }
} // namespace

struct EcsAnimationAssetService::State
{
    struct Entry
    {
        EcsAnimationAssetLoadRef reference;
        EcsAnimationAssetLoadStatus status;
        Resource::ResourceLoadHandle<Resource::AnimationResource> request;
    };

    explicit State(Resource::ResourceSubsystem& resourcesIn)
        : resources(&resourcesIn)
        , ownerThread(std::this_thread::get_id())
    {
    }

    mutable std::mutex mutex;
    Resource::ResourceSubsystem* resources = nullptr;
    std::thread::id ownerThread;
    HandlePool<EcsAnimationAssetLoadHandle> handles;
    std::vector<std::unique_ptr<Entry>> entries;
    std::map<uint64, std::weak_ptr<const Resource::AnimationResource>> resolverCache;
    bool shutdown = false;

    void PruneExpiredResolverCache()
    {
        std::erase_if(resolverCache,
                      [](const auto& entry) { return entry.second.expired(); });
    }
};

EcsAnimationAssetService::EcsAnimationAssetService(Resource::ResourceSubsystem& resources)
    : m_state(std::make_shared<State>(resources))
{
}

EcsAnimationAssetService::~EcsAnimationAssetService()
{
    Shutdown();
}

EcsAnimationAssetLoadRef EcsAnimationAssetService::Request(
    EcsAnimationAssetLoadDesc desc,
    std::string& outError)
{
    outError.clear();
    const std::shared_ptr<State> state = m_state;
    if (!state || std::this_thread::get_id() != state->ownerThread || desc.path.empty() ||
        !desc.expectedSceneRuntimeId.IsValid())
    {
        outError = "Animation requests require the owner thread, a path, and an exact Scene runtime id.";
        return {};
    }

    std::lock_guard lock(state->mutex);
    if (state->shutdown || state->resources == nullptr)
    {
        outError = "Animation asset service is shut down.";
        return {};
    }

    Resource::ResourceLoadHandle<Resource::AnimationResource> resourceRequest =
        state->resources->RequestAsync<Resource::AnimationResource>(
            desc.path, std::move(desc.resourceOptions));
    if (!resourceRequest.IsValid())
    {
        outError = "ResourceSubsystem rejected the asynchronous AnimationResource request.";
        return {};
    }

    const EcsAnimationAssetLoadHandle handle = state->handles.Allocate();
    try
    {
        if (handle.GetIndex() >= state->entries.size())
        {
            state->entries.resize(static_cast<size_t>(handle.GetIndex()) + 1u);
        }

        auto entry = std::make_unique<State::Entry>();
        entry->reference = {.sceneRuntimeId = desc.expectedSceneRuntimeId, .handle = handle};
        entry->request = std::move(resourceRequest);
        entry->status.sceneRuntimeId = entry->reference.sceneRuntimeId;
        entry->status.request = entry->request.GetSnapshot();
        entry->status.assetKey = entry->status.request.assetKey;
        entry->status.resourceLoadState = entry->status.request.state;
        entry->status.state = ToServiceState(entry->status.request.state);
        state->entries[handle.GetIndex()] = std::move(entry);
        return {.sceneRuntimeId = desc.expectedSceneRuntimeId, .handle = handle};
    }
    catch (...)
    {
        static_cast<void>(state->handles.TryFree(handle));
        outError = "Animation asset service could not allocate a generation-safe request entry.";
        return {};
    }
}

bool EcsAnimationAssetService::Update(ECS::SceneRuntimeId sceneRuntimeId)
{
    const std::shared_ptr<State> state = m_state;
    if (!state || !sceneRuntimeId.IsValid() || std::this_thread::get_id() != state->ownerThread)
    {
        return false;
    }

    std::lock_guard lock(state->mutex);
    if (state->shutdown)
    {
        return false;
    }

    for (const std::unique_ptr<State::Entry>& entry : state->entries)
    {
        if (entry == nullptr || entry->reference.sceneRuntimeId != sceneRuntimeId ||
            entry->status.IsTerminal())
        {
            continue;
        }

        entry->status.request = entry->request.GetSnapshot();
        entry->status.assetKey = entry->status.request.assetKey;
        entry->status.resourceLoadState = entry->status.request.state;
        entry->status.state = ToServiceState(entry->status.request.state);
        if (entry->status.state != EcsAnimationAssetLoadState::Ready)
        {
            entry->status.diagnostic = entry->status.request.error.message;
            continue;
        }

        Resource::ResourceHandle<Resource::AnimationResource> resource = entry->request.TryGet();
        if (!resource || !resource->IsLoaded() || resource->GetId() == Resource::InvalidResourceId ||
            !resource->GetSkeleton())
        {
            entry->status.state = EcsAnimationAssetLoadState::Failed;
            entry->status.diagnostic =
                "Ready animation request did not publish a loaded immutable AnimationResource.";
            continue;
        }

        entry->status.animationAssetValue = resource->GetId();
        entry->status.boneCount = static_cast<uint32>(resource->GetSkeleton()->GetBoneCount());
        entry->status.contentVerification = resource->GetContentVerificationReceipt();
        entry->status.clips.clear();
        uint32 ordinal = 0;
        for (const auto& [name, clip] : resource->GetClips())
        {
            if (!clip)
            {
                entry->status.state = EcsAnimationAssetLoadState::Failed;
                entry->status.diagnostic = "Published AnimationResource contains a null lexical clip.";
                break;
            }
            entry->status.clips.push_back({
                .ordinal = ordinal++,
                .name = name,
                .durationUs = clip->duration,
                .hasRootMotion = clip->hasRootMotion,
                .rootMotionBoneName = clip->rootMotionBoneName,
            });
        }
        if (entry->status.state == EcsAnimationAssetLoadState::Ready)
        {
            entry->status.diagnostic.clear();
        }
    }
    return true;
}

bool EcsAnimationAssetService::Cancel(EcsAnimationAssetLoadRef request)
{
    const std::shared_ptr<State> state = m_state;
    if (!state || !request.IsValid() || std::this_thread::get_id() != state->ownerThread)
    {
        return false;
    }

    std::lock_guard lock(state->mutex);
    if (state->shutdown || !state->handles.IsValid(request.handle) ||
        request.handle.GetIndex() >= state->entries.size())
    {
        return false;
    }
    State::Entry* entry = state->entries[request.handle.GetIndex()].get();
    if (entry == nullptr || entry->reference.sceneRuntimeId != request.sceneRuntimeId ||
        entry->status.state == EcsAnimationAssetLoadState::Cancelled)
    {
        return false;
    }

    static_cast<void>(entry->request.Cancel());
    entry->status.state = EcsAnimationAssetLoadState::Cancelled;
    entry->status.resourceLoadState = Resource::ResourceLoadState::Cancelled;
    entry->status.request.state = Resource::ResourceLoadState::Cancelled;
    entry->status.request.cancellationRequested = true;
    entry->status.request.error = {Resource::ResourceLoadErrorCode::Cancelled,
                                   "Cancelled by the owning ECS Scene."};
    entry->status.diagnostic = entry->status.request.error.message;
    return true;
}

std::optional<EcsAnimationAssetLoadStatus> EcsAnimationAssetService::GetStatus(
    EcsAnimationAssetLoadRef request) const
{
    const std::shared_ptr<State> state = m_state;
    if (!state || !request.IsValid())
    {
        return std::nullopt;
    }

    std::lock_guard lock(state->mutex);
    if (!state->handles.IsValid(request.handle) || request.handle.GetIndex() >= state->entries.size())
    {
        return std::nullopt;
    }
    const State::Entry* entry = state->entries[request.handle.GetIndex()].get();
    return entry != nullptr && entry->reference.sceneRuntimeId == request.sceneRuntimeId ?
               std::optional<EcsAnimationAssetLoadStatus>(entry->status) :
               std::nullopt;
}

bool EcsAnimationAssetService::PrepareForSceneShutdown(ECS::SceneRuntimeId sceneRuntimeId)
{
    const std::shared_ptr<State> state = m_state;
    if (!state || !sceneRuntimeId.IsValid() || std::this_thread::get_id() != state->ownerThread)
    {
        return false;
    }

    std::vector<EcsAnimationAssetLoadRef> requests;
    {
        std::lock_guard lock(state->mutex);
        if (state->shutdown)
        {
            return false;
        }
        for (const std::unique_ptr<State::Entry>& entry : state->entries)
        {
            if (entry != nullptr && entry->reference.sceneRuntimeId == sceneRuntimeId &&
                entry->status.state != EcsAnimationAssetLoadState::Cancelled)
            {
                requests.push_back(entry->reference);
            }
        }
    }
    for (const EcsAnimationAssetLoadRef request : requests)
    {
        static_cast<void>(Cancel(request));
    }

    std::lock_guard lock(state->mutex);
    if (state->shutdown)
    {
        return false;
    }
    for (std::unique_ptr<State::Entry>& entry : state->entries)
    {
        if (entry == nullptr || entry->reference.sceneRuntimeId != sceneRuntimeId)
        {
            continue;
        }
        if (entry->status.state != EcsAnimationAssetLoadState::Cancelled ||
            !state->handles.TryFree(entry->reference.handle))
        {
            return false;
        }
        entry.reset();
    }
    state->PruneExpiredResolverCache();
    return true;
}

EcsAnimationAssetServiceSceneDiagnosticsSnapshot
EcsAnimationAssetService::GetSceneDiagnosticsSnapshot(ECS::SceneRuntimeId sceneRuntimeId) const
{
    EcsAnimationAssetServiceSceneDiagnosticsSnapshot diagnostics;
    diagnostics.sceneRuntimeId = sceneRuntimeId;
    const std::shared_ptr<State> state = m_state;
    if (!state)
    {
        diagnostics.shutdown = true;
        diagnostics.lastDiagnostic = "Animation asset service has been destroyed.";
        return diagnostics;
    }

    std::lock_guard lock(state->mutex);
    diagnostics.shutdown = state->shutdown;
    for (const std::unique_ptr<State::Entry>& entry : state->entries)
    {
        if (entry == nullptr || entry->reference.sceneRuntimeId != sceneRuntimeId)
        {
            continue;
        }
        ++diagnostics.requestCount;
        switch (entry->status.state)
        {
        case EcsAnimationAssetLoadState::Requested:
        case EcsAnimationAssetLoadState::Loading:
            ++diagnostics.activeRequestCount;
            break;
        case EcsAnimationAssetLoadState::Ready:
            ++diagnostics.readyRequestCount;
            break;
        case EcsAnimationAssetLoadState::Failed:
            ++diagnostics.failedRequestCount;
            break;
        case EcsAnimationAssetLoadState::Cancelled:
            ++diagnostics.cancelledRequestCount;
            break;
        }
        if (!entry->status.diagnostic.empty())
        {
            diagnostics.lastDiagnostic = entry->status.diagnostic;
        }
    }
    return diagnostics;
}

EcsAnimationAssetServiceDiagnosticsSnapshot EcsAnimationAssetService::GetDiagnosticsSnapshot() const
{
    EcsAnimationAssetServiceDiagnosticsSnapshot diagnostics;
    const std::shared_ptr<State> state = m_state;
    if (!state)
    {
        diagnostics.shutdown = true;
        return diagnostics;
    }

    std::lock_guard lock(state->mutex);
    diagnostics.shutdown = state->shutdown;
    for (const std::unique_ptr<State::Entry>& entry : state->entries)
    {
        if (entry != nullptr)
        {
            ++diagnostics.requestCount;
            if (!entry->status.IsTerminal())
            {
                ++diagnostics.activeRequestCount;
            }
        }
    }
    diagnostics.weakResolverCacheEntryCount = static_cast<uint32>(state->resolverCache.size());
    return diagnostics;
}

ResourceAnimationEcsResolver EcsAnimationAssetService::CreateResolver() const
{
    const std::weak_ptr<State> weakState = m_state;
    return [weakState](uint64 animationAssetValue)
    {
        return ResolveAnimationAsset(weakState.lock(), animationAssetValue);
    };
}

EcsAnimationBindingPreparationResult EcsAnimationAssetService::PrepareCompatibleAnimationBinding(
    EcsAnimationAssetLoadRef request,
    const SceneECS::AnimationSkeletonBinding& targetBinding,
    const EcsAnimationSkeletonTopology& targetTopology,
    uint32 animationClipOrdinal) const
{
    EcsAnimationBindingPreparationResult result;
    if (targetBinding.animationAssetValue != 0)
    {
        return PrepareCompatibleAnimationBinding(request, targetBinding, animationClipOrdinal);
    }
    const std::optional<EcsAnimationAssetLoadStatus> status = GetStatus(request);
    if (!status.has_value())
    {
        result.code = EcsAnimationBindingPreparationCode::InvalidRequestRef;
        result.diagnostic = "Animation binding preparation rejected an invalid or foreign request reference.";
        return result;
    }
    if (IsShutdown())
    {
        result.code = EcsAnimationBindingPreparationCode::ServiceShutDown;
        result.diagnostic = "Animation binding preparation rejected a shut down service.";
        return result;
    }
    if (status->state != EcsAnimationAssetLoadState::Ready || status->animationAssetValue == 0)
    {
        result.code = EcsAnimationBindingPreparationCode::RequestNotReady;
        result.diagnostic = "Animation binding preparation requires an exact Ready request.";
        return result;
    }
    if (!IsValidTopology(targetTopology))
    {
        result.code = EcsAnimationBindingPreparationCode::InvalidTargetTopology;
        result.diagnostic = "Animation binding preparation received an invalid skeleton topology value.";
        return result;
    }
    if (targetBinding.boneCount != targetTopology.bones.size() ||
        status->boneCount != targetTopology.bones.size())
    {
        result.code = EcsAnimationBindingPreparationCode::BoneCountMismatch;
        result.diagnostic = "Animation binding bone counts do not match the complete target topology.";
        return result;
    }
    if (animationClipOrdinal >= status->clips.size())
    {
        result.code = EcsAnimationBindingPreparationCode::ClipOrdinalOutOfRange;
        result.diagnostic = "Animation binding selected a clip ordinal outside the lexical resource clip map.";
        return result;
    }

    const std::shared_ptr<const Resource::AnimationResource> resource =
        ResolveAnimationAsset(m_state, status->animationAssetValue);
    const Animation::Skeleton::ConstPtr sourceSkeleton = resource ? resource->GetSkeleton() : nullptr;
    if (!sourceSkeleton)
    {
        result.code = EcsAnimationBindingPreparationCode::ResourceUnavailable;
        result.diagnostic = "Animation binding could not retain the Ready immutable resource without I/O.";
        return result;
    }
    if (!HasExactTopology(*sourceSkeleton, targetTopology))
    {
        result.code = EcsAnimationBindingPreparationCode::SkeletonTopologyMismatch;
        result.diagnostic = "Animation skeleton names or parent topology differ despite matching bone counts.";
        return result;
    }
    if (!HasExactBindPose(*sourceSkeleton, targetTopology))
    {
        result.code = EcsAnimationBindingPreparationCode::SkeletonBindPoseMismatch;
        result.diagnostic =
            "Animation skeleton bind or inverse-bind values differ despite matching lexical topology.";
        return result;
    }

    result.code = EcsAnimationBindingPreparationCode::Prepared;
    result.binding = targetBinding;
    result.binding.animationAssetValue = status->animationAssetValue;
    result.binding.animationClipOrdinal = animationClipOrdinal;
    result.binding.boneCount = status->boneCount;
    return result;
}

EcsAnimationBindingPreparationResult EcsAnimationAssetService::PrepareCompatibleAnimationBinding(
    EcsAnimationAssetLoadRef request,
    const SceneECS::AnimationSkeletonBinding& targetBinding,
    uint32 animationClipOrdinal) const
{
    EcsAnimationBindingPreparationResult result;
    const std::optional<EcsAnimationAssetLoadStatus> status = GetStatus(request);
    if (!status.has_value())
    {
        result.code = EcsAnimationBindingPreparationCode::InvalidRequestRef;
        result.diagnostic = "Animation binding preparation rejected an invalid or foreign request reference.";
        return result;
    }
    if (IsShutdown())
    {
        result.code = EcsAnimationBindingPreparationCode::ServiceShutDown;
        result.diagnostic = "Animation binding preparation rejected a shut down service.";
        return result;
    }
    if (status->state != EcsAnimationAssetLoadState::Ready || status->animationAssetValue == 0)
    {
        result.code = EcsAnimationBindingPreparationCode::RequestNotReady;
        result.diagnostic = "Animation binding preparation requires an exact Ready request.";
        return result;
    }
    if (targetBinding.animationAssetValue == 0)
    {
        result.code = EcsAnimationBindingPreparationCode::TargetResourceUnavailable;
        result.diagnostic =
            "Resource-backed binding preparation requires a non-zero target animation asset identity.";
        return result;
    }
    if (animationClipOrdinal >= status->clips.size())
    {
        result.code = EcsAnimationBindingPreparationCode::ClipOrdinalOutOfRange;
        result.diagnostic = "Animation binding selected a clip ordinal outside the lexical resource clip map.";
        return result;
    }

    const std::shared_ptr<const Resource::AnimationResource> sourceResource =
        ResolveAnimationAsset(m_state, status->animationAssetValue);
    const std::shared_ptr<const Resource::AnimationResource> targetResource =
        ResolveAnimationAsset(m_state, targetBinding.animationAssetValue);
    const Animation::Skeleton::ConstPtr sourceSkeleton =
        sourceResource ? sourceResource->GetSkeleton() : nullptr;
    const Animation::Skeleton::ConstPtr targetSkeleton =
        targetResource ? targetResource->GetSkeleton() : nullptr;
    if (!sourceSkeleton || !targetSkeleton)
    {
        result.code = EcsAnimationBindingPreparationCode::TargetResourceUnavailable;
        result.diagnostic =
            "Animation binding could not retain both exact immutable resources without I/O.";
        return result;
    }
    if (targetBinding.boneCount != targetSkeleton->GetBoneCount() ||
        status->boneCount != targetSkeleton->GetBoneCount())
    {
        result.code = EcsAnimationBindingPreparationCode::BoneCountMismatch;
        result.diagnostic =
            "Target binding bone count is stale or differs from the immutable target resource.";
        return result;
    }
    if (!HasExactTopology(*sourceSkeleton, *targetSkeleton))
    {
        result.code = EcsAnimationBindingPreparationCode::SkeletonTopologyMismatch;
        result.diagnostic =
            "Source and exact target resource skeleton names or parent topology differ.";
        return result;
    }
    if (!HasExactBindPose(*sourceSkeleton, *targetSkeleton))
    {
        result.code = EcsAnimationBindingPreparationCode::SkeletonBindPoseMismatch;
        result.diagnostic =
            "Source and exact target resource bind or inverse-bind values differ.";
        return result;
    }

    result.code = EcsAnimationBindingPreparationCode::Prepared;
    result.binding = targetBinding;
    result.binding.animationAssetValue = status->animationAssetValue;
    result.binding.animationClipOrdinal = animationClipOrdinal;
    result.binding.boneCount = status->boneCount;
    return result;
}

void EcsAnimationAssetService::Shutdown()
{
    const std::shared_ptr<State> state = m_state;
    if (!state || std::this_thread::get_id() != state->ownerThread)
    {
        return;
    }

    std::vector<EcsAnimationAssetLoadRef> requests;
    {
        std::lock_guard lock(state->mutex);
        if (state->shutdown)
        {
            return;
        }
        for (const std::unique_ptr<State::Entry>& entry : state->entries)
        {
            if (entry != nullptr && entry->status.state != EcsAnimationAssetLoadState::Cancelled)
            {
                requests.push_back(entry->reference);
            }
        }
    }
    for (const EcsAnimationAssetLoadRef request : requests)
    {
        static_cast<void>(Cancel(request));
    }
    std::lock_guard lock(state->mutex);
    state->entries.clear();
    state->handles.Clear();
    state->resolverCache.clear();
    state->shutdown = true;
    state->resources = nullptr;
}

bool EcsAnimationAssetService::IsShutdown() const
{
    const std::shared_ptr<State> state = m_state;
    if (!state)
    {
        return true;
    }
    std::lock_guard lock(state->mutex);
    return state->shutdown;
}

std::shared_ptr<const Resource::AnimationResource>
EcsAnimationAssetService::ResolveAnimationAsset(const std::shared_ptr<State>& state,
                                                 uint64 animationAssetValue)
{
    if (!state || animationAssetValue == 0 || std::this_thread::get_id() != state->ownerThread)
    {
        return {};
    }

    std::lock_guard lock(state->mutex);
    if (state->shutdown || state->resources == nullptr)
    {
        return {};
    }
    if (const auto cached = state->resolverCache.find(animationAssetValue);
        cached != state->resolverCache.end())
    {
        if (const std::shared_ptr<const Resource::AnimationResource> resource = cached->second.lock())
        {
            return resource;
        }
        state->resolverCache.erase(cached);
    }

    Resource::ResourceHandle<Resource::AnimationResource> animation =
        state->resources->TryAcquireLoaded<Resource::AnimationResource>(animationAssetValue);
    const std::shared_ptr<const Resource::AnimationResource> resolved =
        BridgeIntrusiveAnimation(std::move(animation));
    if (!resolved)
    {
        return {};
    }

    state->resolverCache.emplace(animationAssetValue, resolved);
    if (resolved->GetId() != Resource::InvalidResourceId)
    {
        state->resolverCache.emplace(resolved->GetId(), resolved);
    }
    return resolved;
}
} // namespace RVX::AnimationSceneAdapters
