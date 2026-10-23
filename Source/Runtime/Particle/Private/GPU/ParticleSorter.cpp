#include "Particle/GPU/ParticleSorter.h"
#include "Core/Log.h"

namespace RVX::Particle
{

void ParticleSorter::Initialize(uint32 maxParticles)
{
    m_initialized = true;
    m_maxParticles = maxParticles;
    m_lastRequestedParticleCount = 0;
    m_lastRequestedCameraPosition = Vec3(0.0f, 0.0f, 0.0f);
    m_unsupportedReason =
        "Particle sorting is deferred to Render-owned feature passes";

    RVX_CORE_WARN("ParticleSorter: {}", m_unsupportedReason);
}

void ParticleSorter::Shutdown()
{
    m_initialized = false;
    m_maxParticles = 0;
    m_lastRequestedParticleCount = 0;
    m_lastRequestedCameraPosition = Vec3(0.0f, 0.0f, 0.0f);
}

bool ParticleSorter::Sort(uint32 particleCount, const Vec3& cameraPosition)
{
    m_lastRequestedParticleCount = particleCount;
    m_lastRequestedCameraPosition = cameraPosition;
    m_unsupportedReason =
        "Particle sorting is unsupported in Particle; Render-owned feature passes must provide GPU sorting";
    return false;
}

} // namespace RVX::Particle
