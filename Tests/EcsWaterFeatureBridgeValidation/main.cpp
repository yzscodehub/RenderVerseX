#include "Water/ECS/WaterEcsBridge.h"

#include <iostream>
#include <vector>

namespace
{
    class Gateway final : public RVX::Water::IWaterEcsGateway
    {
    public:
        RVX::Water::WaterEcsRuntimeHandle Create(const RVX::Water::WaterEcsRuntimeRequest&) override { m_entries.push_back({true, 1}); return RVX::Water::WaterEcsRuntimeHandle::Create(static_cast<RVX::uint32>(m_entries.size() - 1u), 1); }
        bool Update(RVX::Water::WaterEcsRuntimeHandle handle, const RVX::Water::WaterEcsRuntimeRequest&) override { return IsAlive(handle); }
        bool CaptureSnapshot(RVX::Water::WaterEcsRuntimeHandle handle, RVX::Water::WaterEcsRuntimeSnapshot& out) const override { if (!IsAlive(handle)) return false; out.item.resolution = 32; out.item.renderGpuPathAvailable = true; out.item.cpuSimulationAvailable = true; out.payloadRevision = 9; return true; }
        RVX::Water::WaterEcsReleaseResult Release(RVX::Water::WaterEcsRuntimeHandle handle) override { if (!IsAlive(handle)) return RVX::Water::WaterEcsReleaseResult::AlreadyAbsent; m_entries[handle.GetIndex()].alive = false; ++m_entries[handle.GetIndex()].generation; return RVX::Water::WaterEcsReleaseResult::Released; }
        bool IsAlive(RVX::Water::WaterEcsRuntimeHandle handle) const override { return handle.IsValid() && handle.GetIndex() < m_entries.size() && m_entries[handle.GetIndex()].alive && m_entries[handle.GetIndex()].generation == handle.GetGeneration(); }
    private: struct Entry { bool alive; RVX::uint32 generation; }; std::vector<Entry> m_entries;
    };
    bool Check(bool value, const char* message) { if (!value) std::cerr << message << '\n'; return value; }
}
int main()
{
    RVX::SceneECS::SceneEcsRuntime runtime(1, 1); Gateway gateway; RVX::Water::WaterEcsBridge bridge(runtime, gateway);
    if (!Check(bridge.RegisterProcessors() == RVX::Water::WaterEcsBridgeRegistrationResult::Registered, "registration")) return 1;
    const RVX::ECS::EntityHandle entity = runtime.CreateEntity();
    const RVX::Water::WaterEcsConfig config{.surfaceAssetId = {21}, .materialAssetId = {22}, .configurationRevision = 4, .size = {10.0f, 20.0f}, .depth = 4.0f, .resolution = 32};
    if (!Check(runtime.AddFragment(entity, RVX::SceneECS::WaterRuntimeTag{}) && runtime.AddFragment(entity, config) && runtime.AddFragment(entity, RVX::Water::WaterEcsIntent{}) && runtime.AddFragment(entity, RVX::Water::WaterEcsState{}), "setup")) return 1;
    if (!Check(runtime.Tick().succeeded, "feature tick")) return 1;
    const auto frozen = runtime.GetLatestFrozenSnapshot();
    if (!Check(frozen != nullptr && frozen->water.size() == 1 &&
                   frozen->water.front().state.resolution == 32 &&
                   frozen->water.front().state.surfaceAssetId == RVX::AssetId{21} &&
                   frozen->water.front().state.materialAssetId == RVX::AssetId{22},
               "frozen water")) return 1;
    if (!Check(runtime.RequestDestroy(entity) == RVX::SceneECS::DestroyRequestResult::Accepted && runtime.Tick().succeeded && runtime.Tick().succeeded, "cleanup")) return 1;
    const auto diagnostics = bridge.GetDiagnosticsSnapshot(); return Check(diagnostics.outstandingRuntimeCount == 0 && diagnostics.publishedSnapshotCount == 0 && bridge.PrepareForShutdown(), "cleanup proof") ? 0 : 1;
}
