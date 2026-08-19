#pragma once

/**
 * @file ParticleEcsRuntimeGateway.h
 * @brief Owner-thread particle runtime gateway for pure ECS feature bindings.
 */

#include "Particle/ECS/ParticleEcsBridge.h"

#include <memory>
#include <string>

namespace RVX::Resource
{
    class ResourceManager;
}

namespace RVX::Particle
{
    /**
     * @brief Owns CPU particle instances and their exact loaded-resource leases.
     *
     * The supplied ResourceManager must outlive this gateway. Create accepts
     * only a resource already published as a loaded ParticleSystemResource,
     * then retains both a typed ResourceHandle and an exact AssetResidencyLease
     * for the lifetime of the gateway entry. It never starts asynchronous I/O.
     * All calls, including destruction, must occur on the construction thread.
     */
    class ParticleEcsRuntimeGateway final : public IParticleEcsGateway
    {
    public:
        explicit ParticleEcsRuntimeGateway(Resource::ResourceManager& resourceManager);
        ~ParticleEcsRuntimeGateway() override;

        ParticleEcsRuntimeGateway(const ParticleEcsRuntimeGateway&) = delete;
        ParticleEcsRuntimeGateway& operator=(const ParticleEcsRuntimeGateway&) = delete;
        ParticleEcsRuntimeGateway(ParticleEcsRuntimeGateway&&) = delete;
        ParticleEcsRuntimeGateway& operator=(ParticleEcsRuntimeGateway&&) = delete;

        [[nodiscard]] ParticleEcsRuntimeHandle Create(
            const ParticleEcsRuntimeRequest& request) override;
        [[nodiscard]] bool Update(ParticleEcsRuntimeHandle handle,
                                  const ParticleEcsRuntimeRequest& request) override;
        [[nodiscard]] bool CaptureSnapshot(
            ParticleEcsRuntimeHandle handle,
            ParticleEcsRuntimeSnapshot& outSnapshot) const override;
        [[nodiscard]] ParticleEcsReleaseResult Release(
            ParticleEcsRuntimeHandle handle) override;
        [[nodiscard]] bool IsAlive(ParticleEcsRuntimeHandle handle) const override;

        /** @brief Last owner-thread rejection diagnostic, empty before the first failure. */
        [[nodiscard]] const std::string& GetLastFailureReason() const;
        /** @brief Owner-thread diagnostic count of entries holding runtime/resource ownership. */
        [[nodiscard]] uint32 GetLiveRuntimeCount() const;

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace RVX::Particle
