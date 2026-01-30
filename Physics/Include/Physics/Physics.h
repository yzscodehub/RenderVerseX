/**
 * @file Physics.h
 * @brief Physics module unified header
 * 
 * Provides physics simulation capabilities with pluggable backend support.
 * Features:
 * - Rigid body dynamics with various shape types
 * - Constraint system (fixed, hinge, slider, spring, distance)
 * - Character controller for kinematic movement
 * - Raycasting and shape queries
 * 
 * Primary backend: Jolt Physics (MIT license, high performance)
 * Fallback: Built-in simple physics engine
 */

#pragma once

// Core types
#include "Physics/PhysicsTypes.h"
#include "Physics/PhysicsWorld.h"
#include "Physics/RigidBody.h"

// Shapes
#include "Physics/Shapes/CollisionShape.h"

// Constraints
#include "Physics/Constraints/Constraints.h"

// Character Controller
#include "Physics/CharacterController.h"

// Continuous Collision Detection
#include "Physics/CCD.h"

// Backend Abstraction (optional, for advanced users)
// #include "Physics/Backend/IPhysicsBackend.h"

namespace RVX::Physics
{

/**
 * @brief Physics system version info
 */
struct PhysicsSystemInfo
{
    static constexpr const char* kModuleName = "RVX Physics";
    static constexpr int kMajorVersion = 1;
    static constexpr int kMinorVersion = 0;
    
#ifdef RVX_PHYSICS_JOLT
    static constexpr const char* kBackendName = "Jolt";
    static constexpr bool kUsingJolt = true;
#else
    static constexpr const char* kBackendName = "Built-in";
    static constexpr bool kUsingJolt = false;
#endif
};

/**
 * @brief Available shape types for quick reference
 */
enum class ShapeTypes
{
    Sphere,         ///< SphereShape
    Box,            ///< BoxShape
    Capsule,        ///< CapsuleShape
    Cylinder,       ///< CylinderShape
    ConvexHull,     ///< ConvexHullShape
    TriangleMesh,   ///< TriangleMeshShape (static only)
    HeightField,    ///< HeightFieldShape (terrain)
    Compound        ///< CompoundShape (multiple shapes)
};

/**
 * @brief Available constraint types for quick reference
 */
enum class ConstraintTypes
{
    Fixed,          ///< FixedConstraint - weld joint
    Hinge,          ///< HingeConstraint - revolute joint
    Slider,         ///< SliderConstraint - prismatic joint
    Spring,         ///< SpringConstraint - spring-damper
    Distance        ///< DistanceConstraint - rod/rope
};

} // namespace RVX::Physics
