#include "Water/ECS/WaterEcsBridge.h"

#include <iostream>
#include <vector>

namespace
{
    [[nodiscard]] RVX::Water::WaterEcsConfig MakeConfig()
    {
        RVX::Water::WaterEcsConfig config{
            .surfaceAssetId = {21}, .materialAssetId = {22},
            .configurationRevision = 4, .size = {10.0f, 20.0f}, .depth = 4.0f};
        config.gerstnerWaves.waveCount = 1;
        config.gerstnerWaves.waves[0] = {
            .direction = {1.0f, 0.0f}, .amplitude = 0.3f,
            .wavelength = 8.0f, .steepness = 0.5f,
            .speed = 1.0f, .phaseOffsetRadians = 0.0f};
        static_cast<void>(RVX::CanonicalizeWaterGerstnerWaveSet(config.gerstnerWaves));
        return config;
    }

    [[nodiscard]] RVX::WaterRenderSnapshotItem MakePacket(
        const RVX::Water::WaterEcsConfig& config,
        RVX::uint64 componentId)
    {
        RVX::WaterRenderSnapshotItem item;
        item.payloadStatus = RVX::WaterRenderSnapshotPayloadStatus::Ready;
        item.componentId = componentId;
        item.surfaceAssetId = config.surfaceAssetId;
        item.materialAssetId = config.materialAssetId;
        item.worldBounds = RVX::AABB({-6.0f, -4.3f, -11.0f}, {6.0f, 0.3f, 11.0f});
        item.size = config.size;
        item.depth = config.depth;
        item.surfaceType = config.surfaceType;
        item.simulationType = config.simulationType;
        item.gerstnerWaves = config.gerstnerWaves;
        item.shallowColor = config.shallowColor;
        item.deepColor = config.deepColor;
        item.foamColor = config.foamColor;
        item.absorptionCoefficient = config.absorptionCoefficient;
        item.transparency = config.transparency;
        item.reflectionStrength = config.reflectionStrength;
        item.refractionStrength = config.refractionStrength;
        item.roughness = config.roughness;
        item.normalScale = config.normalScale;
        item.foamIntensity = config.foamIntensity;
        item.foamCrestThreshold = config.foamCrestThreshold;
        item.reflectionEnabled = config.reflectionEnabled;
        item.refractionEnabled = config.refractionEnabled;
        item.foamEnabled = config.foamEnabled;
        item.contentHash = item.ComputeContentHash();
        return item;
    }

    class Gateway final : public RVX::Water::IWaterEcsGateway
    {
    public:
        RVX::Water::WaterEcsRuntimeHandle Create(
            const RVX::Water::WaterEcsRuntimeRequest& request) override
        {
            m_config = request.config;
            m_entries.push_back({true, 1});
            return RVX::Water::WaterEcsRuntimeHandle::Create(
                static_cast<RVX::uint32>(m_entries.size() - 1U), 1);
        }
        bool Update(RVX::Water::WaterEcsRuntimeHandle handle,
                    const RVX::Water::WaterEcsRuntimeRequest& request) override
        {
            m_config = request.config;
            return IsAlive(handle);
        }
        bool CaptureSnapshot(RVX::Water::WaterEcsRuntimeHandle handle,
                             RVX::Water::WaterEcsRuntimeSnapshot& out) const override
        {
            if (!IsAlive(handle)) return false;
            out.item = MakePacket(m_config, handle.GetPackedValue());
            out.payloadRevision = m_config.configurationRevision;
            return out.item.IsStructurallyValid();
        }
        RVX::Water::WaterEcsReleaseResult Release(RVX::Water::WaterEcsRuntimeHandle handle) override
        {
            if (!IsAlive(handle)) return RVX::Water::WaterEcsReleaseResult::AlreadyAbsent;
            m_entries[handle.GetIndex()].alive = false;
            ++m_entries[handle.GetIndex()].generation;
            return RVX::Water::WaterEcsReleaseResult::Released;
        }
        bool IsAlive(RVX::Water::WaterEcsRuntimeHandle handle) const override
        {
            return handle.IsValid() && handle.GetIndex() < m_entries.size() &&
                   m_entries[handle.GetIndex()].alive &&
                   m_entries[handle.GetIndex()].generation == handle.GetGeneration();
        }
    private:
        struct Entry { bool alive; RVX::uint32 generation; };
        std::vector<Entry> m_entries;
        RVX::Water::WaterEcsConfig m_config = MakeConfig();
    };

    bool Check(bool value, const char* message)
    {
        if (!value) std::cerr << message << '\n';
        return value;
    }
}

int main()
{
    RVX::SceneECS::SceneEcsRuntime runtime(1, 1);
    Gateway gateway;
    RVX::Water::WaterEcsBridge bridge(runtime, gateway);
    if (!Check(bridge.RegisterProcessors() ==
                   RVX::Water::WaterEcsBridgeRegistrationResult::Registered,
               "registration")) return 1;
    const RVX::ECS::EntityHandle entity = runtime.CreateEntity();
    const RVX::Water::WaterEcsConfig config = MakeConfig();
    if (!Check(runtime.AddFragment(entity, RVX::SceneECS::WaterRuntimeTag{}) &&
                   runtime.AddFragment(entity, config) &&
                   runtime.AddFragment(entity, RVX::Water::WaterEcsIntent{}) &&
                   runtime.AddFragment(entity, RVX::Water::WaterEcsState{}), "setup")) return 1;
    if (!Check(runtime.Tick().succeeded, "feature tick")) return 1;
    const auto frozen = runtime.GetLatestFrozenSnapshot();
    if (!Check(frozen != nullptr && frozen->water.size() == 1 &&
                   frozen->water.front().state.IsStructurallyValid() &&
                   frozen->water.front().state.surfaceAssetId == RVX::AssetId{21} &&
                   frozen->water.front().state.materialAssetId == RVX::AssetId{22},
               "frozen sealed water")) return 1;
    if (!Check(runtime.RequestDestroy(entity) == RVX::SceneECS::DestroyRequestResult::Accepted &&
                   runtime.Tick().succeeded && runtime.Tick().succeeded, "cleanup")) return 1;
    const auto diagnostics = bridge.GetDiagnosticsSnapshot();
    return Check(diagnostics.outstandingRuntimeCount == 0 &&
                     diagnostics.publishedSnapshotCount == 0 && bridge.PrepareForShutdown(),
                 "cleanup proof") ? 0 : 1;
}
