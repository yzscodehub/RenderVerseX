/**
 * @file SampleEnvironmentLoader.cpp
 * @brief SampleEnvironmentLoader implementation.
 */

#include "Samples/SampleEnvironmentLoader.h"

#include "Scene/Components/SkyboxComponent.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <utility>

namespace RVX
{
    namespace
    {
        Resource::HDRIBLQualityProfile ResolveQualityProfile(
            const SampleEnvironmentLoadOptions& sampleOptions)
        {
            if (sampleOptions.smoke)
            {
                return Resource::HDRIBLQualityProfile::Validation;
            }
            if (sampleOptions.quality == "low")
            {
                return Resource::HDRIBLQualityProfile::Low;
            }
            if (sampleOptions.quality == "high")
            {
                return Resource::HDRIBLQualityProfile::High;
            }
            return Resource::HDRIBLQualityProfile::Default;
        }

        bool IsSupportedEnvironmentPath(const std::filesystem::path& path)
        {
            std::string extension = path.extension().string();
            std::transform(extension.begin(),
                           extension.end(),
                           extension.begin(),
                           [](unsigned char value)
                           {
                               return static_cast<char>(std::tolower(value));
                           });
            return extension == ".hdr" || extension == ".exr";
        }
    } // namespace

    bool LoadedSampleEnvironment::IsValid() const noexcept
    {
        return status.IsFullyResident() && resource.IsLoaded() &&
               environment.IsLoaded() && irradiance.IsLoaded() &&
               prefiltered.IsLoaded() && brdfLUT.IsLoaded() &&
               environmentResolution > 0 && irradianceResolution > 0 &&
               prefilteredResolution > 0 && prefilteredMipLevels > 0 &&
               brdfLUTResolution > 0;
    }

    bool LoadedSampleEnvironment::IsCPUReady() const noexcept
    {
        return status.IsActive() &&
               status.residency >= SceneAssetResidency::CPUReady &&
               resource.IsLoaded();
    }

    SampleEnvironmentLoader::SampleEnvironmentLoader(
        SceneAssetLoadCoordinator& coordinator) noexcept
        : m_coordinator(coordinator)
    {
    }

    bool SampleEnvironmentLoader::Request(
        const std::filesystem::path& path,
        const SampleEnvironmentLoadOptions& options,
        SkyboxComponent& targetSkybox,
        LoadedSampleEnvironment& output,
        std::string& outError) const
    {
        output = {};
        if (path.empty())
        {
            outError = "Environment path must not be empty";
            return false;
        }
        if (!std::isfinite(options.exposure) || options.exposure <= 0.0f)
        {
            outError = "Environment exposure must be finite and positive";
            return false;
        }
        if (!IsSupportedEnvironmentPath(path))
        {
            outError = "Environment must use an .hdr or .exr file: " +
                       path.string();
            return false;
        }

        SceneEnvironmentLoadDesc desc;
        desc.path = path.string();
        desc.environmentOptions.quality = ResolveQualityProfile(options);
        desc.environmentOptions.exposure = options.exposure;
        desc.environmentOptions.applyGamma = false;
        desc.targetSkybox = targetSkybox.GetComponentHandle();
        output.loadHandle = m_coordinator.RequestEnvironment(
            std::move(desc),
            outError);
        if (!output.loadHandle.IsValid())
            return false;
        output.sourcePath = path;
        return true;
    }

    SceneAssetStatus SampleEnvironmentLoader::UpdateReadiness(
        LoadedSampleEnvironment& environment) const
    {
        const SceneAssetStatus* status =
            m_coordinator.GetStatus(environment.loadHandle);
        if (!status)
        {
            environment.status.lifecycle = SceneAssetLifecycle::Failed;
            environment.status.error = {
                Resource::ResourceLoadErrorCode::InvalidRequest,
                "Environment load handle is stale"};
            environment.status.diagnostic = environment.status.error.message;
            return environment.status;
        }
        environment.status = *status;
        environment.resource =
            m_coordinator.GetEnvironment(environment.loadHandle);
        if (environment.resource.IsLoaded())
        {
            const Resource::EnvironmentResourceData& data =
                environment.resource->GetData();
            environment.environment = data.environment;
            environment.irradiance = data.irradiance;
            environment.prefiltered = data.prefiltered;
            environment.brdfLUT = data.brdfLUT;
            environment.environmentResolution = data.environmentResolution;
            environment.irradianceResolution = data.irradianceResolution;
            environment.prefilteredResolution = data.prefilteredResolution;
            environment.prefilteredMipLevels = data.prefilteredMipLevels;
            environment.brdfLUTResolution = data.brdfLUTResolution;
        }
        return environment.status;
    }

    bool SampleEnvironmentLoader::Cancel(
        LoadedSampleEnvironment& environment) const
    {
        const bool cancelled = m_coordinator.Cancel(environment.loadHandle);
        environment = {};
        return cancelled;
    }
} // namespace RVX
