#pragma once

/**
 * @file ParticleEvent.h
 * @brief Particle event types
 */

#include "Particle/ParticleTypes.h"
#include "Core/MathTypes.h"

namespace RVX::Particle
{
    /**
     * @brief Particle event type
     */
    enum class ParticleEventType : uint8
    {
        OnBirth,          ///< Particle was spawned
        OnDeath,          ///< Particle died (age >= lifetime)
        OnCollision,      ///< Particle collided with something
        OnTriggerEnter,   ///< Particle entered a trigger volume
        OnTriggerExit     ///< Particle exited a trigger volume
    };

    /**
     * @brief Particle event data
     */
    struct ParticleEvent
    {
        ParticleEventType type;
        Vec3 position;
        Vec3 velocity;
        Vec3 normal;          ///< Collision normal (for OnCollision)
        Vec4 color;
        float age;
        float lifetime;
        uint32 particleIndex;
        uint32 emitterIndex;
        uint64 instanceId;    ///< ParticleSystemInstance ID
    };

    /**
     * @brief Create an OnBirth event
     */
    inline ParticleEvent MakeBirthEvent(
        const Vec3& position,
        const Vec3& velocity,
        const Vec4& color,
        float lifetime,
        uint32 particleIndex,
        uint32 emitterIndex,
        uint64 instanceId)
    {
        ParticleEvent e;
        e.type = ParticleEventType::OnBirth;
        e.position = position;
        e.velocity = velocity;
        e.normal = Vec3(0);
        e.color = color;
        e.age = 0.0f;
        e.lifetime = lifetime;
        e.particleIndex = particleIndex;
        e.emitterIndex = emitterIndex;
        e.instanceId = instanceId;
        return e;
    }

    /**
     * @brief Create an OnDeath event
     */
    inline ParticleEvent MakeDeathEvent(
        const Vec3& position,
        const Vec3& velocity,
        const Vec4& color,
        float age,
        float lifetime,
        uint32 particleIndex,
        uint32 emitterIndex,
        uint64 instanceId)
    {
        ParticleEvent e;
        e.type = ParticleEventType::OnDeath;
        e.position = position;
        e.velocity = velocity;
        e.normal = Vec3(0);
        e.color = color;
        e.age = age;
        e.lifetime = lifetime;
        e.particleIndex = particleIndex;
        e.emitterIndex = emitterIndex;
        e.instanceId = instanceId;
        return e;
    }

    /**
     * @brief Create an OnCollision event
     */
    inline ParticleEvent MakeCollisionEvent(
        const Vec3& position,
        const Vec3& velocity,
        const Vec3& normal,
        const Vec4& color,
        float age,
        float lifetime,
        uint32 particleIndex,
        uint32 emitterIndex,
        uint64 instanceId)
    {
        ParticleEvent e;
        e.type = ParticleEventType::OnCollision;
        e.position = position;
        e.velocity = velocity;
        e.normal = normal;
        e.color = color;
        e.age = age;
        e.lifetime = lifetime;
        e.particleIndex = particleIndex;
        e.emitterIndex = emitterIndex;
        e.instanceId = instanceId;
        return e;
    }

} // namespace RVX::Particle
