#include "Terrain/ECS/TerrainEcsBridge.h"

#include <iostream>
#include <vector>

namespace
{
    class Gateway final : public RVX::Terrain::ITerrainEcsGateway
    {
    public:
        RVX::Terrain::TerrainEcsRuntimeHandle Create(const RVX::Terrain::TerrainEcsRuntimeRequest&) override { m_entries.push_back({true, 1}); return RVX::Terrain::TerrainEcsRuntimeHandle::Create(static_cast<RVX::uint32>(m_entries.size() - 1u), 1); }
        bool Update(RVX::Terrain::TerrainEcsRuntimeHandle handle, const RVX::Terrain::TerrainEcsRuntimeRequest&) override { return IsAlive(handle); }
        bool CaptureSnapshot(RVX::Terrain::TerrainEcsRuntimeHandle handle, RVX::Terrain::TerrainEcsRuntimeSnapshot& out) const override { if (!IsAlive(handle)) return false; out.item.patchSize = 64; out.item.maxLODLevels = 6; out.item.renderGpuPathAvailable = true; out.item.cpuDataAvailable = true; out.payloadRevision = 13; return true; }
        RVX::Terrain::TerrainEcsReleaseResult Release(RVX::Terrain::TerrainEcsRuntimeHandle handle) override { if (!IsAlive(handle)) return RVX::Terrain::TerrainEcsReleaseResult::AlreadyAbsent; m_entries[handle.GetIndex()].alive = false; ++m_entries[handle.GetIndex()].generation; return RVX::Terrain::TerrainEcsReleaseResult::Released; }
        bool IsAlive(RVX::Terrain::TerrainEcsRuntimeHandle handle) const override { return handle.IsValid() && handle.GetIndex() < m_entries.size() && m_entries[handle.GetIndex()].alive && m_entries[handle.GetIndex()].generation == handle.GetGeneration(); }
    private: struct Entry { bool alive; RVX::uint32 generation; }; std::vector<Entry> m_entries;
    };
    bool Check(bool value, const char* message) { if (!value) std::cerr << message << '\n'; return value; }
}
int main()
{
    RVX::SceneECS::SceneEcsRuntime runtime(1, 1); Gateway gateway; RVX::Terrain::TerrainEcsBridge bridge(runtime, gateway);
    if (!Check(bridge.RegisterProcessors() == RVX::Terrain::TerrainEcsBridgeRegistrationResult::Registered, "registration")) return 1;
    const RVX::ECS::EntityHandle entity = runtime.CreateEntity();
    const RVX::Terrain::TerrainEcsConfig config{.heightmapAssetId = {31}, .materialAssetId = {32}, .configurationRevision = 5, .size = {100.0f, 25.0f, 100.0f}, .patchSize = 64, .maxLODLevels = 6};
    if (!Check(runtime.AddFragment(entity, RVX::SceneECS::TerrainRuntimeTag{}) && runtime.AddFragment(entity, config) && runtime.AddFragment(entity, RVX::Terrain::TerrainEcsIntent{}) && runtime.AddFragment(entity, RVX::Terrain::TerrainEcsState{}), "setup")) return 1;
    if (!Check(runtime.Tick().succeeded, "feature tick")) return 1;
    const auto frozen = runtime.GetLatestFrozenSnapshot();
    if (!Check(frozen != nullptr && frozen->terrain.size() == 1 &&
                   frozen->terrain.front().state.patchSize == 64 &&
                   frozen->terrain.front().state.heightmapAssetId == RVX::AssetId{31} &&
                   frozen->terrain.front().state.materialAssetId == RVX::AssetId{32},
               "frozen terrain")) return 1;
    if (!Check(runtime.RequestDestroy(entity) == RVX::SceneECS::DestroyRequestResult::Accepted && runtime.Tick().succeeded && runtime.Tick().succeeded, "cleanup")) return 1;
    const auto diagnostics = bridge.GetDiagnosticsSnapshot(); return Check(diagnostics.outstandingRuntimeCount == 0 && diagnostics.publishedSnapshotCount == 0 && bridge.PrepareForShutdown(), "cleanup proof") ? 0 : 1;
}
