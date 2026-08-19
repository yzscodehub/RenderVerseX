/**
 * @file EcsFrozenSceneBridge.cpp
 * @brief Pure ECS frozen-scene to render-source value adapter implementation.
 */

#include "RenderExtraction/EcsFrozenSceneBridge.h"

#include "Core/Math/AABB.h"

#include <algorithm>
#include <cstddef>
#include <unordered_map>

namespace RVX
{
namespace
{
    constexpr uint64 Fnv1aOffsetBasis = 14695981039346656037ull;
    constexpr uint64 Fnv1aPrime = 1099511628211ull;

    void MixByte(uint64& value, uint8 byte) noexcept
    {
        value ^= byte;
        value *= Fnv1aPrime;
    }

    template<typename T>
    void MixLittleEndian(uint64& value, T source) noexcept
    {
        uint64 normalized = static_cast<uint64>(source);
        for (uint32 byte = 0; byte < sizeof(T); ++byte)
        {
            MixByte(value, static_cast<uint8>(normalized & 0xffu));
            normalized >>= 8u;
        }
    }

    [[nodiscard]] bool MatchesSnapshotIdentity(
        const SceneECS::FrozenSceneObjectId& id,
        ECS::SceneRuntimeId sceneRuntimeId,
        SceneECS::FrozenSceneObjectType expectedType)
    {
        return id.IsValid() && id.sceneRuntimeId == sceneRuntimeId &&
               id.type == expectedType;
    }

    [[nodiscard]] RenderLightProxy::Type ToRenderLightType(
        SceneECS::LightType type)
    {
        switch (type)
        {
            case SceneECS::LightType::Directional:
                return RenderLightProxy::Type::Directional;
            case SceneECS::LightType::Point:
                return RenderLightProxy::Type::Point;
            case SceneECS::LightType::Spot:
                return RenderLightProxy::Type::Spot;
        }
        return RenderLightProxy::Type::Directional;
    }

    [[nodiscard]] RenderSkyMode ToRenderSkyMode(SceneECS::SkyboxMode mode)
    {
        switch (mode)
        {
            case SceneECS::SkyboxMode::Cubemap:
                return RenderSkyMode::Cubemap;
            case SceneECS::SkyboxMode::Equirectangular:
                return RenderSkyMode::Equirectangular;
            case SceneECS::SkyboxMode::Procedural:
                return RenderSkyMode::Procedural;
            case SceneECS::SkyboxMode::SolidColor:
                return RenderSkyMode::SolidColor;
        }
        return RenderSkyMode::Disabled;
    }

    [[nodiscard]] Vec3 DirectionFromWorldTransform(
        const Mat4& transform,
        const Vec3& localDirection)
    {
        const Vec3 direction = Vec3(transform * Vec4(localDirection, 0.0f));
        const float32 lengthSquared = glm::dot(direction, direction);
        if (lengthSquared <= 0.000000000001f)
        {
            return localDirection;
        }
        return direction / glm::sqrt(lengthSquared);
    }

    [[nodiscard]] AABB MakeWorldBounds(const SceneECS::Bounds& bounds,
                                        const Mat4& worldTransform)
    {
        const Vec3 safeExtents = glm::max(glm::abs(bounds.extents), Vec3(0.0f));
        return AABB(bounds.center - safeExtents, bounds.center + safeExtents)
            .Transformed(worldTransform);
    }

    [[nodiscard]] Mat4 MakeProjection(const SceneECS::Camera& camera)
    {
        if (camera.projection == SceneECS::CameraProjection::Orthographic)
        {
            const float32 halfWidth =
                camera.orthographicHalfHeight * camera.aspectRatio;
            return glm::ortho(-halfWidth,
                              halfWidth,
                              -camera.orthographicHalfHeight,
                              camera.orthographicHalfHeight,
                              camera.nearPlane,
                              camera.farPlane);
        }
        return glm::perspective(camera.verticalFieldOfViewRadians,
                                camera.aspectRatio,
                                camera.nearPlane,
                                camera.farPlane);
    }

    void ClearForFailure(EcsFrozenSceneBridgeOutput& output,
                         uint64 snapshotRevision) noexcept
    {
        output = {};
        output.renderProxies.BeginBuild(snapshotRevision);
        output.renderProxies.MarkIncomplete();
    }

    class IdentityValidator final
    {
    public:
        IdentityValidator(EcsFrozenSceneBridgeIdDeriver idDeriver,
                          ECS::SceneRuntimeId sceneRuntimeId,
                          EcsFrozenSceneBridgeResult& result)
            : m_idDeriver(idDeriver)
            , m_sceneRuntimeId(sceneRuntimeId)
            , m_result(result)
        {
        }

        [[nodiscard]] bool Register(const SceneECS::FrozenSceneObjectId& sourceId,
                                    SceneECS::FrozenSceneObjectType expectedType,
                                    uint64& outRenderId)
        {
            if (!sourceId.IsValid())
            {
                m_result.code = EcsFrozenSceneBridgeCode::InvalidSourceIdentity;
                return false;
            }
            if (sourceId.sceneRuntimeId != m_sceneRuntimeId)
            {
                m_result.code = EcsFrozenSceneBridgeCode::SourceIdentityMismatch;
                return false;
            }
            if (sourceId.type != expectedType)
            {
                m_result.code = EcsFrozenSceneBridgeCode::UnexpectedSourceType;
                return false;
            }

            const uint64 renderId = m_idDeriver(sourceId);
            if (renderId == 0)
            {
                m_result.code = EcsFrozenSceneBridgeCode::InvalidDerivedRenderId;
                return false;
            }

            const auto [existing, inserted] =
                m_sourceByRenderId.emplace(renderId, sourceId);
            if (!inserted)
            {
                m_result.code = existing->second == sourceId
                                    ? EcsFrozenSceneBridgeCode::DuplicateSourceIdentity
                                    : EcsFrozenSceneBridgeCode::RenderIdCollision;
                return false;
            }

            outRenderId = renderId;
            return true;
        }

        [[nodiscard]] bool IsRegistered(
            const SceneECS::FrozenSceneObjectId& sourceId,
            uint64& outRenderId) const
        {
            const uint64 renderId = m_idDeriver(sourceId);
            const auto existing = m_sourceByRenderId.find(renderId);
            if (renderId == 0 || existing == m_sourceByRenderId.end() ||
                existing->second != sourceId)
            {
                return false;
            }
            outRenderId = renderId;
            return true;
        }

    private:
        EcsFrozenSceneBridgeIdDeriver m_idDeriver = nullptr;
        ECS::SceneRuntimeId m_sceneRuntimeId;
        EcsFrozenSceneBridgeResult& m_result;
        std::unordered_map<uint64, SceneECS::FrozenSceneObjectId> m_sourceByRenderId;
    };
} // namespace

uint64 EcsFrozenSceneBridge::DeriveRenderId(
    const SceneECS::FrozenSceneObjectId& id) noexcept
{
    uint64 value = Fnv1aOffsetBasis;
    MixLittleEndian(value, id.sceneRuntimeId.GetValue());
    MixLittleEndian(value, id.entity.GetIndex());
    MixLittleEndian(value, id.entity.GetGeneration());
    MixLittleEndian(value, static_cast<uint8>(id.type));
    MixLittleEndian(value, id.slot);
    return value == 0 ? 1 : value;
}

EcsFrozenSceneBridge::EcsFrozenSceneBridge(
    EcsFrozenSceneBridgeIdDeriver idDeriver) noexcept
    : m_idDeriver(idDeriver != nullptr ? idDeriver : &DeriveRenderId)
{
}

bool EcsFrozenSceneBridge::Build(
    const SceneECS::FrozenSceneSnapshot& snapshot,
    EcsFrozenSceneBridgeOutput& outOutput,
    EcsFrozenSceneBridgeResult* outResult) const
{
    EcsFrozenSceneBridgeResult result;
    ClearForFailure(outOutput, snapshot.revision);

    auto fail = [&]()
    {
        ClearForFailure(outOutput, snapshot.revision);
        if (outResult != nullptr)
        {
            *outResult = result;
        }
        return false;
    };

    if (!snapshot.sceneRuntimeId.IsValid())
    {
        result.code = EcsFrozenSceneBridgeCode::InvalidSceneRuntimeId;
        return fail();
    }
    if (snapshot.skyboxes.size() > 1)
    {
        result.code = EcsFrozenSceneBridgeCode::MultipleSkyboxes;
        return fail();
    }

    outOutput.sceneRuntimeId = snapshot.sceneRuntimeId;
    outOutput.snapshotRevision = snapshot.revision;
    outOutput.structuralJournalSequence = snapshot.structuralJournalSequence;
    outOutput.renderWorldTransformWriteVersion =
        snapshot.renderWorldTransformWriteVersion;
    outOutput.renderProxies.BeginBuild(snapshot.revision);
    outOutput.renderProxies.primitives.reserve(snapshot.meshes.size());
    outOutput.renderProxies.lights.reserve(snapshot.lights.size());
    outOutput.primitiveTemporalValues.reserve(snapshot.meshes.size());
    outOutput.lightValues.reserve(snapshot.lights.size());
    outOutput.cameras.reserve(snapshot.cameras.size());
    outOutput.skyboxes.reserve(snapshot.skyboxes.size());
    outOutput.particles.reserve(snapshot.particles.size());
    outOutput.water.reserve(snapshot.water.size());
    outOutput.terrain.reserve(snapshot.terrain.size());

    size_t materialBindingCount = 0;
    for (const SceneECS::FrozenSceneMesh& mesh : snapshot.meshes)
    {
        materialBindingCount += mesh.materialSlots.size();
    }
    outOutput.materialBindings.reserve(materialBindingCount);

    IdentityValidator identityValidator(m_idDeriver, snapshot.sceneRuntimeId, result);

    for (const SceneECS::FrozenSceneCamera& camera : snapshot.cameras)
    {
        uint64 cameraId = 0;
        if (!identityValidator.Register(camera.id,
                                        SceneECS::FrozenSceneObjectType::Camera,
                                        cameraId))
        {
            return fail();
        }

        EcsFrozenSceneCameraValue value;
        value.id = cameraId;
        value.view.viewMatrix = glm::inverse(camera.worldTransform);
        value.view.projectionMatrix = MakeProjection(camera.source);
        value.view.viewProjectionMatrix =
            value.view.projectionMatrix * value.view.viewMatrix;
        value.view.inverseViewProjectionMatrix =
            glm::inverse(value.view.viewProjectionMatrix);
        value.view.cameraPosition = Vec3(camera.worldTransform[3]);
        value.view.cameraDirection =
            DirectionFromWorldTransform(camera.worldTransform, Vec3(0.0f, 0.0f, -1.0f));
        value.view.cameraUp =
            DirectionFromWorldTransform(camera.worldTransform, Vec3(0.0f, 1.0f, 0.0f));
        value.view.nearPlane = camera.source.nearPlane;
        value.view.farPlane = camera.source.farPlane;
        value.view.exposure = camera.source.exposure;
        value.view.cullingMask = camera.source.cullingMask;
        value.normalizedViewport = camera.source.normalizedViewport;
        value.clearPolicy = camera.source.clearPolicy;
        value.clearColor = camera.source.clearColor;
        value.cullingMask = camera.source.cullingMask;
        value.priority = camera.source.priority;
        value.transformSourceRevision = camera.transformSourceRevision;
        outOutput.cameras.push_back(value);
    }

    for (const SceneECS::FrozenSceneMesh& mesh : snapshot.meshes)
    {
        const ECS::EntityHandle sourceEntity = mesh.sourceEntity.IsValid() ?
                                                    mesh.sourceEntity :
                                                    mesh.id.entity;
        if (sourceEntity != mesh.id.entity)
        {
            result.code = EcsFrozenSceneBridgeCode::SourceIdentityMismatch;
            return fail();
        }
        if (mesh.materialSlotsSource.count > SceneECS::MaterialSlots::MaxSlotCount ||
            mesh.materialSlotsSource.count != mesh.materialSlots.size())
        {
            result.code = EcsFrozenSceneBridgeCode::InvalidMaterialSlotCount;
            return fail();
        }
        if (mesh.source.submeshCount != mesh.materialSlots.size())
        {
            result.code = EcsFrozenSceneBridgeCode::SubmeshMaterialSlotMismatch;
            return fail();
        }

        // Hidden model adoption is a Scene-side residency gate. Keep the
        // authoritative Mesh/Material values in the frozen snapshot, but do
        // not materialize a RenderScene primitive until Visibility is enabled.
        // Otherwise extraction tries to resolve GPU handles for the very
        // resources whose readiness the coordinator is still waiting for,
        // preventing an upload-only frame from making progress. Omission also
        // gives an already-published primitive the ordinary retained-state
        // Remove semantics used by hidden lights below.
        if (!mesh.visibility.visible)
        {
            continue;
        }

        uint64 meshId = 0;
        if (!identityValidator.Register(mesh.id,
                                        SceneECS::FrozenSceneObjectType::Mesh,
                                        meshId))
        {
            return fail();
        }

        RenderPrimitiveProxy proxy;
        proxy.id.value = meshId;
        proxy.ownerId = meshId;
        proxy.worldMatrix = mesh.worldTransform;
        proxy.normalMatrix = glm::inverseTranspose(Mat4(Mat3(mesh.worldTransform)));
        proxy.bounds = MakeWorldBounds(mesh.bounds, mesh.worldTransform);
        proxy.meshAssetId = mesh.source.meshAssetId;
        proxy.layerMask = mesh.visibility.layerMask;
        proxy.visible = mesh.visibility.visible;
        proxy.castsShadow = mesh.visibility.castsShadow;
        proxy.receivesShadow = mesh.visibility.receivesShadow;
        proxy.materialAssetIds.reserve(mesh.materialSlots.size());
        proxy.materialModes.reserve(mesh.materialSlots.size());

        if (mesh.skinningBinding.has_value() != mesh.skinningPalette.has_value())
        {
            result.code = EcsFrozenSceneBridgeCode::InvalidSkinningPalette;
            return fail();
        }
        if (mesh.skinningBinding.has_value())
        {
            const SceneECS::SkinnedMeshBinding& binding = *mesh.skinningBinding;
            const SceneECS::FrozenSceneSkinningPalette& palette =
                *mesh.skinningPalette;
            const SceneECS::FrozenSceneObjectId expectedPaletteId =
                MakeFrozenSceneObjectId(snapshot.sceneRuntimeId,
                                        mesh.id.entity,
                                        SceneECS::FrozenSceneObjectType::SkinningPalette);
            const SkinningPaletteHash paletteHash =
                ComputeSkinningPaletteHash(palette.matrices);
            if (!binding.poseEntity.IsValid() || binding.sourceModelAssetValue == 0 ||
                binding.sourceSkinIndex < 0 || palette.id != expectedPaletteId ||
                palette.poseEntity != binding.poseEntity ||
                palette.sourceModelAssetValue != binding.sourceModelAssetValue ||
                palette.sourceSkinIndex != binding.sourceSkinIndex ||
                palette.poseSequence == 0 || palette.paletteRevision == 0 ||
                palette.paletteBoneCount == 0 ||
                palette.paletteBoneCount != palette.matrices.size() ||
                !paletteHash.IsValid() ||
                paletteHash.matrixCount != palette.paletteBoneCount)
            {
                result.code = EcsFrozenSceneBridgeCode::InvalidSkinningPalette;
                return fail();
            }

            uint64 paletteProviderId = 0;
            if (!identityValidator.Register(palette.id,
                                            SceneECS::FrozenSceneObjectType::SkinningPalette,
                                            paletteProviderId))
            {
                return fail();
            }
            proxy.skinningMatrices = palette.matrices;
            proxy.hasSkinningPaletteProvider = true;
            proxy.skinningPalette = {
                .providerComponentId = paletteProviderId,
                .sourceModelResourceId = binding.sourceModelAssetValue,
                .poseSequence = palette.poseSequence,
                .paletteHash = paletteHash.value,
                .paletteCount = palette.paletteBoneCount,
            };
        }

        for (size_t slotIndex = 0; slotIndex < mesh.materialSlots.size(); ++slotIndex)
        {
            const SceneECS::FrozenSceneMaterialSlot& material =
                mesh.materialSlots[slotIndex];
            const uint32 slot = static_cast<uint32>(slotIndex);
            uint64 materialId = 0;
            if (!identityValidator.Register(
                    material.id,
                    SceneECS::FrozenSceneObjectType::MaterialSlot,
                    materialId))
            {
                return fail();
            }
            if (material.id.entity != mesh.id.entity || material.id.slot != slot)
            {
                result.code = EcsFrozenSceneBridgeCode::SourceIdentityMismatch;
                return fail();
            }

            proxy.materialAssetIds.push_back(material.source.materialAssetId);
            proxy.materialModes.push_back(material.source.materialMode);
            outOutput.materialBindings.push_back({
                .id = materialId,
                .primitiveId = proxy.id,
                .slot = slot,
                .materialAssetId = material.source.materialAssetId,
                .materialMode = material.source.materialMode,
            });
        }
        proxy.sortKey = proxy.materialAssetIds.empty()
                            ? 0
                            : proxy.materialAssetIds.front().value;
        outOutput.renderProxies.primitives.push_back(std::move(proxy));
        outOutput.primitiveTemporalValues.push_back({
            .primitiveId = {.value = meshId},
            .ownerId = meshId,
            .sourceEntity = sourceEntity,
            .visibilityWriteVersion = mesh.visibilityWriteVersion,
            .previousWorldTransform = mesh.previousSimulationWorldTransform,
            .worldTransformSourceRevision = mesh.transformSourceRevision,
            .previousWorldTransformSourceRevision =
                mesh.previousSimulationSourceRevision,
        });
    }

    for (const SceneECS::FrozenSceneLight& light : snapshot.lights)
    {
        const ECS::EntityHandle sourceEntity = light.sourceEntity.IsValid() ?
                                                    light.sourceEntity :
                                                    light.id.entity;
        if (sourceEntity != light.id.entity)
        {
            result.code = EcsFrozenSceneBridgeCode::SourceIdentityMismatch;
            return fail();
        }
        uint64 lightId = 0;
        if (!identityValidator.Register(light.id,
                                        SceneECS::FrozenSceneObjectType::Light,
                                        lightId))
        {
            return fail();
        }

        RenderLightProxy proxy;
        proxy.id.value = lightId;
        proxy.ownerId = lightId;
        proxy.type = ToRenderLightType(light.source.type);
        proxy.position = Vec3(light.worldTransform[3]);
        proxy.direction =
            DirectionFromWorldTransform(light.worldTransform, Vec3(0.0f, 0.0f, -1.0f));
        proxy.color = light.source.color;
        proxy.intensity = light.source.intensity;
        proxy.range = light.source.range;
        proxy.innerConeAngle = light.source.innerConeRadians;
        proxy.outerConeAngle = light.source.outerConeRadians;
        proxy.castsShadow = light.source.castsShadows;

        outOutput.lightValues.push_back({
            .id = lightId,
            .sourceEntity = sourceEntity,
            .proxy = proxy,
            .layerMask = light.visibility.layerMask,
            .visible = light.visibility.visible,
            .transformSourceRevision = light.transformSourceRevision,
        });
        if (light.visibility.visible)
        {
            outOutput.renderProxies.lights.push_back(std::move(proxy));
        }
    }

    for (const SceneECS::FrozenSceneSkybox& skybox : snapshot.skyboxes)
    {
        const ECS::EntityHandle sourceEntity = skybox.sourceEntity.IsValid() ?
                                                    skybox.sourceEntity :
                                                    skybox.id.entity;
        if (sourceEntity != skybox.id.entity)
        {
            result.code = EcsFrozenSceneBridgeCode::SourceIdentityMismatch;
            return fail();
        }
        uint64 skyboxId = 0;
        if (!identityValidator.Register(skybox.id,
                                        SceneECS::FrozenSceneObjectType::Skybox,
                                        skyboxId))
        {
            return fail();
        }

        outOutput.skyboxes.push_back({
            .id = skyboxId,
            .sourceEntity = sourceEntity,
            .skyboxWriteVersion = skybox.skyboxWriteVersion,
            .mode = ToRenderSkyMode(skybox.source.mode),
            .environmentAssetId = skybox.source.environmentAssetId,
            .prefilteredEnvironmentAssetId =
                skybox.source.prefilteredEnvironmentAssetId,
            .irradianceAssetId = skybox.source.irradianceAssetId,
            .brdfLutAssetId = skybox.source.brdfLutAssetId,
            .solidColor = skybox.source.solidColor,
            .sunDirection = skybox.source.sunDirection,
            .sunColor = skybox.source.sunColor,
            .zenithColor = skybox.source.zenithColor,
            .horizonColor = skybox.source.horizonColor,
            .groundColor = skybox.source.groundColor,
            .exposure = skybox.source.exposure,
            .rotationRadians = skybox.source.rotationRadians,
            .blur = skybox.source.blur,
            .scatteringIntensity = skybox.source.scatteringIntensity,
            .contributesToLighting = skybox.source.contributesToLighting,
        });
    }

    for (const SceneECS::FrozenSceneParticle& particle : snapshot.particles)
    {
        if (!particle.sourceEntity.IsValid() ||
            particle.sourceEntity != particle.id.entity)
        {
            result.code = EcsFrozenSceneBridgeCode::SourceIdentityMismatch;
            return fail();
        }
        uint64 particleId = 0;
        if (!identityValidator.Register(particle.id,
                                        SceneECS::FrozenSceneObjectType::Particle,
                                        particleId))
        {
            return fail();
        }

        ParticleRenderSnapshotItem state = particle.state;
        // Legacy instance/system values are intentionally discarded. One
        // feature source has exactly one generation-qualified render identity.
        state.instanceId = particleId;
        state.systemId = particleId;
        outOutput.particles.push_back({
            .id = particleId,
            .sourceEntity = particle.sourceEntity,
            .payloadRevision = particle.payloadRevision,
            .state = std::move(state),
        });
    }

    for (const SceneECS::FrozenSceneWater& water : snapshot.water)
    {
        if (!water.sourceEntity.IsValid() || water.sourceEntity != water.id.entity)
        {
            result.code = EcsFrozenSceneBridgeCode::SourceIdentityMismatch;
            return fail();
        }
        uint64 waterId = 0;
        if (!identityValidator.Register(water.id,
                                        SceneECS::FrozenSceneObjectType::Water,
                                        waterId))
        {
            return fail();
        }

        WaterRenderSnapshotItem state = water.state;
        state.componentId = waterId;
        outOutput.water.push_back({
            .id = waterId,
            .sourceEntity = water.sourceEntity,
            .payloadRevision = water.payloadRevision,
            .state = std::move(state),
        });
    }

    for (const SceneECS::FrozenSceneTerrain& terrain : snapshot.terrain)
    {
        if (!terrain.sourceEntity.IsValid() ||
            terrain.sourceEntity != terrain.id.entity)
        {
            result.code = EcsFrozenSceneBridgeCode::SourceIdentityMismatch;
            return fail();
        }
        uint64 terrainId = 0;
        if (!identityValidator.Register(terrain.id,
                                        SceneECS::FrozenSceneObjectType::Terrain,
                                        terrainId))
        {
            return fail();
        }

        TerrainRenderSnapshotItem state = terrain.state;
        state.componentId = terrainId;
        outOutput.terrain.push_back({
            .id = terrainId,
            .sourceEntity = terrain.sourceEntity,
            .payloadRevision = terrain.payloadRevision,
            .state = std::move(state),
        });
    }

    if (snapshot.selectedCamera.has_value())
    {
        uint64 selectedCameraId = 0;
        if (!MatchesSnapshotIdentity(*snapshot.selectedCamera,
                                     snapshot.sceneRuntimeId,
                                     SceneECS::FrozenSceneObjectType::Camera) ||
            !identityValidator.IsRegistered(*snapshot.selectedCamera,
                                             selectedCameraId))
        {
            result.code = EcsFrozenSceneBridgeCode::SelectedCameraMissing;
            return fail();
        }
        outOutput.selectedCameraId = selectedCameraId;
    }

    outOutput.renderProxies.MarkComplete();
    result.primitiveCount =
        static_cast<uint32>(outOutput.renderProxies.primitives.size());
    result.lightCount = static_cast<uint32>(outOutput.renderProxies.lights.size());
    result.materialBindingCount =
        static_cast<uint32>(outOutput.materialBindings.size());
    result.cameraCount = static_cast<uint32>(outOutput.cameras.size());
    result.skyboxCount = static_cast<uint32>(outOutput.skyboxes.size());
    result.particleCount = static_cast<uint32>(outOutput.particles.size());
    result.waterCount = static_cast<uint32>(outOutput.water.size());
    result.terrainCount = static_cast<uint32>(outOutput.terrain.size());
    if (outResult != nullptr)
    {
        *outResult = result;
    }
    return true;
}
} // namespace RVX
