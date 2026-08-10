#include "ResourceSceneAdapters/SceneAssetInstantiation.h"

#include "RenderContracts/IRenderResourceGateway.h"
#include "Resource/ResourceSubsystem.h"
#include "Resource/Types/ModelResource.h"
#include "Scene/Components/StaticMeshComponent.h"
#include "Scene/SceneEntity.h"
#include "Scene/SceneRuntime.h"

#include <algorithm>

namespace RVX
{
namespace
{
    enum class RequiredResourceReadiness : uint8
    {
        Ready = 0,
        Pending,
        Failed
    };

    void CollectActorHandles(SceneEntity* entity,
                             std::vector<Actor::Handle>& handles)
    {
        if (!entity)
            return;
        handles.push_back(entity->GetHandle());
        for (SceneEntity* child : entity->GetChildren())
            CollectActorHandles(child, handles);
    }

    bool IsFailedStatus(const RenderResourceStatus& status)
    {
        return status.code == RenderResourceStatusCode::StaleGeneration ||
               status.code == RenderResourceStatusCode::InvalidHandle ||
               status.state == RenderResourcePublicState::Failed;
    }

    SceneAssetStatus FailAndRollback(Scene& scene,
                                     SceneAssetInstance& instance,
                                     std::string diagnostic)
    {
        if (Actor* root = scene.ResolveActor(instance.rootActor))
            static_cast<void>(scene.DestroyActor(root));
        instance.rootActor = Actor::InvalidHandle;
        instance.actors.clear();
        instance.status.lifecycle = SceneAssetLifecycle::Failed;
        instance.status.residency = SceneAssetResidency::None;
        instance.status.error = {
            Resource::ResourceLoadErrorCode::PublishFailure,
            diagnostic};
        instance.status.diagnostic = std::move(diagnostic);
        ++instance.status.revision;
        return instance.status;
    }
} // namespace

SceneAssetInstance SceneAssetInstantiator::InstantiateModel(
    Scene& scene,
    const Resource::ModelResource& model,
    const SceneAssetInstantiationOptions& options)
{
    SceneAssetInstance result;
    if (!scene.IsInitialized() || !scene.IsUpdateThread() ||
        model.GetRootNode() == nullptr)
    {
        result.status.lifecycle = SceneAssetLifecycle::Failed;
        result.status.error = {
            Resource::ResourceLoadErrorCode::InvalidRequest,
            "Scene/model is not ready for instantiation"};
        result.status.diagnostic = result.status.error.message;
        return result;
    }

    Actor* actor = model.InstantiateActor(&scene);
    auto* root = dynamic_cast<SceneEntity*>(actor);
    if (!root)
    {
        result.status.lifecycle = SceneAssetLifecycle::Failed;
        result.status.error = {
            Resource::ResourceLoadErrorCode::PublishFailure,
            "Model root could not be instantiated"};
        result.status.diagnostic = result.status.error.message;
        return result;
    }

    result.rootActor = root->GetHandle();
    CollectActorHandles(root, result.actors);
    if (result.actors.size() != model.GetNodeCount())
    {
        static_cast<void>(scene.DestroyActor(root));
        result = {};
        result.status.lifecycle = SceneAssetLifecycle::Failed;
        result.status.error = {
            Resource::ResourceLoadErrorCode::PublishFailure,
            "Model hierarchy instantiation was incomplete and was rolled back"};
        result.status.diagnostic = result.status.error.message;
        return result;
    }

    if (!options.materialOverrides.empty())
    {
        for (Actor::Handle actorHandle : result.actors)
        {
            for (StaticMeshComponent* primitive :
                 scene.GetComponentsForActorImplementing<
                     StaticMeshComponent>(actorHandle))
            {
                if (!primitive)
                    continue;
                for (size_t index = 0;
                     index < options.materialOverrides.size();
                     ++index)
                {
                    if (options.materialOverrides[index].IsValid())
                    {
                        primitive->SetMaterial(
                            index, options.materialOverrides[index]);
                    }
                }
            }
        }
    }

    result.status.lifecycle = SceneAssetLifecycle::Active;
    result.status.residency = SceneAssetResidency::CPUReady;
    result.status.progress = 1.0f;
    result.status.revision = 1;
    return result;
}

SceneAssetStatus SceneAssetInstantiator::UpdateResidency(
    Scene& scene,
    const Resource::ModelResource& model,
    Resource::ResourceSubsystem& resources,
    SceneAssetInstance& instance)
{
    if (!instance.rootActor.IsValid() ||
        scene.ResolveActor(instance.rootActor) == nullptr)
    {
        return FailAndRollback(
            scene, instance, "Instantiated scene root no longer exists");
    }

    bool uploadPending = false;
    const auto inspect = [&](AssetId assetId, RenderResourceKind kind)
    {
        if (!assetId.IsValid())
            return RequiredResourceReadiness::Failed;
        const Resource::RenderResourceResolveResult resolved =
            resources.ResolveRenderResource(assetId, kind);
        if (resolved.code == Resource::RenderResourceResolveCode::NotFound)
        {
            uploadPending = true;
            return RequiredResourceReadiness::Pending;
        }
        if (resolved.code != Resource::RenderResourceResolveCode::Resolved ||
            IsFailedStatus(resolved.status))
        {
            return RequiredResourceReadiness::Failed;
        }
        if (resolved.status.state != RenderResourcePublicState::GPUReady)
        {
            uploadPending = true;
            return RequiredResourceReadiness::Pending;
        }
        return RequiredResourceReadiness::Ready;
    };

    for (const auto& mesh : model.GetMeshes())
    {
        if (!mesh.IsLoaded())
        {
            if (!mesh.IsLoading())
            {
                return FailAndRollback(
                    scene, instance,
                    "A required model mesh failed before GPU upload");
            }
            instance.status.lifecycle = SceneAssetLifecycle::Active;
            instance.status.residency = SceneAssetResidency::CPUReady;
            instance.status.diagnostic.clear();
            return instance.status;
        }
        if (inspect(AssetId{mesh.GetId()}, RenderResourceKind::Mesh) ==
            RequiredResourceReadiness::Failed)
        {
            return FailAndRollback(
                scene, instance,
                "A required model mesh became invalid during GPU upload");
        }
    }
    for (const auto& material : model.GetMaterials())
    {
        if (!material.IsLoaded())
        {
            if (!material.IsLoading())
            {
                return FailAndRollback(
                    scene, instance,
                    "A required model material failed before GPU upload");
            }
            instance.status.lifecycle = SceneAssetLifecycle::Active;
            instance.status.residency = SceneAssetResidency::CPUReady;
            instance.status.diagnostic.clear();
            return instance.status;
        }
        if (inspect(AssetId{material.GetId()}, RenderResourceKind::Material) ==
            RequiredResourceReadiness::Failed)
        {
            return FailAndRollback(
                scene, instance,
                "A required model material became invalid during GPU upload");
        }
    }

    SceneAssetLifecycle nextLifecycle = SceneAssetLifecycle::Active;
    SceneAssetResidency nextResidency = SceneAssetResidency::CPUReady;
    Resource::ResourceLoadError streamingError;
    std::string streamingDiagnostic;
    if (!uploadPending)
    {
        const Resource::ModelTextureStreamingSnapshot streaming =
            model.GetTextureStreamingSnapshot();
        switch (streaming.stage)
        {
            case Resource::ModelTextureStreamingStage::AwaitingMinimumResident:
                nextResidency = SceneAssetResidency::MinimumResident;
                break;
            case Resource::ModelTextureStreamingStage::Decoding:
            case Resource::ModelTextureStreamingStage::Uploading:
                nextResidency = SceneAssetResidency::Streaming;
                break;
            case Resource::ModelTextureStreamingStage::Failed:
                nextLifecycle = SceneAssetLifecycle::Failed;
                nextResidency = SceneAssetResidency::MinimumResident;
                streamingError = {
                    Resource::ResourceLoadErrorCode::LoaderFailure,
                    streaming.error.empty()
                        ? "A streamed model texture failed"
                        : streaming.error};
                streamingDiagnostic = streamingError.message;
                break;
            case Resource::ModelTextureStreamingStage::Cancelled:
                nextLifecycle = SceneAssetLifecycle::Cancelled;
                nextResidency = SceneAssetResidency::MinimumResident;
                streamingError = {
                    Resource::ResourceLoadErrorCode::Cancelled,
                    "Model texture streaming was cancelled"};
                streamingDiagnostic = streamingError.message;
                break;
            case Resource::ModelTextureStreamingStage::None:
            case Resource::ModelTextureStreamingStage::FullyResident:
            default:
                nextResidency = SceneAssetResidency::FullyResident;
                break;
        }
    }
    if (instance.status.lifecycle != nextLifecycle ||
        instance.status.residency != nextResidency)
    {
        ++instance.status.revision;
    }
    instance.status.lifecycle = nextLifecycle;
    instance.status.residency = nextResidency;
    if (nextResidency == SceneAssetResidency::CPUReady)
        instance.status.progress = 0.75f;
    else if (nextResidency == SceneAssetResidency::MinimumResident)
        instance.status.progress = 0.8f;
    else if (nextResidency == SceneAssetResidency::Streaming)
    {
        const Resource::ModelTextureStreamingSnapshot streaming =
            model.GetTextureStreamingSnapshot();
        instance.status.progress = streaming.textureCount == 0
            ? 0.9f
            : 0.8f + 0.19f *
                  (static_cast<float32>(streaming.decodedTextureCount) /
                   static_cast<float32>(streaming.textureCount));
    }
    else
        instance.status.progress = 1.0f;
    instance.status.error = std::move(streamingError);
    instance.status.diagnostic = std::move(streamingDiagnostic);
    return instance.status;
}

bool SceneAssetInstantiator::SetRenderablesEnabled(
    Scene& scene,
    const SceneAssetInstance& instance,
    bool enabled)
{
    if (!scene.IsInitialized() || !scene.IsUpdateThread())
        return false;

    bool foundRenderable = false;
    for (Actor::Handle actorHandle : instance.actors)
    {
        for (StaticMeshComponent* primitive :
             scene.GetComponentsForActorImplementing<StaticMeshComponent>(
                 actorHandle))
        {
            if (!primitive)
                continue;
            primitive->SetEnabled(enabled);
            foundRenderable = true;
        }
    }
    return foundRenderable;
}

bool SceneAssetInstantiator::Destroy(Scene& scene,
                                     SceneAssetInstance& instance)
{
    Actor* root = scene.ResolveActor(instance.rootActor);
    if (!root)
    {
        instance = {};
        instance.status.lifecycle = SceneAssetLifecycle::Failed;
        instance.status.residency = SceneAssetResidency::None;
        instance.status.diagnostic = "Scene asset instance was already absent";
        instance.status.error = {
            Resource::ResourceLoadErrorCode::PublishFailure,
            instance.status.diagnostic};
        return false;
    }

    const bool destroyed = scene.DestroyActor(root);
    if (destroyed)
        instance = {};
    return destroyed;
}

bool SceneAssetInstantiator::Cancel(Scene& scene,
                                    SceneAssetInstance& instance)
{
    const bool existed =
        instance.rootActor.IsValid() &&
        scene.ResolveActor(instance.rootActor) != nullptr;
    static_cast<void>(FailAndRollback(
        scene, instance, "Scene asset instantiation was cancelled"));
    instance.status.lifecycle = SceneAssetLifecycle::Cancelled;
    instance.status.error = {
        Resource::ResourceLoadErrorCode::Cancelled,
        "Scene asset instantiation was cancelled"};
    return existed;
}

} // namespace RVX
