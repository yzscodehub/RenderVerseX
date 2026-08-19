#include "RenderExtraction/EcsFrozenSceneBridge.h"

#include "ECS/Registry.h"

#include <cstddef>
#include <gtest/gtest.h>

namespace
{
using namespace RVX;
using namespace RVX::SceneECS;

[[nodiscard]] Mat4 MakeTranslation(float32 x, float32 y = 0.0f, float32 z = 0.0f)
{
    Mat4 result{1.0f};
    result[3] = Vec4(x, y, z, 1.0f);
    return result;
}

[[nodiscard]] FrozenSceneObjectId MakeId(
    ECS::SceneRuntimeId sceneRuntimeId,
    ECS::EntityHandle entity,
    FrozenSceneObjectType type,
    uint32 slot = 0)
{
    return MakeFrozenSceneObjectId(sceneRuntimeId, entity, type, slot);
}

[[nodiscard]] FrozenSceneMesh MakeMesh(ECS::SceneRuntimeId sceneRuntimeId,
                                       ECS::EntityHandle entity,
                                       uint64 meshAssetId = 101)
{
    FrozenSceneMesh mesh;
    mesh.id = MakeId(sceneRuntimeId, entity, FrozenSceneObjectType::Mesh);
    mesh.sourceEntity = entity;
    mesh.source.meshAssetId = {.value = meshAssetId};
    mesh.source.submeshCount = 2;
    mesh.source.flags = 0x55u;
    mesh.visibility.layerMask = 0x0fu;
    mesh.visibility.visible = true;
    mesh.visibility.castsShadow = false;
    mesh.visibility.receivesShadow = true;
    mesh.visibilityWriteVersion = 47;
    mesh.bounds.center = {1.0f, 2.0f, 3.0f};
    mesh.bounds.extents = {1.0f, 2.0f, 3.0f};
    mesh.worldTransform = MakeTranslation(10.0f);
    mesh.previousSimulationWorldTransform = MakeTranslation(9.0f);
    mesh.transformSourceRevision = 31;
    mesh.previousSimulationSourceRevision = 30;

    mesh.materialSlotsSource.count = 2;
    mesh.materialSlotsSource.values[0].materialAssetId = {.value = 201};
    mesh.materialSlotsSource.values[0].materialMode = RenderMaterialMode::Masked;
    mesh.materialSlotsSource.values[1].materialAssetId = {.value = 202};
    mesh.materialSlotsSource.values[1].materialMode =
        RenderMaterialMode::Transparent;
    mesh.materialSlots.push_back({
        .id = MakeId(sceneRuntimeId,
                     entity,
                     FrozenSceneObjectType::MaterialSlot,
                     0),
        .source = mesh.materialSlotsSource.values[0],
    });
    mesh.materialSlots.push_back({
        .id = MakeId(sceneRuntimeId,
                     entity,
                     FrozenSceneObjectType::MaterialSlot,
                     1),
        .source = mesh.materialSlotsSource.values[1],
    });
    return mesh;
}

[[nodiscard]] FrozenSceneSnapshot MakeCompleteSnapshot()
{
    FrozenSceneSnapshot snapshot;
    snapshot.sceneRuntimeId = ECS::SceneRuntimeId{72};
    snapshot.revision = 901;
    snapshot.structuralJournalSequence = 55;
    snapshot.renderWorldTransformWriteVersion = 66;

    const ECS::EntityHandle cameraEntity = ECS::EntityHandle::Create(3, 7);
    FrozenSceneCamera camera;
    camera.id = MakeId(snapshot.sceneRuntimeId,
                       cameraEntity,
                       FrozenSceneObjectType::Camera);
    camera.source.projection = CameraProjection::Perspective;
    camera.source.clearPolicy = CameraClearPolicy::SolidColor;
    camera.source.verticalFieldOfViewRadians = 0.9f;
    camera.source.nearPlane = 0.25f;
    camera.source.farPlane = 800.0f;
    camera.source.aspectRatio = 1.5f;
    camera.source.exposure = 2.5f;
    camera.source.normalizedViewport = {0.1f, 0.2f, 0.7f, 0.6f};
    camera.source.clearColor = {0.2f, 0.3f, 0.4f, 1.0f};
    camera.source.cullingMask = 0x0au;
    camera.source.priority = 17;
    camera.worldTransform = MakeTranslation(5.0f, 6.0f, 7.0f);
    camera.transformSourceRevision = 19;
    snapshot.cameras.push_back(camera);
    snapshot.selectedCamera = camera.id;

    snapshot.meshes.push_back(MakeMesh(
        snapshot.sceneRuntimeId, ECS::EntityHandle::Create(4, 8)));

    FrozenSceneLight light;
    light.id = MakeId(snapshot.sceneRuntimeId,
                      ECS::EntityHandle::Create(5, 9),
                      FrozenSceneObjectType::Light);
    light.sourceEntity = light.id.entity;
    light.source.type = LightType::Spot;
    light.source.color = {0.1f, 0.2f, 0.3f};
    light.source.intensity = 8.0f;
    light.source.range = 42.0f;
    light.source.innerConeRadians = 0.25f;
    light.source.outerConeRadians = 0.5f;
    light.source.castsShadows = true;
    light.visibility.layerMask = 0x20u;
    light.visibility.visible = true;
    light.worldTransform = MakeTranslation(-2.0f, 3.0f, 4.0f);
    light.transformSourceRevision = 22;
    snapshot.lights.push_back(light);

    FrozenSceneSkybox skybox;
    skybox.id = MakeId(snapshot.sceneRuntimeId,
                        ECS::EntityHandle::Create(6, 10),
                        FrozenSceneObjectType::Skybox);
    skybox.sourceEntity = skybox.id.entity;
    skybox.source.mode = SkyboxMode::Equirectangular;
    skybox.source.environmentAssetId = {.value = 301};
    skybox.source.prefilteredEnvironmentAssetId = {.value = 302};
    skybox.source.irradianceAssetId = {.value = 303};
    skybox.source.brdfLutAssetId = {.value = 304};
    skybox.source.solidColor = {0.4f, 0.5f, 0.6f};
    skybox.source.rotationRadians = 0.7f;
    skybox.source.blur = 0.8f;
    skybox.source.scatteringIntensity = 0.9f;
    skybox.source.contributesToLighting = false;
    skybox.skyboxWriteVersion = 53;
    snapshot.skyboxes.push_back(skybox);

    FrozenSceneParticle particle;
    particle.id = MakeId(snapshot.sceneRuntimeId,
                         ECS::EntityHandle::Create(7, 11),
                         FrozenSceneObjectType::Particle);
    particle.sourceEntity = particle.id.entity;
    particle.state.instanceId = 701;
    particle.state.systemId = 702;
    particle.state.systemAssetId = {.value = 401};
    particle.payloadRevision = 61;
    snapshot.particles.push_back(particle);

    FrozenSceneWater water;
    water.id = MakeId(snapshot.sceneRuntimeId,
                      ECS::EntityHandle::Create(8, 12),
                      FrozenSceneObjectType::Water);
    water.sourceEntity = water.id.entity;
    water.state.componentId = 801;
    water.state.surfaceAssetId = {.value = 501};
    water.state.materialAssetId = {.value = 502};
    water.payloadRevision = 62;
    snapshot.water.push_back(water);

    FrozenSceneTerrain terrain;
    terrain.id = MakeId(snapshot.sceneRuntimeId,
                        ECS::EntityHandle::Create(9, 13),
                        FrozenSceneObjectType::Terrain);
    terrain.sourceEntity = terrain.id.entity;
    terrain.state.componentId = 901;
    terrain.state.heightmapAssetId = {.value = 601};
    terrain.state.materialAssetId = {.value = 602};
    terrain.payloadRevision = 63;
    snapshot.terrain.push_back(terrain);
    return snapshot;
}

void AddMeshSource(ECS::Registry& registry, ECS::EntityHandle entity, uint64 assetId)
{
    Mesh mesh;
    mesh.meshAssetId = {.value = assetId};
    mesh.submeshCount = 1;
    ASSERT_TRUE(registry.Add<Mesh>(entity, mesh));

    MaterialSlots slots;
    slots.count = 1;
    slots.values[0].materialAssetId = {.value = assetId + 100};
    ASSERT_TRUE(registry.Add<MaterialSlots>(entity, slots));

    ASSERT_TRUE(registry.Add<Visibility>(entity, {}));
    ASSERT_TRUE(registry.Add<Bounds>(entity, {}));
    ASSERT_TRUE(registry.Add<RenderWorldTransform>(entity, {}));
    ASSERT_TRUE(registry.Add<PreviousSimulationWorldTransform>(entity, {}));
    ASSERT_TRUE(registry.Add<EntityLifecycleState>(entity, {}));
}

[[nodiscard]] uint64 ConstantRenderId(const FrozenSceneObjectId&) noexcept
{
    return 11;
}
} // namespace

TEST(EcsFrozenSceneBridgeValidation, MapsFrozenValuesToOneCommonRenderPayload)
{
    const FrozenSceneSnapshot snapshot = MakeCompleteSnapshot();
    EcsFrozenSceneBridgeOutput output;
    EcsFrozenSceneBridgeResult result;

    ASSERT_TRUE(EcsFrozenSceneBridge{}.Build(snapshot, output, &result));
    EXPECT_EQ(result.code, EcsFrozenSceneBridgeCode::Complete);
    EXPECT_EQ(output.sceneRuntimeId, snapshot.sceneRuntimeId);
    EXPECT_EQ(output.snapshotRevision, snapshot.revision);
    EXPECT_EQ(output.structuralJournalSequence, snapshot.structuralJournalSequence);
    EXPECT_EQ(output.renderWorldTransformWriteVersion,
              snapshot.renderWorldTransformWriteVersion);
    EXPECT_TRUE(output.renderProxies.metadata.complete);
    EXPECT_EQ(output.renderProxies.metadata.status,
              RenderProxySnapshotStatus::Complete);
    EXPECT_EQ(output.renderProxies.metadata.sequence, snapshot.revision);

    ASSERT_EQ(output.renderProxies.primitives.size(), 1u);
    const RenderPrimitiveProxy& primitive = output.renderProxies.primitives.front();
    EXPECT_TRUE(primitive.id.IsValid());
    EXPECT_EQ(primitive.ownerId, primitive.id.value);
    EXPECT_EQ(primitive.meshAssetId.value, 101u);
    EXPECT_FLOAT_EQ(primitive.worldMatrix[3].x, 10.0f);
    EXPECT_FLOAT_EQ(primitive.bounds.GetMin().x, 10.0f);
    EXPECT_FLOAT_EQ(primitive.bounds.GetMin().y, 0.0f);
    EXPECT_FLOAT_EQ(primitive.bounds.GetMax().x, 12.0f);
    EXPECT_FLOAT_EQ(primitive.bounds.GetMax().z, 6.0f);
    EXPECT_EQ(primitive.layerMask, 0x0fu);
    EXPECT_TRUE(primitive.visible);
    EXPECT_FALSE(primitive.castsShadow);
    EXPECT_TRUE(primitive.receivesShadow);
    ASSERT_EQ(primitive.materialAssetIds.size(), 2u);
    EXPECT_EQ(primitive.materialAssetIds[0].value, 201u);
    EXPECT_EQ(primitive.materialAssetIds[1].value, 202u);
    EXPECT_EQ(primitive.materialModes[0], RenderMaterialMode::Masked);
    EXPECT_EQ(primitive.materialModes[1], RenderMaterialMode::Transparent);
    EXPECT_EQ(primitive.sortKey, 201u);

    ASSERT_EQ(output.materialBindings.size(), 2u);
    EXPECT_TRUE(output.materialBindings[0].id != 0);
    EXPECT_NE(output.materialBindings[0].id, output.materialBindings[1].id);
    EXPECT_EQ(output.materialBindings[0].primitiveId.value, primitive.id.value);
    EXPECT_EQ(output.materialBindings[1].slot, 1u);

    ASSERT_EQ(output.primitiveTemporalValues.size(), 1u);
    const EcsFrozenScenePrimitiveTemporalValue& temporal =
        output.primitiveTemporalValues.front();
    EXPECT_EQ(temporal.primitiveId.value, primitive.id.value);
    EXPECT_EQ(temporal.sourceEntity, snapshot.meshes.front().sourceEntity);
    EXPECT_EQ(temporal.visibilityWriteVersion, 47U);
    EXPECT_FLOAT_EQ(temporal.previousWorldTransform[3].x, 9.0f);
    EXPECT_EQ(temporal.worldTransformSourceRevision, 31u);
    EXPECT_EQ(temporal.previousWorldTransformSourceRevision, 30u);

    ASSERT_EQ(output.renderProxies.lights.size(), 1u);
    const RenderLightProxy& proxyLight = output.renderProxies.lights.front();
    EXPECT_TRUE(proxyLight.id.IsValid());
    EXPECT_EQ(proxyLight.type, RenderLightProxy::Type::Spot);
    EXPECT_FLOAT_EQ(proxyLight.position.x, -2.0f);
    EXPECT_FLOAT_EQ(proxyLight.position.y, 3.0f);
    EXPECT_FLOAT_EQ(proxyLight.direction.z, -1.0f);
    EXPECT_FLOAT_EQ(proxyLight.intensity, 8.0f);
    EXPECT_FLOAT_EQ(proxyLight.range, 42.0f);
    EXPECT_TRUE(proxyLight.castsShadow);
    ASSERT_EQ(output.lightValues.size(), 1u);
    EXPECT_EQ(output.lightValues.front().sourceEntity,
              snapshot.lights.front().sourceEntity);
    EXPECT_EQ(output.lightValues.front().layerMask, 0x20u);
    EXPECT_TRUE(output.lightValues.front().visible);
    EXPECT_EQ(output.lightValues.front().transformSourceRevision, 22u);

    ASSERT_EQ(output.cameras.size(), 1u);
    ASSERT_TRUE(output.selectedCameraId.has_value());
    const EcsFrozenSceneCameraValue& camera = output.cameras.front();
    EXPECT_EQ(*output.selectedCameraId, camera.id);
    EXPECT_FLOAT_EQ(camera.view.cameraPosition.x, 5.0f);
    EXPECT_FLOAT_EQ(camera.view.viewMatrix[3].x, -5.0f);
    EXPECT_FLOAT_EQ(camera.view.nearPlane, 0.25f);
    EXPECT_FLOAT_EQ(camera.view.farPlane, 800.0f);
    EXPECT_FLOAT_EQ(camera.view.exposure, 2.5f);
    EXPECT_EQ(camera.view.cullingMask, 0x0au);
    EXPECT_FLOAT_EQ(camera.normalizedViewport.z, 0.7f);
    EXPECT_EQ(camera.clearPolicy, CameraClearPolicy::SolidColor);
    EXPECT_EQ(camera.cullingMask, 0x0au);
    EXPECT_EQ(camera.priority, 17);
    EXPECT_EQ(camera.transformSourceRevision, 19u);

    ASSERT_EQ(output.skyboxes.size(), 1u);
    EXPECT_EQ(output.skyboxes.front().sourceEntity,
              snapshot.skyboxes.front().sourceEntity);

    ASSERT_EQ(output.skyboxes.size(), 1u);
    const EcsFrozenSceneSkyboxValue& skybox = output.skyboxes.front();
    EXPECT_TRUE(skybox.id != 0);
    EXPECT_EQ(skybox.mode, RenderSkyMode::Equirectangular);
    EXPECT_EQ(skybox.environmentAssetId.value, 301u);
    EXPECT_EQ(skybox.prefilteredEnvironmentAssetId.value, 302u);
    EXPECT_EQ(skybox.irradianceAssetId.value, 303u);
    EXPECT_EQ(skybox.brdfLutAssetId.value, 304u);
    EXPECT_EQ(skybox.skyboxWriteVersion, 53U);
    EXPECT_FLOAT_EQ(skybox.rotationRadians, 0.7f);
    EXPECT_FALSE(skybox.contributesToLighting);

    ASSERT_EQ(output.particles.size(), 1U);
    const EcsFrozenSceneParticleValue& particle = output.particles.front();
    EXPECT_EQ(particle.id,
              EcsFrozenSceneBridge::DeriveRenderId(snapshot.particles.front().id));
    EXPECT_EQ(particle.state.instanceId, particle.id);
    EXPECT_EQ(particle.state.systemId, particle.id);
    EXPECT_EQ(particle.state.systemAssetId,
              snapshot.particles.front().state.systemAssetId);
    EXPECT_EQ(particle.payloadRevision, 61U);

    ASSERT_EQ(output.water.size(), 1U);
    const EcsFrozenSceneWaterValue& water = output.water.front();
    EXPECT_EQ(water.id,
              EcsFrozenSceneBridge::DeriveRenderId(snapshot.water.front().id));
    EXPECT_EQ(water.state.componentId, water.id);
    EXPECT_EQ(water.state.surfaceAssetId,
              snapshot.water.front().state.surfaceAssetId);
    EXPECT_EQ(water.state.materialAssetId,
              snapshot.water.front().state.materialAssetId);
    EXPECT_EQ(water.payloadRevision, 62U);

    ASSERT_EQ(output.terrain.size(), 1U);
    const EcsFrozenSceneTerrainValue& terrain = output.terrain.front();
    EXPECT_EQ(terrain.id,
              EcsFrozenSceneBridge::DeriveRenderId(snapshot.terrain.front().id));
    EXPECT_EQ(terrain.state.componentId, terrain.id);
    EXPECT_EQ(terrain.state.heightmapAssetId,
              snapshot.terrain.front().state.heightmapAssetId);
    EXPECT_EQ(terrain.state.materialAssetId,
              snapshot.terrain.front().state.materialAssetId);
    EXPECT_EQ(terrain.payloadRevision, 63U);
}

TEST(EcsFrozenSceneBridgeValidation, IdentitiesAreDeterministicAndGenerationQualified)
{
    FrozenSceneSnapshot snapshot = MakeCompleteSnapshot();
    snapshot.cameras.clear();
    snapshot.lights.clear();
    snapshot.skyboxes.clear();
    snapshot.selectedCamera.reset();

    EcsFrozenSceneBridgeOutput firstOutput;
    EcsFrozenSceneBridgeOutput repeatedOutput;
    ASSERT_TRUE(EcsFrozenSceneBridge{}.Build(snapshot, firstOutput));
    ASSERT_TRUE(EcsFrozenSceneBridge{}.Build(snapshot, repeatedOutput));
    ASSERT_EQ(firstOutput.renderProxies.primitives.size(), 1u);
    ASSERT_EQ(repeatedOutput.renderProxies.primitives.size(), 1u);
    EXPECT_EQ(firstOutput.renderProxies.primitives[0].id.value,
              repeatedOutput.renderProxies.primitives[0].id.value);
    EXPECT_EQ(firstOutput.materialBindings[0].id, repeatedOutput.materialBindings[0].id);

    snapshot.meshes[0].id.entity = ECS::EntityHandle::Create(4, 99);
    snapshot.meshes[0].sourceEntity = snapshot.meshes[0].id.entity;
    for (size_t slot = 0; slot < snapshot.meshes[0].materialSlots.size(); ++slot)
    {
        snapshot.meshes[0].materialSlots[slot].id.entity =
            snapshot.meshes[0].id.entity;
    }

    EcsFrozenSceneBridgeOutput generationChangedOutput;
    ASSERT_TRUE(EcsFrozenSceneBridge{}.Build(snapshot, generationChangedOutput));
    EXPECT_NE(firstOutput.renderProxies.primitives[0].id.value,
              generationChangedOutput.renderProxies.primitives[0].id.value);
    EXPECT_NE(firstOutput.materialBindings[0].id,
              generationChangedOutput.materialBindings[0].id);
}

TEST(EcsFrozenSceneBridgeValidation,
     FeatureRenderIdsAreGenerationQualifiedWhileAssetIdsRemainSourceValues)
{
    FrozenSceneSnapshot snapshot = MakeCompleteSnapshot();
    EcsFrozenSceneBridgeOutput firstOutput;
    ASSERT_TRUE(EcsFrozenSceneBridge{}.Build(snapshot, firstOutput));
    ASSERT_EQ(firstOutput.particles.size(), 1U);
    ASSERT_EQ(firstOutput.water.size(), 1U);
    ASSERT_EQ(firstOutput.terrain.size(), 1U);

    const uint64 firstParticleId = firstOutput.particles.front().id;
    const uint64 firstWaterId = firstOutput.water.front().id;
    const uint64 firstTerrainId = firstOutput.terrain.front().id;
    const AssetId particleAssetId = firstOutput.particles.front().state.systemAssetId;
    const AssetId waterSurfaceAssetId = firstOutput.water.front().state.surfaceAssetId;
    const AssetId waterMaterialAssetId = firstOutput.water.front().state.materialAssetId;
    const AssetId terrainHeightmapAssetId =
        firstOutput.terrain.front().state.heightmapAssetId;
    const AssetId terrainMaterialAssetId =
        firstOutput.terrain.front().state.materialAssetId;

    snapshot.particles.front().id.entity = ECS::EntityHandle::Create(7, 99);
    snapshot.particles.front().sourceEntity = snapshot.particles.front().id.entity;
    snapshot.water.front().id.entity = ECS::EntityHandle::Create(8, 99);
    snapshot.water.front().sourceEntity = snapshot.water.front().id.entity;
    snapshot.terrain.front().id.entity = ECS::EntityHandle::Create(9, 99);
    snapshot.terrain.front().sourceEntity = snapshot.terrain.front().id.entity;

    EcsFrozenSceneBridgeOutput generationChangedOutput;
    ASSERT_TRUE(EcsFrozenSceneBridge{}.Build(snapshot, generationChangedOutput));
    ASSERT_EQ(generationChangedOutput.particles.size(), 1U);
    ASSERT_EQ(generationChangedOutput.water.size(), 1U);
    ASSERT_EQ(generationChangedOutput.terrain.size(), 1U);
    EXPECT_NE(generationChangedOutput.particles.front().id, firstParticleId);
    EXPECT_NE(generationChangedOutput.water.front().id, firstWaterId);
    EXPECT_NE(generationChangedOutput.terrain.front().id, firstTerrainId);
    EXPECT_EQ(generationChangedOutput.particles.front().state.systemId,
              generationChangedOutput.particles.front().id);
    EXPECT_EQ(generationChangedOutput.particles.front().state.systemAssetId,
              particleAssetId);
    EXPECT_EQ(generationChangedOutput.water.front().state.surfaceAssetId,
              waterSurfaceAssetId);
    EXPECT_EQ(generationChangedOutput.water.front().state.materialAssetId,
              waterMaterialAssetId);
    EXPECT_EQ(generationChangedOutput.terrain.front().state.heightmapAssetId,
              terrainHeightmapAssetId);
    EXPECT_EQ(generationChangedOutput.terrain.front().state.materialAssetId,
              terrainMaterialAssetId);
}

TEST(EcsFrozenSceneBridgeValidation, DetectsInjectedIdentityCollisionAndFailsClosed)
{
    FrozenSceneSnapshot snapshot;
    snapshot.sceneRuntimeId = ECS::SceneRuntimeId{401};
    snapshot.revision = 44;
    FrozenSceneMesh first = MakeMesh(
        snapshot.sceneRuntimeId, ECS::EntityHandle::Create(1, 1), 1);
    FrozenSceneMesh second = MakeMesh(
        snapshot.sceneRuntimeId, ECS::EntityHandle::Create(2, 1), 2);
    first.materialSlots.clear();
    second.materialSlots.clear();
    first.materialSlotsSource.count = 0;
    second.materialSlotsSource.count = 0;
    first.source.submeshCount = 0;
    second.source.submeshCount = 0;
    snapshot.meshes = {first, second};

    EcsFrozenSceneBridgeOutput output;
    EcsFrozenSceneBridgeResult result;
    EcsFrozenSceneBridge bridge(&ConstantRenderId);
    EXPECT_FALSE(bridge.Build(snapshot, output, &result));
    EXPECT_EQ(result.code, EcsFrozenSceneBridgeCode::RenderIdCollision);
    EXPECT_FALSE(output.renderProxies.metadata.complete);
    EXPECT_EQ(output.renderProxies.metadata.status,
              RenderProxySnapshotStatus::Incomplete);
    EXPECT_EQ(output.renderProxies.metadata.sequence, snapshot.revision);
    EXPECT_TRUE(output.renderProxies.primitives.empty());
    EXPECT_TRUE(output.renderProxies.lights.empty());
    EXPECT_TRUE(output.materialBindings.empty());
    EXPECT_TRUE(output.primitiveTemporalValues.empty());
    EXPECT_TRUE(output.lightValues.empty());
    EXPECT_TRUE(output.cameras.empty());
    EXPECT_TRUE(output.skyboxes.empty());
    EXPECT_FALSE(output.selectedCameraId.has_value());
    EXPECT_FALSE(output.sceneRuntimeId.IsValid());
}

TEST(EcsFrozenSceneBridgeValidation, RejectsSubmeshMaterialSlotMismatch)
{
    FrozenSceneSnapshot snapshot = MakeCompleteSnapshot();
    snapshot.meshes.front().source.submeshCount = 3;

    EcsFrozenSceneBridgeOutput output;
    EcsFrozenSceneBridgeResult result;
    EXPECT_FALSE(EcsFrozenSceneBridge{}.Build(snapshot, output, &result));
    EXPECT_EQ(result.code,
              EcsFrozenSceneBridgeCode::SubmeshMaterialSlotMismatch);
    EXPECT_FALSE(output.renderProxies.metadata.complete);
    EXPECT_TRUE(output.renderProxies.primitives.empty());
    EXPECT_TRUE(output.materialBindings.empty());
}

TEST(EcsFrozenSceneBridgeValidation,
     HiddenMeshesRemainAuthoritativeButAreOmittedFromRenderPayload)
{
    FrozenSceneSnapshot snapshot = MakeCompleteSnapshot();
    ASSERT_EQ(snapshot.meshes.size(), 1u);
    snapshot.meshes.front().visibility.visible = false;

    EcsFrozenSceneBridgeOutput output;
    EcsFrozenSceneBridgeResult result;
    ASSERT_TRUE(EcsFrozenSceneBridge{}.Build(snapshot, output, &result));
    EXPECT_EQ(result.code, EcsFrozenSceneBridgeCode::Complete);
    EXPECT_TRUE(output.renderProxies.primitives.empty());
    EXPECT_TRUE(output.materialBindings.empty());
    EXPECT_TRUE(output.primitiveTemporalValues.empty());
    EXPECT_EQ(result.primitiveCount, 0u);

    // The Scene snapshot is still the immutable authority and can publish the
    // same entity later without reconstructing its ECS fragments.
    ASSERT_EQ(snapshot.meshes.front().source.meshAssetId.value, 101u);
    ASSERT_EQ(snapshot.meshes.front().materialSlots.size(), 2u);
}

TEST(EcsFrozenSceneBridgeValidation, RejectsTruncatedMaterialSlotsAndMultipleSkyboxes)
{
    FrozenSceneSnapshot truncated = MakeCompleteSnapshot();
    truncated.meshes.front().materialSlotsSource.count =
        MaterialSlots::MaxSlotCount + 1u;

    EcsFrozenSceneBridgeOutput output;
    EcsFrozenSceneBridgeResult result;
    EXPECT_FALSE(EcsFrozenSceneBridge{}.Build(truncated, output, &result));
    EXPECT_EQ(result.code, EcsFrozenSceneBridgeCode::InvalidMaterialSlotCount);
    EXPECT_FALSE(output.renderProxies.metadata.complete);

    FrozenSceneSnapshot multipleSkyboxes = MakeCompleteSnapshot();
    FrozenSceneSkybox secondSkybox = multipleSkyboxes.skyboxes.front();
    secondSkybox.id.entity = ECS::EntityHandle::Create(7, 10);
    multipleSkyboxes.skyboxes.push_back(secondSkybox);
    EXPECT_FALSE(EcsFrozenSceneBridge{}.Build(multipleSkyboxes, output, &result));
    EXPECT_EQ(result.code, EcsFrozenSceneBridgeCode::MultipleSkyboxes);
    EXPECT_FALSE(output.renderProxies.metadata.complete);
}

TEST(EcsFrozenSceneBridgeValidation, SnapshotBuilderExcludesDisabledEntitiesAndBridgeOwnsValues)
{
    ECS::Registry registry;
    const ECS::EntityHandle enabled = registry.CreateEntity();
    const ECS::EntityHandle disabled = registry.CreateEntity();
    AddMeshSource(registry, enabled, 501);
    AddMeshSource(registry, disabled, 502);
    ASSERT_TRUE(registry.Disable(disabled));

    SceneSnapshotBuilder builder;
    FrozenSceneSnapshot snapshot = builder.Build(registry);
    ASSERT_EQ(snapshot.meshes.size(), 1u);
    ASSERT_EQ(snapshot.meshes.front().source.meshAssetId.value, 501u);

    EcsFrozenSceneBridgeOutput output;
    ASSERT_TRUE(EcsFrozenSceneBridge{}.Build(snapshot, output));
    ASSERT_EQ(output.renderProxies.primitives.size(), 1u);
    ASSERT_EQ(output.materialBindings.size(), 1u);
    const uint64 outputMeshAssetId =
        output.renderProxies.primitives.front().meshAssetId.value;
    const uint64 outputMaterialAssetId =
        output.materialBindings.front().materialAssetId.value;
    const float32 outputTransformX =
        output.renderProxies.primitives.front().worldMatrix[3].x;

    snapshot.meshes.front().source.meshAssetId.value = 999;
    snapshot.meshes.front().materialSlots.front().source.materialAssetId.value = 998;
    snapshot.meshes.front().worldTransform[3].x = 997.0f;

    EXPECT_EQ(output.renderProxies.primitives.front().meshAssetId.value,
              outputMeshAssetId);
    EXPECT_EQ(output.materialBindings.front().materialAssetId.value,
              outputMaterialAssetId);
    EXPECT_FLOAT_EQ(output.renderProxies.primitives.front().worldMatrix[3].x,
                    outputTransformX);
}
