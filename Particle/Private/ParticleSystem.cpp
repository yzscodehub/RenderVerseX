#include "Particle/ParticleSystem.h"
#include "Particle/Emitters/PointEmitter.h"
#include "Particle/Modules/ForceModule.h"
#include "Particle/Modules/ColorOverLifetimeModule.h"
#include "Particle/Modules/SizeOverLifetimeModule.h"

namespace RVX::Particle
{

ParticleSystem::Ptr ParticleSystem::CreateSimple(const std::string& name)
{
    auto system = Create(name);
    
    // Add default point emitter
    auto* emitter = system->AddEmitter<PointEmitter>();
    emitter->emissionRate = 50.0f;
    emitter->initialLifetime = FloatRange(1.0f, 2.0f);
    emitter->initialSpeed = FloatRange(2.0f, 5.0f);
    emitter->initialSize = FloatRange(0.1f, 0.2f);
    emitter->useShapeVelocity = false;
    emitter->initialVelocityDirection = Vec3Range(
        Vec3(-0.5f, 0.5f, -0.5f),
        Vec3(0.5f, 1.0f, 0.5f)
    );
    
    // Add gravity
    auto* force = system->AddModule<ForceModule>();
    force->gravity = Vec3(0.0f, -9.81f, 0.0f);
    force->drag = 0.1f;
    
    // Add color over lifetime (fade out)
    auto* color = system->AddModule<ColorOverLifetimeModule>();
    color->colorGradient = GradientCurve::FadeOut();
    
    // Add size over lifetime
    auto* size = system->AddModule<SizeOverLifetimeModule>();
    size->sizeCurve = AnimationCurve::FadeOut();
    
    return system;
}

} // namespace RVX::Particle
