#include "Particle/ECS/ParticleEcsRuntimeGateway.h"
#include "Particle/ParticleSystemLoader.h"
#include "Core/Log.h"
#include "Resource/ResourceManager.h"
#include "Resource/RuntimeResourcePolicy.h"

#include <iostream>
#include <memory>
#include <optional>
#include <string_view>

namespace
{
    class LogScope final
    {
    public:
        LogScope() { RVX::Log::Initialize(); }
        ~LogScope() { RVX::Log::Shutdown(); }
    };

    class ResourceManagerScope final
    {
    public:
        ResourceManagerScope()
        {
            RVX::Resource::ResourceManagerConfig config;
            config.asyncThreadCount = 0;
            config.runtimePolicy.mode = RVX::Resource::ResourceRuntimeMode::Editor;
            config.runtimePolicy.allowSourceAssetReads = true;
            RVX::Resource::ResourceManager::Get().Initialize(config);
        }

        ~ResourceManagerScope()
        {
            RVX::Resource::ResourceManager::Get().Shutdown();
        }
    };

    bool Check(bool value, const char* message)
    {
        if (!value)
        {
            std::cerr << message << '\n';
        }
        return value;
    }
}

int main()
{
    LogScope logScope;
    ResourceManagerScope managerScope;
    RVX::Resource::ResourceManager& manager = RVX::Resource::ResourceManager::Get();
    manager.RegisterLoader(
        RVX::Particle::ResourceType_ParticleSystem,
        std::make_unique<RVX::Particle::ParticleSystemLoader>());

    RVX::Resource::IResource* loaded = manager.LoadResource(
        "Tests/ParticleEcsRuntimeGatewayValidation/Fixture.particle",
        RVX::Particle::ResourceType_ParticleSystem);
    auto* particleResource = dynamic_cast<RVX::Particle::ParticleSystemResource*>(loaded);
    if (!Check(particleResource != nullptr && particleResource->IsLoaded() &&
                   particleResource->GetSystem() != nullptr,
               "load particle fixture"))
    {
        return 1;
    }

    const RVX::Particle::ParticleEcsConfig config{
        .systemAssetId = {particleResource->GetId()},
        .configurationRevision = 1,
        .emissionRateScale = 1.0f,
        .simulationSpeed = 1.0f,
        .maxParticleCount = particleResource->GetSystem()->maxParticles,
        .autoPlay = true,
    };
    const RVX::Resource::ResourceHandle<RVX::Particle::ParticleSystemResource> retained =
        manager.TryAcquireLoaded<RVX::Particle::ParticleSystemResource>(
            config.systemAssetId.value);
    const std::optional<RVX::Resource::AssetKey> assetKey =
        manager.FindPublishedAssetKey(config.systemAssetId.value);
    if (!Check(retained && retained.IsLoaded() && !assetKey.has_value(),
               "demonstrate unavailable exact particle residency seam"))
    {
        return 1;
    }
    RVX::Particle::ParticleEcsRuntimeGateway gateway(manager);
    const RVX::Particle::ParticleEcsRuntimeHandle first = gateway.Create({
        .config = config,
        .worldTransform = RVX::Mat4(1.0f),
        .command = RVX::Particle::ParticleEcsCommand::Play,
        .deltaSeconds = 1.0 / 60.0,
    });
    if (!Check(!first.IsValid() && !gateway.IsAlive(first) && gateway.GetLiveRuntimeCount() == 0 &&
                   std::string_view(gateway.GetLastFailureReason()).find("published exact AssetKey") !=
                       std::string_view::npos,
               "reject particle runtime without exact residency proof"))
    {
        return 1;
    }

    const RVX::Particle::ParticleEcsRuntimeRequest missingRequest{
        .config = RVX::Particle::ParticleEcsConfig{
            .systemAssetId = {particleResource->GetId() + 1},
            .configurationRevision = 1,
            .maxParticleCount = config.maxParticleCount,
        },
        .worldTransform = RVX::Mat4(1.0f),
    };
    if (!Check(!gateway.Create(missingRequest).IsValid(),
               "reject missing resource without asynchronous fallback"))
    {
        return 1;
    }

    return Check(gateway.GetLiveRuntimeCount() == 0 &&
                     gateway.Release(first) == RVX::Particle::ParticleEcsReleaseResult::AlreadyAbsent,
                 "release absent particle runtime") ?
               0 :
               1;
}
