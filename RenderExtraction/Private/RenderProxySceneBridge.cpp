/**
 * @file RenderProxySceneBridge.cpp
 * @brief RenderProxySceneBridge implementation.
 */

#include "RenderExtraction/RenderProxySceneBridge.h"

#include "Geometry/Asset/AssetMetadata.h"
#include "Scene/Components/ISkinningPaletteProvider.h"
#include "Scene/Components/LightComponent.h"
#include "Scene/Components/MeshRendererComponent.h"
#include "Scene/Components/StaticMeshComponent.h"
#include "Scene/PrimitiveComponent.h"
#include "Scene/SceneEntity.h"
#include "Scene/SceneManager.h"
#include "Scene/SceneRuntime.h"
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

        bool BuildStaticMeshProxy(const Scene* scene,
                                  const StaticMeshComponent& component,
                                  RenderPrimitiveProxy& outProxy)
        {
            if (!component.HasRenderData())
                return false;

            const ISkinningPaletteProvider* skinningProvider = nullptr;
            if (scene && component.GetOwner())
            {
                const auto providers =
                    scene->GetComponentsForActorImplementing<
                        ISkinningPaletteProvider>(
                        component.GetOwner()->GetHandle());
                if (providers.size() > 1)
                    return false;
                if (!providers.empty())
                    skinningProvider = providers.front();
            }
            else if (auto* entity =
                         dynamic_cast<SceneEntity*>(component.GetOwner()))
            {
                // Compatibility-only SceneManager extraction path.
                for (const auto& [componentType, owned] : entity->GetComponents())
                {
                    (void)componentType;
                    auto* candidate = dynamic_cast<
                        ISkinningPaletteProvider*>(owned.get());
                    if (!candidate)
                        continue;
                    if (skinningProvider != nullptr)
                        return false;
                    skinningProvider = candidate;
                }
            }

            RenderPrimitiveProxy proxy;
            const Mat4 worldMatrix = component.GetWorldTransform();
            proxy.worldMatrix = worldMatrix;
            proxy.normalMatrix =
                glm::inverseTranspose(Mat4(Mat3(worldMatrix)));
            proxy.bounds = component.GetWorldBounds();
            proxy.meshAssetId = AssetId{component.GetMesh().GetId()};
            proxy.layerMask = component.GetLayerMask();
            proxy.castsShadow = component.CastsShadow();
            proxy.receivesShadow = component.ReceivesShadow();
            proxy.visible = component.IsVisible();

            const size_t submeshCount = component.GetSubmeshCount();
            proxy.materialAssetIds.resize(submeshCount);
            proxy.materialModes.resize(submeshCount);
            for (size_t index = 0; index < submeshCount; ++index)
            {
                const SceneMaterialHandle material =
                    component.GetMaterial(index);
                proxy.materialAssetIds[index] =
                    AssetId{material.IsValid() ? material.GetId() : 0};
                proxy.materialModes[index] = ToRenderMaterialMode(
                    material.As<IMaterialAssetMetadata>());
            }
            proxy.sortKey = proxy.materialAssetIds.empty()
                                ? 0
                                : proxy.materialAssetIds.front().value;

            if (skinningProvider != nullptr)
            {
                const std::span<const Mat4> palette =
                    skinningProvider->GetSkinningPalette();
                proxy.skinningMatrices.assign(palette.begin(), palette.end());
            }
            outProxy = std::move(proxy);
            return true;
        }

        uint64 ToRenderOwnerId(const Actor* actor)
        {
            return actor && actor->GetHandle().IsValid()
                       ? actor->GetHandle().GetPackedValue()
                       : 0;
        }

        uint64 ToRenderComponentId(const ActorComponent* component)
        {
            if (!component)
                return 0;

            return component->GetComponentHandle().IsValid()
                       ? component->GetComponentHandle().GetPackedValue()
                       : component->GetComponentId();
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

        Scene* scene = world->GetScene();
        outSnapshot.BeginBuild(++m_nextSnapshotSequence);
        if (!scene)
        {
            outSnapshot.MarkIncomplete();
            MarkFallback(result,
                         RenderProxySceneBridgeFallbackReason::NullSceneManager,
                         0);
            CopySnapshotMetadata(outSnapshot, result);
            if (outResult) *outResult = result;
            return false;
        }

        std::unordered_set<uint64> primitiveControlledEntities;
        for (PrimitiveComponent* primitive :
             scene->GetComponentsImplementing<PrimitiveComponent>())
        {
            Actor* owner = primitive ? primitive->GetOwner() : nullptr;
            if (!primitive || !owner || !owner->IsActive())
                continue;

            const bool hasLegacyRenderData = primitive->HasRenderData();
            const auto* staticMesh =
                dynamic_cast<const StaticMeshComponent*>(primitive);
            const bool hasProxyData =
                staticMesh != nullptr && staticMesh->HasRenderData();
            if (hasLegacyRenderData || hasProxyData)
                primitiveControlledEntities.insert(ToRenderOwnerId(owner));
            if (!primitive->IsEnabled() || !primitive->IsVisible())
                continue;
            if (!hasProxyData)
            {
                if (hasLegacyRenderData)
                {
                    MarkFallback(
                        result,
                        RenderProxySceneBridgeFallbackReason::PrimitiveProxyUnavailable,
                        ToRenderOwnerId(owner));
                }
                continue;
            }

            RenderPrimitiveProxy proxy;
            if (!BuildStaticMeshProxy(scene, *staticMesh, proxy))
            {
                MarkFallback(
                    result,
                    RenderProxySceneBridgeFallbackReason::PrimitiveProxyCreationFailed,
                    ToRenderOwnerId(owner));
                continue;
            }
            proxy.ownerId = ToRenderOwnerId(owner);
            if (!proxy.id.IsValid())
                proxy.id.value = ToRenderComponentId(primitive);
            outSnapshot.primitives.push_back(std::move(proxy));
        }

        for (MeshRendererComponent* renderer :
             scene->GetComponentsImplementing<MeshRendererComponent>())
        {
            auto* entity = renderer
                               ? dynamic_cast<SceneEntity*>(renderer->GetOwner())
                               : nullptr;
            if (!entity || !entity->IsActive() || !renderer->IsEnabled() ||
                !renderer->IsVisible() || !renderer->HasValidMesh() ||
                primitiveControlledEntities.contains(ToRenderOwnerId(entity)))
            {
                continue;
            }

            const Mat4 worldMatrix = entity->GetWorldMatrix();
            RenderPrimitiveProxy proxy;
            proxy.id.value = ToRenderComponentId(renderer);
            proxy.ownerId = ToRenderOwnerId(entity);
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
            for (size_t index = 0; index < submeshCount; ++index)
            {
                auto material = renderer->GetMaterial(index);
                proxy.materialAssetIds[index] =
                    AssetId{material.IsValid() ? material.GetId() : 0};
                proxy.materialModes[index] = ToRenderMaterialMode(
                    material.As<IMaterialAssetMetadata>());
            }
            proxy.sortKey = proxy.materialAssetIds.empty()
                                ? 0
                                : proxy.materialAssetIds.front().value;
            outSnapshot.primitives.push_back(std::move(proxy));
        }

        for (LightComponent* lightComponent :
             scene->GetComponentsImplementing<LightComponent>())
        {
            auto* entity = lightComponent
                               ? dynamic_cast<SceneEntity*>(lightComponent->GetOwner())
                               : nullptr;
            if (!entity || !entity->IsActive() || !lightComponent->IsEnabled())
                continue;

            RenderLightProxy light;
            light.id.value = ToRenderComponentId(lightComponent);
            light.ownerId = ToRenderOwnerId(entity);
            switch (lightComponent->GetLightType())
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
            const Mat4 rotation = glm::mat4_cast(entity->GetWorldRotation());
            light.direction = Vec3(rotation * Vec4(0, 0, -1, 0));
            light.color = lightComponent->GetColor();
            light.intensity = lightComponent->GetIntensity();
            light.range = lightComponent->GetRange();
            light.innerConeAngle = lightComponent->GetInnerConeAngle();
            light.outerConeAngle = lightComponent->GetOuterConeAngle();
            light.castsShadow = lightComponent->CastsShadow();
            outSnapshot.lights.push_back(std::move(light));
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
            const auto* staticMesh =
                dynamic_cast<const StaticMeshComponent*>(primitive);
            const bool hasProxyData =
                staticMesh != nullptr && staticMesh->HasRenderData();
            if (hasLegacyRenderData || hasProxyData)
            {
                primitiveControlledEntities.insert(ToRenderOwnerId(entity));
            }

            if (!primitive->IsEnabled() || !primitive->IsVisible())
                continue;

            if (!hasProxyData)
            {
                if (hasLegacyRenderData)
                {
                    MarkFallback(result,
                                 RenderProxySceneBridgeFallbackReason::PrimitiveProxyUnavailable,
                                 ToRenderOwnerId(entity));
                }
                continue;
            }

            RenderPrimitiveProxy proxy;
            if (!BuildStaticMeshProxy(nullptr, *staticMesh, proxy))
            {
                MarkFallback(result,
                             RenderProxySceneBridgeFallbackReason::PrimitiveProxyCreationFailed,
                             ToRenderOwnerId(entity));
                continue;
            }

            proxy.ownerId = ToRenderOwnerId(entity);
            if (!proxy.id.IsValid())
            {
                proxy.id.value = ToRenderComponentId(primitive);
            }
            outSnapshot.primitives.push_back(std::move(proxy));
        }

        const auto& entities = sceneManager->GetEntities();
        for (const auto& [handle, entity] : entities)
        {
            (void)handle;
            if (entity && entity->IsRoot())
            {
                CollectEntity(entity, primitiveControlledEntities, outSnapshot, result);
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

        const uint64 ownerId = ToRenderOwnerId(entity);
        if (primitiveControlledEntities.find(ownerId) == primitiveControlledEntities.end())
        {
            if (auto* renderer = entity->GetComponent<MeshRendererComponent>())
            {
                if (renderer->IsEnabled() && renderer->IsVisible() && renderer->HasValidMesh())
                {
                    const Mat4 worldMatrix = entity->GetWorldMatrix();

                    RenderPrimitiveProxy proxy;
                    proxy.id.value = ToRenderComponentId(renderer);
                    proxy.ownerId = ownerId;
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
                light.id.value = ToRenderComponentId(lightComp);
                light.ownerId = ownerId;

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
