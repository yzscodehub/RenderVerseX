/**
 * @file JoltBackend.cpp
 * @brief Jolt Physics backend implementation
 * 
 * This file implements the Jolt Physics backend when RVX_PHYSICS_JOLT is defined.
 * Jolt Physics is a high-performance physics engine used by Horizon Forbidden West.
 */

#include "Physics/Backend/IPhysicsBackend.h"

#ifdef RVX_PHYSICS_JOLT

// Jolt includes
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>

namespace RVX::Physics
{

// Jolt requires these callbacks
static void TraceImpl(const char* inFMT, ...)
{
    // Forward to RVX logging
    va_list list;
    va_start(list, inFMT);
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), inFMT, list);
    va_end(list);
    // RVX_PHYSICS_TRACE("{}", buffer);
}

#ifdef JPH_ENABLE_ASSERTS
static bool AssertFailedImpl(const char* inExpression, const char* inMessage,
                              const char* inFile, uint32_t inLine)
{
    // RVX_PHYSICS_ERROR("Jolt Assert: {} : {} ({}:{})", 
    //                   inExpression, inMessage ? inMessage : "", inFile, inLine);
    return true;  // Break into debugger
}
#endif

/**
 * @brief Jolt Physics backend implementation
 */
class JoltBackend : public IPhysicsBackend
{
public:
    bool Initialize(const PhysicsBackendConfig& config) override
    {
        m_config = config;

        // Register trace/assert handlers
        JPH::Trace = TraceImpl;
#ifdef JPH_ENABLE_ASSERTS
        JPH::AssertFailed = AssertFailedImpl;
#endif

        // Register default allocator
        JPH::RegisterDefaultAllocator();

        // Create factory
        JPH::Factory::sInstance = new JPH::Factory();

        // Register all types
        JPH::RegisterTypes();

        // Create temp allocator
        m_tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(10 * 1024 * 1024);

        // Create job system
        m_jobSystem = std::make_unique<JPH::JobSystemThreadPool>(
            JPH::cMaxPhysicsJobs,
            JPH::cMaxPhysicsBarriers,
            std::thread::hardware_concurrency() - 1
        );

        // Create physics system
        const uint32_t cMaxBodies = config.maxBodies;
        const uint32_t cNumBodyMutexes = 0;  // Auto
        const uint32_t cMaxBodyPairs = config.maxBodyPairs;
        const uint32_t cMaxContactConstraints = config.maxContactConstraints;

        // Create broad phase layer interface
        // (Would need custom implementation for collision layers)

        m_physicsSystem = std::make_unique<JPH::PhysicsSystem>();
        // m_physicsSystem->Init(cMaxBodies, cNumBodyMutexes, cMaxBodyPairs,
        //                       cMaxContactConstraints, ...);

        // Set gravity
        // m_physicsSystem->SetGravity(ToJolt(config.gravity));

        m_initialized = true;
        return true;
    }

    void Shutdown() override
    {
        m_physicsSystem.reset();
        m_jobSystem.reset();
        m_tempAllocator.reset();

        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;

        m_initialized = false;
    }

    PhysicsBackendType GetType() const override
    {
        return PhysicsBackendType::Jolt;
    }

    const char* GetName() const override
    {
        return "Jolt";
    }

    void Step(float deltaTime) override
    {
        if (!m_initialized || !m_physicsSystem)
            return;

        // Jolt step
        const int cCollisionSteps = 1;
        // m_physicsSystem->Update(deltaTime, cCollisionSteps,
        //                         m_tempAllocator.get(), m_jobSystem.get());
    }

    void SetGravity(const Vec3& gravity) override
    {
        m_config.gravity = gravity;
        if (m_physicsSystem)
        {
            // m_physicsSystem->SetGravity(ToJolt(gravity));
        }
    }

    void* CreateBackendBody(RigidBody* body) override
    {
        if (!m_physicsSystem || !body)
            return nullptr;

        // Create Jolt body settings
        // JPH::BodyCreationSettings settings(...);
        // JPH::BodyID bodyId = m_physicsSystem->GetBodyInterface().CreateAndAddBody(
        //     settings, JPH::EActivation::Activate);

        return nullptr;  // Would return BodyID pointer
    }

    void DestroyBackendBody(void* backendBody) override
    {
        if (!m_physicsSystem || !backendBody)
            return;

        // m_physicsSystem->GetBodyInterface().RemoveBody(...);
        // m_physicsSystem->GetBodyInterface().DestroyBody(...);
    }

    void SyncBodyFromBackend(void* backendBody, RigidBody* body) override
    {
        if (!m_physicsSystem || !backendBody || !body)
            return;

        // Get transform from Jolt
        // JPH::BodyInterface& interface = m_physicsSystem->GetBodyInterface();
        // JPH::RVec3 pos = interface.GetCenterOfMassPosition(...);
        // JPH::Quat rot = interface.GetRotation(...);
        // body->SetPosition(FromJolt(pos));
        // body->SetRotation(FromJolt(rot));
    }

    void SyncBodyToBackend(RigidBody* body, void* backendBody) override
    {
        if (!m_physicsSystem || !body || !backendBody)
            return;

        // Set Jolt body state
        // m_physicsSystem->GetBodyInterface().SetPositionAndRotation(...);
    }

    void* CreateBackendShape(CollisionShape* shape) override
    {
        if (!shape)
            return nullptr;

        // Create Jolt shape based on type
        switch (shape->GetType())
        {
            case ShapeType::Sphere:
            {
                // auto* sphere = static_cast<SphereShape*>(shape);
                // return new JPH::SphereShape(sphere->GetRadius());
                break;
            }
            case ShapeType::Box:
            {
                // auto* box = static_cast<BoxShape*>(shape);
                // return new JPH::BoxShape(ToJolt(box->GetHalfExtents()));
                break;
            }
            // ... other shapes
            default:
                break;
        }

        return nullptr;
    }

    void DestroyBackendShape(void* backendShape) override
    {
        // Jolt shapes are ref-counted
    }

    void* CreateBackendConstraint(IConstraint* constraint) override
    {
        // Create Jolt constraint based on type
        return nullptr;
    }

    void DestroyBackendConstraint(void* backendConstraint) override
    {
        // m_physicsSystem->RemoveConstraint(...);
    }

    bool Raycast(const Vec3& origin, const Vec3& direction,
                 float maxDistance, RaycastHit& hit,
                 uint32 layerMask) override
    {
        if (!m_physicsSystem)
            return false;

        // JPH::RRayCast ray(ToJolt(origin), ToJolt(direction) * maxDistance);
        // JPH::RayCastResult result;
        // bool hasHit = m_physicsSystem->GetNarrowPhaseQuery().CastRay(ray, result, ...);

        return false;
    }

    bool SphereCast(const Vec3& origin, float radius,
                    const Vec3& direction, float maxDistance,
                    ShapeCastHit& hit, uint32 layerMask) override
    {
        if (!m_physicsSystem)
            return false;

        // JPH::SphereShape sphereShape(radius);
        // JPH::ShapeCast cast(&sphereShape, ...);

        return false;
    }

    size_t OverlapSphere(const Vec3& center, float radius,
                         std::vector<BodyHandle>& bodies,
                         uint32 layerMask) override
    {
        bodies.clear();

        if (!m_physicsSystem)
            return 0;

        // JPH::SphereShape sphereShape(radius);
        // m_physicsSystem->GetNarrowPhaseQuery().CollideShape(...);

        return 0;
    }

private:
    // Jolt type conversion helpers
    static JPH::Vec3 ToJolt(const Vec3& v)
    {
        return JPH::Vec3(v.x, v.y, v.z);
    }

    static Vec3 FromJolt(const JPH::Vec3& v)
    {
        return Vec3(v.GetX(), v.GetY(), v.GetZ());
    }

    static JPH::Quat ToJolt(const Quat& q)
    {
        return JPH::Quat(q.x, q.y, q.z, q.w);
    }

    static Quat FromJolt(const JPH::Quat& q)
    {
        return Quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ());
    }

    PhysicsBackendConfig m_config;
    bool m_initialized = false;

    std::unique_ptr<JPH::TempAllocatorImpl> m_tempAllocator;
    std::unique_ptr<JPH::JobSystemThreadPool> m_jobSystem;
    std::unique_ptr<JPH::PhysicsSystem> m_physicsSystem;
};

} // namespace RVX::Physics

#endif // RVX_PHYSICS_JOLT
