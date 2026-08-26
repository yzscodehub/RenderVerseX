#include "Particle/ECS/ParticleEcsRuntimeGateway.h"
#include "Particle/Emitters/PointEmitter.h"
#include "Particle/Modules/CollisionModule.h"
#include "Particle/ParticleSubsystem.h"
#include "Particle/ParticleSystemLoader.h"
#include "Core/Log.h"
#include "Resource/ResourceManager.h"
#include "Resource/RuntimeResourcePolicy.h"

#include <cstddef>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
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
            RVX::Resource::ResourceManagerDependencies dependencies;
            std::string error;
            if (!dependencies.loaderCatalog.Add(
                    std::make_unique<RVX::Particle::ParticleSystemLoader>(),
                    error))
            {
                throw std::runtime_error(error);
            }
            m_manager.Initialize(config, std::move(dependencies));
        }

        ~ResourceManagerScope()
        {
            m_manager.Shutdown();
        }

        RVX::Resource::ResourceManager& GetManager() noexcept
        {
            return m_manager;
        }

    private:
        RVX::Resource::ResourceManager m_manager;
    };

    bool Check(bool value, const char* message)
    {
        if (!value)
        {
            std::cerr << message << '\n';
        }
        return value;
    }

    bool PacketsHaveSameFields(const RVX::ParticleCpuPacket& left,
                               const RVX::ParticleCpuPacket& right)
    {
        if (left.schemaVersion != right.schemaVersion ||
            left.simulationTick != right.simulationTick ||
            left.deterministicSeed != right.deterministicSeed ||
            left.deterministicInstanceKey != right.deterministicInstanceKey ||
            left.effectiveSimulationSeed != right.effectiveSimulationSeed ||
            left.contentHash != right.contentHash ||
            left.particles.size() != right.particles.size())
        {
            return false;
        }

        for (size_t index = 0; index < left.particles.size(); ++index)
        {
            const RVX::ParticleRenderParticleData& a = left.particles[index];
            const RVX::ParticleRenderParticleData& b = right.particles[index];
            if (a.stableParticleId != b.stableParticleId ||
                a.position.x != b.position.x || a.position.y != b.position.y ||
                a.position.z != b.position.z || a.lifetime != b.lifetime ||
                a.velocity.x != b.velocity.x || a.velocity.y != b.velocity.y ||
                a.velocity.z != b.velocity.z || a.age != b.age ||
                a.color.x != b.color.x || a.color.y != b.color.y ||
                a.color.z != b.color.z || a.color.w != b.color.w ||
                a.size.x != b.size.x || a.size.y != b.size.y ||
                a.rotation != b.rotation || a.flags != b.flags)
            {
                return false;
            }
        }
        return true;
    }
}

int main()
{
    LogScope logScope;
    ResourceManagerScope managerScope;
    RVX::Resource::ResourceManager& manager = managerScope.GetManager();

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

    // The checked-in fixture deliberately has no authored emitters.  Add one
    // deterministic runtime-only source so this validation exercises an
    // actual immutable CPU packet rather than metadata-only output.
    RVX::Particle::ParticleSystem::Ptr system = particleResource->GetSystem();
    auto* emitter = system->AddEmitter<RVX::Particle::PointEmitter>();
    emitter->emissionRate = 120.0f;
    system->softParticleConfig.enabled = false;

    const RVX::Particle::ParticleEcsConfig config{
        .systemAssetId = {particleResource->GetId()},
        .configurationRevision = 1,
        .deterministicSeed = 0x51A7u,
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
    if (!Check(retained && retained.IsLoaded() && assetKey.has_value(),
               "publish exact particle residency identity"))
    {
        return 1;
    }
    RVX::Particle::ParticleEcsRuntimeGateway gateway(manager);
    const RVX::Particle::ParticleEcsRuntimeHandle first = gateway.Create({
        .config = config,
        .worldTransform = RVX::Mat4(1.0f),
        .command = RVX::Particle::ParticleEcsCommand::Play,
        .simulationTick = 1,
        .deterministicInstanceKey = 0xC0FFEEu,
    });
    if (!Check(first.IsValid() && gateway.IsAlive(first) && gateway.GetLiveRuntimeCount() == 1,
               "create particle runtime with exact residency proof"))
    {
        return 1;
    }

    RVX::Particle::ParticleEcsRuntimeSnapshot emptySnapshot;
    if (!Check(gateway.CaptureSnapshot(first, emptySnapshot) &&
                   emptySnapshot.item.HasValidCpuPacket() &&
                   emptySnapshot.item.GetParticleCount() == 0 &&
                   emptySnapshot.item.payloadStatus ==
                       RVX::ParticleRenderSnapshotPayloadStatus::CpuPacketReady,
               "publish a supported no-op packet before the first CPU particle emission"))
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
        .deterministicInstanceKey = 0xC0FFEEu,
    };
    if (!Check(!gateway.Create(missingRequest).IsValid(),
               "reject missing resource without asynchronous fallback"))
    {
        return 1;
    }

    const RVX::Particle::ParticleEcsRuntimeRequest tickOneRequest{
        .config = config,
        .worldTransform = RVX::Mat4(1.0f),
        .deltaSeconds = 1.0 / 60.0,
        .simulationTick = 1,
        .deterministicInstanceKey = 0xC0FFEEu,
    };
    if (!Check(gateway.Update(first, tickOneRequest),
               "advance first deterministic particle tick"))
    {
        return 1;
    }
    RVX::Particle::ParticleEcsRuntimeSnapshot firstSnapshot;
    if (!Check(gateway.CaptureSnapshot(first, firstSnapshot) &&
                   firstSnapshot.item.HasValidCpuPacket() &&
                   firstSnapshot.item.cpuPacket.simulationTick == 1 &&
                   firstSnapshot.item.GetParticleCount() > 0,
               "capture sealed deterministic CPU packet"))
    {
        return 1;
    }
    const RVX::uint64 firstHash = firstSnapshot.item.cpuPacket.contentHash;
    const float firstPositionX = firstSnapshot.item.GetParticles().front().position.x;

    if (!Check(!gateway.Update(first, tickOneRequest),
               "reject duplicate positive simulation tick"))
    {
        return 1;
    }

    const RVX::Particle::ParticleEcsRuntimeHandle second = gateway.Create({
        .config = config,
        .worldTransform = RVX::Mat4(1.0f),
        .command = RVX::Particle::ParticleEcsCommand::Play,
        .simulationTick = 1,
        .deterministicInstanceKey = 0xC0FFEEu,
    });
    if (!Check(second.IsValid() && gateway.Update(second, tickOneRequest),
               "create second equivalent deterministic particle runtime"))
    {
        return 1;
    }
    RVX::Particle::ParticleEcsRuntimeSnapshot secondSnapshot;
    if (!Check(gateway.CaptureSnapshot(second, secondSnapshot) &&
                   secondSnapshot.item.HasValidCpuPacket() &&
                   secondSnapshot.item.cpuPacket.contentHash == firstHash &&
                   PacketsHaveSameFields(firstSnapshot.item.cpuPacket,
                                         secondSnapshot.item.cpuPacket),
               "fresh equivalent runtimes publish field-identical packet content"))
    {
        return 1;
    }

    const RVX::Particle::ParticleEcsRuntimeRequest tickTwoRequest{
        .config = config,
        .worldTransform = RVX::Mat4(1.0f),
        .deltaSeconds = 1.0 / 60.0,
        .simulationTick = 2,
        .deterministicInstanceKey = 0xC0FFEEu,
    };
    if (!Check(gateway.Update(first, tickTwoRequest) &&
                   firstSnapshot.item.cpuPacket.contentHash == firstHash &&
                   firstSnapshot.item.GetParticles().front().position.x == firstPositionX,
               "later simulation cannot mutate an already published packet"))
    {
        return 1;
    }
    if (!Check(!gateway.Update(first, tickOneRequest),
               "reject a positive simulation tick rollback"))
    {
        return 1;
    }

    RVX::Particle::ParticleEcsConfig changedSeedConfig = config;
    changedSeedConfig.configurationRevision = 2;
    changedSeedConfig.deterministicSeed = 0xBADC0DEu;
    const RVX::Particle::ParticleEcsRuntimeRequest resetTickOneRequest{
        .config = changedSeedConfig,
        .worldTransform = RVX::Mat4(1.0f),
        .deltaSeconds = 1.0 / 60.0,
        .simulationTick = 1,
        .deterministicInstanceKey = 0xC0FFEEu,
    };
    if (!Check(gateway.Update(first, resetTickOneRequest),
               "rebuild runtime when its deterministic seed changes"))
    {
        return 1;
    }
    RVX::Particle::ParticleEcsRuntimeSnapshot resetSnapshot;
    if (!Check(gateway.CaptureSnapshot(first, resetSnapshot) &&
                   resetSnapshot.item.HasValidCpuPacket() &&
                   resetSnapshot.item.cpuPacket.simulationTick == 1 &&
                   resetSnapshot.item.cpuPacket.deterministicSeed ==
                       changedSeedConfig.deterministicSeed,
               "rebuild publishes the changed seed provenance"))
    {
        return 1;
    }

    const RVX::Particle::ParticleEcsRuntimeHandle resetReference = gateway.Create({
        .config = changedSeedConfig,
        .worldTransform = RVX::Mat4(1.0f),
        .command = RVX::Particle::ParticleEcsCommand::Play,
        .simulationTick = 1,
        .deterministicInstanceKey = 0xC0FFEEu,
    });
    if (!Check(resetReference.IsValid() &&
                   gateway.Update(resetReference, resetTickOneRequest),
               "create fresh reference after deterministic seed change"))
    {
        return 1;
    }
    RVX::Particle::ParticleEcsRuntimeSnapshot resetReferenceSnapshot;
    if (!Check(gateway.CaptureSnapshot(resetReference, resetReferenceSnapshot) &&
                   resetReferenceSnapshot.item.HasValidCpuPacket() &&
                   resetReferenceSnapshot.item.cpuPacket.contentHash ==
                       resetSnapshot.item.cpuPacket.contentHash &&
                   PacketsHaveSameFields(resetSnapshot.item.cpuPacket,
                                         resetReferenceSnapshot.item.cpuPacket),
               "rebuilt runtime matches a fresh simulator seeded from the new identity"))
    {
        return 1;
    }

    if (!Check(gateway.GetLiveRuntimeCount() == 3 &&
                   gateway.Release(first) == RVX::Particle::ParticleEcsReleaseResult::Released &&
                   !gateway.IsAlive(first) && gateway.GetLiveRuntimeCount() == 2 &&
                   gateway.Release(second) == RVX::Particle::ParticleEcsReleaseResult::Released &&
                   gateway.GetLiveRuntimeCount() == 1 &&
                   gateway.Release(resetReference) == RVX::Particle::ParticleEcsReleaseResult::Released &&
                   gateway.GetLiveRuntimeCount() == 0,
               "release resident particle runtimes after deterministic validation"))
    {
        return 1;
    }

    if (!Check(gateway.Release(first) == RVX::Particle::ParticleEcsReleaseResult::AlreadyAbsent,
               "release absent particle runtime"))
    {
        return 1;
    }

    RVX::Particle::ParticleSubsystem directSubsystem;
    directSubsystem.GetConfig().useConfiguredCpuSimulationSeed = true;
    directSubsystem.GetConfig().cpuSimulationSeed = config.deterministicSeed;
    directSubsystem.Initialize();
    RVX::Particle::ParticleSystemInstance* directFirst =
        directSubsystem.CreateInstance(system);
    if (!Check(directFirst != nullptr, "create direct CPU particle instance"))
    {
        return 1;
    }
    directFirst->Play();
    directSubsystem.Simulate(1.0f / 60.0f);
    RVX::ParticleRenderSnapshot directFirstSnapshot;
    if (!Check(directSubsystem.BuildRenderSnapshot(directFirstSnapshot) &&
                   directFirstSnapshot.items.size() == 1 &&
                   directFirstSnapshot.items.front().HasValidCpuPacket(),
               "capture direct CPU packet before subsystem reinitialize"))
    {
        return 1;
    }
    const RVX::uint64 directFirstHash =
        directFirstSnapshot.items.front().cpuPacket.contentHash;
    const float directFirstPositionX =
        directFirstSnapshot.items.front().GetParticles().front().position.x;

    directFirst->Restart();
    directFirst->SetSimulationTick(1);
    directFirst->Simulate(1.0f / 60.0f);
    RVX::ParticleRenderSnapshot restartSnapshot;
    if (!Check(directSubsystem.BuildRenderSnapshot(restartSnapshot) &&
                   restartSnapshot.items.size() == 1 &&
                   restartSnapshot.items.front().HasValidCpuPacket() &&
                   PacketsHaveSameFields(
                       directFirstSnapshot.items.front().cpuPacket,
                       restartSnapshot.items.front().cpuPacket) &&
                   directFirstSnapshot.items.front().cpuPacket.contentHash ==
                       directFirstHash &&
                   directFirstSnapshot.items.front().GetParticles().front().position.x ==
                       directFirstPositionX,
               "restart reseeds CPU RNG to reproduce the fresh sealed packet"))
    {
        return 1;
    }

    directFirst->Stop();
    directFirst->Play();
    directFirst->SetSimulationTick(1);
    directFirst->Simulate(1.0f / 60.0f);
    RVX::ParticleRenderSnapshot stopPlaySnapshot;
    if (!Check(directSubsystem.BuildRenderSnapshot(stopPlaySnapshot) &&
                   stopPlaySnapshot.items.size() == 1 &&
                   stopPlaySnapshot.items.front().HasValidCpuPacket() &&
                   PacketsHaveSameFields(
                       directFirstSnapshot.items.front().cpuPacket,
                       stopPlaySnapshot.items.front().cpuPacket) &&
                   directFirstSnapshot.items.front().cpuPacket.contentHash ==
                       directFirstHash &&
                   directFirstSnapshot.items.front().GetParticles().front().position.x ==
                       directFirstPositionX,
               "stop/play reseeds CPU RNG to reproduce the fresh sealed packet"))
    {
        return 1;
    }
    directSubsystem.Deinitialize();

    directSubsystem.Initialize();
    RVX::Particle::ParticleSystemInstance* directSecond =
        directSubsystem.CreateInstance(system);
    if (!Check(directSecond != nullptr, "create direct CPU particle after subsystem reinitialize"))
    {
        return 1;
    }
    directSecond->Play();
    directSubsystem.Simulate(1.0f / 60.0f);
    RVX::ParticleRenderSnapshot directSecondSnapshot;
    const bool directReinitDeterministic =
        directSubsystem.BuildRenderSnapshot(directSecondSnapshot) &&
        directSecondSnapshot.items.size() == 1 &&
        directSecondSnapshot.items.front().HasValidCpuPacket() &&
        directSecondSnapshot.items.front().cpuPacket.deterministicInstanceKey == 1 &&
        directSecondSnapshot.items.front().cpuPacket.contentHash == directFirstHash;
    directSubsystem.Deinitialize();

    auto collisionSystem = RVX::Particle::ParticleSystem::Create("CollisionLifetimeLoss");
    collisionSystem->maxParticles = 4;
    // GFX16 deliberately rejects soft particles. Keep this focused test on
    // the supported CPU Billboard/Alpha path so it validates collision
    // lifetime handling rather than the independent policy fallback.
    collisionSystem->softParticleConfig.enabled = false;
    auto* collisionEmitter =
        collisionSystem->AddEmitter<RVX::Particle::PointEmitter>();
    collisionEmitter->emissionRate = 60.0f;
    collisionEmitter->initialLifetime = {1.0f, 1.0f};
    collisionEmitter->initialSpeed = {0.0f, 0.0f};
    collisionEmitter->initialSize = {0.1f, 0.1f};
    collisionEmitter->useShapeVelocity = false;
    auto* collision =
        collisionSystem->AddModule<RVX::Particle::CollisionModule>();
    collision->bounce = 0.0f;
    collision->lifetimeLoss = 1.0f;
    collision->radiusScale = 1.0f;
    collision->AddGroundPlane();

    RVX::Particle::ParticleSubsystem collisionSubsystem;
    collisionSubsystem.GetConfig().useConfiguredCpuSimulationSeed = true;
    collisionSubsystem.GetConfig().cpuSimulationSeed = config.deterministicSeed;
    collisionSubsystem.Initialize();
    RVX::Particle::ParticleSystemInstance* collisionInstance =
        collisionSubsystem.CreateInstance(collisionSystem);
    if (!Check(collisionInstance != nullptr,
               "create collision lifetime-loss particle instance"))
    {
        return 1;
    }
    collisionInstance->Play();
    collisionSubsystem.Simulate(1.0f / 60.0f);
    RVX::ParticleRenderSnapshot collisionSnapshot;
    const bool collisionPacketIsPublishable =
        collisionInstance->GetAliveCount() == 0 &&
        collisionSubsystem.BuildRenderSnapshot(collisionSnapshot) &&
        collisionSnapshot.items.size() == 1 &&
        collisionSnapshot.items.front().HasValidCpuPacket() &&
        collisionSnapshot.items.front().payloadStatus ==
            RVX::ParticleRenderSnapshotPayloadStatus::CpuPacketReady &&
        collisionSnapshot.items.front().GetParticleCount() == 0;
    collisionSubsystem.Deinitialize();

    return Check(directReinitDeterministic,
                 "direct CPU packet is deterministic across a clean subsystem reinitialize") &&
                   Check(collisionPacketIsPublishable,
                         "collision lifetime loss retires expired particles before publishing an empty CPU packet") ?
               0 :
               1;
}
