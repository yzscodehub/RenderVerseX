/** @file SampleEnvironmentLoader.cpp @brief Pure ECS sample Environment adapter. */

#include "Samples/SampleEnvironmentLoader.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <system_error>
#include <utility>

namespace RVX
{
    namespace
    {
        [[nodiscard]] bool TryResolveQualityProfile(
            const SampleEnvironmentLoadOptions& sampleOptions,
            Resource::HDRIBLQualityProfile& outProfile)
        {
            if (sampleOptions.quality != "default" && sampleOptions.quality != "low" &&
                sampleOptions.quality != "high")
            {
                return false;
            }
            if (sampleOptions.smoke)
            {
                outProfile = Resource::HDRIBLQualityProfile::Validation;
                return true;
            }
            if (sampleOptions.quality == "low")
            {
                outProfile = Resource::HDRIBLQualityProfile::Low;
                return true;
            }
            if (sampleOptions.quality == "high")
            {
                outProfile = Resource::HDRIBLQualityProfile::High;
                return true;
            }
            outProfile = Resource::HDRIBLQualityProfile::Default;
            return true;
        }

        [[nodiscard]] bool IsSupportedEnvironmentPath(const std::filesystem::path& path)
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

        [[nodiscard]] bool IsCpuReadyState(
            ResourceSceneAdapters::EcsEnvironmentLoadState state) noexcept
        {
            using State = ResourceSceneAdapters::EcsEnvironmentLoadState;
            return state == State::PendingGpuReadiness ||
                   state == State::PendingAdopt ||
                   state == State::PendingPresentation ||
                   state == State::FullyResident;
        }
    } // namespace

    bool LoadedSampleEnvironment::IsValid() const noexcept
    {
        using State = ResourceSceneAdapters::EcsEnvironmentLoadState;
        return request.IsValid() && status.sceneRuntimeId == request.sceneRuntimeId &&
               status.state == State::FullyResident &&
               contentVerificationReceipt.has_value() &&
               contentVerificationReceipt->IsVerified() &&
               environmentResolution > 0 && irradianceResolution > 0 &&
               prefilteredResolution > 0 && prefilteredMipLevels > 0 &&
               brdfLUTResolution > 0 && std::isfinite(exposure) && exposure > 0.0f;
    }

    bool LoadedSampleEnvironment::IsCPUReady() const noexcept
    {
        return request.IsValid() && status.sceneRuntimeId == request.sceneRuntimeId &&
               IsCpuReadyState(status.state) && environmentResolution > 0 &&
               irradianceResolution > 0 && prefilteredResolution > 0 &&
               prefilteredMipLevels > 0 && brdfLUTResolution > 0;
    }

    SampleEnvironmentLoader::SampleEnvironmentLoader(
        IWorldEcsRuntimeServices& runtimeServices,
        ECS::SceneRuntimeId sceneRuntimeId) noexcept
        : m_runtimeServices(runtimeServices), m_sceneRuntimeId(sceneRuntimeId)
    {
    }

    SampleEnvironmentLoader::SampleEnvironmentLoader(
        IWorldEcsRuntimeServices& runtimeServices,
        ECS::SceneRuntimeId sceneRuntimeId,
        SampleAssetRegistry assetRegistry)
        : m_runtimeServices(runtimeServices)
        , m_sceneRuntimeId(sceneRuntimeId)
        , m_assetRegistry(std::move(assetRegistry))
    {
    }

    bool SampleEnvironmentLoader::RequestByAssetId(
        std::string_view assetId,
        const SampleEnvironmentLoadOptions& options,
        LoadedSampleEnvironment& output,
        std::string& outError,
        SampleAssetRegistryLookupResult* outLookup) const
    {
        if (output.request.IsValid())
        {
            outError = "Environment request output already owns a live ECS request reference.";
            return false;
        }

        const SampleAssetRegistryLookupResult lookup = m_assetRegistry.Lookup(
            assetId, SampleAssetKind::Environment);
        if (outLookup != nullptr)
        {
            *outLookup = lookup;
        }
        if (!lookup.IsFound())
        {
            if (lookup.code == SampleAssetRegistryLookupCode::KindMismatch)
            {
                outError =
                    "SampleAssetRegistry kind mismatch for environment id '" +
                    std::string(assetId) + "': actual=" +
                    GetSampleAssetKindName(lookup.actualKind);
            }
            else
            {
                outError = "SampleAssetRegistry unknown environment id: " +
                           std::string(assetId);
            }
            return false;
        }

        if (!lookup.entry->contentIdentity.IsValid())
        {
            outError = "SampleAssetRegistry environment entry has no valid content identity: " +
                       std::string(assetId);
            return false;
        }
        return RequestResolved(lookup.entry->resolvedPath,
                               lookup.entry->contentIdentity,
                               options,
                               output,
                               outError);
    }

    bool SampleEnvironmentLoader::Request(
        const std::filesystem::path& path,
        const Resource::ResourceContentIdentity& contentIdentity,
        const SampleEnvironmentLoadOptions& options,
        LoadedSampleEnvironment& output,
        std::string& outError) const
    {
        if (output.request.IsValid())
        {
            outError = "Environment request output already owns a live ECS request reference.";
            return false;
        }
        return RequestResolved(path, contentIdentity, options, output, outError);
    }

    bool SampleEnvironmentLoader::RequestResolved(
        const std::filesystem::path& path,
        const Resource::ResourceContentIdentity& expectedContentIdentity,
        const SampleEnvironmentLoadOptions& options,
        LoadedSampleEnvironment& output,
        std::string& outError) const
    {
        if (!m_sceneRuntimeId.IsValid())
        {
            outError = "Environment request requires a valid exact Scene runtime id.";
            return false;
        }
        if (path.empty())
        {
            outError = "Environment path must not be empty";
            return false;
        }
        if (!expectedContentIdentity.IsValid())
        {
            outError = "Environment request requires an exact valid content identity.";
            return false;
        }
        if (!std::isfinite(options.exposure) || options.exposure <= 0.0f)
        {
            outError = "Environment exposure must be finite and positive";
            return false;
        }
        if (!IsSupportedEnvironmentPath(path))
        {
            outError = "Environment must use an .hdr or .exr file: " + path.string();
            return false;
        }

        Resource::HDRIBLQualityProfile quality;
        if (!TryResolveQualityProfile(options, quality))
        {
            outError = "Environment quality must be one of: default, low, high.";
            return false;
        }

        ResourceSceneAdapters::EcsEnvironmentLoadDesc desc;
        desc.path = path.string();
        desc.environmentOptions.quality = quality;
        desc.environmentOptions.exposure = options.exposure;
        desc.environmentOptions.applyGamma = false;
        desc.expectedSceneRuntimeId = m_sceneRuntimeId;
        desc.resourceOptions.expectedContentIdentity = expectedContentIdentity;
        LoadedSampleEnvironment requested;
        requested.request = m_runtimeServices.RequestEnvironment(
            std::move(desc), outError);
        if (!requested.request.IsValid() ||
            requested.request.sceneRuntimeId != m_sceneRuntimeId)
        {
            if (outError.empty())
            {
                outError = "Environment runtime service returned an invalid or foreign request reference.";
            }
            return false;
        }
        requested.sourcePath = path;
        requested.status.sceneRuntimeId = m_sceneRuntimeId;
        output = std::move(requested);
        return true;
    }

    ResourceSceneAdapters::EcsEnvironmentLoadStatus
    SampleEnvironmentLoader::UpdateReadiness(LoadedSampleEnvironment& environment) const
    {
        const std::optional<ResourceSceneAdapters::EcsEnvironmentLoadStatus> status =
            environment.request.IsValid() &&
                    environment.request.sceneRuntimeId == m_sceneRuntimeId ?
                m_runtimeServices.GetEnvironmentStatus(environment.request) :
                std::nullopt;
        if (!status.has_value() || status->sceneRuntimeId != m_sceneRuntimeId)
        {
            environment.status = {};
            environment.status.state =
                ResourceSceneAdapters::EcsEnvironmentLoadState::Failed;
            environment.status.sceneRuntimeId = m_sceneRuntimeId;
            environment.status.request.error = {
                Resource::ResourceLoadErrorCode::InvalidRequest,
                "Environment ECS load handle is stale"};
            environment.status.diagnostic = environment.status.request.error.message;
            environment.contentVerificationReceipt.reset();
            environment.environmentResolution = 0;
            environment.irradianceResolution = 0;
            environment.prefilteredResolution = 0;
            environment.prefilteredMipLevels = 0;
            environment.brdfLUTResolution = 0;
            environment.exposure = 1.0f;
            return environment.status;
        }

        environment.status = *status;
        environment.contentVerificationReceipt = status->contentVerificationReceipt;
        environment.environmentResolution = status->environmentResolution;
        environment.irradianceResolution = status->irradianceResolution;
        environment.prefilteredResolution = status->prefilteredResolution;
        environment.prefilteredMipLevels = status->prefilteredMipLevels;
        environment.brdfLUTResolution = status->brdfLUTResolution;
        environment.exposure = status->exposure;
        if (environment.contentVerificationReceipt.has_value() &&
            environment.contentVerificationReceipt->IsVerified())
        {
            m_contentVerificationReceipts.insert_or_assign(
                MakeLogicalPathKey(environment.sourcePath),
                *environment.contentVerificationReceipt);
        }
        return environment.status;
    }

    bool SampleEnvironmentLoader::Cancel(LoadedSampleEnvironment& environment) const
    {
        if (!environment.request.IsValid() ||
            environment.request.sceneRuntimeId != m_sceneRuntimeId ||
            !m_runtimeServices.CancelEnvironment(environment.request))
        {
            return false;
        }
        environment = {};
        return true;
    }

    std::optional<Resource::ResourceContentVerificationReceipt>
    SampleEnvironmentLoader::GetContentVerificationReceipt(
        const std::filesystem::path& path) const
    {
        const auto receipt = m_contentVerificationReceipts.find(
            MakeLogicalPathKey(path));
        return receipt != m_contentVerificationReceipts.end() ?
                   std::optional<Resource::ResourceContentVerificationReceipt>(
                       receipt->second) :
                   std::nullopt;
    }

    std::string SampleEnvironmentLoader::MakeLogicalPathKey(
        const std::filesystem::path& path)
    {
        std::error_code error;
        std::filesystem::path absolute = path;
        if (!absolute.is_absolute())
        {
            absolute = std::filesystem::absolute(absolute, error);
        }
        if (!error)
        {
            const std::filesystem::path canonical =
                std::filesystem::weakly_canonical(absolute, error);
            if (!error)
            {
                absolute = canonical;
            }
        }

        std::string key = absolute.lexically_normal().generic_string();
#if defined(_WIN32)
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char character)
                       {
                           return static_cast<char>(std::tolower(character));
                       });
#endif
        return key;
    }
} // namespace RVX
