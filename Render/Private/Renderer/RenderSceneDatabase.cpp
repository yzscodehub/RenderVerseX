#include "Render/Renderer/RenderSceneDatabase.h"

#include "Core/Diagnostics/ContentHash.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace RVX
{
namespace
{
    std::atomic<uint64> g_nextRenderSceneDatabaseInstanceId{1};

    void MixLightFloat(uint64& hash, float32 value)
    {
        Diagnostics::MixContentHashValue(
            hash,
            static_cast<uint64>(std::bit_cast<uint32>(value)));
    }

    void MixLightVec3(uint64& hash, const Vec3& value)
    {
        MixLightFloat(hash, value.x);
        MixLightFloat(hash, value.y);
        MixLightFloat(hash, value.z);
    }

    template<typename Values>
    void SortUnique(Values& values)
    {
        std::sort(values.begin(), values.end());
        values.erase(std::unique(values.begin(), values.end()), values.end());
    }

    template<typename Mutations, typename Values, typename IdMember>
    void AppendMutationIds(const Mutations& mutations,
                           Values& values,
                           IdMember idMember)
    {
        values.reserve(values.size() + mutations.size());
        for (const auto& mutation : mutations)
            values.push_back(mutation.*idMember);
    }

    template<typename Values>
    void AppendIds(const Values& source, Values& destination)
    {
        destination.reserve(destination.size() + source.size());
        destination.insert(destination.end(), source.begin(), source.end());
    }

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

    [[nodiscard]] uint64 MakePackedHandle(uint32 slot,
                                          uint32 generation) noexcept
    {
        return (static_cast<uint64>(generation) << 32U) |
               static_cast<uint64>(slot);
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
            const auto accepted = acceptedGenerations.find(slot);
            const uint64 currentId = accepted != acceptedGenerations.end()
                ? MakePackedHandle(slot, accepted->second)
                : 0;
            const bool hasCurrent = currentId != 0 &&
                                    current.contains(currentId);
            if (hasCurrent && currentId == id)
                continue;

            if (!hasCurrent)
            {
                if (accepted != acceptedGenerations.end() &&
                    GetHandleGeneration(id) <= accepted->second)
                {
                    return false;
                }
                continue;
            }

            if (GetHandleGeneration(id) <= GetHandleGeneration(currentId) ||
                !removals.contains(currentId))
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

    template<typename Map>
    struct IncrementalMapPlan
    {
        Map upserts;
        std::vector<typename Map::key_type> removals;
        size_t finalSize = 0;
    };

    template<typename Mutation, typename State>
    IncrementalMapPlan<std::unordered_map<uint64, State>>
        PrepareIncrementalValuePlan(
            const std::vector<Mutation>& mutations,
            const std::unordered_map<uint64, State>& current,
            uint64 Mutation::* idMember)
    {
        IncrementalMapPlan<std::unordered_map<uint64, State>> plan;
        plan.finalSize = current.size();
        plan.upserts.reserve(mutations.size());
        plan.removals.reserve(mutations.size());
        for (const Mutation& mutation : mutations)
        {
            const uint64 id = mutation.*idMember;
            if (mutation.operation == RenderSceneMutationOperation::Remove)
            {
                plan.removals.push_back(id);
                if (current.contains(id))
                    --plan.finalSize;
            }
            else
            {
                if (!current.contains(id))
                    ++plan.finalSize;
                plan.upserts.emplace(id, mutation.state);
            }
        }
        return plan;
    }

    IncrementalMapPlan<std::unordered_map<uint64, uint64>>
        PreparePrimitiveRevisionPlan(
            const std::vector<RenderPrimitiveMutation>& mutations,
            const std::unordered_map<uint64, uint64>& current,
            uint64 targetRevision)
    {
        IncrementalMapPlan<std::unordered_map<uint64, uint64>> plan;
        plan.finalSize = current.size();
        plan.upserts.reserve(mutations.size());
        plan.removals.reserve(mutations.size());
        for (const RenderPrimitiveMutation& mutation : mutations)
        {
            if (mutation.operation == RenderSceneMutationOperation::Remove)
            {
                plan.removals.push_back(mutation.objectId);
                if (current.contains(mutation.objectId))
                    --plan.finalSize;
            }
            else
            {
                if (!current.contains(mutation.objectId))
                    ++plan.finalSize;
                plan.upserts.emplace(mutation.objectId, targetRevision);
            }
        }
        return plan;
    }

    template<typename Mutation>
    IncrementalMapPlan<std::unordered_map<uint32, uint32>>
        PrepareGenerationPlan(
            const std::vector<Mutation>& mutations,
            const std::unordered_map<uint32, uint32>& current,
            uint64 Mutation::* idMember)
    {
        IncrementalMapPlan<std::unordered_map<uint32, uint32>> plan;
        plan.finalSize = current.size();
        plan.upserts.reserve(mutations.size());
        for (const Mutation& mutation : mutations)
        {
            const uint64 id = mutation.*idMember;
            const uint32 slot = GetHandleSlot(id);
            const uint32 generation = GetHandleGeneration(id);
            auto staged = plan.upserts.find(slot);
            if (staged != plan.upserts.end())
            {
                staged->second = std::max(staged->second, generation);
                continue;
            }
            const auto accepted = current.find(slot);
            const uint32 next = accepted != current.end()
                ? std::max(accepted->second, generation)
                : generation;
            if (accepted == current.end())
                ++plan.finalSize;
            plan.upserts.emplace(slot, next);
        }
        return plan;
    }

    template<typename Map>
    void ReserveIncrementalTarget(Map& target,
                                  const IncrementalMapPlan<Map>& plan)
    {
        if (plan.finalSize > target.size())
            target.reserve(plan.finalSize);
    }

    template<typename Map>
    void CommitIncrementalPlan(Map& target, IncrementalMapPlan<Map>& plan)
    {
        for (const typename Map::key_type& key : plan.removals)
            target.erase(key);
        while (!plan.upserts.empty())
        {
            typename Map::node_type node =
                plan.upserts.extract(plan.upserts.begin());
            target.erase(node.key());
            static_cast<void>(target.insert(std::move(node)));
        }
    }

    template<typename State>
    std::optional<std::optional<State>> PrepareIncrementalSingleton(
        const std::optional<RenderSingletonMutation<State>>& mutation)
    {
        if (!mutation.has_value())
            return std::nullopt;
        if (mutation->operation == RenderSceneMutationOperation::Remove)
        {
            return std::optional<std::optional<State>>{
                std::in_place, std::nullopt};
        }
        return std::optional<std::optional<State>>{
            std::in_place, mutation->state};
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
            left.skinMatrices.size() != right.skinMatrices.size() ||
            left.hasSkinningPaletteProvider !=
                right.hasSkinningPaletteProvider ||
            left.skinningPalette.providerComponentId !=
                right.skinningPalette.providerComponentId ||
            left.skinningPalette.sourceModelResourceId !=
                right.skinningPalette.sourceModelResourceId ||
            left.skinningPalette.poseSequence !=
                right.skinningPalette.poseSequence ||
            left.skinningPalette.paletteHash !=
                right.skinningPalette.paletteHash ||
            left.skinningPalette.paletteCount !=
                right.skinningPalette.paletteCount)
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
               left.layerMask == right.layerMask &&
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

RenderSceneDatabase::RenderSceneDatabase()
    : m_instanceId(g_nextRenderSceneDatabaseInstanceId.fetch_add(
          1, std::memory_order_relaxed))
{
    if (m_instanceId == 0)
    {
        m_instanceId = g_nextRenderSceneDatabaseInstanceId.fetch_add(
            1, std::memory_order_relaxed);
    }
}

uint64 RenderSceneDatabase::ComputeLightStateHash() const noexcept
{
    uint64 hash = Diagnostics::RVX_DIAGNOSTICS_FNV1A64_OFFSET_BASIS;
    std::vector<uint64> lightIds;
    lightIds.reserve(m_lights.size());
    for (const auto& [lightId, light] : m_lights)
    {
        static_cast<void>(light);
        lightIds.push_back(lightId);
    }
    std::sort(lightIds.begin(), lightIds.end());

    Diagnostics::MixContentHashValue(hash, lightIds.size());
    for (uint64 lightId : lightIds)
    {
        const RenderLightSnapshot& light = m_lights.at(lightId);
        Diagnostics::MixContentHashValue(hash, lightId);
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(light.type));
        MixLightVec3(hash, light.position);
        MixLightVec3(hash, light.direction);
        MixLightVec3(hash, light.color);
        MixLightFloat(hash, light.intensity);
        MixLightFloat(hash, light.range);
        MixLightFloat(hash, light.innerConeRadians);
        MixLightFloat(hash, light.outerConeRadians);
        Diagnostics::MixContentHashValue(hash, light.shadowResource.slot);
        Diagnostics::MixContentHashValue(hash, light.shadowResource.generation);
        Diagnostics::MixContentHashValue(hash, light.layerMask);
        Diagnostics::MixContentHashValue(hash, light.castsShadows ? 1U : 0U);
    }
    return hash;
}

RenderSceneDatabase::ChangeJournalEntry
RenderSceneDatabase::BuildChangeJournalEntry(
    const RenderSceneUpdateBatch& batch)
{
    ChangeJournalEntry entry;
    entry.baseRevision = batch.baseSceneRevision;
    entry.targetRevision = batch.targetSceneRevision;
    entry.fullReset = batch.fullReset;
    entry.skyChanged = batch.fullReset || batch.sky.has_value();
    entry.environmentChanged =
        batch.fullReset || batch.environment.has_value();
    if (batch.fullReset)
        return entry;

    AppendMutationIds(batch.primitives,
                      entry.primitives,
                      &RenderPrimitiveMutation::objectId);
    AppendMutationIds(batch.lights,
                      entry.lights,
                      &RenderLightMutation::lightId);
    AppendMutationIds(batch.decals,
                      entry.decals,
                      &RenderDecalMutation::decalId);
    AppendMutationIds(batch.probes,
                      entry.probes,
                      &RenderProbeMutation::probeId);
    AppendMutationIds(batch.particles,
                      entry.particles,
                      &RenderParticleMutation::instanceId);
    AppendMutationIds(batch.water,
                      entry.water,
                      &RenderWaterMutation::componentId);
    AppendMutationIds(batch.terrain,
                      entry.terrain,
                      &RenderTerrainMutation::componentId);
    return entry;
}

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

    if (!batch.fullReset)
    {
        auto primitives = PrepareIncrementalValuePlan(
            batch.primitives,
            m_primitives,
            &RenderPrimitiveMutation::objectId);
        auto primitiveRevisions = PreparePrimitiveRevisionPlan(
            batch.primitives,
            m_primitiveRevisions,
            batch.targetSceneRevision);
        auto lights = PrepareIncrementalValuePlan(
            batch.lights, m_lights, &RenderLightMutation::lightId);
        auto decals = PrepareIncrementalValuePlan(
            batch.decals, m_decals, &RenderDecalMutation::decalId);
        auto probes = PrepareIncrementalValuePlan(
            batch.probes, m_probes, &RenderProbeMutation::probeId);
        auto particles = PrepareIncrementalValuePlan(
            batch.particles,
            m_particles,
            &RenderParticleMutation::instanceId);
        auto water = PrepareIncrementalValuePlan(
            batch.water, m_water, &RenderWaterMutation::componentId);
        auto terrain = PrepareIncrementalValuePlan(
            batch.terrain, m_terrain, &RenderTerrainMutation::componentId);
        auto primitiveGenerations = PrepareGenerationPlan(
            batch.primitives,
            m_primitiveGenerations,
            &RenderPrimitiveMutation::objectId);
        auto lightGenerations = PrepareGenerationPlan(
            batch.lights,
            m_lightGenerations,
            &RenderLightMutation::lightId);
        auto decalGenerations = PrepareGenerationPlan(
            batch.decals,
            m_decalGenerations,
            &RenderDecalMutation::decalId);
        auto probeGenerations = PrepareGenerationPlan(
            batch.probes,
            m_probeGenerations,
            &RenderProbeMutation::probeId);
        auto waterGenerations = PrepareGenerationPlan(
            batch.water,
            m_waterGenerations,
            &RenderWaterMutation::componentId);
        auto terrainGenerations = PrepareGenerationPlan(
            batch.terrain,
            m_terrainGenerations,
            &RenderTerrainMutation::componentId);
        auto sky = PrepareIncrementalSingleton(batch.sky);
        auto environment = PrepareIncrementalSingleton(batch.environment);

        // Every allocation and copy happens before the live value maps change.
        // Existing-key updates need no rehash; growth reserves once and remains
        // amortized independently of the total retained scene size.
        ReserveIncrementalTarget(m_primitives, primitives);
        ReserveIncrementalTarget(m_primitiveRevisions, primitiveRevisions);
        ReserveIncrementalTarget(m_lights, lights);
        ReserveIncrementalTarget(m_decals, decals);
        ReserveIncrementalTarget(m_probes, probes);
        ReserveIncrementalTarget(m_particles, particles);
        ReserveIncrementalTarget(m_water, water);
        ReserveIncrementalTarget(m_terrain, terrain);
        ReserveIncrementalTarget(m_primitiveGenerations, primitiveGenerations);
        ReserveIncrementalTarget(m_lightGenerations, lightGenerations);
        ReserveIncrementalTarget(m_decalGenerations, decalGenerations);
        ReserveIncrementalTarget(m_probeGenerations, probeGenerations);
        ReserveIncrementalTarget(m_waterGenerations, waterGenerations);
        ReserveIncrementalTarget(m_terrainGenerations, terrainGenerations);
        m_changeHistory.push_back(BuildChangeJournalEntry(batch));

        CommitIncrementalPlan(m_primitives, primitives);
        CommitIncrementalPlan(m_primitiveRevisions, primitiveRevisions);
        CommitIncrementalPlan(m_lights, lights);
        CommitIncrementalPlan(m_decals, decals);
        CommitIncrementalPlan(m_probes, probes);
        CommitIncrementalPlan(m_particles, particles);
        CommitIncrementalPlan(m_water, water);
        CommitIncrementalPlan(m_terrain, terrain);
        CommitIncrementalPlan(m_primitiveGenerations, primitiveGenerations);
        CommitIncrementalPlan(m_lightGenerations, lightGenerations);
        CommitIncrementalPlan(m_decalGenerations, decalGenerations);
        CommitIncrementalPlan(m_probeGenerations, probeGenerations);
        CommitIncrementalPlan(m_waterGenerations, waterGenerations);
        CommitIncrementalPlan(m_terrainGenerations, terrainGenerations);
        if (sky.has_value())
            m_sky = std::move(*sky);
        if (environment.has_value())
            m_environment = std::move(*environment);
        m_revision = batch.targetSceneRevision;

        result.code = RenderSceneUpdateApplyCode::Applied;
        result.acceptedRevision = m_revision;
        result.sceneMutated = true;
        return result;
    }

    auto primitives = batch.fullReset ? decltype(m_primitives){} : m_primitives;
    auto primitiveRevisions = batch.fullReset
        ? decltype(m_primitiveRevisions){}
        : m_primitiveRevisions;
    auto lights = batch.fullReset ? decltype(m_lights){} : m_lights;
    auto decals = batch.fullReset ? decltype(m_decals){} : m_decals;
    auto probes = batch.fullReset ? decltype(m_probes){} : m_probes;
    auto primitiveGenerations = decltype(m_primitiveGenerations){};
    auto lightGenerations = decltype(m_lightGenerations){};
    auto decalGenerations = decltype(m_decalGenerations){};
    auto probeGenerations = decltype(m_probeGenerations){};
    auto waterGenerations = decltype(m_waterGenerations){};
    auto terrainGenerations = decltype(m_terrainGenerations){};
    auto particles = batch.fullReset ? decltype(m_particles){} : m_particles;
    auto water = batch.fullReset ? decltype(m_water){} : m_water;
    auto terrain = batch.fullReset ? decltype(m_terrain){} : m_terrain;
    auto sky = batch.fullReset ? decltype(m_sky){} : m_sky;
    auto environment = batch.fullReset ? decltype(m_environment){} : m_environment;

    ApplyMutations(batch.primitives, primitives, &RenderPrimitiveMutation::objectId);
    for (const RenderPrimitiveMutation& mutation : batch.primitives)
    {
        if (mutation.operation == RenderSceneMutationOperation::Remove)
            primitiveRevisions.erase(mutation.objectId);
        else
            primitiveRevisions.insert_or_assign(
                mutation.objectId, batch.targetSceneRevision);
    }
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

    // Allocate/copy history before the no-throw state swaps below. An
    // allocation failure therefore preserves both retained state and history.
    m_changeHistory.push_back(BuildChangeJournalEntry(batch));

    m_primitives.swap(primitives);
    m_primitiveRevisions.swap(primitiveRevisions);
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
    if (batch.fullReset)
    {
        while (m_changeHistory.size() > 1)
            m_changeHistory.pop_front();
    }

    result.code = RenderSceneUpdateApplyCode::Applied;
    result.acceptedRevision = m_revision;
    result.sceneMutated = true;
    return result;
}

void RenderSceneDatabase::Clear()
{
    m_revision = 0;
    m_primitives.clear();
    m_primitiveRevisions.clear();
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
    m_changeHistory.clear();
}

RenderSceneDatabaseChanges RenderSceneDatabase::CollectChangesSince(
    uint64 baseRevision) const
{
    RenderSceneDatabaseChanges result;
    result.baseRevision = baseRevision;
    result.targetRevision = m_revision;
    if (baseRevision == m_revision)
    {
        result.available = true;
        return result;
    }
    if (baseRevision > m_revision)
        return result;

    uint64 expectedRevision = baseRevision;
    bool observed = false;
    for (const ChangeJournalEntry& batch : m_changeHistory)
    {
        if (batch.targetRevision <= baseRevision)
            continue;

        if (batch.fullReset)
        {
            result.fullReset = true;
            result.skyChanged = true;
            result.environmentChanged = true;
            expectedRevision = batch.targetRevision;
            observed = true;
            continue;
        }
        if (batch.baseRevision != expectedRevision)
            return result;

        AppendIds(batch.primitives, result.primitives);
        AppendIds(batch.lights, result.lights);
        AppendIds(batch.decals, result.decals);
        AppendIds(batch.probes, result.probes);
        AppendIds(batch.particles, result.particles);
        AppendIds(batch.water, result.water);
        AppendIds(batch.terrain, result.terrain);
        result.skyChanged = result.skyChanged || batch.skyChanged;
        result.environmentChanged = result.environmentChanged ||
                                    batch.environmentChanged;
        expectedRevision = batch.targetRevision;
        observed = true;
    }
    if (!observed || expectedRevision != m_revision)
        return result;

    SortUnique(result.primitives);
    SortUnique(result.lights);
    SortUnique(result.decals);
    SortUnique(result.probes);
    SortUnique(result.particles);
    SortUnique(result.water);
    SortUnique(result.terrain);
    result.available = true;
    return result;
}

void RenderSceneDatabase::AcknowledgeChangesThrough(uint64 revision) noexcept
{
    while (!m_changeHistory.empty() &&
           m_changeHistory.front().targetRevision <= revision)
    {
        m_changeHistory.pop_front();
    }
}

const RenderPrimitiveSnapshot* RenderSceneDatabase::FindPrimitive(
    uint64 objectId) const noexcept
{
    const auto found = m_primitives.find(objectId);
    return found != m_primitives.end() ? &found->second : nullptr;
}

uint64 RenderSceneDatabase::GetPrimitiveRevision(uint64 objectId) const noexcept
{
    const auto found = m_primitiveRevisions.find(objectId);
    return found != m_primitiveRevisions.end() ? found->second : 0;
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

} // namespace RVX
