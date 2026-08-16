#include "Particle/ECS/ParticleEcsBridge.h"
#include "Terrain/ECS/TerrainEcsBridge.h"
#include "Water/ECS/WaterEcsBridge.h"

#include <iostream>
#include <vector>

namespace
{
    class Gateway final : public RVX::Particle::IParticleEcsGateway
    {
    public:
        RVX::Particle::ParticleEcsRuntimeHandle Create(const RVX::Particle::ParticleEcsRuntimeRequest& request) override
        {
            m_entries.push_back({true, 1, request.config.systemAssetId.value, request.config.maxParticleCount});
            return RVX::Particle::ParticleEcsRuntimeHandle::Create(static_cast<RVX::uint32>(m_entries.size() - 1u), 1);
        }
        bool Update(RVX::Particle::ParticleEcsRuntimeHandle handle, const RVX::Particle::ParticleEcsRuntimeRequest& request) override
        { return IsAlive(handle) && request.config.systemAssetId.IsValid(); }
        bool CaptureSnapshot(RVX::Particle::ParticleEcsRuntimeHandle handle, RVX::Particle::ParticleEcsRuntimeSnapshot& out) const override
        {
            if (!IsAlive(handle)) return false;
            const Entry& entry = m_entries[handle.GetIndex()];
            out.item.instanceId = handle.GetPackedValue(); out.item.systemId = entry.systemId;
            out.item.visible = true; out.item.simulationSupported = true; out.item.renderPayloadAvailable = true;
            out.item.maxParticleCount = entry.maxParticleCount; out.payloadRevision = 7; return true;
        }
        RVX::Particle::ParticleEcsReleaseResult Release(RVX::Particle::ParticleEcsRuntimeHandle handle) override
        { if (!handle.IsValid() || handle.GetIndex() >= m_entries.size() || m_entries[handle.GetIndex()].generation != handle.GetGeneration()) return RVX::Particle::ParticleEcsReleaseResult::AlreadyAbsent; m_entries[handle.GetIndex()].alive = false; ++m_entries[handle.GetIndex()].generation; return RVX::Particle::ParticleEcsReleaseResult::Released; }
        bool IsAlive(RVX::Particle::ParticleEcsRuntimeHandle handle) const override
        { return handle.IsValid() && handle.GetIndex() < m_entries.size() && m_entries[handle.GetIndex()].alive && m_entries[handle.GetIndex()].generation == handle.GetGeneration(); }
    private: struct Entry { bool alive; RVX::uint32 generation; RVX::uint64 systemId; RVX::uint32 maxParticleCount; }; std::vector<Entry> m_entries;
    };
    bool Check(bool value, const char* message) { if (!value) std::cerr << message << '\n'; return value; }

    class WaterGateway final : public RVX::Water::IWaterEcsGateway
    {
    public:
        RVX::Water::WaterEcsRuntimeHandle Create(const RVX::Water::WaterEcsRuntimeRequest&) override { m_alive = true; return RVX::Water::WaterEcsRuntimeHandle::Create(0, 1); }
        bool Update(RVX::Water::WaterEcsRuntimeHandle handle, const RVX::Water::WaterEcsRuntimeRequest&) override { return IsAlive(handle); }
        bool CaptureSnapshot(RVX::Water::WaterEcsRuntimeHandle handle, RVX::Water::WaterEcsRuntimeSnapshot& out) const override { if (!IsAlive(handle)) return false; out.item.resolution = 8; out.payloadRevision = 1; return true; }
        RVX::Water::WaterEcsReleaseResult Release(RVX::Water::WaterEcsRuntimeHandle handle) override { if (!IsAlive(handle)) return RVX::Water::WaterEcsReleaseResult::AlreadyAbsent; m_alive = false; return RVX::Water::WaterEcsReleaseResult::Released; }
        bool IsAlive(RVX::Water::WaterEcsRuntimeHandle handle) const override { return m_alive && handle == RVX::Water::WaterEcsRuntimeHandle::Create(0, 1); }
    private: bool m_alive = false;
    };
    class TerrainGateway final : public RVX::Terrain::ITerrainEcsGateway
    {
    public:
        RVX::Terrain::TerrainEcsRuntimeHandle Create(const RVX::Terrain::TerrainEcsRuntimeRequest&) override { m_alive = true; return RVX::Terrain::TerrainEcsRuntimeHandle::Create(0, 1); }
        bool Update(RVX::Terrain::TerrainEcsRuntimeHandle handle, const RVX::Terrain::TerrainEcsRuntimeRequest&) override { return IsAlive(handle); }
        bool CaptureSnapshot(RVX::Terrain::TerrainEcsRuntimeHandle handle, RVX::Terrain::TerrainEcsRuntimeSnapshot& out) const override { if (!IsAlive(handle)) return false; out.item.patchSize = 8; out.payloadRevision = 1; return true; }
        RVX::Terrain::TerrainEcsReleaseResult Release(RVX::Terrain::TerrainEcsRuntimeHandle handle) override { if (!IsAlive(handle)) return RVX::Terrain::TerrainEcsReleaseResult::AlreadyAbsent; m_alive = false; return RVX::Terrain::TerrainEcsReleaseResult::Released; }
        bool IsAlive(RVX::Terrain::TerrainEcsRuntimeHandle handle) const override { return m_alive && handle == RVX::Terrain::TerrainEcsRuntimeHandle::Create(0, 1); }
    private: bool m_alive = false;
    };

    bool RunMultiFeatureEntityRegression()
    {
        RVX::SceneECS::SceneEcsRuntime runtime(1, 1);
        Gateway particleGateway; WaterGateway waterGateway; TerrainGateway terrainGateway;
        RVX::Particle::ParticleEcsBridge particle(runtime, particleGateway);
        RVX::Water::WaterEcsBridge water(runtime, waterGateway);
        RVX::Terrain::TerrainEcsBridge terrain(runtime, terrainGateway);
        if (!Check(particle.RegisterProcessors() == RVX::Particle::ParticleEcsBridgeRegistrationResult::Registered &&
                       water.RegisterProcessors() == RVX::Water::WaterEcsBridgeRegistrationResult::Registered &&
                       terrain.RegisterProcessors() == RVX::Terrain::TerrainEcsBridgeRegistrationResult::Registered,
                   "multi registration")) return false;
        const RVX::ECS::EntityHandle entity = runtime.CreateEntity();
        if (!Check(runtime.AddFragment(entity, RVX::SceneECS::ParticleRuntimeTag{}) &&
                       runtime.AddFragment(entity, RVX::SceneECS::WaterRuntimeTag{}) &&
                       runtime.AddFragment(entity, RVX::SceneECS::TerrainRuntimeTag{}) &&
                       runtime.AddFragment(entity, RVX::Particle::ParticleEcsConfig{.systemAssetId = {101}}) &&
                       runtime.AddFragment(entity, RVX::Particle::ParticleEcsIntent{}) &&
                       runtime.AddFragment(entity, RVX::Particle::ParticleEcsState{}) &&
                       runtime.AddFragment(entity, RVX::Water::WaterEcsConfig{.surfaceAssetId = {102}, .materialAssetId = {103}}) &&
                       runtime.AddFragment(entity, RVX::Water::WaterEcsIntent{}) &&
                       runtime.AddFragment(entity, RVX::Water::WaterEcsState{}) &&
                       runtime.AddFragment(entity, RVX::Terrain::TerrainEcsConfig{.heightmapAssetId = {104}, .materialAssetId = {105}}) &&
                       runtime.AddFragment(entity, RVX::Terrain::TerrainEcsIntent{}) &&
                       runtime.AddFragment(entity, RVX::Terrain::TerrainEcsState{}),
                   "multi setup")) return false;
        if (!Check(runtime.Tick().succeeded && runtime.GetFeatureSnapshotCounts().particleCount == 1 &&
                       runtime.GetFeatureSnapshotCounts().waterCount == 1 && runtime.GetFeatureSnapshotCounts().terrainCount == 1,
                   "multi publication")) return false;
        if (!Check(runtime.RequestDestroy(
                       entity,
                       RVX::SceneECS::ToCleanupDomainMask(
                           RVX::SceneECS::CleanupDomain::None)) ==
                           RVX::SceneECS::DestroyRequestResult::Accepted &&
                       runtime.Tick().succeeded,
                   "multi destruction")) return false;
        const RVX::SceneECS::EntityLifecycleState* lifecycle = runtime.GetRegistry().TryGet<RVX::SceneECS::EntityLifecycleState>(entity);
        const RVX::SceneECS::CleanupDomainMask features =
            RVX::SceneECS::ToCleanupDomainMask(RVX::SceneECS::CleanupDomain::ParticleFeature) |
            RVX::SceneECS::ToCleanupDomainMask(RVX::SceneECS::CleanupDomain::WaterFeature) |
            RVX::SceneECS::ToCleanupDomainMask(RVX::SceneECS::CleanupDomain::TerrainFeature);
        if (!Check(lifecycle != nullptr && (lifecycle->acknowledgedCleanupDomains & features) == features &&
                       runtime.GetFeatureSnapshotCounts().particleCount == 0 && runtime.GetFeatureSnapshotCounts().waterCount == 0 && runtime.GetFeatureSnapshotCounts().terrainCount == 0 &&
                       particle.GetDiagnosticsSnapshot().outstandingRuntimeCount == 0 && water.GetDiagnosticsSnapshot().outstandingRuntimeCount == 0 && terrain.GetDiagnosticsSnapshot().outstandingRuntimeCount == 0,
                   "multi independent cleanup")) return false;
        if (!Check(runtime.AcknowledgeCleanup(
                       entity,
                       RVX::SceneECS::ToCleanupDomainMask(
                           RVX::SceneECS::CleanupDomain::Render)),
                   "multi render cleanup acknowledgement")) return false;
        const RVX::SceneECS::SceneEcsTickResult recycleTick = runtime.Tick();
        if (!Check(recycleTick.succeeded, "multi recycle tick")) return false;
        if (!Check(!runtime.GetRegistry().IsAlive(entity),
                   "multi recycle after all acknowledgements"))
        {
            const RVX::SceneECS::EntityLifecycleState* retained =
                runtime.GetRegistry().TryGet<RVX::SceneECS::EntityLifecycleState>(entity);
            if (retained != nullptr)
            {
                std::cerr << "phase=" << static_cast<unsigned>(retained->phase)
                          << " required=" << retained->requiredCleanupDomains
                          << " acknowledged=" << retained->acknowledgedCleanupDomains
                          << " advanced=" << recycleTick.advancedRetirementCount
                          << " recycled=" << recycleTick.recycledEntityCount << '\n';
            }
            return false;
        }
        return true;
    }
}

int main()
{
    RVX::SceneECS::SceneEcsRuntime runtime(1, 1); Gateway gateway; RVX::Particle::ParticleEcsBridge bridge(runtime, gateway);
    if (!Check(bridge.RegisterProcessors() == RVX::Particle::ParticleEcsBridgeRegistrationResult::Registered, "registration")) return 1;
    const RVX::ECS::EntityHandle entity = runtime.CreateEntity();
    if (!Check(runtime.AddFragment(entity, RVX::SceneECS::ParticleRuntimeTag{}) && runtime.AddFragment(entity, RVX::Particle::ParticleEcsConfig{.systemAssetId = {11}, .configurationRevision = 3, .maxParticleCount = 16}) && runtime.AddFragment(entity, RVX::Particle::ParticleEcsIntent{}) && runtime.AddFragment(entity, RVX::Particle::ParticleEcsState{}), "setup")) return 1;
    if (!Check(runtime.Tick({.variableDeltaSeconds = 1.0 / 60.0}).succeeded, "feature tick")) return 1;
    const auto frozen = runtime.GetLatestFrozenSnapshot();
    if (!Check(frozen != nullptr && frozen->particles.size() == 1 &&
                   frozen->particles.front().state.systemAssetId == RVX::AssetId{11},
               "frozen particle")) return 1;
    if (!Check(bridge.FindRuntime(runtime.GetSceneRuntimeId(), entity).IsValid(), "runtime identity")) return 1;
    if (!Check(runtime.RequestDestroy(
                   entity,
                   RVX::SceneECS::ToCleanupDomainMask(
                       RVX::SceneECS::CleanupDomain::None)) ==
                   RVX::SceneECS::DestroyRequestResult::Accepted,
               "destroy request")) return 1;
    if (!Check(runtime.Tick().succeeded && runtime.Tick().succeeded, "cleanup ticks")) return 1;
    const auto diagnostics = bridge.GetDiagnosticsSnapshot();
    return Check(diagnostics.outstandingRuntimeCount == 0 && diagnostics.publishedSnapshotCount == 0 && bridge.PrepareForShutdown(), "cleanup proof") &&
                   RunMultiFeatureEntityRegression() ? 0 : 1;
}
