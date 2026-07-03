#pragma once

/**
 * @file RenderProxySceneBridge.h
 * @brief Synchronous scene-to-render-proxy snapshot bridge.
 */

#include "Core/Types.h"
#include "RenderContracts/RenderProxy.h"

#include <cstddef>
#include <unordered_set>

namespace RVX
{
    class SceneEntity;
    class SceneManager;
    class World;

    enum class RenderProxySceneBridgeFallbackReason : uint8
    {
        None = 0,
        NullWorld,
        NullSceneManager,
        PrimitiveProxyUnavailable,
        PrimitiveProxyCreationFailed
    };

    const char* ToString(RenderProxySceneBridgeFallbackReason reason);

    struct RenderProxySceneBridgeResult
    {
        bool usedProxyPath = false;
        bool requiresLegacyFallback = false;
        RenderProxySceneBridgeFallbackReason fallbackReason = RenderProxySceneBridgeFallbackReason::None;
        uint64 fallbackOwnerId = 0;
        uint32 snapshotSchemaVersion = RVX_RENDER_PROXY_SNAPSHOT_SCHEMA_VERSION;
        uint64 snapshotSequence = 0;
        bool snapshotComplete = false;
        size_t primitiveCount = 0;
        size_t lightCount = 0;
    };

    class RenderProxySceneBridge
    {
    public:
        bool BuildSnapshot(World* world,
                           RenderProxySnapshot& outSnapshot,
                           RenderProxySceneBridgeResult* outResult = nullptr) const;
        bool BuildSnapshot(SceneManager* sceneManager,
                           RenderProxySnapshot& outSnapshot,
                           RenderProxySceneBridgeResult* outResult = nullptr) const;

    private:
        void CollectEntity(SceneEntity* entity,
                           const std::unordered_set<uint64>& primitiveControlledEntities,
                           RenderProxySnapshot& outSnapshot,
                           RenderProxySceneBridgeResult& result) const;

        void MarkFallback(RenderProxySceneBridgeResult& result,
                          RenderProxySceneBridgeFallbackReason reason,
                          uint64 ownerId) const;

        mutable uint64 m_nextSnapshotSequence = 0;
    };

} // namespace RVX
