/** @file SampleModelLoader.cpp @brief Production model path adapter. */

#include "Samples/SampleModelLoader.h"

#include "Scene/SceneEntity.h"
#include "Scene/SceneRuntime.h"

namespace RVX
{
    SceneEntity* LoadedSampleModel::ResolveRoot(Scene& scene) const
    {
        return dynamic_cast<SceneEntity*>(
            scene.ResolveActor(instance.rootActor));
    }

    SampleModelLoader::SampleModelLoader(
        SceneAssetLoadCoordinator& coordinator) noexcept
        : m_coordinator(coordinator)
    {
    }

    bool SampleModelLoader::Request(const std::filesystem::path& path,
                                    LoadedSampleModel& outModel,
                                    std::string& outError,
                                    bool activateWhenResident) const
    {
        outModel = {};
        if (path.empty())
        {
            outError = "Model path must not be empty";
            return false;
        }

        SceneModelLoadDesc desc;
        desc.path = path.string();
        desc.activateWhenResident = activateWhenResident;
        outModel.loadHandle = m_coordinator.RequestModel(
            std::move(desc),
            outError);
        if (!outModel.loadHandle.IsValid())
            return false;
        outModel.sourcePath = path;
        return true;
    }

    bool SampleModelLoader::Instantiate(
        const Resource::ResourceHandle<Resource::ModelResource>& resource,
        const std::filesystem::path& sourcePath,
        LoadedSampleModel& outModel,
        std::string& outError,
        bool activateWhenResident) const
    {
        outModel = {};
        if (!resource.IsValid() || !resource.IsLoaded())
        {
            outError = "Model resource is not CPU-ready for scene instantiation";
            return false;
        }

        outModel.loadHandle = m_coordinator.InstantiateModel(
            resource,
            {},
            activateWhenResident,
            outError);
        if (!outModel.loadHandle.IsValid())
        {
            return false;
        }
        outModel.sourcePath = sourcePath;
        static_cast<void>(UpdateReadiness(outModel));
        return true;
    }

    SceneAssetStatus SampleModelLoader::UpdateReadiness(
        LoadedSampleModel& model) const
    {
        const SceneAssetStatus* status =
            m_coordinator.GetStatus(model.loadHandle);
        if (!status)
        {
            model.status.lifecycle = SceneAssetLifecycle::Failed;
            model.status.residency = SceneAssetResidency::None;
            model.status.error = {
                Resource::ResourceLoadErrorCode::InvalidRequest,
                "Model load handle is stale"};
            model.status.diagnostic = model.status.error.message;
            return model.status;
        }
        model.status = *status;
        model.resource = m_coordinator.GetModel(model.loadHandle);
        if (const SceneAssetInstance* instance =
                m_coordinator.GetModelInstance(model.loadHandle))
        {
            model.instance = *instance;
        }
        return model.status;
    }

    bool SampleModelLoader::Cancel(LoadedSampleModel& model) const
    {
        const bool cancelled = m_coordinator.Cancel(model.loadHandle);
        model = {};
        return cancelled;
    }
} // namespace RVX
