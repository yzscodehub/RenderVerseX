#include "Terrain/ECS/TerrainEcsRuntimeGateway.h"

#include <thread>
#include <utility>

namespace RVX::Terrain
{
namespace
{
    constexpr const char* RVX_TERRAIN_ECS_RESOURCE_SEAM_MISSING =
        "Terrain ECS requires an owner-thread AssetId-to-loaded Heightmap and TerrainMaterial "
        "resolver; Terrain has no Resource-backed heightfield/material contract.";

    constexpr const char* RVX_TERRAIN_ECS_INVALID_REQUEST =
        "Terrain ECS gateway rejected an invalid terrain runtime request.";

    [[nodiscard]] bool IsValidRequest(const TerrainEcsRuntimeRequest& request)
    {
        return request.config.heightmapAssetId.IsValid() && request.config.materialAssetId.IsValid() &&
               request.config.configurationRevision != 0 && request.config.patchSize != 0 &&
               request.config.maxLODLevels != 0;
    }
} // namespace

struct TerrainEcsRuntimeGateway::State
{
    State()
        : ownerThread(std::this_thread::get_id())
    {
    }

    [[nodiscard]] bool IsOwnerThread() const
    {
        return std::this_thread::get_id() == ownerThread;
    }

    std::thread::id ownerThread;
    std::string lastFailureReason;
};

TerrainEcsRuntimeGateway::TerrainEcsRuntimeGateway()
    : m_state(std::make_unique<State>())
{
}

TerrainEcsRuntimeGateway::~TerrainEcsRuntimeGateway() = default;

TerrainEcsRuntimeHandle TerrainEcsRuntimeGateway::Create(const TerrainEcsRuntimeRequest& request)
{
    if (m_state == nullptr || !m_state->IsOwnerThread())
    {
        return TerrainEcsRuntimeHandle::Invalid();
    }

    m_state->lastFailureReason = IsValidRequest(request)
                                     ? RVX_TERRAIN_ECS_RESOURCE_SEAM_MISSING
                                     : RVX_TERRAIN_ECS_INVALID_REQUEST;
    return TerrainEcsRuntimeHandle::Invalid();
}

bool TerrainEcsRuntimeGateway::Update(TerrainEcsRuntimeHandle handle,
                                      const TerrainEcsRuntimeRequest& request)
{
    (void)handle;
    if (m_state == nullptr || !m_state->IsOwnerThread())
    {
        return false;
    }

    m_state->lastFailureReason = IsValidRequest(request)
                                     ? RVX_TERRAIN_ECS_RESOURCE_SEAM_MISSING
                                     : RVX_TERRAIN_ECS_INVALID_REQUEST;
    return false;
}

bool TerrainEcsRuntimeGateway::CaptureSnapshot(TerrainEcsRuntimeHandle handle,
                                               TerrainEcsRuntimeSnapshot& outSnapshot) const
{
    (void)handle;
    (void)outSnapshot;
    return false;
}

TerrainEcsReleaseResult TerrainEcsRuntimeGateway::Release(TerrainEcsRuntimeHandle handle)
{
    if (m_state == nullptr || !m_state->IsOwnerThread())
    {
        return TerrainEcsReleaseResult::Failed;
    }

    (void)handle;
    return TerrainEcsReleaseResult::AlreadyAbsent;
}

bool TerrainEcsRuntimeGateway::IsAlive(TerrainEcsRuntimeHandle handle) const
{
    (void)handle;
    return false;
}

const std::string& TerrainEcsRuntimeGateway::GetLastFailureReason() const
{
    static const std::string empty;
    return m_state != nullptr ? m_state->lastFailureReason : empty;
}
} // namespace RVX::Terrain
