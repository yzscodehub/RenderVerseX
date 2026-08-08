/**
 * @file RenderFrameExtractor.cpp
 * @brief RenderFrameExtractor implementation.
 */

#include "RenderExtraction/RenderFrameExtractor.h"

#include "RenderContracts/RenderFrameValidation.h"
#include "RenderExtraction/RenderFeatureSceneBridge.h"
#include "RenderExtraction/RenderProxySceneBridge.h"
#include "RenderExtraction/SceneEnvironmentIBLBridge.h"
#include "RenderExtraction/SceneSkyboxPassBridge.h"
#include "Resource/ResourceSubsystem.h"
#include "Scene/Components/DecalComponent.h"
#include "Scene/Components/LightProbeComponent.h"
#include "Scene/Components/ReflectionProbeComponent.h"
#include "Scene/SceneEntity.h"
#include "Scene/SceneRuntime.h"
#include "World/World.h"

#include <cmath>
#include <limits>
#include <utility>

namespace RVX
{
namespace
{
    constexpr uint32 PRIMITIVE_VISIBLE = 1U << 0U;
    constexpr uint32 PRIMITIVE_CASTS_SHADOW = 1U << 1U;
    constexpr uint32 PRIMITIVE_RECEIVES_SHADOW = 1U << 2U;
    constexpr uint32 PRIMITIVE_MATERIAL_MODE_SHIFT = 8U;

    bool IsFinite(float32 value)
    {
        return std::isfinite(value);
    }

    RenderLightType ToRenderLightType(RenderLightProxy::Type type)
    {
        switch (type)
        {
            case RenderLightProxy::Type::Point:
                return RenderLightType::Point;
            case RenderLightProxy::Type::Spot:
                return RenderLightType::Spot;
            case RenderLightProxy::Type::Directional:
            default:
                return RenderLightType::Directional;
        }
    }

    RenderDecalBlendMode ToRenderDecalBlendMode(DecalBlendMode mode)
    {
        switch (mode)
        {
            case DecalBlendMode::Stain:
                return RenderDecalBlendMode::Stain;
            case DecalBlendMode::Emissive:
                return RenderDecalBlendMode::Emissive;
            case DecalBlendMode::Normal:
                return RenderDecalBlendMode::Normal;
            case DecalBlendMode::Default:
            default:
                return RenderDecalBlendMode::Default;
        }
    }

    RenderProbeMode ToRenderProbeMode(ReflectionProbeMode mode)
    {
        switch (mode)
        {
            case ReflectionProbeMode::Realtime:
                return RenderProbeMode::Realtime;
            case ReflectionProbeMode::Custom:
                return RenderProbeMode::Custom;
            case ReflectionProbeMode::Baked:
            default:
                return RenderProbeMode::Baked;
        }
    }

    RenderProbeMode ToRenderProbeMode(LightProbeMode mode)
    {
        switch (mode)
        {
            case LightProbeMode::Realtime:
                return RenderProbeMode::Realtime;
            case LightProbeMode::Custom:
                return RenderProbeMode::Custom;
            case LightProbeMode::Baked:
            default:
                return RenderProbeMode::Baked;
        }
    }

    uint32 BuildPrimitiveFlags(const RenderPrimitiveProxy& proxy)
    {
        uint32 flags = proxy.visible ? PRIMITIVE_VISIBLE : 0U;
        flags |= proxy.castsShadow ? PRIMITIVE_CASTS_SHADOW : 0U;
        flags |= proxy.receivesShadow ? PRIMITIVE_RECEIVES_SHADOW : 0U;
        if (!proxy.materialModes.empty())
        {
            flags |= static_cast<uint32>(proxy.materialModes.front())
                     << PRIMITIVE_MATERIAL_MODE_SHIFT;
        }
        return flags;
    }

    void StampFeatureSequence(RenderFeatureSnapshot& features,
                              uint64 sequence)
    {
        features.metadata.sequence = sequence;
        features.particles.metadata.sequence = sequence;
        features.water.metadata.sequence = sequence;
        features.terrain.metadata.sequence = sequence;
    }

    bool FitsUint32(size_t value)
    {
        return value <= std::numeric_limits<uint32>::max();
    }

    template<typename State, typename Equal, typename Upsert, typename Remove>
    void AccumulateDifferences(
        const std::unordered_map<uint64, State>& published,
        const std::unordered_map<uint64, State>& current,
        Equal equal,
        Upsert upsert,
        Remove remove)
    {
        for (const auto& [id, state] : current)
        {
            const auto previous = published.find(id);
            if (previous == published.end())
                static_cast<void>(upsert(state, true));
            else if (!equal(previous->second, state))
                static_cast<void>(upsert(state, false));
        }
        for (const auto& [id, state] : published)
        {
            (void)state;
            if (current.find(id) == current.end())
                static_cast<void>(remove(id));
        }
    }
} // namespace

RenderFrameExtractionResult RenderFrameExtractor::Extract(
    const RenderFrameExtractionInput& input)
{
    if (m_publicationPending)
    {
        // Extraction can be used without the Engine composition in tools and
        // tests. An unacknowledged candidate was not published and therefore
        // cannot become the next incremental baseline.
        ResolveLastPublication(
            RenderFramePublicationDisposition::NotAccepted);
    }

    RenderFrameExtractionResult result;
    auto fail = [&result](RenderFrameExtractionResultCode code)
    {
        result.code = code;
        result.packet.reset();
        result.sceneUpdate.reset();
        result.frameV5.reset();
        result.diagnostics.complete = false;
        return std::move(result);
    };

    if (input.sequence == 0 || input.sequence <= m_lastCompletedSequence)
    {
        return fail(
            RenderFrameExtractionResultCode::NonMonotonicSequence);
    }
    if (input.world == nullptr)
    {
        return fail(RenderFrameExtractionResultCode::NullWorld);
    }
    if (input.resources == nullptr)
    {
        return fail(
            RenderFrameExtractionResultCode::MissingResourceSubsystem);
    }
    if (!IsValidRenderFrameSettings(input.settings))
    {
        result.diagnostics.code = RenderExtractionCode::InvalidNumericValue;
        return fail(RenderFrameExtractionResultCode::InvalidSettings);
    }
    if (!IsValidRenderFrameCaptureRequest(input.captureRequest))
    {
        return fail(
            RenderFrameExtractionResultCode::InvalidCaptureRequest);
    }

    RenderViewSnapshot view;
    const WorldCameraBridgeCode cameraCode =
        WorldCameraBridge{}.Extract(input.world, input.view, view);
    if (cameraCode != WorldCameraBridgeCode::Complete)
    {
        return fail(RenderFrameExtractionResultCode::MissingCamera);
    }

    RenderProxySnapshot proxies;
    RenderProxySceneBridgeResult proxyResult;
    if (!RenderProxySceneBridge{}.BuildSnapshot(
            input.world, proxies, &proxyResult))
    {
        result.diagnostics.code = RenderExtractionCode::MissingProvider;
        result.diagnostics.skippedPrimitiveCount = 1;
        return fail(
            RenderFrameExtractionResultCode::ProxyExtractionFailed);
    }

    RenderFeatureSnapshot features;
    RenderFeatureSceneBridgeResult featureResult;
    if (!RenderFeatureSceneBridge{}.BuildSnapshot(
            input.world, features, &featureResult))
    {
        result.diagnostics.code = RenderExtractionCode::MissingProvider;
        result.diagnostics.skippedFeatureProviderCount =
            static_cast<uint32>(featureResult.skippedProviderCount);
        return fail(
            RenderFrameExtractionResultCode::FeatureExtractionFailed);
    }
    StampFeatureSequence(features, input.sequence);

    if (!FitsUint32(proxies.primitives.size()) ||
        !FitsUint32(proxies.lights.size()) ||
        !FitsUint32(features.metadata.providerCount))
    {
        result.diagnostics.code = RenderExtractionCode::CountMismatch;
        return fail(RenderFrameExtractionResultCode::SealFailed);
    }

    RenderFramePacketBuilder builder;
    for (const RenderPrimitiveProxy& proxy : proxies.primitives)
    {
        if (proxy.materialAssetIds.size() != proxy.materialModes.size() ||
            !FitsUint32(proxy.materialAssetIds.size()))
        {
            result.diagnostics.code = RenderExtractionCode::CountMismatch;
            result.diagnostics.skippedPrimitiveCount = 1;
            return fail(
                RenderFrameExtractionResultCode::ProxyExtractionFailed);
        }
        const Resource::RenderResourceResolveResult mesh =
            input.resources->ResolveRenderResource(
                proxy.meshAssetId, RenderResourceKind::Mesh);
        if (!proxy.meshAssetId.IsValid() ||
            mesh.code != Resource::RenderResourceResolveCode::Resolved ||
            !mesh.handle.IsValid())
        {
            result.diagnostics.code =
                RenderExtractionCode::InvalidResourceReference;
            result.diagnostics.skippedPrimitiveCount = 1;
            return fail(
                RenderFrameExtractionResultCode::RequiredResourceUnresolved);
        }

        RenderPrimitiveSnapshot primitive;
        primitive.objectId = proxy.id.IsValid() ? proxy.id.value
                                                : proxy.ownerId;
        primitive.mesh = mesh.handle;
        primitive.submeshes.reserve(proxy.materialAssetIds.size());
        for (size_t index = 0; index < proxy.materialAssetIds.size(); ++index)
        {
            RenderSubmeshMaterialBinding binding;
            binding.submeshIndex = static_cast<uint32>(index);
            binding.materialMode = proxy.materialModes[index];
            if (proxy.materialAssetIds[index].IsValid())
            {
                const Resource::RenderResourceResolveResult material =
                    input.resources->ResolveRenderResource(
                        proxy.materialAssetIds[index],
                        RenderResourceKind::Material);
                if (material.code ==
                    Resource::RenderResourceResolveCode::Resolved)
                {
                    binding.material = material.handle;
                }
            }
            primitive.submeshes.push_back(binding);
        }
        if (!primitive.submeshes.empty())
        {
            primitive.material = primitive.submeshes.front().material;
        }
        primitive.worldTransform = proxy.worldMatrix;
        primitive.previousWorldTransform = proxy.worldMatrix;
        primitive.boundsMin = proxy.bounds.GetMin();
        primitive.boundsMax = proxy.bounds.GetMax();
        primitive.flags = BuildPrimitiveFlags(proxy);
        primitive.layerMask = proxy.layerMask;
        primitive.sortKey = proxy.sortKey;
        primitive.skinMatrices = proxy.skinningMatrices;
        static_cast<void>(builder.AddPrimitive(std::move(primitive)));
    }

    for (const RenderLightProxy& proxy : proxies.lights)
    {
        RenderLightSnapshot light;
        light.lightId = proxy.id.IsValid() ? proxy.id.value : proxy.ownerId;
        light.type = ToRenderLightType(proxy.type);
        light.position = proxy.position;
        light.direction = proxy.direction;
        light.color = proxy.color;
        light.intensity = proxy.intensity;
        light.range = proxy.range;
        light.innerConeRadians = proxy.innerConeAngle;
        light.outerConeRadians = proxy.outerConeAngle;
        // Shadow allocation is Render-owned. The frame packet carries the
        // update-side intent while an optional shadowResource is reserved for
        // externally prepared shadow data.
        light.castsShadows = proxy.castsShadow;
        static_cast<void>(builder.AddLight(std::move(light)));
    }

    RenderSkySnapshot sky;
    SceneSkyboxSnapshot extractedSky;
    if (SceneSkyboxPassBridge{}.Extract(input.world, extractedSky))
    {
        switch (extractedSky.mode)
        {
            case SceneSkyboxSnapshotMode::Disabled:
                sky.mode = RenderSkyMode::Disabled;
                break;
            case SceneSkyboxSnapshotMode::Cubemap:
                sky.mode = RenderSkyMode::Cubemap;
                break;
            case SceneSkyboxSnapshotMode::Equirectangular:
                sky.mode = RenderSkyMode::Equirectangular;
                break;
            case SceneSkyboxSnapshotMode::Procedural:
                sky.mode = RenderSkyMode::Procedural;
                break;
            case SceneSkyboxSnapshotMode::SolidColor:
                sky.mode = RenderSkyMode::SolidColor;
                break;
        }
        sky.tint = extractedSky.tint;
        sky.sunDirection = extractedSky.sunDirection;
        sky.sunColor = extractedSky.sunColor;
        sky.zenithColor = extractedSky.zenithColor;
        sky.horizonColor = extractedSky.horizonColor;
        sky.groundColor = extractedSky.groundColor;
        sky.intensity = extractedSky.intensity;
        sky.rotationRadians = extractedSky.rotationRadians;
        sky.blurLevel = extractedSky.blurLevel;
        sky.scatteringIntensity = extractedSky.scatteringIntensity;
        if (extractedSky.textureAssetId.IsValid())
        {
            const Resource::RenderResourceResolveResult resolved =
                input.resources->ResolveRenderResource(
                    extractedSky.textureAssetId,
                    RenderResourceKind::Texture);
            if (resolved.code ==
                Resource::RenderResourceResolveCode::Resolved)
            {
                sky.skyTexture = resolved.handle;
            }
        }
    }

    RenderEnvironmentSnapshot environment;
    SceneEnvironmentIBLSnapshot extractedEnvironment;
    if (SceneEnvironmentIBLBridge{}.Extract(
            input.world, extractedEnvironment))
    {
        const Resource::RenderResourceResolveResult irradiance =
            input.resources->ResolveRenderResource(
                extractedEnvironment.irradianceAssetId,
                RenderResourceKind::Texture);
        const Resource::RenderResourceResolveResult prefiltered =
            input.resources->ResolveRenderResource(
                extractedEnvironment.prefilteredAssetId,
                RenderResourceKind::Texture);
        const Resource::RenderResourceResolveResult brdfLut =
            input.resources->ResolveRenderResource(
                extractedEnvironment.brdfLutAssetId,
                RenderResourceKind::Texture);
        if (irradiance.code ==
                Resource::RenderResourceResolveCode::Resolved &&
            prefiltered.code ==
                Resource::RenderResourceResolveCode::Resolved &&
            brdfLut.code ==
                Resource::RenderResourceResolveCode::Resolved)
        {
            environment.irradianceTexture = irradiance.handle;
            environment.prefilteredTexture = prefiltered.handle;
            environment.brdfLutTexture = brdfLut.handle;
            environment.intensity = extractedEnvironment.intensity;
        }
    }

    std::unordered_map<uint64, RenderDecalSnapshot> extractedDecals;
    std::unordered_map<uint64, RenderProbeSnapshot> extractedProbes;
    Scene* scene = input.world->GetScene();
    if (scene == nullptr)
        return fail(RenderFrameExtractionResultCode::ProxyExtractionFailed);

    for (DecalComponent* component :
         scene->GetComponentsImplementing<DecalComponent>())
    {
        SceneEntity* owner = component ? component->GetOwner() : nullptr;
        if (!component || !component->IsEnabled() || !owner ||
            !owner->IsActive() ||
            !component->GetComponentHandle().IsValid())
        {
            continue;
        }

        RenderDecalSnapshot decal;
        decal.decalId = component->GetComponentHandle().GetPackedValue();
        decal.worldTransform = owner->GetWorldMatrix();
        decal.halfExtent = component->GetSize();
        decal.color = component->GetColor();
        decal.opacity = component->GetOpacity();
        decal.normalStrength = component->GetNormalStrength();
        decal.angleFade = component->GetAngleFade();
        decal.fadeDistance = component->GetFadeDistance();
        decal.fadeWidth = component->GetFadeWidth();
        decal.layerMask = component->GetDecalMask();
        decal.sortOrder = component->GetSortOrder();
        decal.blendMode = ToRenderDecalBlendMode(component->GetBlendMode());
        const SceneMaterialHandle material = component->GetMaterial();
        if (material.IsValid())
        {
            const Resource::RenderResourceResolveResult resolved =
                input.resources->ResolveRenderResource(
                    AssetId{material.GetId()}, RenderResourceKind::Material);
            if (resolved.code ==
                Resource::RenderResourceResolveCode::Resolved)
            {
                decal.material = resolved.handle;
            }
        }
        if (!extractedDecals.emplace(decal.decalId, std::move(decal)).second)
            return fail(RenderFrameExtractionResultCode::ProxyExtractionFailed);
    }

    for (ReflectionProbeComponent* component :
         scene->GetComponentsImplementing<ReflectionProbeComponent>())
    {
        SceneEntity* owner = component ? component->GetOwner() : nullptr;
        if (!component || !component->IsEnabled() || !owner ||
            !owner->IsActive() ||
            !component->GetComponentHandle().IsValid())
        {
            continue;
        }

        RenderProbeSnapshot probe;
        probe.probeId = component->GetComponentHandle().GetPackedValue();
        probe.kind = RenderProbeKind::Reflection;
        probe.shape = component->GetShape() == ReflectionProbeShape::Sphere
                          ? RenderProbeShape::Sphere
                          : RenderProbeShape::Box;
        probe.mode = ToRenderProbeMode(component->GetMode());
        probe.worldTransform = owner->GetWorldMatrix();
        probe.influenceExtent = component->GetSize();
        probe.blendDistance = component->GetBlendDistance();
        probe.boxProjectionSize = component->GetBoxProjectionSize();
        probe.boxProjectionOffset = component->GetBoxProjectionOffset();
        probe.cullingMask = component->GetCullingMask();
        probe.priority = component->GetImportance();
        probe.nearClip = component->GetNearClip();
        probe.farClip = component->GetFarClip();
        probe.useBoxProjection = component->UseBoxProjection();
        probe.useHDR = component->UseHDR();
        const SceneTextureHandle cubemap = component->GetCubemap();
        if (cubemap.IsValid())
        {
            const Resource::RenderResourceResolveResult resolved =
                input.resources->ResolveRenderResource(
                    AssetId{cubemap.GetId()}, RenderResourceKind::Texture);
            if (resolved.code ==
                Resource::RenderResourceResolveCode::Resolved)
            {
                probe.texture = resolved.handle;
            }
        }
        probe.hasValidData = component->HasValidCubemap() &&
                             probe.texture.IsValid();
        if (!extractedProbes.emplace(probe.probeId, std::move(probe)).second)
            return fail(RenderFrameExtractionResultCode::ProxyExtractionFailed);
    }

    for (LightProbeComponent* component :
         scene->GetComponentsImplementing<LightProbeComponent>())
    {
        SceneEntity* owner = component ? component->GetOwner() : nullptr;
        if (!component || !component->IsEnabled() || !owner ||
            !owner->IsActive() ||
            !component->GetComponentHandle().IsValid())
        {
            continue;
        }

        RenderProbeSnapshot probe;
        probe.probeId = component->GetComponentHandle().GetPackedValue();
        probe.kind = RenderProbeKind::Light;
        probe.shape = RenderProbeShape::Point;
        probe.mode = ToRenderProbeMode(component->GetMode());
        probe.worldTransform = owner->GetWorldMatrix();
        probe.sphericalHarmonics = component->GetSH().GetShaderData();
        probe.cullingMask = component->GetCullingMask();
        probe.priority = component->GetGroupID();
        probe.nearClip = component->GetNearClip();
        probe.farClip = component->GetFarClip();
        probe.hasValidData = component->HasValidData();
        if (!extractedProbes.emplace(probe.probeId, std::move(probe)).second)
            return fail(RenderFrameExtractionResultCode::ProxyExtractionFailed);
    }

    RenderFrameHeader header;
    header.sequence = input.sequence;
    header.worldRevision = input.worldRevision;
    header.temporalEpoch = input.temporalEpoch;
    header.expectedPrimitiveCount =
        static_cast<uint32>(proxies.primitives.size());
    header.extractedPrimitiveCount = header.expectedPrimitiveCount;
    header.expectedLightCount =
        static_cast<uint32>(proxies.lights.size());
    header.extractedLightCount = header.expectedLightCount;
    header.expectedFeatureProviderCount =
        static_cast<uint32>(features.metadata.providerCount);
    header.extractedFeatureProviderCount =
        header.expectedFeatureProviderCount;
    header.explicitDiscontinuity = input.explicitDiscontinuity;

    result.diagnostics.code = RenderExtractionCode::Complete;
    result.diagnostics.complete = true;
    static_cast<void>(builder.SetHeader(header));
    static_cast<void>(builder.SetView(std::move(view)));
    static_cast<void>(builder.SetSky(std::move(sky)));
    static_cast<void>(builder.SetEnvironment(std::move(environment)));
    static_cast<void>(builder.SetSettings(input.settings));
    static_cast<void>(builder.SetCaptureRequest(input.captureRequest));
    static_cast<void>(builder.SetFeatures(std::move(features)));
    static_cast<void>(builder.SetExtractionDiagnostics(result.diagnostics));
    result.packet = builder.Seal();
    result.sealCode = builder.GetLastSealCode();
    if (result.packet == nullptr)
    {
        return fail(RenderFrameExtractionResultCode::SealFailed);
    }

    RetainedSceneState currentScene;
    for (const RenderPrimitiveSnapshot& primitive :
         result.packet->GetPrimitives())
    {
        if (!currentScene.primitives.emplace(primitive.objectId, primitive)
                 .second)
        {
            return fail(RenderFrameExtractionResultCode::ProxyExtractionFailed);
        }
    }
    for (const RenderLightSnapshot& light : result.packet->GetLights())
    {
        if (!currentScene.lights.emplace(light.lightId, light).second)
            return fail(RenderFrameExtractionResultCode::ProxyExtractionFailed);
    }
    currentScene.decals = std::move(extractedDecals);
    currentScene.probes = std::move(extractedProbes);
    for (const ParticleRenderSnapshotItem& particle :
         result.packet->GetFeatures().particles.items)
    {
        if (!currentScene.particles.emplace(particle.instanceId, particle)
                 .second)
        {
            return fail(RenderFrameExtractionResultCode::FeatureExtractionFailed);
        }
    }
    for (const WaterRenderSnapshotItem& water :
         result.packet->GetFeatures().water.items)
    {
        if (!currentScene.water.emplace(water.componentId, water).second)
            return fail(RenderFrameExtractionResultCode::FeatureExtractionFailed);
    }
    for (const TerrainRenderSnapshotItem& terrain :
         result.packet->GetFeatures().terrain.items)
    {
        if (!currentScene.terrain.emplace(terrain.componentId, terrain).second)
            return fail(RenderFrameExtractionResultCode::FeatureExtractionFailed);
    }
    currentScene.sky = result.packet->GetSky();
    currentScene.environment = result.packet->GetEnvironment();

    const bool fullReset = m_forceFullReset || !m_publishedScene.has_value() ||
                           input.worldRevision != m_publishedWorldRevision;
    RenderSceneMutationAccumulator sceneAccumulator;
    sceneAccumulator.Begin(fullReset ? 0 : m_publishedSceneRevision,
                           fullReset);
    if (fullReset)
    {
        for (const auto& [id, primitive] : currentScene.primitives)
        {
            (void)id;
            static_cast<void>(
                sceneAccumulator.UpsertPrimitive(primitive, true));
        }
        for (const auto& [id, light] : currentScene.lights)
        {
            (void)id;
            static_cast<void>(sceneAccumulator.UpsertLight(light, true));
        }
        for (const auto& [id, decal] : currentScene.decals)
        {
            (void)id;
            static_cast<void>(sceneAccumulator.UpsertDecal(decal, true));
        }
        for (const auto& [id, probe] : currentScene.probes)
        {
            (void)id;
            static_cast<void>(sceneAccumulator.UpsertProbe(probe, true));
        }
        for (const auto& [id, particle] : currentScene.particles)
        {
            (void)id;
            static_cast<void>(sceneAccumulator.UpsertParticle(particle, true));
        }
        for (const auto& [id, water] : currentScene.water)
        {
            (void)id;
            static_cast<void>(sceneAccumulator.UpsertWater(water, true));
        }
        for (const auto& [id, terrain] : currentScene.terrain)
        {
            (void)id;
            static_cast<void>(sceneAccumulator.UpsertTerrain(terrain, true));
        }
        sceneAccumulator.UpsertSky(currentScene.sky);
        sceneAccumulator.UpsertEnvironment(currentScene.environment);
    }
    else
    {
        const RetainedSceneState& published = *m_publishedScene;
        AccumulateDifferences(
            published.primitives,
            currentScene.primitives,
            AreRenderPrimitiveSnapshotsEqual,
            [&sceneAccumulator](const RenderPrimitiveSnapshot& state,
                                bool created)
            {
                return sceneAccumulator.UpsertPrimitive(state, created);
            },
            [&sceneAccumulator](uint64 id)
            {
                return sceneAccumulator.RemovePrimitive(id);
            });
        AccumulateDifferences(
            published.lights,
            currentScene.lights,
            AreRenderLightSnapshotsEqual,
            [&sceneAccumulator](const RenderLightSnapshot& state,
                                bool created)
            {
                return sceneAccumulator.UpsertLight(state, created);
            },
            [&sceneAccumulator](uint64 id)
            {
                return sceneAccumulator.RemoveLight(id);
            });
        AccumulateDifferences(
            published.decals,
            currentScene.decals,
            AreRenderDecalSnapshotsEqual,
            [&sceneAccumulator](const RenderDecalSnapshot& state,
                                bool created)
            {
                return sceneAccumulator.UpsertDecal(state, created);
            },
            [&sceneAccumulator](uint64 id)
            {
                return sceneAccumulator.RemoveDecal(id);
            });
        AccumulateDifferences(
            published.probes,
            currentScene.probes,
            AreRenderProbeSnapshotsEqual,
            [&sceneAccumulator](const RenderProbeSnapshot& state,
                                bool created)
            {
                return sceneAccumulator.UpsertProbe(state, created);
            },
            [&sceneAccumulator](uint64 id)
            {
                return sceneAccumulator.RemoveProbe(id);
            });
        AccumulateDifferences(
            published.particles,
            currentScene.particles,
            AreParticleRenderSnapshotItemsEqual,
            [&sceneAccumulator](const ParticleRenderSnapshotItem& state,
                                bool created)
            {
                return sceneAccumulator.UpsertParticle(state, created);
            },
            [&sceneAccumulator](uint64 id)
            {
                return sceneAccumulator.RemoveParticle(id);
            });
        AccumulateDifferences(
            published.water,
            currentScene.water,
            AreWaterRenderSnapshotItemsEqual,
            [&sceneAccumulator](const WaterRenderSnapshotItem& state,
                                bool created)
            {
                return sceneAccumulator.UpsertWater(state, created);
            },
            [&sceneAccumulator](uint64 id)
            {
                return sceneAccumulator.RemoveWater(id);
            });
        AccumulateDifferences(
            published.terrain,
            currentScene.terrain,
            AreTerrainRenderSnapshotItemsEqual,
            [&sceneAccumulator](const TerrainRenderSnapshotItem& state,
                                bool created)
            {
                return sceneAccumulator.UpsertTerrain(state, created);
            },
            [&sceneAccumulator](uint64 id)
            {
                return sceneAccumulator.RemoveTerrain(id);
            });
        if (!AreRenderSkySnapshotsEqual(published.sky, currentScene.sky))
            sceneAccumulator.UpsertSky(currentScene.sky);
        if (!AreRenderEnvironmentSnapshotsEqual(
                published.environment, currentScene.environment))
        {
            sceneAccumulator.UpsertEnvironment(currentScene.environment);
        }
    }

    RenderSceneUpdateBatch builtSceneUpdate =
        sceneAccumulator.Build(input.sequence);
    if (!builtSceneUpdate.IsStructurallyValid())
        return fail(RenderFrameExtractionResultCode::SealFailed);

    const bool publishesSceneUpdate =
        builtSceneUpdate.fullReset || !builtSceneUpdate.Empty();
    const uint64 requiredSceneRevision = publishesSceneUpdate
        ? builtSceneUpdate.targetSceneRevision
        : m_publishedSceneRevision;
    if (requiredSceneRevision == 0)
        return fail(RenderFrameExtractionResultCode::SealFailed);

    RenderFrameHeaderV5 headerV5;
    headerV5.sequence = result.packet->GetHeader().sequence;
    headerV5.requiredSceneRevision = requiredSceneRevision;
    headerV5.worldRevision = result.packet->GetHeader().worldRevision;
    headerV5.temporalEpoch = result.packet->GetHeader().temporalEpoch;
    headerV5.explicitDiscontinuity =
        result.packet->GetHeader().explicitDiscontinuity;
    result.frameV5 = RenderFramePacketV5::Create(
        headerV5,
        result.packet->GetView(),
        result.packet->GetSettings(),
        result.packet->GetCaptureRequest(),
        result.packet->GetExtractionDiagnostics());
    if (result.frameV5 == nullptr)
        return fail(RenderFrameExtractionResultCode::SealFailed);
    if (publishesSceneUpdate)
    {
        result.sceneUpdate = std::make_unique<RenderSceneUpdateBatch>(
            std::move(builtSceneUpdate));
    }

    m_candidateScene = std::move(currentScene);
    m_candidateSceneRevision = requiredSceneRevision;
    m_candidateWorldRevision = input.worldRevision;
    m_publicationPending = true;

    m_lastCompletedSequence = input.sequence;
    result.code = RenderFrameExtractionResultCode::Complete;
    return result;
}

void RenderFrameExtractor::ResolveLastPublication(
    RenderFramePublicationDisposition disposition) noexcept
{
    if (!m_publicationPending)
        return;

    if (disposition == RenderFramePublicationDisposition::Accepted &&
        m_candidateScene.has_value())
    {
        m_publishedScene = std::move(m_candidateScene);
        m_publishedSceneRevision = m_candidateSceneRevision;
        m_publishedWorldRevision = m_candidateWorldRevision;
        m_forceFullReset = false;
    }
    else if (disposition ==
             RenderFramePublicationDisposition::SceneUpdateAcceptedWithoutFrame)
    {
        // The reliable update channel may already have advanced while neither
        // frame mailbox accepted the matching frame. A checkpoint is the only
        // safe way to re-establish both channel baselines.
        m_forceFullReset = true;
    }
    // A queue-full/not-accepted candidate never advanced the reliable queue.
    // Preserve the last published baseline so the next extraction naturally
    // coalesces every intervening create/update/remove into one complete diff.
    m_candidateScene.reset();
    m_candidateSceneRevision = 0;
    m_candidateWorldRevision = 0;
    m_publicationPending = false;
}
} // namespace RVX
