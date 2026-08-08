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
        result.readiness = SceneAssetReadiness::Failed;
        result.diagnostic = "Scene/model is not ready for instantiation";
        return result;
    }

    Actor* actor = model.InstantiateActor(scene.GetSceneManager());
    auto* root = dynamic_cast<SceneEntity*>(actor);
    if (!root)
    {
        result.readiness = SceneAssetReadiness::Failed;
        result.diagnostic = "Model root could not be instantiated";
        return result;
    }

    result.rootActor = root->GetHandle();
    CollectActorHandles(root, result.actors);
    if (result.actors.size() != model.GetNodeCount())
    {
        static_cast<void>(scene.DestroyActor(root));
        result = {};
        result.readiness = SceneAssetReadiness::Failed;
        result.diagnostic =
            "Model hierarchy instantiation was incomplete and was rolled back";
        return result;
    }

    if (!options.materialOverrides.empty())
    {
        for (StaticMeshComponent* primitive :
             scene.GetComponentsImplementing<StaticMeshComponent>())
        {
            if (!primitive || !primitive->GetOwner())
                continue;
            const Actor::Handle owner = primitive->GetOwner()->GetHandle();
            if (std::find(result.actors.begin(), result.actors.end(), owner) ==
                result.actors.end())
            {
                continue;
            }
            for (size_t index = 0; index < options.materialOverrides.size(); ++index)
            {
                if (options.materialOverrides[index].IsValid())
                    primitive->SetMaterial(index, options.materialOverrides[index]);
            }
        }
    }

    result.readiness = SceneAssetReadiness::CPUReady;
    return result;
}

SceneAssetReadiness SceneAssetInstantiator::UpdateReadiness(
    Scene& scene,
    const Resource::ModelResource& model,
    Resource::ResourceSubsystem& resources,
    SceneAssetInstance& instance)
{
    if (!instance.rootActor.IsValid() ||
        scene.ResolveActor(instance.rootActor) == nullptr)
    {
        instance.readiness = SceneAssetReadiness::Failed;
        instance.diagnostic = "Instantiated scene root no longer exists";
        return instance.readiness;
    }

    bool uploadPending = false;
    const auto inspect = [&](AssetId assetId, RenderResourceKind kind)
    {
        if (!assetId.IsValid())
            return false;
        const Resource::RenderResourceResolveResult resolved =
            resources.ResolveRenderResource(assetId, kind);
        if (resolved.code != Resource::RenderResourceResolveCode::Resolved)
        {
            uploadPending = true;
            return true;
        }
        if (IsFailedStatus(resolved.status))
            return false;
        if (resolved.status.state != RenderResourcePublicState::GPUReady)
            uploadPending = true;
        return true;
    };

    for (const auto& mesh : model.GetMeshes())
    {
        if (!mesh.IsLoaded() || !inspect(AssetId{mesh.GetId()},
                                         RenderResourceKind::Mesh))
        {
            instance.readiness = mesh.IsLoading()
                                     ? SceneAssetReadiness::Loading
                                     : SceneAssetReadiness::Failed;
            instance.diagnostic = "A required model mesh is unavailable";
            return instance.readiness;
        }
    }
    for (const auto& material : model.GetMaterials())
    {
        if (!material.IsLoaded() ||
            !inspect(AssetId{material.GetId()}, RenderResourceKind::Material))
        {
            instance.readiness = material.IsLoading()
                                     ? SceneAssetReadiness::Loading
                                     : SceneAssetReadiness::Failed;
            instance.diagnostic = "A required model material is unavailable";
            return instance.readiness;
        }
    }

    instance.readiness = uploadPending
                             ? SceneAssetReadiness::GPUUploadPending
                             : SceneAssetReadiness::RenderReady;
    instance.diagnostic.clear();
    return instance.readiness;
}

bool SceneAssetInstantiator::Destroy(Scene& scene,
                                     SceneAssetInstance& instance)
{
    Actor* root = scene.ResolveActor(instance.rootActor);
    if (!root)
    {
        instance = {};
        instance.readiness = SceneAssetReadiness::Failed;
        instance.diagnostic = "Scene asset instance was already absent";
        return false;
    }

    const bool destroyed = scene.DestroyActor(root);
    if (destroyed)
        instance = {};
    return destroyed;
}

} // namespace RVX
