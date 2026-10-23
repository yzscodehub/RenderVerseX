#pragma once

/**
 * @file ParticleEffectRenderPolicy.h
 * @brief Deterministic CPU-particle effect packet policy.
 */

#include "Particle/Modules/TextureSheetModule.h"
#include "RenderContracts/ParticleRenderSnapshot.h"

#include <string_view>

namespace RVX::Resource
{
    class IResourcePublicationView;
}

namespace RVX::Particle
{
    enum class ParticleEffectRenderPolicyCode : uint8
    {
        Ready = 0,
        InvalidDescriptor,
        RequiredPublicationViewMissing,
        RequiredTextureUnavailable,
        UnsupportedRenderMode,
        UnsupportedBlendMode,
        UnsupportedSimulationBackend,
        FrameRangeInvalid,
        PacketInvalid,
    };

    struct ParticleEffectDescriptorResolution
    {
        ParticleRenderEffectDescriptor descriptor;
        ParticleEffectRenderPolicyCode code =
            ParticleEffectRenderPolicyCode::InvalidDescriptor;

        [[nodiscard]] bool IsReady() const noexcept
        {
            return code == ParticleEffectRenderPolicyCode::Ready;
        }
    };

    /**
     * @brief Pure policy shared by ParticleSubsystem and the ECS gateway.
     *
     * The policy has no wall-clock, entropy, Scene, Render, or RHI input.
     * All frame and light choices come from the sealed packet identity and
     * immutable descriptor.
     */
    class ParticleEffectRenderPolicy final
    {
    public:
        [[nodiscard]] static ParticleEffectDescriptorResolution ResolveDescriptor(
            const ParticleRenderEffectDescriptor& authored,
            const Resource::IResourcePublicationView* publicationView) noexcept;

        [[nodiscard]] static bool SupportsCpuBillboard(
            ParticleRenderSnapshotMode renderMode,
            ParticleRenderSnapshotBlendMode blendMode,
            ParticleRenderSnapshotSimulationBackend simulationBackend) noexcept;

        /**
         * @brief Resolves every final atlas frame and the packet light hint.
         *
         * The optional module supplies the authored Lifetime/Speed and random
         * row/start policy. The packet carries all identity/time inputs.
         */
        [[nodiscard]] static bool FinalizeCpuPacket(
            ParticleCpuPacket& packet,
            const ParticleRenderEffectDescriptor& descriptor,
            const TextureSheetModule* textureSheet) noexcept;

        [[nodiscard]] static std::string_view Describe(
            ParticleEffectRenderPolicyCode code) noexcept;
    };
} // namespace RVX::Particle
