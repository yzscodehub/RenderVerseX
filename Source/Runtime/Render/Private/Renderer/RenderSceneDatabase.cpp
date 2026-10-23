#include "Render/Renderer/RenderSceneDatabase.h"

#include "Core/Diagnostics/ContentHash.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <limits>
#include <optional>
#include <tuple>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace RVX
{
namespace
{
    std::atomic<uint64> g_nextRenderSceneDatabaseInstanceId{1};

    void MixFloat(uint64& hash, float32 value)
    {
        Diagnostics::MixContentHashValue(
            hash,
            static_cast<uint64>(std::bit_cast<uint32>(value)));
    }

    void MixOptionalFloat(uint64& hash,
                          const std::optional<float32>& value)
    {
        Diagnostics::MixContentHashValue(hash, value.has_value() ? 1U : 0U);
        if (value.has_value())
            MixFloat(hash, *value);
    }

    void MixVec2(uint64& hash, const Vec2& value)
    {
        MixFloat(hash, value.x);
        MixFloat(hash, value.y);
    }

    void MixVec3(uint64& hash, const Vec3& value)
    {
        MixFloat(hash, value.x);
        MixFloat(hash, value.y);
        MixFloat(hash, value.z);
    }

    void MixVec4(uint64& hash, const Vec4& value)
    {
        MixFloat(hash, value.x);
        MixFloat(hash, value.y);
        MixFloat(hash, value.z);
        MixFloat(hash, value.w);
    }

    void MixMatrix(uint64& hash, const Mat4& value)
    {
        for (uint32 column = 0; column < 4; ++column)
        {
            for (uint32 row = 0; row < 4; ++row)
                MixFloat(hash, value[column][row]);
        }
    }

    void MixAABB(uint64& hash, const AABB& value)
    {
        MixVec3(hash, value.GetMin());
        MixVec3(hash, value.GetMax());
    }

    void MixString(uint64& hash, const std::string& value)
    {
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(value.size()));
        Diagnostics::MixContentHashString(hash, value);
    }

    void MixResourceHandle(uint64& hash, RenderResourceHandle value)
    {
        Diagnostics::MixContentHashValue(hash, value.slot);
        Diagnostics::MixContentHashValue(hash, value.generation);
    }

    void MixFeatureHeader(uint64& hash,
                          const RenderFeatureChannelHeader& header)
    {
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(header.channelId));
        Diagnostics::MixContentHashValue(hash, header.schemaVersion);
        Diagnostics::MixContentHashValue(hash, header.captureSequence);
        Diagnostics::MixContentHashValue(hash, header.configurationRevision);
        Diagnostics::MixContentHashValue(hash,
                                         header.source.sceneRuntimeId);
        Diagnostics::MixContentHashValue(hash, header.source.entitySlot);
        Diagnostics::MixContentHashValue(hash,
                                         header.source.entityGeneration);
    }

    void MixSceneObjectSource(
        uint64& hash,
        const RenderSceneObjectSourceIdentity& source)
    {
        Diagnostics::MixContentHashValue(hash, source.sceneRuntimeId);
        Diagnostics::MixContentHashValue(hash, source.entitySlot);
        Diagnostics::MixContentHashValue(hash, source.entityGeneration);
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

    enum class MutationDigestKind : uint8
    {
        Primitive = 1,
        Light,
        Decal,
        Probe,
        Particle,
        Water,
        Terrain,
        Sky,
        Environment
    };

    struct MutationDigestTuple
    {
        MutationDigestKind kind = MutationDigestKind::Primitive;
        RenderSceneMutationOperation operation =
            RenderSceneMutationOperation::Upsert;
        uint64 id = 0;
        uint64 payloadDigest = 0;
    };

    static_assert(
        RVX_RENDER_SCENE_UPDATE_SCHEMA_VERSION == 5,
        "Audit the complete mutation payload digest when the Scene update schema changes");

    uint64 ComputeMutationPayloadDigest(
        const RenderPrimitiveSnapshot& state)
    {
        uint64 hash = Diagnostics::RVX_DIAGNOSTICS_FNV1A64_OFFSET_BASIS;
        Diagnostics::MixContentHashValue(hash, state.objectId);
        MixSceneObjectSource(hash, state.source);
        MixResourceHandle(hash, state.mesh);
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(state.submeshes.size()));
        for (const RenderSubmeshMaterialBinding& binding : state.submeshes)
        {
            Diagnostics::MixContentHashValue(hash, binding.submeshIndex);
            MixResourceHandle(hash, binding.material);
            Diagnostics::MixContentHashValue(
                hash, static_cast<uint64>(binding.materialMode));
        }
        MixResourceHandle(hash, state.material);
        MixResourceHandle(hash, state.fallbackMesh);
        MixResourceHandle(hash, state.fallbackMaterial);
        MixMatrix(hash, state.worldTransform);
        MixMatrix(hash, state.previousWorldTransform);
        MixVec3(hash, state.boundsMin);
        MixVec3(hash, state.boundsMax);
        Diagnostics::MixContentHashValue(hash, state.flags);
        Diagnostics::MixContentHashValue(hash, state.layerMask);
        Diagnostics::MixContentHashValue(hash, state.sortKey);
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(state.skinMatrices.size()));
        for (const Mat4& matrix : state.skinMatrices)
            MixMatrix(hash, matrix);
        Diagnostics::MixContentHashValue(
            hash, state.hasSkinningPaletteProvider ? 1U : 0U);
        Diagnostics::MixContentHashValue(
            hash, state.skinningPalette.providerComponentId);
        Diagnostics::MixContentHashValue(
            hash, state.skinningPalette.sourceModelResourceId);
        Diagnostics::MixContentHashValue(
            hash, state.skinningPalette.poseSequence);
        Diagnostics::MixContentHashValue(
            hash, state.skinningPalette.paletteHash);
        Diagnostics::MixContentHashValue(
            hash, state.skinningPalette.paletteCount);
        return hash;
    }

    uint64 ComputeMutationPayloadDigest(const RenderLightSnapshot& state)
    {
        uint64 hash = Diagnostics::RVX_DIAGNOSTICS_FNV1A64_OFFSET_BASIS;
        Diagnostics::MixContentHashValue(hash, state.lightId);
        MixSceneObjectSource(hash, state.source);
        Diagnostics::MixContentHashValue(hash,
                                         static_cast<uint64>(state.type));
        MixVec3(hash, state.position);
        MixVec3(hash, state.direction);
        MixVec3(hash, state.color);
        MixFloat(hash, state.intensity);
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(state.intensityUnit));
        MixOptionalFloat(hash, state.colorTemperatureKelvin);
        MixFloat(hash, state.range);
        MixFloat(hash, state.innerConeRadians);
        MixFloat(hash, state.outerConeRadians);
        MixResourceHandle(hash, state.shadowResource);
        Diagnostics::MixContentHashValue(hash, state.layerMask);
        Diagnostics::MixContentHashValue(hash,
                                         state.castsShadows ? 1U : 0U);
        return hash;
    }

    uint64 ComputeMutationPayloadDigest(const RenderDecalSnapshot& state)
    {
        uint64 hash = Diagnostics::RVX_DIAGNOSTICS_FNV1A64_OFFSET_BASIS;
        Diagnostics::MixContentHashValue(hash, state.decalId);
        MixSceneObjectSource(hash, state.source);
        MixMatrix(hash, state.worldTransform);
        MixVec3(hash, state.halfExtent);
        MixResourceHandle(hash, state.material);
        MixVec4(hash, state.color);
        MixFloat(hash, state.opacity);
        MixFloat(hash, state.normalStrength);
        MixFloat(hash, state.angleFade);
        MixFloat(hash, state.fadeDistance);
        MixFloat(hash, state.fadeWidth);
        Diagnostics::MixContentHashValue(hash, state.layerMask);
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(static_cast<uint32>(state.sortOrder)));
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(state.blendMode));
        return hash;
    }

    uint64 ComputeMutationPayloadDigest(const RenderProbeSnapshot& state)
    {
        uint64 hash = Diagnostics::RVX_DIAGNOSTICS_FNV1A64_OFFSET_BASIS;
        Diagnostics::MixContentHashValue(hash, state.probeId);
        MixSceneObjectSource(hash, state.source);
        Diagnostics::MixContentHashValue(hash,
                                         static_cast<uint64>(state.kind));
        Diagnostics::MixContentHashValue(hash,
                                         static_cast<uint64>(state.shape));
        Diagnostics::MixContentHashValue(hash,
                                         static_cast<uint64>(state.mode));
        MixMatrix(hash, state.worldTransform);
        MixVec3(hash, state.influenceExtent);
        MixFloat(hash, state.blendDistance);
        MixVec3(hash, state.boxProjectionSize);
        MixVec3(hash, state.boxProjectionOffset);
        MixResourceHandle(hash, state.texture);
        for (const Vec4& coefficient : state.sphericalHarmonics)
            MixVec4(hash, coefficient);
        Diagnostics::MixContentHashValue(hash, state.cullingMask);
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(static_cast<uint32>(state.priority)));
        MixFloat(hash, state.nearClip);
        MixFloat(hash, state.farClip);
        Diagnostics::MixContentHashValue(hash,
                                         state.useBoxProjection ? 1U : 0U);
        Diagnostics::MixContentHashValue(hash, state.useHDR ? 1U : 0U);
        Diagnostics::MixContentHashValue(hash,
                                         state.hasValidData ? 1U : 0U);
        return hash;
    }

    uint64 ComputeMutationPayloadDigest(
        const ParticleRenderSnapshotItem& state)
    {
        uint64 hash = Diagnostics::RVX_DIAGNOSTICS_FNV1A64_OFFSET_BASIS;
        MixFeatureHeader(hash, state.header);
        Diagnostics::MixContentHashValue(hash, state.instanceId);
        Diagnostics::MixContentHashValue(hash, state.systemId);
        Diagnostics::MixContentHashValue(hash, state.systemAssetId.value);
        MixString(hash, state.systemName);
        MixMatrix(hash, state.worldMatrix);
        MixAABB(hash, state.worldBounds);
        MixVec3(hash, state.position);
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(state.renderMode));
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(state.blendMode));
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(state.simulationBackend));
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(state.payloadStatus));
        Diagnostics::MixContentHashValue(hash, state.aliveParticleCount);
        Diagnostics::MixContentHashValue(hash, state.maxParticleCount);
        Diagnostics::MixContentHashValue(hash, state.lodLevel);
        MixFloat(hash, state.normalizedTime);
        Diagnostics::MixContentHashValue(hash, state.visible ? 1U : 0U);
        Diagnostics::MixContentHashValue(
            hash, state.simulationSupported ? 1U : 0U);
        Diagnostics::MixContentHashValue(
            hash, state.sortingSupported ? 1U : 0U);
        Diagnostics::MixContentHashValue(
            hash, state.softParticlesEnabled ? 1U : 0U);
        MixFloat(hash, state.softParticleFadeDistance);
        const bool hasCpuPacket = state.payloadStatus ==
            ParticleRenderSnapshotPayloadStatus::CpuPacketReady;
        Diagnostics::MixContentHashValue(hash, hasCpuPacket ? 1U : 0U);
        if (hasCpuPacket)
        {
            // The immutable packet's producer-sealed content hash is the
            // transfer identity. Rehashing every particle here would turn
            // each Scene boundary into another O(N) validation pass.
            Diagnostics::MixContentHashValue(hash,
                                             state.cpuPacket.schemaVersion);
            Diagnostics::MixContentHashValue(hash,
                                             state.cpuPacket.simulationTick);
            Diagnostics::MixContentHashValue(hash,
                                             state.cpuPacket.deterministicSeed);
            Diagnostics::MixContentHashValue(
                hash, state.cpuPacket.deterministicInstanceKey);
            Diagnostics::MixContentHashValue(
                hash, state.cpuPacket.effectiveSimulationSeed);
            Diagnostics::MixContentHashValue(
                hash, static_cast<uint64>(state.cpuPacket.particles.size()));
            Diagnostics::MixContentHashValue(hash,
                                             state.cpuPacket.contentHash);
        }
        MixString(hash, state.unsupportedReason);
        MixString(hash, state.renderPayloadReason);
        MixString(hash, state.sortingReason);
        return hash;
    }

    uint64 ComputeMutationPayloadDigest(const WaterRenderSnapshotItem& state)
    {
        uint64 hash = Diagnostics::RVX_DIAGNOSTICS_FNV1A64_OFFSET_BASIS;
        MixFeatureHeader(hash, state.header);
        Diagnostics::MixContentHashValue(hash, state.componentId);
        Diagnostics::MixContentHashValue(hash, state.surfaceAssetId.value);
        Diagnostics::MixContentHashValue(hash, state.materialAssetId.value);
        MixVec3(hash, state.worldPosition);
        MixAABB(hash, state.worldBounds);
        MixVec2(hash, state.size);
        MixFloat(hash, state.depth);
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(state.surfaceType));
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(state.simulationType));
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(state.payloadStatus));
        Diagnostics::MixContentHashValue(
            hash, state.gerstnerWaves.contentHash);
        Diagnostics::MixContentHashValue(
            hash, state.gerstnerWaves.waveCount);
        MixVec3(hash, state.shallowColor);
        MixVec3(hash, state.deepColor);
        MixVec3(hash, state.foamColor);
        MixVec3(hash, state.absorptionCoefficient);
        MixFloat(hash, state.transparency);
        MixFloat(hash, state.reflectionStrength);
        MixFloat(hash, state.refractionStrength);
        MixFloat(hash, state.roughness);
        MixFloat(hash, state.normalScale);
        MixFloat(hash, state.foamIntensity);
        MixFloat(hash, state.foamCrestThreshold);
        Diagnostics::MixContentHashValue(
            hash, state.reflectionEnabled ? 1U : 0U);
        Diagnostics::MixContentHashValue(
            hash, state.refractionEnabled ? 1U : 0U);
        Diagnostics::MixContentHashValue(hash, state.foamEnabled ? 1U : 0U);
        Diagnostics::MixContentHashValue(hash, state.contentHash);
        return hash;
    }

    uint64 ComputeMutationPayloadDigest(
        const TerrainRenderSnapshotItem& state)
    {
        uint64 hash = Diagnostics::RVX_DIAGNOSTICS_FNV1A64_OFFSET_BASIS;
        MixFeatureHeader(hash, state.header);
        Diagnostics::MixContentHashValue(hash, state.componentId);
        Diagnostics::MixContentHashValue(hash, state.heightmapAssetId.value);
        Diagnostics::MixContentHashValue(hash, state.materialAssetId.value);
        MixVec3(hash, state.worldPosition);
        MixAABB(hash, state.worldBounds);
        MixVec3(hash, state.size);
        MixFloat(hash, state.lodBias);
        Diagnostics::MixContentHashValue(hash, state.patchSize);
        Diagnostics::MixContentHashValue(hash, state.maxLODLevels);
        Diagnostics::MixContentHashValue(hash, state.castsShadow ? 1U : 0U);
        Diagnostics::MixContentHashValue(hash,
                                         state.receivesShadow ? 1U : 0U);
        Diagnostics::MixContentHashValue(hash,
                                         state.collisionEnabled ? 1U : 0U);
        Diagnostics::MixContentHashValue(hash,
                                         state.hasHeightmap ? 1U : 0U);
        Diagnostics::MixContentHashValue(hash,
                                         state.heightmapValid ? 1U : 0U);
        Diagnostics::MixContentHashValue(hash, state.hasMaterial ? 1U : 0U);
        Diagnostics::MixContentHashValue(hash,
                                         state.gpuInitialized ? 1U : 0U);
        Diagnostics::MixContentHashValue(hash,
                                         state.cpuDataAvailable ? 1U : 0U);
        Diagnostics::MixContentHashValue(
            hash, state.renderGpuPathAvailable ? 1U : 0U);
        MixString(hash, state.renderPathReason);
        MixString(hash, state.heightmapDiagnostic);
        MixString(hash, state.materialDiagnostic);
        MixString(hash, state.lodDiagnostic);
        return hash;
    }

    uint64 ComputeMutationPayloadDigest(const RenderSkySnapshot& state)
    {
        uint64 hash = Diagnostics::RVX_DIAGNOSTICS_FNV1A64_OFFSET_BASIS;
        Diagnostics::MixContentHashValue(hash,
                                         static_cast<uint64>(state.mode));
        MixResourceHandle(hash, state.skyTexture);
        MixVec3(hash, state.tint);
        MixVec3(hash, state.sunDirection);
        MixVec3(hash, state.sunColor);
        MixVec3(hash, state.zenithColor);
        MixVec3(hash, state.horizonColor);
        MixVec3(hash, state.groundColor);
        MixFloat(hash, state.intensity);
        MixFloat(hash, state.rotationRadians);
        MixFloat(hash, state.blurLevel);
        MixFloat(hash, state.scatteringIntensity);
        return hash;
    }

    uint64 ComputeMutationPayloadDigest(
        const RenderEnvironmentSnapshot& state)
    {
        uint64 hash = Diagnostics::RVX_DIAGNOSTICS_FNV1A64_OFFSET_BASIS;
        MixResourceHandle(hash, state.irradianceTexture);
        MixResourceHandle(hash, state.prefilteredTexture);
        MixResourceHandle(hash, state.brdfLutTexture);
        MixFloat(hash, state.intensity);
        return hash;
    }

    void SaturatingAddCount(uint64& target, uint64 value) noexcept
    {
        if (value > std::numeric_limits<uint64>::max() - target)
            target = std::numeric_limits<uint64>::max();
        else
            target += value;
    }

    void AccumulateOperationSummary(
        RenderSceneMutationOperationSummary& target,
        const RenderSceneMutationOperationSummary& source) noexcept
    {
        SaturatingAddCount(target.upsertCount, source.upsertCount);
        SaturatingAddCount(target.removeCount, source.removeCount);
    }

    void AccumulateMutationSummary(RenderSceneMutationSummary& target,
                                   const RenderSceneMutationSummary& source) noexcept
    {
        AccumulateOperationSummary(target.primitives, source.primitives);
        AccumulateOperationSummary(target.lights, source.lights);
        AccumulateOperationSummary(target.decals, source.decals);
        AccumulateOperationSummary(target.probes, source.probes);
        AccumulateOperationSummary(target.particles, source.particles);
        AccumulateOperationSummary(target.water, source.water);
        AccumulateOperationSummary(target.terrain, source.terrain);
        AccumulateOperationSummary(target.sky, source.sky);
        AccumulateOperationSummary(target.environment, source.environment);
    }

    template<typename Mutations, typename IdMember>
    void AppendMutationEvidence(
        const Mutations& mutations,
        RenderSceneMutationOperationSummary& summary,
        MutationDigestKind kind,
        IdMember idMember,
        std::vector<MutationDigestTuple>& tuples)
    {
        tuples.reserve(tuples.size() + mutations.size());
        for (const auto& mutation : mutations)
        {
            if (mutation.operation == RenderSceneMutationOperation::Remove)
                SaturatingAddCount(summary.removeCount, 1);
            else
                SaturatingAddCount(summary.upsertCount, 1);
            const uint64 payloadDigest =
                mutation.operation == RenderSceneMutationOperation::Upsert
                ? ComputeMutationPayloadDigest(mutation.state)
                : 0;
            tuples.push_back(
                {kind, mutation.operation, mutation.*idMember, payloadDigest});
        }
    }

    template<typename State>
    void AppendSingletonEvidence(
        const std::optional<RenderSingletonMutation<State>>& mutation,
        RenderSceneMutationOperationSummary& summary,
        MutationDigestKind kind,
        std::vector<MutationDigestTuple>& tuples)
    {
        if (!mutation.has_value())
            return;
        if (mutation->operation == RenderSceneMutationOperation::Remove)
            SaturatingAddCount(summary.removeCount, 1);
        else
            SaturatingAddCount(summary.upsertCount, 1);
        const uint64 payloadDigest =
            mutation->operation == RenderSceneMutationOperation::Upsert
            ? ComputeMutationPayloadDigest(mutation->state)
            : 0;
        tuples.push_back({kind, mutation->operation, 0, payloadDigest});
    }

    uint64 ComputeCanonicalMutationDigest(
        const RenderSceneUpdateBatch& batch,
        std::vector<MutationDigestTuple> tuples)
    {
        std::sort(tuples.begin(), tuples.end(),
                  [](const MutationDigestTuple& left,
                     const MutationDigestTuple& right)
                  {
                      return std::tie(left.kind,
                                      left.operation,
                                      left.id,
                                      left.payloadDigest) <
                             std::tie(right.kind,
                                      right.operation,
                                      right.id,
                                      right.payloadDigest);
                  });

        uint64 hash = Diagnostics::RVX_DIAGNOSTICS_FNV1A64_OFFSET_BASIS;
        Diagnostics::MixContentHashValue(hash, batch.schemaId);
        Diagnostics::MixContentHashValue(hash, batch.schemaVersion);
        Diagnostics::MixContentHashValue(hash, batch.baseSceneRevision);
        Diagnostics::MixContentHashValue(hash, batch.targetSceneRevision);
        Diagnostics::MixContentHashValue(hash, batch.fullReset ? 1U : 0U);
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(tuples.size()));
        for (const MutationDigestTuple& tuple : tuples)
        {
            Diagnostics::MixContentHashValue(
                hash, static_cast<uint64>(tuple.kind));
            Diagnostics::MixContentHashValue(
                hash, static_cast<uint64>(tuple.operation));
            Diagnostics::MixContentHashValue(hash, tuple.id);
            Diagnostics::MixContentHashValue(hash, tuple.payloadDigest);
        }
        return hash;
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

    /**
     * Render IDs are opaque keys produced by the bridge. Source slot reuse is
     * governed only by the explicit provenance copied into each snapshot; do
     * not infer ECS generations by bit-decoding the opaque ID.
     */
    template<typename Mutation, typename State, typename SourceGenerations>
    bool ValidateSceneObjectGenerationSafeMutations(
        const std::vector<Mutation>& mutations,
        const std::unordered_map<uint64, State>& current,
        const SourceGenerations& acceptedSources,
        uint64 Mutation::* idMember,
        bool fullReset)
    {
        using SourceSlot = typename SourceGenerations::key_type;
        struct PendingUpsert
        {
            uint64 renderId = 0;
            RenderSceneObjectSourceIdentity source{};
        };

        std::unordered_set<uint64> removals;
        std::map<SourceSlot, PendingUpsert> upsertsBySource;
        removals.reserve(mutations.size());
        for (const Mutation& mutation : mutations)
        {
            const uint64 renderId = mutation.*idMember;
            if (renderId == 0)
                return false;

            if (mutation.operation == RenderSceneMutationOperation::Remove)
            {
                if (fullReset || !current.contains(renderId) ||
                    !removals.insert(renderId).second)
                {
                    return false;
                }
                continue;
            }

            const RenderSceneObjectSourceIdentity& source = mutation.state.source;
            if (!source.IsValid())
                return false;
            const auto retainedById = current.find(renderId);
            if (retainedById != current.end() &&
                retainedById->second.source != source)
            {
                return false;
            }

            const SourceSlot slot{source.sceneRuntimeId, source.entitySlot};
            const auto [upsert, inserted] = upsertsBySource.emplace(
                slot, PendingUpsert{renderId, source});
            if (!inserted && upsert->second.renderId != renderId)
                return false;
        }

        for (const auto& [slot, pending] : upsertsBySource)
        {
            for (const auto& [acceptedSlot, acceptedSource] : acceptedSources)
            {
                if (acceptedSlot != slot &&
                    acceptedSource.renderId == pending.renderId)
                {
                    return false;
                }
            }

            const auto accepted = acceptedSources.find(slot);
            if (accepted == acceptedSources.end())
                continue;

            const auto& acceptedSource = accepted->second;
            const auto retained = current.find(acceptedSource.renderId);
            const bool hasCurrent = retained != current.end();
            if (hasCurrent &&
                (retained->second.source.sceneRuntimeId != slot.first ||
                 retained->second.source.entitySlot != slot.second ||
                 retained->second.source.entityGeneration !=
                     acceptedSource.entityGeneration))
            {
                return false;
            }

            if (pending.source.entityGeneration ==
                acceptedSource.entityGeneration)
            {
                if (pending.renderId != acceptedSource.renderId || !hasCurrent)
                {
                    return false;
                }
                continue;
            }
            if (pending.source.entityGeneration <
                acceptedSource.entityGeneration)
            {
                return false;
            }
            if (pending.renderId == acceptedSource.renderId)
            {
                return false;
            }
            if (hasCurrent && !fullReset &&
                !removals.contains(acceptedSource.renderId))
            {
                return false;
            }
        }
        return true;
    }

    template<typename Mutation, typename SourceGenerations>
    void AdvanceAcceptedSceneObjectSources(
        const std::vector<Mutation>& mutations,
        SourceGenerations& generations,
        uint64 Mutation::* idMember)
    {
        // Tombstones intentionally survive full resets. A stale producer must
        // not resurrect an entity slot simply because a retained scene was
        // rebuilt from scratch.
        for (const Mutation& mutation : mutations)
        {
            if (mutation.operation == RenderSceneMutationOperation::Remove)
                continue;

            const RenderSceneObjectSourceIdentity& source = mutation.state.source;
            const typename SourceGenerations::key_type slot{
                source.sceneRuntimeId, source.entitySlot};
            typename SourceGenerations::mapped_type accepted;
            accepted.entityGeneration = source.entityGeneration;
            accepted.renderId = mutation.*idMember;
            generations.insert_or_assign(slot, accepted);
        }
    }

    template<typename Mutation, typename State, typename FeatureGenerations>
    bool ValidateFeatureGenerationSafeMutations(
        const std::vector<Mutation>& mutations,
        const std::unordered_map<uint64, State>& current,
        const FeatureGenerations& acceptedSources,
        uint64 Mutation::* idMember,
        RenderFeatureChannelId expectedChannel,
        bool fullReset)
    {
        using SourceSlot = typename FeatureGenerations::key_type;
        struct PendingUpsert
        {
            uint64 renderId = 0;
            RenderFeatureSourceIdentity source{};
            uint64 captureSequence = 0;
        };

        std::unordered_set<uint64> removals;
        std::map<SourceSlot, PendingUpsert> upsertsBySource;
        removals.reserve(mutations.size());
        for (const Mutation& mutation : mutations)
        {
            const uint64 renderId = mutation.*idMember;
            if (renderId == 0)
                return false;

            if (mutation.operation == RenderSceneMutationOperation::Remove)
            {
                const auto retained = current.find(renderId);
                if (fullReset || retained == current.end() ||
                    retained->second.ValidateChannel() !=
                        RenderFeatureChannelValidationCode::Valid ||
                    !removals.insert(renderId).second)
                {
                    return false;
                }
                continue;
            }

            if (mutation.state.ValidateChannel() !=
                    RenderFeatureChannelValidationCode::Valid ||
                mutation.state.header.channelId != expectedChannel)
            {
                return false;
            }
            const RenderFeatureSourceIdentity& source =
                mutation.state.header.source;
            const auto retainedById = current.find(renderId);
            if (retainedById != current.end() &&
                retainedById->second.header.source != source)
            {
                return false;
            }
            const SourceSlot slot{source.sceneRuntimeId, source.entitySlot};
            const auto [upsert, inserted] = upsertsBySource.emplace(
                slot,
                PendingUpsert{renderId,
                              source,
                              mutation.state.header.captureSequence});
            if (!inserted && upsert->second.renderId != renderId)
                return false;
        }

        for (const auto& [slot, pending] : upsertsBySource)
        {
            // Render IDs are opaque bridge keys, but they retain exclusive
            // source ownership across accepted snapshots. Tombstones remain
            // in acceptedSources after removal so a different source cannot
            // reuse an ID once it has been observed by this database.
            for (const auto& [acceptedSlot, acceptedSource] : acceptedSources)
            {
                if (acceptedSlot != slot &&
                    acceptedSource.renderId == pending.renderId)
                {
                    return false;
                }
            }

            const auto accepted = acceptedSources.find(slot);
            if (accepted == acceptedSources.end())
                continue;

            const auto& acceptedSource = accepted->second;
            const auto retained = current.find(acceptedSource.renderId);
            const bool hasCurrent = retained != current.end();
            if (hasCurrent &&
                (retained->second.header.source.sceneRuntimeId != slot.first ||
                 retained->second.header.source.entitySlot != slot.second ||
                 retained->second.header.source.entityGeneration !=
                     acceptedSource.entityGeneration))
            {
                return false;
            }

            if (pending.source.entityGeneration <
                acceptedSource.entityGeneration)
            {
                return false;
            }

            const bool sameGeneration =
                acceptedSource.entityGeneration ==
                pending.source.entityGeneration;
            if (sameGeneration &&
                pending.captureSequence <= acceptedSource.captureSequence &&
                !(fullReset && hasCurrent &&
                  pending.captureSequence == acceptedSource.captureSequence &&
                  acceptedSource.renderId == pending.renderId))
            {
                return false;
            }

            if (acceptedSource.renderId == pending.renderId && sameGeneration)
            {
                if (!hasCurrent)
                    return false;
                continue;
            }
            if (sameGeneration)
                return false;

            if (pending.renderId == acceptedSource.renderId)
                return false;

            if (hasCurrent && !fullReset &&
                !removals.contains(acceptedSource.renderId))
                return false;
        }
        return true;
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

    template<typename Mutation, typename SourceGenerations>
    IncrementalMapPlan<SourceGenerations> PrepareSceneObjectGenerationPlan(
        const std::vector<Mutation>& mutations,
        const SourceGenerations& current,
        uint64 Mutation::* idMember)
    {
        IncrementalMapPlan<SourceGenerations> plan;
        plan.finalSize = current.size();
        for (const Mutation& mutation : mutations)
        {
            if (mutation.operation == RenderSceneMutationOperation::Remove)
                continue;

            const RenderSceneObjectSourceIdentity& source = mutation.state.source;
            const typename SourceGenerations::key_type slot{
                source.sceneRuntimeId, source.entitySlot};
            typename SourceGenerations::mapped_type accepted;
            accepted.entityGeneration = source.entityGeneration;
            accepted.renderId = mutation.*idMember;
            if (!current.contains(slot))
                ++plan.finalSize;
            plan.upserts.insert_or_assign(slot, accepted);
        }
        return plan;
    }

    template<typename Mutation, typename FeatureGenerations>
    IncrementalMapPlan<FeatureGenerations> PrepareFeatureGenerationPlan(
        const std::vector<Mutation>& mutations,
        const FeatureGenerations& current,
        uint64 Mutation::* idMember)
    {
        IncrementalMapPlan<FeatureGenerations> plan;
        plan.finalSize = current.size();
        for (const Mutation& mutation : mutations)
        {
            if (mutation.operation == RenderSceneMutationOperation::Remove)
                continue;

            const RenderFeatureSourceIdentity& source =
                mutation.state.header.source;
            const typename FeatureGenerations::key_type slot{
                source.sceneRuntimeId, source.entitySlot};
            typename FeatureGenerations::mapped_type accepted;
            accepted.entityGeneration = source.entityGeneration;
            accepted.renderId = mutation.*idMember;
            accepted.captureSequence = mutation.state.header.captureSequence;
            if (!current.contains(slot))
                ++plan.finalSize;
            plan.upserts.insert_or_assign(slot, accepted);
        }
        return plan;
    }

    template<typename Mutation, typename FeatureGenerations>
    void AdvanceAcceptedFeatureSources(
        const std::vector<Mutation>& mutations,
        FeatureGenerations& generations,
        uint64 Mutation::* idMember)
    {
        for (const Mutation& mutation : mutations)
        {
            if (mutation.operation == RenderSceneMutationOperation::Remove)
                continue;
            const RenderFeatureSourceIdentity& source =
                mutation.state.header.source;
            const typename FeatureGenerations::key_type slot{
                source.sceneRuntimeId, source.entitySlot};
            typename FeatureGenerations::mapped_type accepted;
            accepted.entityGeneration = source.entityGeneration;
            accepted.renderId = mutation.*idMember;
            accepted.captureSequence = mutation.state.header.captureSequence;
            generations.insert_or_assign(slot, accepted);
        }
    }

    template<typename Map>
    void ReserveIncrementalTarget(Map& target,
                                  const IncrementalMapPlan<Map>& plan)
    {
        if constexpr (requires(Map& value, size_t size) { value.reserve(size); })
        {
            if (plan.finalSize > target.size())
                target.reserve(plan.finalSize);
        }
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
        if (left.objectId != right.objectId || left.source != right.source ||
            left.mesh != right.mesh ||
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
        return left.lightId == right.lightId && left.source == right.source &&
               left.type == right.type &&
               left.position == right.position && left.direction == right.direction &&
               left.color == right.color && left.intensity == right.intensity &&
               left.intensityUnit == right.intensityUnit &&
               left.colorTemperatureKelvin == right.colorTemperatureKelvin &&
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
        MixVec3(hash, light.position);
        MixVec3(hash, light.direction);
        MixVec3(hash, light.color);
        MixFloat(hash, light.intensity);
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(light.intensityUnit));
        MixOptionalFloat(hash, light.colorTemperatureKelvin);
        MixFloat(hash, light.range);
        MixFloat(hash, light.innerConeRadians);
        MixFloat(hash, light.outerConeRadians);
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
    std::vector<MutationDigestTuple> digestTuples;
    entry.baseRevision = batch.baseSceneRevision;
    entry.targetRevision = batch.targetSceneRevision;
    entry.fullReset = batch.fullReset;
    entry.skyChanged = batch.fullReset || batch.sky.has_value();
    entry.environmentChanged =
        batch.fullReset || batch.environment.has_value();

    AppendMutationIds(batch.primitives,
                      entry.primitives,
                      &RenderPrimitiveMutation::objectId);
    AppendMutationEvidence(batch.primitives,
                           entry.mutationSummary.primitives,
                           MutationDigestKind::Primitive,
                           &RenderPrimitiveMutation::objectId,
                           digestTuples);
    AppendMutationIds(batch.lights,
                      entry.lights,
                      &RenderLightMutation::lightId);
    AppendMutationEvidence(batch.lights,
                           entry.mutationSummary.lights,
                           MutationDigestKind::Light,
                           &RenderLightMutation::lightId,
                           digestTuples);
    AppendMutationIds(batch.decals,
                      entry.decals,
                      &RenderDecalMutation::decalId);
    AppendMutationEvidence(batch.decals,
                           entry.mutationSummary.decals,
                           MutationDigestKind::Decal,
                           &RenderDecalMutation::decalId,
                           digestTuples);
    AppendMutationIds(batch.probes,
                      entry.probes,
                      &RenderProbeMutation::probeId);
    AppendMutationEvidence(batch.probes,
                           entry.mutationSummary.probes,
                           MutationDigestKind::Probe,
                           &RenderProbeMutation::probeId,
                           digestTuples);
    AppendMutationIds(batch.particles,
                      entry.particles,
                      &RenderParticleMutation::instanceId);
    AppendMutationEvidence(batch.particles,
                           entry.mutationSummary.particles,
                           MutationDigestKind::Particle,
                           &RenderParticleMutation::instanceId,
                           digestTuples);
    AppendMutationIds(batch.water,
                      entry.water,
                      &RenderWaterMutation::componentId);
    AppendMutationEvidence(batch.water,
                           entry.mutationSummary.water,
                           MutationDigestKind::Water,
                           &RenderWaterMutation::componentId,
                           digestTuples);
    AppendMutationIds(batch.terrain,
                      entry.terrain,
                      &RenderTerrainMutation::componentId);
    AppendMutationEvidence(batch.terrain,
                           entry.mutationSummary.terrain,
                           MutationDigestKind::Terrain,
                           &RenderTerrainMutation::componentId,
                           digestTuples);
    AppendSingletonEvidence(batch.sky,
                            entry.mutationSummary.sky,
                            MutationDigestKind::Sky,
                            digestTuples);
    AppendSingletonEvidence(batch.environment,
                            entry.mutationSummary.environment,
                            MutationDigestKind::Environment,
                            digestTuples);
    entry.canonicalMutationDigest = ComputeCanonicalMutationDigest(
        batch, std::move(digestTuples));
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
    if (!ValidateSceneObjectGenerationSafeMutations(
            batch.primitives,
            m_primitives,
            m_primitiveGenerations,
            &RenderPrimitiveMutation::objectId,
            batch.fullReset) ||
        !ValidateSceneObjectGenerationSafeMutations(
            batch.lights,
            m_lights,
            m_lightGenerations,
            &RenderLightMutation::lightId,
            batch.fullReset) ||
        !ValidateSceneObjectGenerationSafeMutations(
            batch.decals,
            m_decals,
            m_decalGenerations,
            &RenderDecalMutation::decalId,
            batch.fullReset) ||
        !ValidateSceneObjectGenerationSafeMutations(
            batch.probes,
            m_probes,
            m_probeGenerations,
            &RenderProbeMutation::probeId,
            batch.fullReset) ||
        !ValidateFeatureGenerationSafeMutations(
            batch.particles,
            m_particles,
            m_particleGenerations,
            &RenderParticleMutation::instanceId,
            RenderFeatureChannelId::Particle,
            batch.fullReset) ||
        !ValidateFeatureGenerationSafeMutations(
            batch.water,
            m_water,
            m_waterGenerations,
            &RenderWaterMutation::componentId,
            RenderFeatureChannelId::Water,
            batch.fullReset) ||
        !ValidateFeatureGenerationSafeMutations(
            batch.terrain,
            m_terrain,
            m_terrainGenerations,
            &RenderTerrainMutation::componentId,
            RenderFeatureChannelId::Terrain,
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
        auto primitiveGenerations = PrepareSceneObjectGenerationPlan(
            batch.primitives,
            m_primitiveGenerations,
            &RenderPrimitiveMutation::objectId);
        auto lightGenerations = PrepareSceneObjectGenerationPlan(
            batch.lights,
            m_lightGenerations,
            &RenderLightMutation::lightId);
        auto decalGenerations = PrepareSceneObjectGenerationPlan(
            batch.decals,
            m_decalGenerations,
            &RenderDecalMutation::decalId);
        auto probeGenerations = PrepareSceneObjectGenerationPlan(
            batch.probes,
            m_probeGenerations,
            &RenderProbeMutation::probeId);
        auto particleGenerations = PrepareFeatureGenerationPlan(
            batch.particles,
            m_particleGenerations,
            &RenderParticleMutation::instanceId);
        auto waterGenerations = PrepareFeatureGenerationPlan(
            batch.water,
            m_waterGenerations,
            &RenderWaterMutation::componentId);
        auto terrainGenerations = PrepareFeatureGenerationPlan(
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
        ReserveIncrementalTarget(m_particleGenerations, particleGenerations);
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
        CommitIncrementalPlan(m_particleGenerations, particleGenerations);
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
    auto primitiveGenerations = m_primitiveGenerations;
    auto lightGenerations = m_lightGenerations;
    auto decalGenerations = m_decalGenerations;
    auto probeGenerations = m_probeGenerations;
    auto particleGenerations = m_particleGenerations;
    auto waterGenerations = m_waterGenerations;
    auto terrainGenerations = m_terrainGenerations;
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
    AdvanceAcceptedSceneObjectSources(batch.primitives,
                                      primitiveGenerations,
                                      &RenderPrimitiveMutation::objectId);
    AdvanceAcceptedSceneObjectSources(batch.lights,
                                      lightGenerations,
                                      &RenderLightMutation::lightId);
    AdvanceAcceptedSceneObjectSources(batch.decals,
                                      decalGenerations,
                                      &RenderDecalMutation::decalId);
    AdvanceAcceptedSceneObjectSources(batch.probes,
                                      probeGenerations,
                                      &RenderProbeMutation::probeId);
    ApplyMutations(batch.particles, particles, &RenderParticleMutation::instanceId);
    ApplyMutations(batch.water, water, &RenderWaterMutation::componentId);
    ApplyMutations(batch.terrain, terrain, &RenderTerrainMutation::componentId);
    AdvanceAcceptedFeatureSources(batch.particles,
                                  particleGenerations,
                                  &RenderParticleMutation::instanceId);
    AdvanceAcceptedFeatureSources(batch.water,
                                  waterGenerations,
                                  &RenderWaterMutation::componentId);
    AdvanceAcceptedFeatureSources(batch.terrain,
                                  terrainGenerations,
                                  &RenderTerrainMutation::componentId);
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
    m_particleGenerations.swap(particleGenerations);
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
    m_particleGenerations.clear();
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
    uint64 rangeDigest = Diagnostics::RVX_DIAGNOSTICS_FNV1A64_OFFSET_BASIS;
    Diagnostics::MixContentHashValue(rangeDigest, baseRevision);
    Diagnostics::MixContentHashValue(rangeDigest, m_revision);
    if (baseRevision == m_revision)
    {
        result.available = true;
        result.canonicalMutationDigest = rangeDigest;
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

        AccumulateMutationSummary(result.mutationSummary,
                                  batch.mutationSummary);
        Diagnostics::MixContentHashValue(rangeDigest, batch.baseRevision);
        Diagnostics::MixContentHashValue(rangeDigest, batch.targetRevision);
        Diagnostics::MixContentHashValue(rangeDigest,
                                         batch.canonicalMutationDigest);

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
    result.canonicalMutationDigest = rangeDigest;
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
