/**
 * @file RenderFeatureSceneBridge.cpp
 * @brief RenderFeatureSceneBridge implementation.
 */

#include "RenderExtraction/RenderFeatureSceneBridge.h"

#include "Scene/Component.h"
#include "Scene/SceneEntity.h"
#include "Scene/SceneManager.h"
#include "World/World.h"

namespace RVX
{
    namespace
    {
        void CopySnapshotMetadata(const RenderFeatureSnapshot& snapshot,
                                  RenderFeatureSceneBridgeResult& result)
        {
            const RenderFeatureSnapshotMetadata metadata = snapshot.GetMetadata();
            result.snapshotSchemaVersion = metadata.schemaVersion;
            result.snapshotSequence = metadata.sequence;
            result.snapshotComplete = metadata.complete;
            result.providerCount = metadata.providerCount;
            result.skippedProviderCount = metadata.skippedProviderCount;
            result.particleItemCount = metadata.particleItemCount;
            result.waterItemCount = metadata.waterItemCount;
            result.terrainItemCount = metadata.terrainItemCount;
        }
    } // namespace

    const char* ToString(RenderFeatureSceneBridgeFallbackReason reason)
    {
        switch (reason)
        {
            case RenderFeatureSceneBridgeFallbackReason::None:
                return "None";
            case RenderFeatureSceneBridgeFallbackReason::NullWorld:
                return "NullWorld";
            case RenderFeatureSceneBridgeFallbackReason::NullSceneManager:
                return "NullSceneManager";
            case RenderFeatureSceneBridgeFallbackReason::ProviderAppendFailed:
                return "ProviderAppendFailed";
        }

        return "Unknown";
    }

    bool RenderFeatureSceneBridge::BuildSnapshot(World* world,
                                                 RenderFeatureSnapshot& outSnapshot,
                                                 RenderFeatureSceneBridgeResult* outResult) const
    {
        RenderFeatureSceneBridgeResult result;

        if (!world)
        {
            outSnapshot.BeginBuild(++m_nextSnapshotSequence);
            outSnapshot.MarkIncomplete();
            MarkFallback(result, RenderFeatureSceneBridgeFallbackReason::NullWorld, 0);
            CopySnapshotMetadata(outSnapshot, result);
            if (outResult) *outResult = result;
            return false;
        }

        return BuildSnapshot(world->GetSceneManager(), outSnapshot, outResult);
    }

    bool RenderFeatureSceneBridge::BuildSnapshot(SceneManager* sceneManager,
                                                 RenderFeatureSnapshot& outSnapshot,
                                                 RenderFeatureSceneBridgeResult* outResult) const
    {
        RenderFeatureSceneBridgeResult result;
        outSnapshot.BeginBuild(++m_nextSnapshotSequence);

        if (!sceneManager)
        {
            outSnapshot.MarkIncomplete();
            MarkFallback(result, RenderFeatureSceneBridgeFallbackReason::NullSceneManager, 0);
            CopySnapshotMetadata(outSnapshot, result);
            if (outResult) *outResult = result;
            return false;
        }

        const auto& entities = sceneManager->GetEntities();
        for (const auto& [handle, entity] : entities)
        {
            (void)handle;
            if (entity && entity->IsRoot())
            {
                CollectEntity(entity.get(), outSnapshot, result);
            }
        }

        if (result.requiresLegacyFallback)
        {
            outSnapshot.MarkIncomplete();
            CopySnapshotMetadata(outSnapshot, result);
            if (outResult) *outResult = result;
            return false;
        }

        outSnapshot.MarkComplete();
        result.usedProviderPath = true;
        result.fallbackReason = RenderFeatureSceneBridgeFallbackReason::None;
        CopySnapshotMetadata(outSnapshot, result);
        if (outResult) *outResult = result;
        return true;
    }

    void RenderFeatureSceneBridge::CollectEntity(SceneEntity* entity,
                                                 RenderFeatureSnapshot& outSnapshot,
                                                 RenderFeatureSceneBridgeResult& result) const
    {
        if (!entity || !entity->IsActive())
            return;

        for (const auto& [type, component] : entity->GetComponents())
        {
            (void)type;
            if (!component || !component->IsEnabled())
                continue;

            auto* provider = dynamic_cast<const IRenderFeatureSnapshotProvider*>(component.get());
            if (!provider)
                continue;

            ++outSnapshot.metadata.providerCount;
            if (!provider->AppendRenderFeatureSnapshot(outSnapshot))
            {
                ++outSnapshot.metadata.skippedProviderCount;
                MarkFallback(result,
                             RenderFeatureSceneBridgeFallbackReason::ProviderAppendFailed,
                             entity->GetHandle());
                return;
            }
        }

        for (auto* child : entity->GetChildren())
        {
            CollectEntity(child, outSnapshot, result);
            if (result.requiresLegacyFallback)
                return;
        }
    }

    void RenderFeatureSceneBridge::MarkFallback(RenderFeatureSceneBridgeResult& result,
                                                RenderFeatureSceneBridgeFallbackReason reason,
                                                uint64 ownerId) const
    {
        if (result.requiresLegacyFallback)
            return;

        result.usedProviderPath = false;
        result.requiresLegacyFallback = true;
        result.fallbackReason = reason;
        result.fallbackOwnerId = ownerId;
    }

} // namespace RVX
