#include "Render/Particles/ParticleBillboardDrawPlan.h"

#include "Core/MathTypes.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <tuple>
#include <utility>

namespace RVX
{
    namespace
    {
        [[nodiscard]] bool IsFinite(const Vec2& value) noexcept
        {
            return std::isfinite(value.x) && std::isfinite(value.y);
        }

        [[nodiscard]] bool IsFinite(const Vec3& value) noexcept
        {
            return std::isfinite(value.x) && std::isfinite(value.y) &&
                   std::isfinite(value.z);
        }

        [[nodiscard]] bool IsFinite(const Vec4& value) noexcept
        {
            return std::isfinite(value.x) && std::isfinite(value.y) &&
                   std::isfinite(value.z) && std::isfinite(value.w);
        }

        [[nodiscard]] Vec3 NormalizeOr(const Vec3& value,
                                       const Vec3& fallback) noexcept
        {
            const float valueLength = length(value);
            return std::isfinite(valueLength) && valueLength > 0.0001f
                ? value / valueLength
                : fallback;
        }

        [[nodiscard]] bool IsSupportedBlendMode(
            ParticleRenderSnapshotBlendMode blendMode) noexcept
        {
            switch (blendMode)
            {
                case ParticleRenderSnapshotBlendMode::AlphaBlend:
                case ParticleRenderSnapshotBlendMode::Premultiplied:
                case ParticleRenderSnapshotBlendMode::Additive:
                    return true;
                case ParticleRenderSnapshotBlendMode::Multiply:
                default:
                    return false;
            }
        }

        [[nodiscard]] bool IsValidParticle(
            const ParticleRenderParticleData& particle) noexcept
        {
            return IsFinite(particle.position) && IsFinite(particle.velocity) &&
                   IsFinite(particle.color) && IsFinite(particle.size) &&
                   std::isfinite(particle.rotation) &&
                   std::isfinite(particle.age) &&
                   std::isfinite(particle.lifetime) && particle.lifetime > 0.0f &&
                   particle.age >= 0.0f && particle.age <= particle.lifetime &&
                   particle.size.x > 0.0f && particle.size.y > 0.0f;
        }

        void Fail(ParticleBillboardDrawPlan& plan,
                  ParticleBillboardDrawPlanFailure failure,
                  std::string diagnostic)
        {
            plan.valid = false;
            plan.failure = failure;
            plan.diagnostic = std::move(diagnostic);
            plan.items.clear();
            plan.batches.clear();
        }
    } // namespace

    const char* GetParticleBillboardDrawPlanFailureName(
        ParticleBillboardDrawPlanFailure failure) noexcept
    {
        switch (failure)
        {
            case ParticleBillboardDrawPlanFailure::None: return "None";
            case ParticleBillboardDrawPlanFailure::IncompleteSnapshot:
                return "IncompleteSnapshot";
            case ParticleBillboardDrawPlanFailure::InvalidChannelEnvelope:
                return "InvalidChannelEnvelope";
            case ParticleBillboardDrawPlanFailure::DuplicateSourceIdentity:
                return "DuplicateSourceIdentity";
            case ParticleBillboardDrawPlanFailure::DuplicateSystemInstanceIdentity:
                return "DuplicateSystemInstanceIdentity";
            case ParticleBillboardDrawPlanFailure::MetadataOnlyPayload:
                return "MetadataOnlyPayload";
            case ParticleBillboardDrawPlanFailure::UnsupportedSimulationBackend:
                return "UnsupportedSimulationBackend";
            case ParticleBillboardDrawPlanFailure::InvalidCpuPacket:
                return "InvalidCpuPacket";
            case ParticleBillboardDrawPlanFailure::UnsupportedRenderMode:
                return "UnsupportedRenderMode";
            case ParticleBillboardDrawPlanFailure::UnsupportedSoftParticles:
                return "UnsupportedSoftParticles";
            case ParticleBillboardDrawPlanFailure::UnsupportedBlendMode:
                return "UnsupportedBlendMode";
            case ParticleBillboardDrawPlanFailure::InvalidParticleCount:
                return "InvalidParticleCount";
            case ParticleBillboardDrawPlanFailure::InvalidParticleData:
                return "InvalidParticleData";
            default: return "Unknown";
        }
    }

    ParticleBillboardDrawPlan BuildParticleBillboardDrawPlan(
        const ParticleRenderSnapshot& snapshot,
        const ViewData& view)
    {
        ParticleBillboardDrawPlan plan;
        if (snapshot.metadata.status != ParticleRenderSnapshotStatus::Complete ||
            !snapshot.metadata.complete ||
            snapshot.metadata.itemCount != snapshot.items.size())
        {
            Fail(plan,
                 ParticleBillboardDrawPlanFailure::IncompleteSnapshot,
                 "Particle billboard draw plan requires a complete particle snapshot");
            return plan;
        }

        if (snapshot.items.empty())
        {
            plan.valid = true;
            return plan;
        }

        const Vec3 forward = NormalizeOr(
            view.cameraForward, Vec3(0.0f, 0.0f, -1.0f));
        std::set<std::tuple<uint64, uint32, uint32>> channelSourceIds;
        std::set<std::tuple<uint64, uint64>> systemInstanceIds;
        uint64 totalAliveParticles = 0;

        for (const ParticleRenderSnapshotItem& item : snapshot.items)
        {
            totalAliveParticles += item.aliveParticleCount;
            if (totalAliveParticles > std::numeric_limits<uint32>::max())
            {
                Fail(plan,
                     ParticleBillboardDrawPlanFailure::InvalidParticleCount,
                     "Particle snapshot aggregate count exceeds the renderable limit");
                return plan;
            }
            if (item.aliveParticleCount > item.maxParticleCount)
            {
                Fail(plan,
                     ParticleBillboardDrawPlanFailure::InvalidParticleCount,
                     "Particle snapshot item alive count exceeds its declared capacity");
                return plan;
            }
            if (item.header.Validate(
                    ParticleRenderFeatureChannelDescriptor::ChannelId,
                    ParticleRenderFeatureChannelDescriptor::SchemaVersion) !=
                RenderFeatureChannelValidationCode::Valid)
            {
                Fail(plan,
                     ParticleBillboardDrawPlanFailure::InvalidChannelEnvelope,
                     "Particle billboard draw plan requires a valid particle channel envelope");
                return plan;
            }
            const auto channelSource = std::make_tuple(
                item.header.source.sceneRuntimeId,
                item.header.source.entitySlot,
                item.header.source.entityGeneration);
            if (!channelSourceIds.insert(channelSource).second)
            {
                Fail(plan,
                     ParticleBillboardDrawPlanFailure::DuplicateSourceIdentity,
                     "Particle snapshot contains duplicate particle channel source identities");
                return plan;
            }
            if (!systemInstanceIds.insert(
                    std::make_tuple(item.systemId, item.instanceId)).second)
            {
                Fail(plan,
                     ParticleBillboardDrawPlanFailure::DuplicateSystemInstanceIdentity,
                     "Particle snapshot contains duplicate system-instance identities");
                return plan;
            }
            if (item.payloadStatus ==
                ParticleRenderSnapshotPayloadStatus::MetadataOnly)
            {
                if (!item.visible && item.HasNoCpuPacket() &&
                    item.aliveParticleCount == 0)
                {
                    // A dormant source is deliberately metadata-only until a
                    // CPU packet exists. It contributes no draw work.
                    continue;
                }
                Fail(plan,
                     ParticleBillboardDrawPlanFailure::MetadataOnlyPayload,
                     "Particle snapshots contain metadata-only items; Render-owned particle draw implementation is not connected");
                return plan;
            }
            if (item.simulationBackend !=
                ParticleRenderSnapshotSimulationBackend::CPU)
            {
                Fail(plan,
                     ParticleBillboardDrawPlanFailure::UnsupportedSimulationBackend,
                     "Particle billboard rendering requires a CPU particle packet");
                return plan;
            }
            if (!item.HasValidCpuPacket())
            {
                Fail(plan,
                     ParticleBillboardDrawPlanFailure::InvalidCpuPacket,
                     "Particle billboard rendering requires a structurally valid immutable CPU packet");
                return plan;
            }
            if (item.renderMode != ParticleRenderSnapshotMode::Billboard)
            {
                Fail(plan,
                     ParticleBillboardDrawPlanFailure::UnsupportedRenderMode,
                     "Particle billboard rendering supports only Billboard render mode");
                return plan;
            }
            if (!IsSupportedBlendMode(item.blendMode))
            {
                Fail(plan,
                     ParticleBillboardDrawPlanFailure::UnsupportedBlendMode,
                     "Particle billboard rendering supports only AlphaBlend, Premultiplied, and Additive blending");
                return plan;
            }

            const std::vector<ParticleRenderParticleData>& particles =
                item.GetParticles();
            if (particles.size() != item.GetParticleCount() ||
                particles.size() != item.aliveParticleCount ||
                particles.size() > item.maxParticleCount)
            {
                Fail(plan,
                     ParticleBillboardDrawPlanFailure::InvalidParticleCount,
                     "Particle CPU packet count does not match the extracted particle payload");
                return plan;
            }
            for (const ParticleRenderParticleData& particle : particles)
            {
                if (!IsValidParticle(particle))
                {
                    Fail(plan,
                         ParticleBillboardDrawPlanFailure::InvalidParticleData,
                         "Particle CPU packet contains non-finite or invalid billboard data");
                    return plan;
                }
                if (!item.visible)
                {
                    continue;
                }
                ParticleBillboardDrawItem drawItem;
                drawItem.particle = particle;
                // CPU simulation seals world-space positions into the packet.
                // Do not apply item.worldMatrix again: that transform was
                // already consumed by the emitter and a second application
                // would move translated systems twice.
                drawItem.blendMode = item.blendMode;
                drawItem.effectDescriptor = item.effectDescriptor;
                drawItem.effectDescriptorHash =
                    item.cpuPacket.effectDescriptorHash;
                drawItem.systemAssetId = item.systemAssetId.value;
                drawItem.deterministicInstanceKey =
                    item.cpuPacket.deterministicInstanceKey;
                drawItem.source = item.header.source;
                drawItem.systemId = item.systemId;
                drawItem.instanceId = item.instanceId;
                drawItem.viewDepth = dot(drawItem.particle.position - view.cameraPosition,
                                         forward);
                plan.items.push_back(std::move(drawItem));
            }
        }

        if (snapshot.metadata.totalAliveParticles != totalAliveParticles)
        {
            Fail(plan,
                 ParticleBillboardDrawPlanFailure::InvalidParticleCount,
                 "Particle snapshot aggregate alive count does not match item payloads");
            return plan;
        }

        std::stable_sort(
            plan.items.begin(), plan.items.end(),
            [](const ParticleBillboardDrawItem& lhs,
               const ParticleBillboardDrawItem& rhs)
            {
                if (lhs.viewDepth != rhs.viewDepth)
                {
                    return lhs.viewDepth > rhs.viewDepth;
                }
                if (lhs.deterministicInstanceKey != rhs.deterministicInstanceKey)
                {
                    return lhs.deterministicInstanceKey <
                        rhs.deterministicInstanceKey;
                }
                if (lhs.systemAssetId != rhs.systemAssetId)
                {
                    return lhs.systemAssetId < rhs.systemAssetId;
                }
                if (lhs.source.sceneRuntimeId != rhs.source.sceneRuntimeId)
                {
                    return lhs.source.sceneRuntimeId < rhs.source.sceneRuntimeId;
                }
                if (lhs.source.entitySlot != rhs.source.entitySlot)
                {
                    return lhs.source.entitySlot < rhs.source.entitySlot;
                }
                if (lhs.source.entityGeneration != rhs.source.entityGeneration)
                {
                    return lhs.source.entityGeneration < rhs.source.entityGeneration;
                }
                return lhs.particle.stableParticleId <
                    rhs.particle.stableParticleId;
            });

        for (uint32 index = 0; index < plan.items.size(); ++index)
        {
            const ParticleBillboardDrawItem& item = plan.items[index];
            if (plan.batches.empty() ||
                plan.batches.back().blendMode != item.blendMode ||
                plan.batches.back().effectDescriptorHash !=
                    item.effectDescriptorHash ||
                plan.batches.back().flipbookTexture !=
                    item.effectDescriptor.flipbook.texture ||
                plan.batches.back().flipbookContentRevision !=
                    item.effectDescriptor.flipbook.committedContentRevision)
            {
                plan.batches.push_back(ParticleBillboardDrawBatch{
                    .blendMode = item.blendMode,
                    .effectDescriptorHash = item.effectDescriptorHash,
                    .flipbookTexture = item.effectDescriptor.flipbook.texture,
                    .flipbookContentRevision =
                        item.effectDescriptor.flipbook.committedContentRevision,
                    .firstItem = index,
                    .itemCount = 0,
                });
            }
            ++plan.batches.back().itemCount;
        }

        plan.valid = true;
        return plan;
    }
} // namespace RVX
