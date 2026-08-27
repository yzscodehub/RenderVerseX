/** @file SampleParticleEffectLoader.cpp @brief Cooked-only Sample particle adapter. */

#include "Samples/SampleParticleEffectLoader.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace RVX
{
namespace
{
    [[nodiscard]] bool HasCookedEffectExtension(
        const std::filesystem::path& path)
    {
        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](const unsigned char value)
                       {
                           return static_cast<char>(std::tolower(value));
                       });
        return extension == ".rvxvfx";
    }

    [[nodiscard]] SampleParticleEffectLoadState ToSampleState(
        const Resource::ResourceLoadState state) noexcept
    {
        switch (state)
        {
            case Resource::ResourceLoadState::Queued:
            case Resource::ResourceLoadState::Loading:
            case Resource::ResourceLoadState::AwaitingPublish:
                return SampleParticleEffectLoadState::Pending;
            case Resource::ResourceLoadState::Ready:
                return SampleParticleEffectLoadState::Ready;
            case Resource::ResourceLoadState::Cancelled:
                return SampleParticleEffectLoadState::Cancelled;
            case Resource::ResourceLoadState::Failed:
            default:
                return SampleParticleEffectLoadState::Failed;
        }
    }
} // namespace

SampleParticleEffectLoader::SampleParticleEffectLoader(
    Resource::ResourceSubsystem& resources,
    SampleAssetRegistry assetRegistry) noexcept
    : m_resources(resources)
    , m_assetRegistry(std::move(assetRegistry))
{
}

bool SampleParticleEffectLoader::RequestByAssetId(
    const std::string_view assetId,
    LoadedSampleParticleEffect& outEffect,
    std::string& outError,
    SampleAssetRegistryLookupResult* outLookup) const
{
    if (outEffect.request.has_value() || outEffect.resource)
    {
        outError = "Particle effect output already owns a live request or resource.";
        return false;
    }

    const SampleAssetRegistryLookupResult lookup =
        m_assetRegistry.Lookup(assetId, SampleAssetKind::Effect);
    if (outLookup != nullptr)
    {
        *outLookup = lookup;
    }
    if (!lookup.IsFound())
    {
        outError = lookup.code == SampleAssetRegistryLookupCode::KindMismatch
            ? "SampleAssetRegistry kind mismatch for effect id '" +
                  std::string(assetId) + "': actual=" +
                  GetSampleAssetKindName(lookup.actualKind)
            : "SampleAssetRegistry unknown effect id: " + std::string(assetId);
        return false;
    }
    if (!lookup.entry->packageOnly ||
        !HasCookedEffectExtension(lookup.entry->resolvedPath) ||
        !lookup.entry->contentIdentity.IsValid() ||
        lookup.entry->contentIdentity.domain !=
            Resource::ResourceContentIdentityDomain::CookedArtifact)
    {
        outError = "Package-only sample effect entry must resolve to a verified "
                   "CookedArtifact .rvxvfx file: " + std::string(assetId);
        return false;
    }

    Resource::ResourceLoadOptions options;
    options.expectedContentIdentity = lookup.entry->contentIdentity;
    auto request = m_resources.RequestAsync<Particle::ParticleSystemResource>(
        lookup.entry->resolvedPath.string(), std::move(options));
    if (!request.IsValid())
    {
        outError = "ResourceSubsystem rejected cooked particle effect request: " +
                   std::string(assetId);
        return false;
    }

    outEffect.resourceRequestId = request.GetRequestId();
    outEffect.request.emplace(std::move(request));
    outEffect.state = SampleParticleEffectLoadState::Pending;
    outEffect.cookedPath = lookup.entry->resolvedPath;
    outEffect.diagnostic.clear();
    outError.clear();
    return true;
}

void SampleParticleEffectLoader::UpdateReadiness(
    LoadedSampleParticleEffect& effect) const
{
    if (!effect.request.has_value())
    {
        return;
    }

    const Resource::ResourceLoadSnapshot snapshot = effect.request->GetSnapshot();
    effect.state = ToSampleState(snapshot.state);
    if (effect.state == SampleParticleEffectLoadState::Ready)
    {
        effect.resource = effect.request->TryGet();
        if (!effect.resource || !effect.resource->IsLoaded())
        {
            effect.state = SampleParticleEffectLoadState::Failed;
            effect.diagnostic =
                "Cooked particle effect reported Ready without a published typed resource.";
        }
        else
        {
            effect.diagnostic.clear();
            m_receipts.insert_or_assign(
                effect.cookedPath, effect.resource->GetContentVerificationReceipt());
        }
    }
    else if (effect.state == SampleParticleEffectLoadState::Failed ||
             effect.state == SampleParticleEffectLoadState::Cancelled)
    {
        effect.diagnostic = snapshot.error.message.empty()
            ? (effect.state == SampleParticleEffectLoadState::Cancelled
                   ? "Cooked particle effect request was cancelled."
                   : "Cooked particle effect request failed.")
            : snapshot.error.message;
    }
}

bool SampleParticleEffectLoader::Cancel(LoadedSampleParticleEffect& effect) const
{
    bool cancelled = false;
    if (effect.request.has_value())
    {
        cancelled = effect.request->Cancel();
        effect.request.reset();
    }
    effect.resource = {};
    effect.state = SampleParticleEffectLoadState::Cancelled;
    if (effect.diagnostic.empty())
    {
        effect.diagnostic = "Cooked particle effect request was cancelled.";
    }
    return cancelled;
}

std::optional<Resource::ResourceContentVerificationReceipt>
SampleParticleEffectLoader::GetContentVerificationReceipt(
    const std::filesystem::path& cookedPath) const
{
    const auto receipt = m_receipts.find(cookedPath);
    return receipt == m_receipts.end()
        ? std::nullopt
        : std::optional<Resource::ResourceContentVerificationReceipt>(receipt->second);
}
} // namespace RVX
