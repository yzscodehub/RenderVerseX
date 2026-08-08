/** @file SampleModelLoader.cpp @brief Production model path adapter. */

#include "Samples/SampleModelLoader.h"

#include "Resource/ResourceManager.h"
#include "Scene/SceneEntity.h"
#include "Scene/SceneRuntime.h"

#include <system_error>

namespace RVX
{
    SceneEntity* LoadedSampleModel::ResolveRoot(Scene& scene) const
    {
        return dynamic_cast<SceneEntity*>(
            scene.ResolveActor(instance.rootActor));
    }

    SampleModelLoader::SampleModelLoader(
        Resource::ResourceManager& resources) noexcept
        : m_resources(resources)
    {
    }

    bool SampleModelLoader::Load(const std::filesystem::path& path,
                                 Scene& scene,
                                 LoadedSampleModel& outModel,
                                 std::string& outError) const
    {
        outModel = {};
        if (path.empty())
        {
            outError = "Model path must not be empty";
            return false;
        }

        std::error_code error;
        const std::filesystem::path normalized =
            std::filesystem::weakly_canonical(path, error);
        if (error || !std::filesystem::is_regular_file(normalized, error) || error)
        {
            outError = "Model file does not exist: " + path.string();
            return false;
        }

        auto resource =
            m_resources.Load<Resource::ModelResource>(normalized.string());
        if (!resource.IsValid() || !resource.IsLoaded())
        {
            outError = "ResourceManager failed to load model: " +
                       normalized.string();
            return false;
        }

        return Instantiate(resource, normalized, scene, outModel, outError);
    }

    bool SampleModelLoader::Instantiate(
        const Resource::ResourceHandle<Resource::ModelResource>& resource,
        const std::filesystem::path& sourcePath,
        Scene& scene,
        LoadedSampleModel& outModel,
        std::string& outError) const
    {
        outModel = {};
        if (!resource.IsValid() || !resource.IsLoaded())
        {
            outError = "Model resource is not CPU-ready for scene instantiation";
            return false;
        }

        SceneAssetInstance instance =
            SceneAssetInstantiator::InstantiateModel(scene, *resource);
        SceneEntity* root = dynamic_cast<SceneEntity*>(
            scene.ResolveActor(instance.rootActor));
        if (!instance.IsValid() || !root)
        {
            outError = "ModelResource failed to instantiate scene actors: " +
                       sourcePath.string();
            return false;
        }

        outModel.resource = resource;
        outModel.instance = std::move(instance);
        outModel.sourcePath = sourcePath;
        return true;
    }
} // namespace RVX
