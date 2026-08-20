#include "RenderContracts/RenderSceneUpdate.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace RVX
{
namespace
{
    bool EqualMatrix(const Mat4& left, const Mat4& right) noexcept
    {
        for (uint32 column = 0; column < 4; ++column)
        {
            for (uint32 row = 0; row < 4; ++row)
            {
                if (left[column][row] != right[column][row])
                    return false;
            }
        }
        return true;
    }

    bool EqualBounds(const AABB& left, const AABB& right) noexcept
    {
        return left.GetMin() == right.GetMin() &&
               left.GetMax() == right.GetMax();
    }

    bool EqualParticleData(const ParticleRenderParticleData& left,
                           const ParticleRenderParticleData& right) noexcept
    {
        return left.position == right.position &&
               left.lifetime == right.lifetime &&
               left.velocity == right.velocity && left.age == right.age &&
               left.color == right.color && left.size == right.size &&
               left.rotation == right.rotation && left.flags == right.flags;
    }

    template<typename Mutation, typename IdAccessor>
    bool ValidateMutations(const std::vector<Mutation>& mutations,
                           IdAccessor idAccessor)
    {
        std::unordered_set<uint64> ids;
        ids.reserve(mutations.size());
        for (const Mutation& mutation : mutations)
        {
            const uint64 id = idAccessor(mutation);
            if (id == 0 || !ids.insert(id).second)
                return false;
        }
        return true;
    }

    template<typename Mutation, typename States, typename IdAccessor>
    std::vector<Mutation> BuildMutations(const States& states,
                                         IdAccessor idAccessor)
    {
        std::vector<uint64> ids;
        ids.reserve(states.size());
        for (const auto& [id, state] : states)
        {
            (void)state;
            ids.push_back(id);
        }
        std::sort(ids.begin(), ids.end());

        std::vector<Mutation> result;
        result.reserve(ids.size());
        for (uint64 id : ids)
        {
            const auto& pending = states.at(id);
            Mutation mutation;
            mutation.operation = pending.operation;
            idAccessor(mutation) = id;
            if (pending.operation == RenderSceneMutationOperation::Upsert)
                mutation.state = pending.state;
            result.push_back(std::move(mutation));
        }
        return result;
    }
}

bool AreRenderPrimitiveSnapshotsEqual(
    const RenderPrimitiveSnapshot& left,
    const RenderPrimitiveSnapshot& right) noexcept
{
    if (left.objectId != right.objectId || left.mesh != right.mesh ||
        left.material != right.material ||
        left.fallbackMesh != right.fallbackMesh ||
        left.fallbackMaterial != right.fallbackMaterial ||
        !EqualMatrix(left.worldTransform, right.worldTransform) ||
        !EqualMatrix(left.previousWorldTransform,
                     right.previousWorldTransform) ||
        left.boundsMin != right.boundsMin ||
        left.boundsMax != right.boundsMax || left.flags != right.flags ||
        left.layerMask != right.layerMask || left.sortKey != right.sortKey ||
        left.submeshes.size() != right.submeshes.size() ||
        left.skinMatrices.size() != right.skinMatrices.size() ||
        left.hasSkinningPaletteProvider != right.hasSkinningPaletteProvider ||
        left.skinningPalette.providerComponentId !=
            right.skinningPalette.providerComponentId ||
        left.skinningPalette.sourceModelResourceId !=
            right.skinningPalette.sourceModelResourceId ||
        left.skinningPalette.poseSequence != right.skinningPalette.poseSequence ||
        left.skinningPalette.paletteHash != right.skinningPalette.paletteHash ||
        left.skinningPalette.paletteCount != right.skinningPalette.paletteCount)
    {
        return false;
    }
    for (size_t index = 0; index < left.submeshes.size(); ++index)
    {
        const RenderSubmeshMaterialBinding& a = left.submeshes[index];
        const RenderSubmeshMaterialBinding& b = right.submeshes[index];
        if (a.submeshIndex != b.submeshIndex || a.material != b.material ||
            a.materialMode != b.materialMode)
        {
            return false;
        }
    }
    for (size_t index = 0; index < left.skinMatrices.size(); ++index)
    {
        if (!EqualMatrix(left.skinMatrices[index], right.skinMatrices[index]))
            return false;
    }
    return true;
}

bool AreRenderLightSnapshotsEqual(const RenderLightSnapshot& left,
                                  const RenderLightSnapshot& right) noexcept
{
    return left.lightId == right.lightId && left.type == right.type &&
           left.position == right.position &&
           left.direction == right.direction && left.color == right.color &&
           left.intensity == right.intensity && left.range == right.range &&
           left.innerConeRadians == right.innerConeRadians &&
           left.outerConeRadians == right.outerConeRadians &&
           left.shadowResource == right.shadowResource &&
           left.layerMask == right.layerMask &&
           left.castsShadows == right.castsShadows;
}

bool AreRenderDecalSnapshotsEqual(const RenderDecalSnapshot& left,
                                  const RenderDecalSnapshot& right) noexcept
{
    return left.decalId == right.decalId &&
           EqualMatrix(left.worldTransform, right.worldTransform) &&
           left.halfExtent == right.halfExtent &&
           left.material == right.material && left.color == right.color &&
           left.opacity == right.opacity &&
           left.normalStrength == right.normalStrength &&
           left.angleFade == right.angleFade &&
           left.fadeDistance == right.fadeDistance &&
           left.fadeWidth == right.fadeWidth &&
           left.layerMask == right.layerMask &&
           left.sortOrder == right.sortOrder &&
           left.blendMode == right.blendMode;
}

bool AreRenderProbeSnapshotsEqual(const RenderProbeSnapshot& left,
                                  const RenderProbeSnapshot& right) noexcept
{
    return left.probeId == right.probeId && left.kind == right.kind &&
           left.shape == right.shape && left.mode == right.mode &&
           EqualMatrix(left.worldTransform, right.worldTransform) &&
           left.influenceExtent == right.influenceExtent &&
           left.blendDistance == right.blendDistance &&
           left.boxProjectionSize == right.boxProjectionSize &&
           left.boxProjectionOffset == right.boxProjectionOffset &&
           left.texture == right.texture &&
           left.sphericalHarmonics == right.sphericalHarmonics &&
           left.cullingMask == right.cullingMask &&
           left.priority == right.priority &&
           left.nearClip == right.nearClip &&
           left.farClip == right.farClip &&
           left.useBoxProjection == right.useBoxProjection &&
           left.useHDR == right.useHDR &&
           left.hasValidData == right.hasValidData;
}

bool AreRenderSkySnapshotsEqual(const RenderSkySnapshot& left,
                                const RenderSkySnapshot& right) noexcept
{
    return left.mode == right.mode && left.skyTexture == right.skyTexture &&
           left.tint == right.tint &&
           left.sunDirection == right.sunDirection &&
           left.sunColor == right.sunColor &&
           left.zenithColor == right.zenithColor &&
           left.horizonColor == right.horizonColor &&
           left.groundColor == right.groundColor &&
           left.intensity == right.intensity &&
           left.rotationRadians == right.rotationRadians &&
           left.blurLevel == right.blurLevel &&
           left.scatteringIntensity == right.scatteringIntensity;
}

bool AreRenderEnvironmentSnapshotsEqual(
    const RenderEnvironmentSnapshot& left,
    const RenderEnvironmentSnapshot& right) noexcept
{
    return left.irradianceTexture == right.irradianceTexture &&
           left.prefilteredTexture == right.prefilteredTexture &&
           left.brdfLutTexture == right.brdfLutTexture &&
           left.intensity == right.intensity;
}

bool AreParticleRenderSnapshotItemsEqual(
    const ParticleRenderSnapshotItem& left,
    const ParticleRenderSnapshotItem& right) noexcept
{
    if (left.instanceId != right.instanceId ||
        left.systemId != right.systemId ||
        left.systemAssetId != right.systemAssetId ||
        left.systemName != right.systemName ||
        !EqualMatrix(left.worldMatrix, right.worldMatrix) ||
        !EqualBounds(left.worldBounds, right.worldBounds) ||
        left.position != right.position ||
        left.renderMode != right.renderMode ||
        left.blendMode != right.blendMode ||
        left.simulationBackend != right.simulationBackend ||
        left.payloadStatus != right.payloadStatus ||
        left.aliveParticleCount != right.aliveParticleCount ||
        left.maxParticleCount != right.maxParticleCount ||
        left.lodLevel != right.lodLevel ||
        left.normalizedTime != right.normalizedTime ||
        left.visible != right.visible ||
        left.simulationSupported != right.simulationSupported ||
        left.renderPayloadAvailable != right.renderPayloadAvailable ||
        left.sortingSupported != right.sortingSupported ||
        left.softParticlesEnabled != right.softParticlesEnabled ||
        left.softParticleFadeDistance != right.softParticleFadeDistance ||
        left.unsupportedReason != right.unsupportedReason ||
        left.renderPayloadReason != right.renderPayloadReason ||
        left.sortingReason != right.sortingReason ||
        left.particles.size() != right.particles.size())
    {
        return false;
    }
    for (size_t index = 0; index < left.particles.size(); ++index)
    {
        if (!EqualParticleData(left.particles[index],
                               right.particles[index]))
        {
            return false;
        }
    }
    return true;
}

bool AreWaterRenderSnapshotItemsEqual(
    const WaterRenderSnapshotItem& left,
    const WaterRenderSnapshotItem& right) noexcept
{
    return left.componentId == right.componentId &&
           left.surfaceAssetId == right.surfaceAssetId &&
           left.materialAssetId == right.materialAssetId &&
           left.worldPosition == right.worldPosition &&
           EqualBounds(left.worldBounds, right.worldBounds) &&
           left.size == right.size && left.depth == right.depth &&
           left.resolution == right.resolution &&
           left.surfaceType == right.surfaceType &&
           left.simulationType == right.simulationType &&
           left.shallowColor == right.shallowColor &&
           left.deepColor == right.deepColor &&
           left.foamColor == right.foamColor &&
           left.transparency == right.transparency &&
           left.reflectionStrength == right.reflectionStrength &&
           left.refractionStrength == right.refractionStrength &&
           left.roughness == right.roughness &&
           left.foamIntensity == right.foamIntensity &&
           left.reflectionEnabled == right.reflectionEnabled &&
           left.refractionEnabled == right.refractionEnabled &&
           left.causticsEnabled == right.causticsEnabled &&
           left.underwaterEffectsEnabled == right.underwaterEffectsEnabled &&
           left.foamEnabled == right.foamEnabled &&
           left.gpuInitialized == right.gpuInitialized &&
           left.cpuSimulationAvailable == right.cpuSimulationAvailable &&
           left.renderGpuPathAvailable == right.renderGpuPathAvailable &&
           left.gpuInitializationReason == right.gpuInitializationReason &&
           left.simulationFallbackReason ==
               right.simulationFallbackReason &&
           left.renderPathReason == right.renderPathReason;
}

bool AreTerrainRenderSnapshotItemsEqual(
    const TerrainRenderSnapshotItem& left,
    const TerrainRenderSnapshotItem& right) noexcept
{
    return left.componentId == right.componentId &&
           left.heightmapAssetId == right.heightmapAssetId &&
           left.materialAssetId == right.materialAssetId &&
           left.worldPosition == right.worldPosition &&
           EqualBounds(left.worldBounds, right.worldBounds) &&
           left.size == right.size && left.lodBias == right.lodBias &&
           left.patchSize == right.patchSize &&
           left.maxLODLevels == right.maxLODLevels &&
           left.castsShadow == right.castsShadow &&
           left.receivesShadow == right.receivesShadow &&
           left.collisionEnabled == right.collisionEnabled &&
           left.hasHeightmap == right.hasHeightmap &&
           left.heightmapValid == right.heightmapValid &&
           left.hasMaterial == right.hasMaterial &&
           left.gpuInitialized == right.gpuInitialized &&
           left.cpuDataAvailable == right.cpuDataAvailable &&
           left.renderGpuPathAvailable == right.renderGpuPathAvailable &&
           left.renderPathReason == right.renderPathReason &&
           left.heightmapDiagnostic == right.heightmapDiagnostic &&
           left.materialDiagnostic == right.materialDiagnostic &&
           left.lodDiagnostic == right.lodDiagnostic;
}

bool RenderSceneUpdateBatch::IsStructurallyValid() const noexcept
{
    if (schemaId != RVX_RENDER_SCENE_UPDATE_SCHEMA_ID ||
        schemaVersion != RVX_RENDER_SCENE_UPDATE_SCHEMA_VERSION ||
        targetSceneRevision == 0 || targetSceneRevision <= baseSceneRevision ||
        (fullReset && baseSceneRevision != 0))
    {
        return false;
    }

    const bool uniquePrimitives = ValidateMutations(primitives, [](const auto& item) {
        return item.objectId;
    });
    const bool uniqueLights = ValidateMutations(lights, [](const auto& item) {
        return item.lightId;
    });
    const bool uniqueDecals = ValidateMutations(decals, [](const auto& item) {
        return item.decalId;
    });
    const bool uniqueProbes = ValidateMutations(probes, [](const auto& item) {
        return item.probeId;
    });
    const bool uniqueParticles = ValidateMutations(particles, [](const auto& item) {
        return item.instanceId;
    });
    const bool uniqueWater = ValidateMutations(water, [](const auto& item) {
        return item.componentId;
    });
    const bool uniqueTerrain = ValidateMutations(terrain, [](const auto& item) {
        return item.componentId;
    });
    if (!uniquePrimitives || !uniqueLights || !uniqueDecals ||
        !uniqueProbes || !uniqueParticles || !uniqueWater || !uniqueTerrain)
    {
        return false;
    }

    for (const auto& mutation : primitives)
    {
        if (mutation.operation == RenderSceneMutationOperation::Upsert &&
            mutation.state.objectId != mutation.objectId)
            return false;
    }
    for (const auto& mutation : lights)
    {
        if (mutation.operation == RenderSceneMutationOperation::Upsert &&
            mutation.state.lightId != mutation.lightId)
            return false;
    }
    for (const auto& mutation : decals)
    {
        if (mutation.operation == RenderSceneMutationOperation::Upsert &&
            mutation.state.decalId != mutation.decalId)
            return false;
    }
    for (const auto& mutation : probes)
    {
        if (mutation.operation == RenderSceneMutationOperation::Upsert &&
            mutation.state.probeId != mutation.probeId)
            return false;
    }
    for (const auto& mutation : particles)
    {
        if (mutation.operation == RenderSceneMutationOperation::Upsert &&
            (mutation.state.instanceId != mutation.instanceId ||
             mutation.state.systemId != mutation.instanceId))
            return false;
    }
    for (const auto& mutation : water)
    {
        if (mutation.operation == RenderSceneMutationOperation::Upsert &&
            mutation.state.componentId != mutation.componentId)
            return false;
    }
    for (const auto& mutation : terrain)
    {
        if (mutation.operation == RenderSceneMutationOperation::Upsert &&
            mutation.state.componentId != mutation.componentId)
            return false;
    }
    return true;
}

bool RenderSceneUpdateBatch::Empty() const noexcept
{
    return primitives.empty() && lights.empty() && decals.empty() &&
           probes.empty() && !sky.has_value() && !environment.has_value() &&
           particles.empty() && water.empty() && terrain.empty();
}

void RenderSceneMutationAccumulator::Begin(uint64 baseSceneRevision, bool fullReset)
{
    Clear();
    m_baseSceneRevision = baseSceneRevision;
    m_fullReset = fullReset;
}

void RenderSceneMutationAccumulator::Clear()
{
    m_baseSceneRevision = 0;
    m_fullReset = false;
    m_primitives.clear();
    m_lights.clear();
    m_decals.clear();
    m_probes.clear();
    m_particles.clear();
    m_water.clear();
    m_terrain.clear();
    m_sky.reset();
    m_environment.reset();
}

template<typename State>
bool RenderSceneMutationAccumulator::Upsert(
    std::unordered_map<uint64, PendingState<State>>& states,
    uint64 id,
    State state,
    bool newlyCreated)
{
    if (id == 0)
        return false;
    auto it = states.find(id);
    if (it != states.end() &&
        it->second.operation == RenderSceneMutationOperation::Remove)
    {
        return false;
    }
    PendingState<State>& pending = states[id];
    pending.operation = RenderSceneMutationOperation::Upsert;
    pending.state = std::move(state);
    pending.newlyCreated = pending.newlyCreated || newlyCreated || m_fullReset;
    return true;
}

template<typename State>
bool RenderSceneMutationAccumulator::Remove(
    std::unordered_map<uint64, PendingState<State>>& states,
    uint64 id)
{
    if (id == 0)
        return false;
    auto it = states.find(id);
    if (it == states.end())
    {
        if (!m_fullReset)
        {
            PendingState<State> pending;
            pending.operation = RenderSceneMutationOperation::Remove;
            states.emplace(id, std::move(pending));
        }
        return true;
    }
    if (it->second.operation == RenderSceneMutationOperation::Upsert &&
        it->second.newlyCreated)
    {
        states.erase(it);
        return true;
    }
    it->second.operation = RenderSceneMutationOperation::Remove;
    it->second.state = {};
    it->second.newlyCreated = false;
    return true;
}

bool RenderSceneMutationAccumulator::UpsertPrimitive(RenderPrimitiveSnapshot state,
                                                     bool newlyCreated)
{
    return Upsert(m_primitives, state.objectId, std::move(state), newlyCreated);
}
bool RenderSceneMutationAccumulator::RemovePrimitive(uint64 id) { return Remove(m_primitives, id); }
bool RenderSceneMutationAccumulator::UpsertLight(RenderLightSnapshot state, bool created)
{
    return Upsert(m_lights, state.lightId, std::move(state), created);
}
bool RenderSceneMutationAccumulator::RemoveLight(uint64 id) { return Remove(m_lights, id); }
bool RenderSceneMutationAccumulator::UpsertDecal(RenderDecalSnapshot state,
                                                 bool created)
{
    return Upsert(m_decals, state.decalId, std::move(state), created);
}
bool RenderSceneMutationAccumulator::RemoveDecal(uint64 id)
{
    return Remove(m_decals, id);
}
bool RenderSceneMutationAccumulator::UpsertProbe(RenderProbeSnapshot state,
                                                 bool created)
{
    return Upsert(m_probes, state.probeId, std::move(state), created);
}
bool RenderSceneMutationAccumulator::RemoveProbe(uint64 id)
{
    return Remove(m_probes, id);
}
bool RenderSceneMutationAccumulator::UpsertParticle(ParticleRenderSnapshotItem state, bool created)
{
    return Upsert(m_particles, state.instanceId, std::move(state), created);
}
bool RenderSceneMutationAccumulator::RemoveParticle(uint64 id) { return Remove(m_particles, id); }
bool RenderSceneMutationAccumulator::UpsertWater(WaterRenderSnapshotItem state, bool created)
{
    return Upsert(m_water, state.componentId, std::move(state), created);
}
bool RenderSceneMutationAccumulator::RemoveWater(uint64 id) { return Remove(m_water, id); }
bool RenderSceneMutationAccumulator::UpsertTerrain(TerrainRenderSnapshotItem state, bool created)
{
    return Upsert(m_terrain, state.componentId, std::move(state), created);
}
bool RenderSceneMutationAccumulator::RemoveTerrain(uint64 id) { return Remove(m_terrain, id); }

void RenderSceneMutationAccumulator::UpsertSky(RenderSkySnapshot state)
{
    m_sky = RenderSingletonMutation<RenderSkySnapshot>{
        RenderSceneMutationOperation::Upsert, std::move(state)};
}
void RenderSceneMutationAccumulator::RemoveSky()
{
    if (m_fullReset) m_sky.reset();
    else m_sky = RenderSingletonMutation<RenderSkySnapshot>{RenderSceneMutationOperation::Remove, {}};
}
void RenderSceneMutationAccumulator::UpsertEnvironment(RenderEnvironmentSnapshot state)
{
    m_environment = RenderSingletonMutation<RenderEnvironmentSnapshot>{
        RenderSceneMutationOperation::Upsert, std::move(state)};
}
void RenderSceneMutationAccumulator::RemoveEnvironment()
{
    if (m_fullReset) m_environment.reset();
    else m_environment = RenderSingletonMutation<RenderEnvironmentSnapshot>{RenderSceneMutationOperation::Remove, {}};
}

RenderSceneUpdateBatch RenderSceneMutationAccumulator::Build(uint64 targetSceneRevision) const
{
    RenderSceneUpdateBatch batch;
    batch.baseSceneRevision = m_fullReset ? 0 : m_baseSceneRevision;
    batch.targetSceneRevision = targetSceneRevision;
    batch.fullReset = m_fullReset;
    batch.primitives = BuildMutations<RenderPrimitiveMutation>(
        m_primitives, [](auto& item) -> uint64& { return item.objectId; });
    batch.lights = BuildMutations<RenderLightMutation>(
        m_lights, [](auto& item) -> uint64& { return item.lightId; });
    batch.decals = BuildMutations<RenderDecalMutation>(
        m_decals, [](auto& item) -> uint64& { return item.decalId; });
    batch.probes = BuildMutations<RenderProbeMutation>(
        m_probes, [](auto& item) -> uint64& { return item.probeId; });
    batch.particles = BuildMutations<RenderParticleMutation>(
        m_particles, [](auto& item) -> uint64& { return item.instanceId; });
    batch.water = BuildMutations<RenderWaterMutation>(
        m_water, [](auto& item) -> uint64& { return item.componentId; });
    batch.terrain = BuildMutations<RenderTerrainMutation>(
        m_terrain, [](auto& item) -> uint64& { return item.componentId; });
    batch.sky = m_sky;
    batch.environment = m_environment;
    return batch;
}

} // namespace RVX
