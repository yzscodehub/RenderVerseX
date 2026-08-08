/**
 * @file SampleEnvironmentLoader.cpp
 * @brief SampleEnvironmentLoader implementation.
 */

#include "Samples/SampleEnvironmentLoader.h"

#include "Resource/Loader/HDRTextureLoader.h"
#include "Resource/ResourceSubsystem.h"
#include "Scene/Components/SkyboxComponent.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <system_error>
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
        return resource.IsLoaded() && environment.IsLoaded() && irradiance.IsLoaded() &&
               prefiltered.IsLoaded() && brdfLUT.IsLoaded() &&
               environmentResolution > 0 && irradianceResolution > 0 &&
               prefilteredResolution > 0 && prefilteredMipLevels > 0 &&
               brdfLUTResolution > 0;
    }

    SampleEnvironmentLoader::SampleEnvironmentLoader(
        Resource::ResourceManager& resources,
        Resource::ResourceSubsystem& resourceSubsystem) noexcept
        : m_resources(resources),
          m_resourceSubsystem(resourceSubsystem)
    {
    }

    bool SampleEnvironmentLoader::Load(
        const std::filesystem::path& path,
        const SampleEnvironmentLoadOptions& options,
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

        std::error_code error;
        const std::filesystem::path absolute =
            std::filesystem::weakly_canonical(path, error);
        if (error || !std::filesystem::is_regular_file(absolute, error) || error)
        {
            outError = "Environment file does not exist: " + path.string();
            return false;
        }

        Resource::HDRTextureLoader loader(&m_resources);
        const Resource::IBLData ibl = loader.LoadIBL(
            absolute.string(),
            ResolveQualityProfile(options),
            options.exposure,
            false);
        if (!ibl.IsValid())
        {
            outError = "Failed to generate environment IBL from: " +
                       absolute.string();
            return false;
        }

        LoadedSampleEnvironment candidate;
        candidate.sourcePath = absolute;
        candidate.environment = Resource::TextureHandle(ibl.environmentMap);
        candidate.irradiance = Resource::TextureHandle(ibl.irradianceMap);
        candidate.prefiltered = Resource::TextureHandle(ibl.prefilteredMap);
        candidate.brdfLUT = Resource::TextureHandle(ibl.brdfLUT);
        candidate.environmentResolution = candidate.environment->GetWidth();
        candidate.irradianceResolution = candidate.irradiance->GetWidth();
        candidate.prefilteredResolution = candidate.prefiltered->GetWidth();
        candidate.prefilteredMipLevels = ibl.prefilteredMipLevels;
        candidate.brdfLUTResolution = candidate.brdfLUT->GetWidth();
        Resource::EnvironmentResourceData environmentData;
        environmentData.sourcePath = absolute;
        environmentData.environment = candidate.environment;
        environmentData.irradiance = candidate.irradiance;
        environmentData.prefiltered = candidate.prefiltered;
        environmentData.brdfLUT = candidate.brdfLUT;
        environmentData.environmentResolution = candidate.environmentResolution;
        environmentData.irradianceResolution = candidate.irradianceResolution;
        environmentData.prefilteredResolution = candidate.prefilteredResolution;
        environmentData.prefilteredMipLevels = candidate.prefilteredMipLevels;
        environmentData.brdfLUTResolution = candidate.brdfLUTResolution;
        environmentData.intensity = options.exposure;
        auto* environmentResource = new Resource::EnvironmentResource();
        environmentResource->SetId(Resource::GenerateResourceId(
            absolute.string() + "#environment"));
        environmentResource->SetPath(absolute.string());
        environmentResource->SetName(absolute.filename().string());
        candidate.resource = Resource::EnvironmentHandle(environmentResource);
        if (!environmentResource->SetData(std::move(environmentData)) ||
            !candidate.IsValid())
        {
            outError = "Environment IBL generation returned incomplete resources: " +
                       absolute.string();
            return false;
        }

        const auto publish = [this](const Resource::TextureHandle& texture)
        {
            return m_resourceSubsystem.PublishRenderResource(
                Resource::ResourceHandle<Resource::IResource>(texture));
        };
        if (!publish(candidate.environment) || !publish(candidate.irradiance) ||
            !publish(candidate.prefiltered) || !publish(candidate.brdfLUT))
        {
            outError = "Environment IBL resource publication was rejected: " +
                       absolute.string();
            return false;
        }

        output = std::move(candidate);
        return true;
    }

    bool SampleEnvironmentLoader::BindToSkybox(
        SkyboxComponent& skybox,
        const LoadedSampleEnvironment& environment,
        float32 exposure,
        std::string& outError) const
    {
        if (!environment.IsValid())
        {
            outError = "Cannot bind an incomplete environment IBL";
            return false;
        }
        if (!(exposure > 0.0f))
        {
            outError = "Environment exposure must be positive";
            return false;
        }

        skybox.SetCubemap(environment.environment);
        skybox.SetIrradianceMap(environment.irradiance);
        skybox.SetPrefilteredMap(environment.prefiltered);
        skybox.SetBRDFLUT(environment.brdfLUT);
        skybox.SetExposure(exposure);
        skybox.SetContributesToLighting(true);
        return true;
    }
} // namespace RVX
