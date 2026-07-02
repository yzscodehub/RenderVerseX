/**
 * @file RenderProxySceneBridge.cpp
 * @brief RenderProxySceneBridge implementation.
 */

#include "RenderExtraction/RenderProxySceneBridge.h"

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
        RenderMaterialMode ToRenderMaterialMode(const IRenderMaterialSource* material)
        {
            if (!material)
                return RenderMaterialMode::Opaque;

            switch (material->GetRenderMaterialSourceData().alphaMode)
            {
                case MaterialSourceAlphaMode::Mask:
                    return RenderMaterialMode::Masked;
                case MaterialSourceAlphaMode::Blend:
                    return RenderMaterialMode::Transparent;
                case MaterialSourceAlphaMode::Opaque:
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
        outSnapshot.Clear();

        if (!world)
        {
            MarkFallback(result, RenderProxySceneBridgeFallbackReason::NullWorld, 0);
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
        outSnapshot.Clear();

        if (!sceneManager)
        {
            MarkFallback(result, RenderProxySceneBridgeFallbackReason::NullSceneManager, 0);
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
            outSnapshot.Clear();
            result.primitiveCount = 0;
            result.lightCount = 0;
            if (outResult) *outResult = result;
            return false;
        }

        result.usedProxyPath = true;
        result.fallbackReason = RenderProxySceneBridgeFallbackReason::None;
        result.primitiveCount = outSnapshot.primitives.size();
        result.lightCount = outSnapshot.lights.size();
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
                    proxy.meshResource = renderer->GetMesh().As<IRenderMeshUploadSource>();
                    proxy.meshId = renderer->GetMesh().GetId();
                    proxy.layerMask = ~0u;
                    proxy.visible = renderer->IsVisible();
                    proxy.castsShadow = renderer->CastsShadow();
                    proxy.receivesShadow = renderer->ReceivesShadow();

                    const size_t submeshCount = renderer->GetSubmeshCount();
                    proxy.materialIds.resize(submeshCount);
                    proxy.materialModes.resize(submeshCount);
                    proxy.materialResources.resize(submeshCount);
                    for (size_t i = 0; i < submeshCount; ++i)
                    {
                        auto material = renderer->GetMaterial(i);
                        proxy.materialIds[i] = material.IsValid() ? material.GetId() : 0;
                        proxy.materialModes[i] = ToRenderMaterialMode(material.As<IRenderMaterialSource>());
                        proxy.materialResources[i] = material.As<IRenderMaterialSource>();
                    }

                    proxy.sortKey = proxy.materialIds.empty() ? 0 : proxy.materialIds[0];
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
