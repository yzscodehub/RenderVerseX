#pragma once

/**
 * @file ParticleSubsystemRenderAccess.h
 * @brief Private validation hooks for particle subsystem runtime setup.
 */

namespace RVX
{
    class IRHIDevice;
}

namespace RVX::Particle
{
    class ParticleSubsystem;

    class ParticleSubsystemRenderAccess final
    {
    public:
        static void SetDeviceForTesting(ParticleSubsystem& subsystem, IRHIDevice* device);
    };

} // namespace RVX::Particle
