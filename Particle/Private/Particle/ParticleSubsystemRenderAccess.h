#pragma once

/**
 * @file ParticleSubsystemRenderAccess.h
 * @brief Private validation hooks for legacy particle render integration.
 */

#include "Core/MathTypes.h"

#include <functional>
#include <memory>

namespace RVX
{
    class IRHIDevice;
}

namespace RVX::Particle
{
    class ParticlePass;
    class ParticleSubsystem;
    struct ParticleRendererConfig;

    using ParticlePreGraphPrepareCallback = std::function<void(const Vec3& cameraPosition)>;

    class IParticleRenderIntegrationHost
    {
    public:
        virtual ~IParticleRenderIntegrationHost() = default;

        virtual bool AddParticlePass(std::unique_ptr<ParticlePass> pass) = 0;
        virtual bool RemoveParticlePass(const char* name) = 0;
        virtual bool AddPreGraphPrepareCallback(const void* owner,
                                                ParticlePreGraphPrepareCallback callback) = 0;
        virtual bool RemovePreGraphPrepareCallback(const void* owner) = 0;
    };

    class ParticleSubsystemRenderAccess final
    {
    public:
        static void SetDeviceForTesting(ParticleSubsystem& subsystem, IRHIDevice* device);
        static void SetRenderHostForTesting(ParticleSubsystem& subsystem,
                                            IParticleRenderIntegrationHost* host);
        static void SetRendererConfigForTesting(ParticleSubsystem& subsystem, const ParticleRendererConfig& config);
        static ParticlePass* GetRenderPassForTesting(ParticleSubsystem& subsystem);
        static const ParticlePass* GetRenderPassForTesting(const ParticleSubsystem& subsystem);
    };

} // namespace RVX::Particle
