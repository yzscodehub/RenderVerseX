#include "Particle/ParticleEffectRenderPolicy.h"

#include "Resource/ResourcePublicationView.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace RVX::Particle
{
namespace
{
    [[nodiscard]] uint64 MixIdentity(uint32 seed,
                                     uint64 instanceKey,
                                     uint64 simulationTick,
                                     uint32 particleId) noexcept
    {
        uint64 value = (static_cast<uint64>(seed) << 32U) ^ instanceKey;
        value ^= simulationTick + 0x9e3779b97f4a7c15ull + (value << 6U) +
                 (value >> 2U);
        value ^= static_cast<uint64>(particleId) * 0xbf58476d1ce4e5b9ull;
        value ^= value >> 30U;
        value *= 0xbf58476d1ce4e5b9ull;
        value ^= value >> 27U;
        value *= 0x94d049bb133111ebull;
        return value ^ (value >> 31U);
    }

    [[nodiscard]] ParticleRenderLightHint DeriveLightHint(
        const ParticleRenderEffectDescriptor& descriptor,
        const ParticleCpuPacket& packet) noexcept
    {
        ParticleRenderLightHint hint;
        if (!descriptor.lightHint.enabled || packet.particles.empty())
        {
            return hint;
        }

        uint64 sampleTick = packet.simulationTick;
        if (descriptor.lightHint.flickerFrequencyHz > 0.0f &&
            packet.fixedDeltaSeconds > 0.0f)
        {
            const double ticksPerSampleValue = 1.0 /
                (static_cast<double>(descriptor.lightHint.flickerFrequencyHz) *
                 static_cast<double>(packet.fixedDeltaSeconds));
            const double maximumTicks =
                static_cast<double>(std::numeric_limits<uint64>::max());
            const uint64 ticksPerSample =
                !std::isfinite(ticksPerSampleValue) ||
                        ticksPerSampleValue >= maximumTicks
                    ? std::numeric_limits<uint64>::max()
                    : std::max<uint64>(
                          1U,
                          static_cast<uint64>(std::ceil(ticksPerSampleValue)));
            sampleTick = (packet.simulationTick / ticksPerSample) * ticksPerSample;
        }

        const uint64 selection = MixIdentity(packet.deterministicSeed,
                                              packet.deterministicInstanceKey,
                                              sampleTick,
                                              0U);
        const ParticleRenderParticleData& particle = packet.particles[
            static_cast<size_t>(selection % packet.particles.size())];
        const float phase = static_cast<float>(selection & 0xffffU) /
                            static_cast<float>(0x10000U);
        constexpr float twoPi = 6.28318530717958647692f;
        const float temporalPhase = twoPi *
            (descriptor.lightHint.flickerFrequencyHz *
                 static_cast<float>(sampleTick) * packet.fixedDeltaSeconds +
             phase);
        const float modulation = descriptor.lightHint.flickerFrequencyHz > 0.0f
            ? std::clamp(0.75f + 0.25f * std::sin(temporalPhase), 0.0f, 1.0f)
            : 1.0f;

        hint.enabled = true;
        hint.stableLightId = MixIdentity(packet.deterministicSeed,
                                         packet.deterministicInstanceKey,
                                         0U,
                                         0x4c495448U);
        if (hint.stableLightId == 0)
        {
            hint.stableLightId = 1;
        }
        hint.sampleTick = sampleTick;
        hint.sourceParticleId = particle.stableParticleId;
        hint.position = particle.position;
        hint.color = Vec3(descriptor.lightHint.color.x * particle.color.x,
                          descriptor.lightHint.color.y * particle.color.y,
                          descriptor.lightHint.color.z * particle.color.z);
        hint.intensityCandela = descriptor.lightHint.intensityCandela * modulation;
        hint.rangeMeters = descriptor.lightHint.rangeMeters;
        hint.modulation = modulation;
        return hint;
    }
} // namespace

ParticleEffectDescriptorResolution ParticleEffectRenderPolicy::ResolveDescriptor(
    const ParticleRenderEffectDescriptor& authored,
    const Resource::IResourcePublicationView* publicationView) noexcept
{
    ParticleEffectDescriptorResolution result;
    result.descriptor = authored;

    if (!result.descriptor.IsStructurallyValid())
    {
        result.code = ParticleEffectRenderPolicyCode::InvalidDescriptor;
        return result;
    }

    ParticleRenderFlipbookDescriptor& flipbook = result.descriptor.flipbook;
    if (!flipbook.textureAssetId.IsValid())
    {
        result.code = flipbook.required
            ? ParticleEffectRenderPolicyCode::InvalidDescriptor
            : ParticleEffectRenderPolicyCode::Ready;
        return result;
    }

    if (publicationView == nullptr)
    {
        result.code = flipbook.required
            ? ParticleEffectRenderPolicyCode::RequiredPublicationViewMissing
            : ParticleEffectRenderPolicyCode::RequiredTextureUnavailable;
        return result;
    }

    const Resource::ResourcePublicationQueryResult publication =
        publicationView->QueryResourcePublication(
            static_cast<Resource::ResourceId>(flipbook.textureAssetId.value),
            Resource::ResourceType::Texture);
    if (!publication.IsResolved() ||
        publication.kind != RenderResourceKind::Texture ||
        !publication.handle.IsValid() ||
        publication.status.code != RenderResourceStatusCode::Current ||
        publication.status.state != RenderResourcePublicState::GPUReady ||
        publication.status.committedContentRevision == 0)
    {
        result.code = ParticleEffectRenderPolicyCode::RequiredTextureUnavailable;
        return result;
    }

    flipbook.texture = publication.handle;
    flipbook.committedContentRevision = publication.status.committedContentRevision;
    result.descriptor.contentHash = result.descriptor.ComputeContentHash();
    if (!result.descriptor.IsStructurallyValid())
    {
        result.code = ParticleEffectRenderPolicyCode::InvalidDescriptor;
        return result;
    }

    result.code = ParticleEffectRenderPolicyCode::Ready;
    return result;
}

bool ParticleEffectRenderPolicy::SupportsCpuBillboard(
    ParticleRenderSnapshotMode renderMode,
    ParticleRenderSnapshotBlendMode blendMode,
    ParticleRenderSnapshotSimulationBackend simulationBackend) noexcept
{
    if (renderMode != ParticleRenderSnapshotMode::Billboard ||
        simulationBackend != ParticleRenderSnapshotSimulationBackend::CPU)
    {
        return false;
    }

    return blendMode == ParticleRenderSnapshotBlendMode::AlphaBlend ||
           blendMode == ParticleRenderSnapshotBlendMode::Premultiplied ||
           blendMode == ParticleRenderSnapshotBlendMode::Additive;
}

bool ParticleEffectRenderPolicy::FinalizeCpuPacket(
    ParticleCpuPacket& packet,
    const ParticleRenderEffectDescriptor& descriptor,
    const TextureSheetModule* textureSheet) noexcept
{
    if (!descriptor.IsStructurallyValid() ||
        (!descriptor.flipbook.IsBound() &&
         !descriptor.flipbook.IsCanonicalUntextured()))
    {
        return false;
    }

    for (ParticleRenderParticleData& particle : packet.particles)
    {
        uint32 frame = descriptor.flipbook.firstFrame;
        if (textureSheet != nullptr)
        {
            frame = textureSheet->GetFrameIndex(
                particle.age,
                particle.lifetime,
                packet.deterministicSeed,
                packet.deterministicInstanceKey,
                particle.stableParticleId,
                packet.simulationTick,
                packet.fixedDeltaSeconds);
        }

        const uint64 firstFrame = descriptor.flipbook.firstFrame;
        const uint64 endFrame = firstFrame + descriptor.flipbook.frameCount;
        if (static_cast<uint64>(frame) < firstFrame ||
            static_cast<uint64>(frame) >= endFrame)
        {
            return false;
        }
        particle.flipbookFrameIndex = frame;
    }

    packet.effectDescriptorHash = descriptor.contentHash;
    packet.lightHint = DeriveLightHint(descriptor, packet);
    packet.contentHash = packet.ComputeContentHash();
    return packet.lightHint.IsStructurallyValid();
}

std::string_view ParticleEffectRenderPolicy::Describe(
    ParticleEffectRenderPolicyCode code) noexcept
{
    switch (code)
    {
        case ParticleEffectRenderPolicyCode::Ready:
            return "Particle effect descriptor is ready for a CPU billboard packet.";
        case ParticleEffectRenderPolicyCode::InvalidDescriptor:
            return "Particle effect descriptor is invalid or internally inconsistent.";
        case ParticleEffectRenderPolicyCode::RequiredPublicationViewMissing:
            return "Particle effect requires a resource publication view for its flipbook.";
        case ParticleEffectRenderPolicyCode::RequiredTextureUnavailable:
            return "Particle effect flipbook has no current GPU-ready texture publication.";
        case ParticleEffectRenderPolicyCode::UnsupportedRenderMode:
            return "Particle effect requires CPU billboard render mode.";
        case ParticleEffectRenderPolicyCode::UnsupportedBlendMode:
            return "Particle effect blend mode is not supported by the CPU billboard path.";
        case ParticleEffectRenderPolicyCode::UnsupportedSimulationBackend:
            return "Particle effect requires the CPU simulation backend.";
        case ParticleEffectRenderPolicyCode::FrameRangeInvalid:
            return "Particle effect produced a flipbook frame outside its declared atlas range.";
        case ParticleEffectRenderPolicyCode::PacketInvalid:
            return "Particle effect packet failed immutable validation.";
    }
    return "Particle effect policy code is unknown.";
}
} // namespace RVX::Particle
