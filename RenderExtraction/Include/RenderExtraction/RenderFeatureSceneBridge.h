#pragma once

/**
 * @file RenderFeatureSceneBridge.h
 * @brief Synchronous scene-to-feature-snapshot extraction bridge.
 */

#include "Core/Types.h"
#include "RenderContracts/FeatureRenderSnapshot.h"

#include <cstddef>

namespace RVX
{
    class SceneEntity;
    class SceneManager;
    class World;

    enum class RenderFeatureSceneBridgeFallbackReason : uint8
    {
        None = 0,
        NullWorld,
        NullSceneManager,
        ProviderAppendFailed
    };

    const char* ToString(RenderFeatureSceneBridgeFallbackReason reason);

    struct RenderFeatureSceneBridgeResult
    {
        bool usedProviderPath = false;
        bool requiresLegacyFallback = false;
        RenderFeatureSceneBridgeFallbackReason fallbackReason = RenderFeatureSceneBridgeFallbackReason::None;
        uint64 fallbackOwnerId = 0;
        uint32 snapshotSchemaVersion = RVX_RENDER_FEATURE_SNAPSHOT_SCHEMA_VERSION;
        uint64 snapshotSequence = 0;
        bool snapshotComplete = false;
        size_t providerCount = 0;
        size_t skippedProviderCount = 0;
        size_t particleItemCount = 0;
        size_t waterItemCount = 0;
        size_t terrainItemCount = 0;
    };

    class RenderFeatureSceneBridge
    {
    public:
        bool BuildSnapshot(World* world,
                           RenderFeatureSnapshot& outSnapshot,
                           RenderFeatureSceneBridgeResult* outResult = nullptr) const;
        bool BuildSnapshot(SceneManager* sceneManager,
                           RenderFeatureSnapshot& outSnapshot,
                           RenderFeatureSceneBridgeResult* outResult = nullptr) const;

    private:
        void CollectEntity(SceneEntity* entity,
                           RenderFeatureSnapshot& outSnapshot,
                           RenderFeatureSceneBridgeResult& result) const;

        void MarkFallback(RenderFeatureSceneBridgeResult& result,
                          RenderFeatureSceneBridgeFallbackReason reason,
                          uint64 ownerId) const;

        mutable uint64 m_nextSnapshotSequence = 0;
    };

} // namespace RVX
