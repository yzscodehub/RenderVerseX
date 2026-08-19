#include "Terrain/ECS/TerrainEcsRuntimeGateway.h"

#include <iostream>
#include <string_view>

namespace
{
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
    RVX::Terrain::TerrainEcsRuntimeGateway gateway;
    const RVX::Terrain::TerrainEcsRuntimeRequest request{
        .config = {
            .heightmapAssetId = {201},
            .materialAssetId = {202},
            .configurationRevision = 1,
            .size = {32.0f, 8.0f, 32.0f},
            .patchSize = 8,
            .maxLODLevels = 3,
        },
        .worldTransform = RVX::Mat4(1.0f),
    };
    const RVX::Terrain::TerrainEcsRuntimeHandle handle = gateway.Create(request);
    RVX::Terrain::TerrainEcsRuntimeSnapshot snapshot;
    return Check(!handle.IsValid() && !gateway.IsAlive(handle) &&
                     !gateway.Update(handle, request) &&
                     !gateway.CaptureSnapshot(handle, snapshot) &&
                     gateway.Release(handle) == RVX::Terrain::TerrainEcsReleaseResult::AlreadyAbsent &&
                     std::string_view(gateway.GetLastFailureReason()).find(
                         "AssetId-to-loaded Heightmap and TerrainMaterial resolver") !=
                         std::string_view::npos,
                 "fail closed when the terrain resource seam is unavailable") ?
               0 :
               1;
}
