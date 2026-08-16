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
    const RVX::Water::WaterEcsConfig config{
        .surfaceAssetId = {101},
        .materialAssetId = {102},
        .configurationRevision = 1,
        .size = {8.0f, 12.0f},
        .depth = 3.0f,
        .resolution = 8,
        .causticsEnabled = true,
        .underwaterEffectsEnabled = true,
    };
    const RVX::Water::WaterEcsRuntimeHandle first = gateway.Create({
        .config = config,
        .worldTransform = RVX::Mat4(1.0f),
        .command = RVX::Water::WaterEcsCommand::Activate,
        .deltaSeconds = 1.0 / 60.0,
    });
    if (!Check(first.IsValid() && gateway.IsAlive(first), "create water runtime"))
    {
        return 1;
    }

    RVX::Water::WaterEcsRuntimeSnapshot snapshot;
    if (!Check(gateway.CaptureSnapshot(first, snapshot) &&
                   snapshot.item.surfaceAssetId == config.surfaceAssetId &&
                   snapshot.item.materialAssetId == config.materialAssetId &&
                   snapshot.item.resolution == config.resolution &&
                   snapshot.item.cpuSimulationAvailable && snapshot.payloadRevision == 1,
               "capture water snapshot"))
    {
        return 1;
    }

    RVX::Water::WaterEcsRuntimeRequest refreshRequest{
        .config = config,
        .worldTransform = RVX::Mat4(1.0f),
        .command = RVX::Water::WaterEcsCommand::Refresh,
        .deltaSeconds = 1.0 / 60.0,
    };
    refreshRequest.config.configurationRevision = 2;
    refreshRequest.config.resolution = 4;
    if (!Check(gateway.Update(first, refreshRequest) &&
                   gateway.CaptureSnapshot(first, snapshot) && snapshot.item.resolution == 4 &&
                   snapshot.payloadRevision == 2,
               "refresh water runtime"))
    {
        return 1;
    }

    if (!Check(gateway.Release(first) == RVX::Water::WaterEcsReleaseResult::Released &&
                   !gateway.IsAlive(first) &&
                   gateway.Release(first) == RVX::Water::WaterEcsReleaseResult::AlreadyAbsent,
               "release water generation"))
    {
        return 1;
    }

    const RVX::Water::WaterEcsRuntimeHandle second = gateway.Create({.config = refreshRequest.config});
    return Check(second.IsValid() && second.GetIndex() == first.GetIndex() &&
                     second.GetGeneration() != first.GetGeneration() &&
                     gateway.Release(second) == RVX::Water::WaterEcsReleaseResult::Released,
                 "reuse water slot with a new generation") ?
               0 :
               1;
}
