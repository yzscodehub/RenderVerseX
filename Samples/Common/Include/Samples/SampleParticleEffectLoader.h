#pragma once

/**
 * @file SampleParticleEffectLoader.h
 * @brief Catalog-gated cooked particle-effect request adapter for samples.
 */

#include "Particle/ParticleSystemLoader.h"
#include "Resource/ResourceLoadOperation.h"
#include "Resource/ResourceSubsystem.h"
#include "Samples/SampleContext.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace RVX
{
    enum class SampleParticleEffectLoadState : uint8
    {
        Idle = 0,
        Pending,
        Ready,
        Failed,
        Cancelled,
    };

    /**
     * @brief One scenario-owned asynchronous cooked-effect request.
     *
     * The request owns neither ResourceManager nor render state.  Once ready
     * it retains the exact published ParticleSystemResource so a scenario can
     * publish only its public ResourceId into ParticleEcsConfig.
     */
    struct LoadedSampleParticleEffect final
    {
        Resource::ResourceLoadRequestId resourceRequestId{};
        Resource::ResourceHandle<Particle::ParticleSystemResource> resource;
        std::optional<Resource::ResourceLoadHandle<Particle::ParticleSystemResource>>
            request;
        SampleParticleEffectLoadState state =
            SampleParticleEffectLoadState::Idle;
        std::filesystem::path cookedPath;
        std::string diagnostic;

        [[nodiscard]] bool IsReady() const noexcept
        {
            return state == SampleParticleEffectLoadState::Ready &&
                   resource && resource->IsLoaded();
        }

        [[nodiscard]] Resource::ResourceId GetResourceId() const noexcept
        {
            return IsReady() ? resource->GetId() : Resource::InvalidResourceId;
        }
    };

    /**
     * @brief Narrow Sample-facing loader for catalog-admitted `.rvxvfx` packages.
     *
     * It only accepts immutable runner-admitted Effect entries.  Source paths,
     * ResourceManager internals, RHI state, and render objects never cross
     * into scenario code.
     */
    class SampleParticleEffectLoader final
    {
    public:
        SampleParticleEffectLoader(Resource::ResourceSubsystem& resources,
                                   SampleAssetRegistry assetRegistry) noexcept;

        [[nodiscard]] bool RequestByAssetId(
            std::string_view assetId,
            LoadedSampleParticleEffect& outEffect,
            std::string& outError,
            SampleAssetRegistryLookupResult* outLookup = nullptr) const;

        /** @brief Refreshes a copied terminal state; never blocks the owner thread. */
        void UpdateReadiness(LoadedSampleParticleEffect& effect) const;

        /** @brief Cancels only this scenario subscription and releases its strong handle. */
        [[nodiscard]] bool Cancel(LoadedSampleParticleEffect& effect) const;

        [[nodiscard]] std::optional<Resource::ResourceContentVerificationReceipt>
        GetContentVerificationReceipt(const std::filesystem::path& cookedPath) const;

    private:
        Resource::ResourceSubsystem& m_resources;
        SampleAssetRegistry m_assetRegistry;
        mutable std::map<std::filesystem::path,
                         Resource::ResourceContentVerificationReceipt> m_receipts;
    };
} // namespace RVX
