#pragma once

/**
 * @file ParticleSubsystemRenderAccess.h
 * @brief Private validation hooks for legacy particle render integration.
 */

namespace RVX
{
    class IRHIDevice;
    class SceneRenderer;
}

namespace RVX::Particle
{
    class ParticlePass;
    class ParticleSubsystem;
    struct ParticleRendererConfig;

    class ParticleSubsystemRenderAccess final
    {
    public:
        static void SetDeviceForTesting(ParticleSubsystem& subsystem, IRHIDevice* device);
        static void SetSceneRendererForTesting(ParticleSubsystem& subsystem, SceneRenderer* renderer);
        static void SetRendererConfigForTesting(ParticleSubsystem& subsystem, const ParticleRendererConfig& config);
        static ParticlePass* GetRenderPassForTesting(ParticleSubsystem& subsystem);
        static const ParticlePass* GetRenderPassForTesting(const ParticleSubsystem& subsystem);
    };

} // namespace RVX::Particle
