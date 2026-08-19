#pragma once

/**
 * @file PhysicsFragments.h
 * @brief Data-only physics intent and status fragments for the Scene ECS.
 *
 * These types deliberately do not include Physics headers or retain backend
 * handles. PhysicsSceneAdapters owns the runtime handle mapping and converts
 * these neutral values to the selected physics implementation.
 */

#include "Core/MathTypes.h"
#include "ECS/Fragment.h"

namespace RVX::SceneECS
{
    /** @brief Physics-owned motion policy for an ECS entity. */
    enum class RigidBodyMotionType : uint8
    {
        Static = 0,
        Kinematic,
        Dynamic,
    };

    /** @brief Continuous-collision policy expressed without a backend type. */
    enum class RigidBodyMotionQuality : uint8
    {
        Discrete = 0,
        LinearCast,
    };

    /** @brief Data-only rigid-body configuration. Pose remains a Scene transform concern. */
    struct RigidBody
    {
        RigidBodyMotionType motionType = RigidBodyMotionType::Dynamic;
        RigidBodyMotionQuality motionQuality = RigidBodyMotionQuality::Discrete;
        float mass = 1.0f;
        float linearDamping = 0.05f;
        float angularDamping = 0.05f;
        float gravityScale = 1.0f;
        uint8 positionConstraints = 0;
        uint8 rotationConstraints = 0;
        uint16 collisionLayer = 2;
        uint32 collisionMask = 0xFFFFFFFFu;
        uint32 collisionGroup = 0;
        uint32 collisionSubGroup = 0;
        bool allowSleep = true;
        bool startAsleep = false;
        bool isTrigger = false;
    };

    /**
     * @brief Collider geometry supported by the first pure-ECS physics slice.
     *
     * Convex and Mesh values are intentionally represented so authored data can
     * be rejected explicitly. The Built-in bridge never substitutes a box or
     * other fallback geometry for either value.
     */
    enum class ColliderShapeType : uint8
    {
        Box = 0,
        Sphere,
        Capsule,
        Convex,
        Mesh,
    };

    /** @brief Data-only single-collider configuration. */
    struct Collider
    {
        ColliderShapeType shape = ColliderShapeType::Box;
        Vec3 boxHalfExtents{0.5f};
        float sphereRadius = 0.5f;
        float capsuleRadius = 0.5f;
        float capsuleHalfHeight = 0.5f;
        Vec3 localOffset{0.0f};
        Quat localRotation{1.0f, 0.0f, 0.0f, 0.0f};
        float friction = 0.5f;
        float restitution = 0.0f;
        float density = 1000.0f;
    };

    /** @brief Observable body-binding state; backend handles remain bridge-private. */
    enum class PhysicsBodyBindingStatus : uint8
    {
        Unbound = 0,
        Active,
        PendingDestroy,
        UnsupportedCollider,
        BackendUnavailable,
        CreateFailed,
        InvalidConfiguration,
        /** @brief Dynamic world-pose writeback is unsafe under the current parent ancestry. */
        InvalidHierarchy,
    };

    /** @brief Runtime body state written by the bridge without exposing body pointers. */
    struct PhysicsBodyState
    {
        PhysicsBodyBindingStatus status = PhysicsBodyBindingStatus::Unbound;
        Vec3 linearVelocity{0.0f};
        Vec3 angularVelocity{0.0f};
        uint64 lastSynchronizedFixedStep = 0;
        uint64 bindingRevision = 0;
    };

    static_assert(ECS::Fragment<RigidBody>);
    static_assert(ECS::Fragment<Collider>);
    static_assert(ECS::Fragment<PhysicsBodyState>);
} // namespace RVX::SceneECS
