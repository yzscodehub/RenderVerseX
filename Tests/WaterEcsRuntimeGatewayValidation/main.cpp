#include "Water/ECS/WaterEcsRuntimeGateway.h"

#include "Core/Log.h"

#include <iostream>

namespace
{
    class LogScope final
    {
    public:
        LogScope() { RVX::Log::Initialize(); }
        ~LogScope() { RVX::Log::Shutdown(); }
    };

    [[nodiscard]] RVX::Water::WaterEcsConfig MakeConfig()
    {
        RVX::Water::WaterEcsConfig config{
            .surfaceAssetId = {101}, .materialAssetId = {102},
            .configurationRevision = 1, .size = {8.0f, 12.0f},
            .depth = 3.0f,
        };
        config.gerstnerWaves.waveCount = 2;
        config.gerstnerWaves.waves[0] = {
            .direction = {1.0f, 0.0f}, .amplitude = 0.4f,
            .wavelength = 8.0f, .steepness = 0.35f, .speed = 1.0f,
            .phaseOffsetRadians = 0.0f,
        };
        config.gerstnerWaves.waves[1] = {
            .direction = {0.0f, 1.0f}, .amplitude = 0.2f,
            .wavelength = 4.0f, .steepness = 0.25f, .speed = 0.8f,
            .phaseOffsetRadians = 0.25f,
        };
        static_cast<void>(RVX::CanonicalizeWaterGerstnerWaveSet(config.gerstnerWaves));
        return config;
    }

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
    RVX::Water::WaterEcsRuntimeGateway gateway;
    const RVX::Water::WaterEcsConfig config = MakeConfig();
    const RVX::Water::WaterEcsRuntimeHandle first = gateway.Create({
        .config = config, .worldTransform = RVX::Mat4(1.0f),
        .command = RVX::Water::WaterEcsCommand::Activate,
    });
    if (!Check(first.IsValid() && gateway.IsAlive(first), "create water runtime")) return 1;

    RVX::Water::WaterEcsRuntimeSnapshot snapshot;
    if (!Check(gateway.CaptureSnapshot(first, snapshot) &&
                   snapshot.item.IsStructurallyValid() &&
                   snapshot.item.surfaceAssetId == config.surfaceAssetId &&
                   snapshot.item.materialAssetId == config.materialAssetId &&
                   snapshot.item.contentHash != 0 && snapshot.payloadRevision == 1,
               "capture sealed water snapshot")) return 1;

    RVX::Water::WaterEcsRuntimeRequest refreshRequest{
        .config = config, .worldTransform = RVX::Mat4(1.0f),
        .command = RVX::Water::WaterEcsCommand::Refresh,
    };
    refreshRequest.config.configurationRevision = 2;
    if (!Check(gateway.Update(first, refreshRequest) &&
                   gateway.CaptureSnapshot(first, snapshot) &&
                   snapshot.item.IsStructurallyValid() &&
                   snapshot.payloadRevision == 2,
               "refresh water runtime")) return 1;

    RVX::Mat4 unsupportedRotation{1.0f};
    unsupportedRotation[0][0] = 0.0f;
    unsupportedRotation[0][2] = -1.0f;
    unsupportedRotation[2][0] = 1.0f;
    unsupportedRotation[2][2] = 0.0f;
    if (!Check(!gateway.Update(first, {.config = refreshRequest.config,
                                       .worldTransform = unsupportedRotation}),
               "reject rotation until water transform support is implemented")) return 1;

    if (!Check(gateway.Update(first, {.config = refreshRequest.config,
                                      .worldTransform = RVX::Mat4(1.0f),
                                      .command = RVX::Water::WaterEcsCommand::Deactivate}) &&
                   gateway.CaptureSnapshot(first, snapshot) &&
                   snapshot.item.IsCanonicalDisabled(),
               "deactivate emits a canonical disabled packet")) return 1;

    if (!Check(gateway.Release(first) == RVX::Water::WaterEcsReleaseResult::Released &&
                   !gateway.IsAlive(first) &&
                   gateway.Release(first) == RVX::Water::WaterEcsReleaseResult::AlreadyAbsent,
               "release water generation")) return 1;

    const RVX::Water::WaterEcsRuntimeHandle second =
        gateway.Create({.config = refreshRequest.config});
    return Check(second.IsValid() && second.GetIndex() == first.GetIndex() &&
                     second.GetGeneration() != first.GetGeneration() &&
                     gateway.Release(second) == RVX::Water::WaterEcsReleaseResult::Released,
                 "reuse water slot with a new generation") ? 0 : 1;
}
