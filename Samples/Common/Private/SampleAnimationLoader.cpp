/** @file SampleAnimationLoader.cpp  @brief Pure-ECS animation sample adapter. */

#include "Samples/SampleAnimationLoader.h"

#include <algorithm>
#include <cctype>
#include <system_error>
#include <utility>

namespace RVX
{
    namespace
    {
        AnimationSceneAdapters::EcsAnimationAssetLoadStatus MakeFailedStatus(
            ECS::SceneRuntimeId sceneRuntimeId,
            std::string diagnostic)
        {
            AnimationSceneAdapters::EcsAnimationAssetLoadStatus status;
            status.state =
                AnimationSceneAdapters::EcsAnimationAssetLoadState::Failed;
            status.resourceLoadState = Resource::ResourceLoadState::Failed;
            status.sceneRuntimeId = sceneRuntimeId;
            status.diagnostic = std::move(diagnostic);
            return status;
        }
    } // namespace

    SampleAnimationLoader::SampleAnimationLoader(
        IWorldEcsRuntimeServices& runtimeServices,
        ECS::SceneRuntimeId sceneRuntimeId,
        SampleAssetRegistry assetRegistry)
        : m_runtimeServices(runtimeServices),
          m_sceneRuntimeId(sceneRuntimeId),
          m_assetRegistry(std::move(assetRegistry))
    {
    }

    bool SampleAnimationLoader::RequestByAssetId(
        std::string_view assetId,
        LoadedSampleAnimation& output,
        std::string& outError,
        SampleAssetRegistryLookupResult* outLookup) const
    {
        if (output.request.IsValid())
        {
            outError =
                "Sample animation output already owns a live ECS request";
            return false;
        }

        const SampleAssetRegistryLookupResult lookup = m_assetRegistry.Lookup(
            assetId, SampleAssetKind::Animation);
        if (outLookup != nullptr)
        {
            *outLookup = lookup;
        }
        if (!lookup.IsFound())
        {
            if (lookup.code == SampleAssetRegistryLookupCode::KindMismatch)
            {
                outError = "SampleAssetRegistry kind mismatch for animation id '" +
                           std::string(assetId) + "': actual=" +
                           GetSampleAssetKindName(lookup.actualKind);
            }
            else
            {
                outError = "SampleAssetRegistry unknown animation id: " +
                           std::string(assetId);
            }
            return false;
        }
        if (!m_sceneRuntimeId.IsValid())
        {
            outError = "Sample animation request has no valid ECS Scene runtime";
            return false;
        }
        if (!lookup.entry->contentIdentity.IsValid())
        {
            outError = "Sample animation catalog entry has no verified content identity: " +
                       std::string(assetId);
            return false;
        }

        AnimationSceneAdapters::EcsAnimationAssetLoadDesc desc;
        desc.path = lookup.entry->resolvedPath.string();
        desc.expectedSceneRuntimeId = m_sceneRuntimeId;
        desc.resourceOptions.expectedContentIdentity =
            lookup.entry->contentIdentity;

        std::string requestError;
        const AnimationSceneAdapters::EcsAnimationAssetLoadRef request =
            m_runtimeServices.RequestAnimation(std::move(desc), requestError);
        if (!request.IsValid() || request.sceneRuntimeId != m_sceneRuntimeId)
        {
            outError = requestError.empty()
                           ? "ECS World rejected animation request: " +
                                 lookup.entry->resolvedPath.string()
                           : std::move(requestError);
            return false;
        }

        output = {};
        output.sourcePath = lookup.entry->resolvedPath;
        output.request = request;
        output.status.sceneRuntimeId = m_sceneRuntimeId;
        if (const auto status = m_runtimeServices.GetAnimationStatus(request))
        {
            output.status = *status;
        }
        outError.clear();
        return true;
    }

    AnimationSceneAdapters::EcsAnimationAssetLoadStatus
    SampleAnimationLoader::UpdateReadiness(
        LoadedSampleAnimation& animation) const
    {
        if (!animation.request.IsValid() ||
            animation.request.sceneRuntimeId != m_sceneRuntimeId)
        {
            animation.status = MakeFailedStatus(
                m_sceneRuntimeId,
                "Animation request is stale or belongs to another ECS Scene");
            return animation.status;
        }

        const auto status =
            m_runtimeServices.GetAnimationStatus(animation.request);
        if (!status.has_value() || status->sceneRuntimeId != m_sceneRuntimeId)
        {
            animation.status = MakeFailedStatus(
                m_sceneRuntimeId,
                "ECS World no longer recognizes the animation request");
            return animation.status;
        }

        animation.status = *status;
        if (animation.status.state ==
                AnimationSceneAdapters::EcsAnimationAssetLoadState::Ready &&
            animation.status.contentVerification.IsVerified())
        {
            m_contentVerificationReceipts.insert_or_assign(
                MakeLogicalPathKey(animation.sourcePath),
                animation.status.contentVerification);
        }
        return animation.status;
    }

    bool SampleAnimationLoader::Cancel(LoadedSampleAnimation& animation) const
    {
        if (!animation.request.IsValid() ||
            animation.request.sceneRuntimeId != m_sceneRuntimeId)
        {
            return false;
        }

        if (!m_runtimeServices.CancelAnimation(animation.request))
        {
            return false;
        }

        animation = {};
        return true;
    }

    AnimationSceneAdapters::EcsAnimationBindingPreparationResult
    SampleAnimationLoader::PrepareCompatibleBinding(
        const LoadedSampleAnimation& animation,
        SceneECS::SceneEntityRef targetEntity,
        uint32 animationClipOrdinal) const
    {
        if (!animation.request.IsValid() ||
            animation.request.sceneRuntimeId != m_sceneRuntimeId ||
            !targetEntity.IsValid() ||
            targetEntity.sceneRuntimeId != m_sceneRuntimeId)
        {
            AnimationSceneAdapters::EcsAnimationBindingPreparationResult result;
            result.code = !targetEntity.IsValid() ||
                                  targetEntity.sceneRuntimeId != m_sceneRuntimeId
                              ? AnimationSceneAdapters::
                                    EcsAnimationBindingPreparationCode::
                                        InvalidTargetEntity
                              : AnimationSceneAdapters::
                                    EcsAnimationBindingPreparationCode::
                                        InvalidRequestRef;
            result.diagnostic =
                "Animation binding request or target does not belong to this ECS Scene";
            return result;
        }

        return m_runtimeServices.PrepareCompatibleAnimationBinding(
            animation.request, targetEntity, animationClipOrdinal);
    }

    std::optional<Resource::ResourceContentVerificationReceipt>
    SampleAnimationLoader::GetContentVerificationReceipt(
        const std::filesystem::path& path) const
    {
        const auto receipt = m_contentVerificationReceipts.find(
            MakeLogicalPathKey(path));
        return receipt != m_contentVerificationReceipts.end()
                   ? std::optional<Resource::ResourceContentVerificationReceipt>(
                         receipt->second)
                   : std::nullopt;
    }

    std::string SampleAnimationLoader::MakeLogicalPathKey(
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
