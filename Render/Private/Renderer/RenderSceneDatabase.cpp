#include "Render/Renderer/RenderSceneDatabase.h"

#include <algorithm>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace RVX
{
namespace
{
    template<typename Mutation, typename State>
    void ApplyMutations(const std::vector<Mutation>& mutations,
                        std::unordered_map<uint64, State>& values,
                        uint64 Mutation::* idMember)
    {
        for (const Mutation& mutation : mutations)
        {
            const uint64 id = mutation.*idMember;
            if (mutation.operation == RenderSceneMutationOperation::Remove)
            {
                values.erase(id);
            }
            else
            {
                values.insert_or_assign(id, mutation.state);
            }
        }
    }

    [[nodiscard]] uint32 GetHandleSlot(uint64 id) noexcept
    {
        return static_cast<uint32>(id & 0xFFFFFFFFull);
    }

    [[nodiscard]] uint32 GetHandleGeneration(uint64 id) noexcept
    {
        return static_cast<uint32>(id >> 32u);
    }

    /**
     * Render primitive/light IDs are packed generation-safe ComponentHandles.
     * Validate the entire replacement transaction before applying any value so
     * an old producer cannot resurrect a recycled component slot.
     */
    template<typename Mutation, typename State>
    bool ValidateGenerationSafeMutations(
        const std::vector<Mutation>& mutations,
        const std::unordered_map<uint64, State>& current,
        const std::unordered_map<uint32, uint32>& acceptedGenerations,
        uint64 Mutation::* idMember,
        bool fullReset)
    {
        std::unordered_map<uint32, uint64> currentBySlot;
        currentBySlot.reserve(current.size());
        for (const auto& [id, state] : current)
        {
            (void)state;
            const uint32 slot = GetHandleSlot(id);
            if (slot == 0 || !currentBySlot.emplace(slot, id).second)
                return false;
        }

        std::unordered_set<uint64> removals;
        std::unordered_map<uint32, uint64> upsertsBySlot;
        removals.reserve(mutations.size());
        upsertsBySlot.reserve(mutations.size());
        for (const Mutation& mutation : mutations)
        {
            const uint64 id = mutation.*idMember;
            const uint32 slot = GetHandleSlot(id);
            if (slot == 0)
                return false;

            if (mutation.operation == RenderSceneMutationOperation::Remove)
            {
                if (fullReset || !current.contains(id) ||
                    !removals.insert(id).second)
                {
                    return false;
                }
                continue;
            }

            const auto [upsert, inserted] = upsertsBySlot.emplace(slot, id);
            if (!inserted && upsert->second != id)
                return false;
        }

        if (fullReset)
            return true;

        for (const auto& [slot, id] : upsertsBySlot)
        {
            const auto existing = currentBySlot.find(slot);
            if (existing != currentBySlot.end() && existing->second == id)
                continue;

            if (existing == currentBySlot.end())
            {
                const auto accepted = acceptedGenerations.find(slot);
                if (accepted != acceptedGenerations.end() &&
                    GetHandleGeneration(id) <= accepted->second)
                {
                    return false;
                }
                continue;
            }

            if (GetHandleGeneration(id) <=
                    GetHandleGeneration(existing->second) ||
                !removals.contains(existing->second))
            {
                return false;
            }
        }
        return true;
    }

    template<typename Mutation>
    void AdvanceAcceptedGenerations(
        const std::vector<Mutation>& mutations,
        std::unordered_map<uint32, uint32>& generations,
        uint64 Mutation::* idMember,
        bool fullReset)
    {
        if (fullReset)
            generations.clear();
        for (const Mutation& mutation : mutations)
        {
            const uint64 id = mutation.*idMember;
            const uint32 slot = GetHandleSlot(id);
            const uint32 generation = GetHandleGeneration(id);
            auto [it, inserted] = generations.emplace(slot, generation);
            if (!inserted)
                it->second = std::max(it->second, generation);
        }
    }

    template<typename State>
    void ApplySingleton(const std::optional<RenderSingletonMutation<State>>& mutation,
                        std::optional<State>& value)
    {
        if (!mutation.has_value())
            return;
        if (mutation->operation == RenderSceneMutationOperation::Remove)
            value.reset();
        else
            value = mutation->state;
    }

    bool EqualMatrix(const Mat4& left, const Mat4& right)
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

    bool EqualPrimitive(const RenderPrimitiveSnapshot& left,
                        const RenderPrimitiveSnapshot& right)
    {
        if (left.objectId != right.objectId || left.mesh != right.mesh ||
            left.material != right.material ||
            left.fallbackMesh != right.fallbackMesh ||
            left.fallbackMaterial != right.fallbackMaterial ||
            !EqualMatrix(left.worldTransform, right.worldTransform) ||
            !EqualMatrix(left.previousWorldTransform, right.previousWorldTransform) ||
            left.boundsMin != right.boundsMin || left.boundsMax != right.boundsMax ||
            left.flags != right.flags || left.layerMask != right.layerMask ||
            left.sortKey != right.sortKey ||
            left.submeshes.size() != right.submeshes.size() ||
            left.skinMatrices.size() != right.skinMatrices.size())
        {
            return false;
        }
        for (size_t index = 0; index < left.submeshes.size(); ++index)
        {
            const auto& a = left.submeshes[index];
            const auto& b = right.submeshes[index];
            if (a.submeshIndex != b.submeshIndex || a.material != b.material ||
                a.materialMode != b.materialMode)
                return false;
        }
        for (size_t index = 0; index < left.skinMatrices.size(); ++index)
        {
            if (!EqualMatrix(left.skinMatrices[index], right.skinMatrices[index]))
                return false;
        }
        return true;
    }

    bool EqualLight(const RenderLightSnapshot& left,
                    const RenderLightSnapshot& right)
    {
        return left.lightId == right.lightId && left.type == right.type &&
               left.position == right.position && left.direction == right.direction &&
               left.color == right.color && left.intensity == right.intensity &&
               left.range == right.range &&
               left.innerConeRadians == right.innerConeRadians &&
               left.outerConeRadians == right.outerConeRadians &&
               left.shadowResource == right.shadowResource &&
               left.castsShadows == right.castsShadows;
    }

    bool EqualSky(const RenderSkySnapshot& left, const RenderSkySnapshot& right)
    {
        return left.mode == right.mode && left.skyTexture == right.skyTexture &&
               left.tint == right.tint && left.sunDirection == right.sunDirection &&
               left.sunColor == right.sunColor &&
               left.zenithColor == right.zenithColor &&
               left.horizonColor == right.horizonColor &&
               left.groundColor == right.groundColor &&
               left.intensity == right.intensity &&
               left.rotationRadians == right.rotationRadians &&
               left.blurLevel == right.blurLevel &&
               left.scatteringIntensity == right.scatteringIntensity;
    }

    bool EqualEnvironment(const RenderEnvironmentSnapshot& left,
                          const RenderEnvironmentSnapshot& right)
    {
        return left.irradianceTexture == right.irradianceTexture &&
               left.prefilteredTexture == right.prefilteredTexture &&
               left.brdfLutTexture == right.brdfLutTexture &&
               left.intensity == right.intensity;
    }
} // namespace

RenderSceneUpdateApplyResult RenderSceneDatabase::Apply(
    const RenderSceneUpdateBatch& batch)
{
    RenderSceneUpdateApplyResult result;
    result.previousRevision = m_revision;
    result.acceptedRevision = m_revision;
    result.fullReset = batch.fullReset;

    if (batch.schemaId != RVX_RENDER_SCENE_UPDATE_SCHEMA_ID ||
        batch.schemaVersion != RVX_RENDER_SCENE_UPDATE_SCHEMA_VERSION)
    {
        result.code = RenderSceneUpdateApplyCode::UnsupportedSchema;
        return result;
    }
    if (!batch.IsStructurallyValid())
    {
        result.code = RenderSceneUpdateApplyCode::InvalidMutation;
        return result;
    }
    if (batch.targetSceneRevision <= m_revision)
    {
        result.code = RenderSceneUpdateApplyCode::OutOfOrder;
        return result;
    }
    if (!batch.fullReset && batch.baseSceneRevision != m_revision)
    {
        result.code = RenderSceneUpdateApplyCode::RevisionGap;
        return result;
    }
    if (!ValidateGenerationSafeMutations(
            batch.primitives,
            m_primitives,
            m_primitiveGenerations,
            &RenderPrimitiveMutation::objectId,
            batch.fullReset) ||
        !ValidateGenerationSafeMutations(
            batch.lights,
            m_lights,
            m_lightGenerations,
            &RenderLightMutation::lightId,
            batch.fullReset) ||
        !ValidateGenerationSafeMutations(
            batch.decals,
            m_decals,
            m_decalGenerations,
            &RenderDecalMutation::decalId,
            batch.fullReset) ||
        !ValidateGenerationSafeMutations(
            batch.probes,
            m_probes,
            m_probeGenerations,
            &RenderProbeMutation::probeId,
            batch.fullReset) ||
        !ValidateGenerationSafeMutations(
            batch.water,
            m_water,
            m_waterGenerations,
            &RenderWaterMutation::componentId,
            batch.fullReset) ||
        !ValidateGenerationSafeMutations(
            batch.terrain,
            m_terrain,
            m_terrainGenerations,
            &RenderTerrainMutation::componentId,
            batch.fullReset))
    {
        result.code = RenderSceneUpdateApplyCode::InvalidMutation;
        return result;
    }

    auto primitives = batch.fullReset ? decltype(m_primitives){} : m_primitives;
    auto lights = batch.fullReset ? decltype(m_lights){} : m_lights;
    auto decals = batch.fullReset ? decltype(m_decals){} : m_decals;
    auto probes = batch.fullReset ? decltype(m_probes){} : m_probes;
    auto primitiveGenerations = m_primitiveGenerations;
    auto lightGenerations = m_lightGenerations;
    auto decalGenerations = m_decalGenerations;
    auto probeGenerations = m_probeGenerations;
    auto waterGenerations = m_waterGenerations;
    auto terrainGenerations = m_terrainGenerations;
    auto particles = batch.fullReset ? decltype(m_particles){} : m_particles;
    auto water = batch.fullReset ? decltype(m_water){} : m_water;
    auto terrain = batch.fullReset ? decltype(m_terrain){} : m_terrain;
    auto sky = batch.fullReset ? decltype(m_sky){} : m_sky;
    auto environment = batch.fullReset ? decltype(m_environment){} : m_environment;

    ApplyMutations(batch.primitives, primitives, &RenderPrimitiveMutation::objectId);
    ApplyMutations(batch.lights, lights, &RenderLightMutation::lightId);
    ApplyMutations(batch.decals, decals, &RenderDecalMutation::decalId);
    ApplyMutations(batch.probes, probes, &RenderProbeMutation::probeId);
    AdvanceAcceptedGenerations(batch.primitives,
                               primitiveGenerations,
                               &RenderPrimitiveMutation::objectId,
                               batch.fullReset);
    AdvanceAcceptedGenerations(batch.lights,
                               lightGenerations,
                               &RenderLightMutation::lightId,
                               batch.fullReset);
    AdvanceAcceptedGenerations(batch.decals,
                               decalGenerations,
                               &RenderDecalMutation::decalId,
                               batch.fullReset);
    AdvanceAcceptedGenerations(batch.probes,
                               probeGenerations,
                               &RenderProbeMutation::probeId,
                               batch.fullReset);
    ApplyMutations(batch.particles, particles, &RenderParticleMutation::instanceId);
    ApplyMutations(batch.water, water, &RenderWaterMutation::componentId);
    ApplyMutations(batch.terrain, terrain, &RenderTerrainMutation::componentId);
    AdvanceAcceptedGenerations(batch.water,
                               waterGenerations,
                               &RenderWaterMutation::componentId,
                               batch.fullReset);
    AdvanceAcceptedGenerations(batch.terrain,
                               terrainGenerations,
                               &RenderTerrainMutation::componentId,
                               batch.fullReset);
    ApplySingleton(batch.sky, sky);
    ApplySingleton(batch.environment, environment);

    m_primitives.swap(primitives);
    m_lights.swap(lights);
    m_decals.swap(decals);
    m_probes.swap(probes);
    m_primitiveGenerations.swap(primitiveGenerations);
    m_lightGenerations.swap(lightGenerations);
    m_decalGenerations.swap(decalGenerations);
    m_probeGenerations.swap(probeGenerations);
    m_particles.swap(particles);
    m_water.swap(water);
    m_terrain.swap(terrain);
    m_waterGenerations.swap(waterGenerations);
    m_terrainGenerations.swap(terrainGenerations);
    m_sky.swap(sky);
    m_environment.swap(environment);
    m_revision = batch.targetSceneRevision;

    result.code = RenderSceneUpdateApplyCode::Applied;
    result.acceptedRevision = m_revision;
    result.sceneMutated = true;
    return result;
}

void RenderSceneDatabase::Clear()
{
    m_revision = 0;
    m_primitives.clear();
    m_lights.clear();
    m_decals.clear();
    m_probes.clear();
    m_primitiveGenerations.clear();
    m_lightGenerations.clear();
    m_decalGenerations.clear();
    m_probeGenerations.clear();
    m_particles.clear();
    m_water.clear();
    m_terrain.clear();
    m_waterGenerations.clear();
    m_terrainGenerations.clear();
    m_sky.reset();
    m_environment.reset();
}

const RenderPrimitiveSnapshot* RenderSceneDatabase::FindPrimitive(
    uint64 objectId) const noexcept
{
    const auto found = m_primitives.find(objectId);
    return found != m_primitives.end() ? &found->second : nullptr;
}

const RenderLightSnapshot* RenderSceneDatabase::FindLight(uint64 lightId) const noexcept
{
    const auto found = m_lights.find(lightId);
    return found != m_lights.end() ? &found->second : nullptr;
}

const RenderDecalSnapshot* RenderSceneDatabase::FindDecal(
    uint64 decalId) const noexcept
{
    const auto found = m_decals.find(decalId);
    return found != m_decals.end() ? &found->second : nullptr;
}

const RenderProbeSnapshot* RenderSceneDatabase::FindProbe(
    uint64 probeId) const noexcept
{
    const auto found = m_probes.find(probeId);
    return found != m_probes.end() ? &found->second : nullptr;
}

RenderSceneShadowComparison RenderSceneDatabase::Compare(
    const RenderFramePacket& packet) const
{
    RenderSceneShadowComparison result;
    for (const RenderPrimitiveSnapshot& primitive : packet.GetPrimitives())
    {
        const RenderPrimitiveSnapshot* retained = FindPrimitive(primitive.objectId);
        if (retained == nullptr)
            ++result.missingPrimitiveCount;
        else if (!EqualPrimitive(*retained, primitive))
            ++result.changedPrimitiveCount;
    }
    if (m_primitives.size() > packet.GetPrimitives().size())
    {
        result.missingPrimitiveCount += static_cast<uint32>(
            m_primitives.size() - packet.GetPrimitives().size());
    }

    for (const RenderLightSnapshot& light : packet.GetLights())
    {
        const RenderLightSnapshot* retained = FindLight(light.lightId);
        if (retained == nullptr)
            ++result.missingLightCount;
        else if (!EqualLight(*retained, light))
            ++result.changedLightCount;
    }
    if (m_lights.size() > packet.GetLights().size())
    {
        result.missingLightCount += static_cast<uint32>(
            m_lights.size() - packet.GetLights().size());
    }

    result.skyMismatch = !m_sky.has_value() ||
                         !EqualSky(*m_sky, packet.GetSky());
    result.environmentMismatch = !m_environment.has_value() ||
        !EqualEnvironment(*m_environment, packet.GetEnvironment());

    const RenderFeatureSnapshot& features = packet.GetFeatures();
    for (const ParticleRenderSnapshotItem& item : features.particles.items)
    {
        const auto found = m_particles.find(item.instanceId);
        if (found == m_particles.end() ||
            !AreParticleRenderSnapshotItemsEqual(found->second, item))
        {
            ++result.featureMismatchCount;
        }
    }
    if (m_particles.size() > features.particles.items.size())
    {
        result.featureMismatchCount += static_cast<uint32>(
            m_particles.size() - features.particles.items.size());
    }
    for (const WaterRenderSnapshotItem& item : features.water.items)
    {
        const auto found = m_water.find(item.componentId);
        if (found == m_water.end() ||
            !AreWaterRenderSnapshotItemsEqual(found->second, item))
        {
            ++result.featureMismatchCount;
        }
    }
    if (m_water.size() > features.water.items.size())
    {
        result.featureMismatchCount += static_cast<uint32>(
            m_water.size() - features.water.items.size());
    }
    for (const TerrainRenderSnapshotItem& item : features.terrain.items)
    {
        const auto found = m_terrain.find(item.componentId);
        if (found == m_terrain.end() ||
            !AreTerrainRenderSnapshotItemsEqual(found->second, item))
        {
            ++result.featureMismatchCount;
        }
    }
    if (m_terrain.size() > features.terrain.items.size())
    {
        result.featureMismatchCount += static_cast<uint32>(
            m_terrain.size() - features.terrain.items.size());
    }

    result.matches = result.missingPrimitiveCount == 0 &&
                     result.changedPrimitiveCount == 0 &&
                     result.missingLightCount == 0 &&
                     result.changedLightCount == 0 &&
                     result.featureMismatchCount == 0 &&
                     !result.skyMismatch && !result.environmentMismatch;
    return result;
}

std::unique_ptr<const RenderFramePacket>
RenderSceneDatabase::BuildCompatibilityFrame(
    const RenderFramePacketV5& frame) const
{
    if (m_revision < frame.GetHeader().requiredSceneRevision)
        return {};

    const auto copySorted = [](const auto& values)
    {
        using State = typename std::decay_t<decltype(values)>::mapped_type;
        std::vector<std::pair<uint64, const State*>> ordered;
        ordered.reserve(values.size());
        for (const auto& [id, state] : values)
            ordered.emplace_back(id, &state);
        std::sort(ordered.begin(), ordered.end(),
                  [](const auto& left, const auto& right)
                  {
                      return left.first < right.first;
                  });
        std::vector<State> result;
        result.reserve(ordered.size());
        for (const auto& [id, state] : ordered)
        {
            (void)id;
            result.push_back(*state);
        }
        return result;
    };

    std::vector<RenderPrimitiveSnapshot> primitives = copySorted(m_primitives);
    std::vector<RenderLightSnapshot> lights = copySorted(m_lights);
    RenderFeatureSnapshot features;
    features.BeginBuild(frame.GetHeader().sequence);
    features.particles.items = copySorted(m_particles);
    features.water.items = copySorted(m_water);
    features.terrain.items = copySorted(m_terrain);
    features.metadata.providerCount = features.particles.items.size() +
                                      features.water.items.size() +
                                      features.terrain.items.size();
    features.MarkComplete();

    RenderFrameHeader header;
    header.sequence = frame.GetHeader().sequence;
    header.worldRevision = frame.GetHeader().worldRevision;
    header.temporalEpoch = frame.GetHeader().temporalEpoch;
    header.expectedPrimitiveCount = static_cast<uint32>(primitives.size());
    header.extractedPrimitiveCount = header.expectedPrimitiveCount;
    header.expectedLightCount = static_cast<uint32>(lights.size());
    header.extractedLightCount = header.expectedLightCount;
    header.expectedFeatureProviderCount =
        static_cast<uint32>(features.metadata.providerCount);
    header.extractedFeatureProviderCount =
        header.expectedFeatureProviderCount;
    header.explicitDiscontinuity = frame.GetHeader().explicitDiscontinuity;

    return RenderFramePacket::CreateCompatibility(
        header,
        frame.GetView(),
        std::move(primitives),
        std::move(lights),
        m_sky.value_or(RenderSkySnapshot{}),
        m_environment.value_or(RenderEnvironmentSnapshot{}),
        frame.GetSettings(),
        frame.GetCaptureRequest(),
        std::move(features),
        frame.GetExtractionDiagnostics());
}

} // namespace RVX
