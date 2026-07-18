/**
 * @file RenderProxySceneBridge.cpp
 * @brief RenderProxySceneBridge implementation.
 */

#include "RenderExtraction/RenderProxySceneBridge.h"

#include "Geometry/Asset/AssetMetadata.h"
#include "Scene/Components/LightComponent.h"
#include "Scene/Components/MeshRendererComponent.h"
#include "Scene/PrimitiveComponent.h"
#include "Scene/SceneEntity.h"
#include "Scene/SceneManager.h"
#include "World/World.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/quaternion.hpp>

#include <utility>

namespace RVX
{
    namespace
    {
        void CopySnapshotMetadata(const RenderProxySnapshot& snapshot,
                                  RenderProxySceneBridgeResult& result)
        {
            const RenderProxySnapshotMetadata metadata = snapshot.GetMetadata();
            result.snapshotSchemaVersion = metadata.schemaVersion;
            result.snapshotSequence = metadata.sequence;
            result.snapshotComplete = metadata.complete;
            result.primitiveCount = metadata.primitiveCount;
            result.lightCount = metadata.lightCount;
        }

        RenderMaterialMode ToRenderMaterialMode(
            const IMaterialAssetMetadata* material)
        {
            if (!material)
                return RenderMaterialMode::Opaque;

            switch (material->GetAssetMaterialMode())
            {
                case AssetMaterialMode::Masked:
                    return RenderMaterialMode::Masked;
                case AssetMaterialMode::Transparent:
                    return RenderMaterialMode::Transparent;
                case AssetMaterialMode::Opaque:
                default:
                    return RenderMaterialMode::Opaque;
            }
        }
    } // namespace

    const char* ToString(RenderProxySceneBridgeFallbackReason reason)
    {
        switch (reason)
        {
            case RenderProxySceneBridgeFallbackReason::None:
                return "None";
            case RenderProxySceneBridgeFallbackReason::NullWorld:
                return "NullWorld";
            case RenderProxySceneBridgeFallbackReason::NullSceneManager:
                return "NullSceneManager";
            case RenderProxySceneBridgeFallbackReason::PrimitiveProxyUnavailable:
                return "PrimitiveProxyUnavailable";
            case RenderProxySceneBridgeFallbackReason::PrimitiveProxyCreationFailed:
                return "PrimitiveProxyCreationFailed";
        }

        return "Unknown";
    }

    bool RenderProxySceneBridge::BuildSnapshot(World* world,
                                               RenderProxySnapshot& outSnapshot,
                                               RenderProxySceneBridgeResult* outResult) const
    {
        RenderProxySceneBridgeResult result;

        if (!world)
        {
            outSnapshot.BeginBuild(++m_nextSnapshotSequence);
            outSnapshot.MarkIncomplete();
            MarkFallback(result, RenderProxySceneBridgeFallbackReason::NullWorld, 0);
            CopySnapshotMetadata(outSnapshot, result);
            if (outResult) *outResult = result;
            return false;
        }

        SceneManager* sceneManager = world->GetSceneManager();
        return BuildSnapshot(sceneManager, outSnapshot, outResult);
    }

    bool RenderProxySceneBridge::BuildSnapshot(SceneManager* sceneManager,
                                               RenderProxySnapshot& outSnapshot,
                                               RenderProxySceneBridgeResult* outResult) const
    {
        RenderProxySceneBridgeResult result;
        outSnapshot.BeginBuild(++m_nextSnapshotSequence);

        if (!sceneManager)
        {
            outSnapshot.MarkIncomplete();
            MarkFallback(result, RenderProxySceneBridgeFallbackReason::NullSceneManager, 0);
            CopySnapshotMetadata(outSnapshot, result);
            if (outResult) *outResult = result;
            return false;
        }

        std::unordered_set<uint64> primitiveControlledEntities;
        for (PrimitiveComponent* primitive : sceneManager->GetPrimitives())
        {
            if (!primitive)
                continue;

            auto* entity = dynamic_cast<SceneEntity*>(primitive->GetOwner());
            if (!entity || !entity->IsActive())
                continue;

            const bool hasLegacyRenderData = primitive->HasRenderData();
            const bool hasProxyData = primitive->HasRenderProxy();
            if (hasLegacyRenderData || hasProxyData)
            {
                primitiveControlledEntities.insert(entity->GetHandle());
            }

            if (!primitive->IsEnabled() || !primitive->IsVisible())
                continue;

            if (!hasProxyData)
            {
                if (hasLegacyRenderData)
                {
                    MarkFallback(result,
                                 RenderProxySceneBridgeFallbackReason::PrimitiveProxyUnavailable,
                                 entity->GetHandle());
                }
                continue;
            }

            RenderPrimitiveProxy proxy;
            if (!primitive->CreateRenderProxy(proxy))
            {
                MarkFallback(result,
                             RenderProxySceneBridgeFallbackReason::PrimitiveProxyCreationFailed,
                             entity->GetHandle());
                continue;
            }

            proxy.ownerId = entity->GetHandle();
            if (!proxy.id.IsValid())
            {
                proxy.id.value = proxy.ownerId;
            }
            outSnapshot.primitives.push_back(std::move(proxy));
        }

        const auto& entities = sceneManager->GetEntities();
        for (const auto& [handle, entity] : entities)
        {
            (void)handle;
            if (entity && entity->IsRoot())
            {
                CollectEntity(entity.get(), primitiveControlledEntities, outSnapshot, result);
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
        result.usedProxyPath = true;
        result.fallbackReason = RenderProxySceneBridgeFallbackReason::None;
        CopySnapshotMetadata(outSnapshot, result);
        if (outResult) *outResult = result;
        return true;
    }

    void RenderProxySceneBridge::CollectEntity(
        SceneEntity* entity,
        const std::unordered_set<uint64>& primitiveControlledEntities,
        RenderProxySnapshot& outSnapshot,
        RenderProxySceneBridgeResult& result) const
    {
        if (!entity || !entity->IsActive())
            return;

        if (primitiveControlledEntities.find(entity->GetHandle()) == primitiveControlledEntities.end())
        {
            if (auto* renderer = entity->GetComponent<MeshRendererComponent>())
            {
                if (renderer->IsEnabled() && renderer->IsVisible() && renderer->HasValidMesh())
                {
                    const Mat4 worldMatrix = entity->GetWorldMatrix();

                    RenderPrimitiveProxy proxy;
                    proxy.id.value = entity->GetHandle();
                    proxy.ownerId = entity->GetHandle();
                    proxy.worldMatrix = worldMatrix;
                    proxy.normalMatrix = glm::inverseTranspose(Mat4(Mat3(worldMatrix)));
                    proxy.bounds = entity->GetWorldBounds();
                    proxy.meshAssetId = AssetId{renderer->GetMesh().GetId()};
                    proxy.layerMask = ~0u;
                    proxy.visible = renderer->IsVisible();
                    proxy.castsShadow = renderer->CastsShadow();
                    proxy.receivesShadow = renderer->ReceivesShadow();

                    const size_t submeshCount = renderer->GetSubmeshCount();
                    proxy.materialAssetIds.resize(submeshCount);
                    proxy.materialModes.resize(submeshCount);
                    for (size_t i = 0; i < submeshCount; ++i)
                    {
                        auto material = renderer->GetMaterial(i);
                        proxy.materialAssetIds[i] =
                            AssetId{material.IsValid() ? material.GetId() : 0};
                        proxy.materialModes[i] = ToRenderMaterialMode(
                            material.As<IMaterialAssetMetadata>());
                    }

                    proxy.sortKey = proxy.materialAssetIds.empty()
                                        ? 0
                                        : proxy.materialAssetIds[0].value;
                    outSnapshot.primitives.push_back(std::move(proxy));
                }
            }
        }

        if (auto* lightComp = entity->GetComponent<LightComponent>())
        {
            if (lightComp->IsEnabled())
            {
                RenderLightProxy light;
                light.id.value = entity->GetHandle();
                light.ownerId = entity->GetHandle();

                switch (lightComp->GetLightType())
                {
                    case LightType::Directional:
                        light.type = RenderLightProxy::Type::Directional;
                        break;
                    case LightType::Point:
                        light.type = RenderLightProxy::Type::Point;
                        break;
                    case LightType::Spot:
                        light.type = RenderLightProxy::Type::Spot;
                        break;
                }

                light.position = entity->GetWorldPosition();
                const Mat4 rotMat = glm::mat4_cast(entity->GetWorldRotation());
                light.direction = Vec3(rotMat * Vec4(0, 0, -1, 0));
                light.color = lightComp->GetColor();
                light.intensity = lightComp->GetIntensity();
                light.range = lightComp->GetRange();
                light.innerConeAngle = lightComp->GetInnerConeAngle();
                light.outerConeAngle = lightComp->GetOuterConeAngle();
                light.castsShadow = lightComp->CastsShadow();

                outSnapshot.lights.push_back(light);
            }
        }

        for (auto* child : entity->GetChildren())
        {
            CollectEntity(child, primitiveControlledEntities, outSnapshot, result);
        }
    }

    void RenderProxySceneBridge::MarkFallback(RenderProxySceneBridgeResult& result,
                                              RenderProxySceneBridgeFallbackReason reason,
                                              uint64 ownerId) const
    {
        if (result.requiresLegacyFallback)
            return;

        result.usedProxyPath = false;
        result.requiresLegacyFallback = true;
        result.fallbackReason = reason;
        result.fallbackOwnerId = ownerId;
    }

} // namespace RVX
