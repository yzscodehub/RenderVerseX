/**
 * @file PhysicsTypes.h
 * @brief Core physics types and enumerations
 */

#pragma once

#include "Core/Types.h"
#include "Core/MathTypes.h"
#include <cstdint>

namespace RVX::Physics
{

/**
 * @brief Physics body type
 */
enum class BodyType : uint8
{
    Static,         ///< Never moves, infinite mass
    Kinematic,      ///< Controlled by code, not physics
    Dynamic         ///< Fully simulated
};

/**
 * @brief Motion quality for bodies
 */
enum class MotionQuality : uint8
{
    Discrete,       ///< Standard discrete simulation
    LinearCast      ///< Continuous collision detection
};

/**
 * @brief Activation state
 */
enum class ActivationState : uint8
{
    Active,
    Sleeping
};

/**
 * @brief Collision layer for filtering
 */
using CollisionLayer = uint16;

namespace Layers
{
    constexpr CollisionLayer Default = 0;
    constexpr CollisionLayer Static = 1;
    constexpr CollisionLayer Dynamic = 2;
    constexpr CollisionLayer Kinematic = 3;
    constexpr CollisionLayer Trigger = 4;
    constexpr CollisionLayer Character = 5;
    constexpr CollisionLayer Projectile = 6;
    constexpr CollisionLayer Debris = 7;
}

/**
 * @brief Collision group for fine-grained filtering
 */
struct CollisionGroup
{
    uint32 groupId = 0;
    uint32 subGroupId = 0;
};

/**
 * @brief Physics material properties
 */
struct PhysicsMaterial
{
    float friction = 0.5f;
    float restitution = 0.0f;      ///< Bounciness (0-1)
    float density = 1000.0f;        ///< kg/m^3
    
    static PhysicsMaterial Default() { return PhysicsMaterial{}; }
    static PhysicsMaterial Bouncy() { return {0.3f, 0.8f, 1000.0f}; }
    static PhysicsMaterial Ice() { return {0.05f, 0.0f, 900.0f}; }
    static PhysicsMaterial Rubber() { return {0.9f, 0.6f, 1200.0f}; }
};

/**
 * @brief Mass properties
 */
struct MassProperties
{
    float mass = 1.0f;
    Vec3 centerOfMass{0.0f};
    Mat3 inertiaTensor = Mat3(1.0f);  ///< Identity = sphere approximation
    
    static MassProperties FromMass(float m)
    {
        MassProperties props;
        props.mass = m;
        return props;
    }
};

/**
 * @brief Contact point information
 */
struct ContactPoint
{
    Vec3 position;          ///< World-space contact position
    Vec3 normal;            ///< Contact normal (from B to A)
    float depth;            ///< Penetration depth
    float impulse;          ///< Applied impulse magnitude
};

/**
 * @brief Collision event data
 */
struct CollisionEvent
{
    uint64 bodyIdA;
    uint64 bodyIdB;
    Vec3 contactPoint;
    Vec3 contactNormal;
    float impulse;
    bool isTrigger;
};

/**
 * @brief Raycast hit result
 */
struct RaycastHit
{
    bool hit = false;
    float distance = 0.0f;
    Vec3 point{0.0f};
    Vec3 normal{0.0f};
    uint64 bodyId = 0;
    uint32 shapeIndex = 0;
};

/**
 * @brief Shape cast result
 */
struct ShapeCastHit
{
    bool hit = false;
    float fraction = 1.0f;      ///< 0-1 along cast path
    Vec3 point{0.0f};
    Vec3 normal{0.0f};
    uint64 bodyId = 0;
};

/**
 * @brief Body handle for referencing physics bodies
 */
class BodyHandle
{
public:
    using Index = uint32;

    static constexpr Index InvalidIndex = static_cast<Index>(-1);

    constexpr BodyHandle() = default;
    /**
     * @brief Reconstruct a handle from its stable packed representation.
     *
     * Kept for source compatibility with the former single-id body handle.
     */
    constexpr explicit BodyHandle(uint64 packedValue) : m_packedValue(packedValue) {}

    /** @brief Create a generation-safe body handle. */
    static constexpr BodyHandle Create(Index index, uint32 generation)
    {
        if (index == InvalidIndex)
        {
            return Invalid();
        }

        return BodyHandle((static_cast<uint64>(generation) << 32u) |
                          static_cast<uint64>(index + 1u));
    }

    static constexpr BodyHandle Invalid()
    {
        return BodyHandle{};
    }

    constexpr bool IsValid() const
    {
        return static_cast<uint32>(m_packedValue) != 0u;
    }

    /** @brief Legacy name for the packed handle value. */
    constexpr uint64 GetId() const { return m_packedValue; }
    constexpr uint64 GetPackedValue() const { return m_packedValue; }
    constexpr Index GetIndex() const
    {
        return IsValid() ? static_cast<Index>(static_cast<uint32>(m_packedValue) - 1u)
                         : InvalidIndex;
    }
    constexpr uint32 GetGeneration() const
    {
        return static_cast<uint32>(m_packedValue >> 32u);
    }

    constexpr bool operator==(const BodyHandle& other) const
    {
        return m_packedValue == other.m_packedValue;
    }
    constexpr bool operator!=(const BodyHandle& other) const { return !(*this == other); }
    constexpr bool operator<(const BodyHandle& other) const
    {
        return m_packedValue < other.m_packedValue;
    }

private:
    uint64 m_packedValue = 0;
};

} // namespace RVX::Physics
