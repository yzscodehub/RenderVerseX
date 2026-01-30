/**
 * @file PhysicsWorld.cpp
 * @brief PhysicsWorld implementation
 */

#include "Physics/PhysicsWorld.h"
#include "Physics/Shapes/CollisionShape.h"
#include "Physics/Constraints/IConstraint.h"
#include <algorithm>
#include <cmath>

namespace RVX::Physics
{

PhysicsWorld::~PhysicsWorld()
{
    Shutdown();
}

bool PhysicsWorld::Initialize(const PhysicsWorldConfig& config)
{
    m_config = config;
    
#ifdef RVX_PHYSICS_JOLT
    // Jolt Physics initialization would go here
    // JPH::RegisterDefaultAllocator();
    // JPH::Factory::sInstance = new JPH::Factory();
    // ...
#else
    // Built-in physics engine - nothing special needed
#endif

    m_initialized = true;
    return true;
}

void PhysicsWorld::Shutdown()
{
    m_bodies.clear();
    m_bodyLookup.clear();
    m_constraints.clear();
    m_initialized = false;
    
#ifdef RVX_PHYSICS_JOLT
    // Jolt cleanup
#endif
}

void PhysicsWorld::Step(float deltaTime)
{
    if (!m_initialized) return;

    m_accumulatedTime += deltaTime;

    // Fixed timestep simulation
    while (m_accumulatedTime >= m_config.fixedTimeStep)
    {
        StepInternal(m_config.fixedTimeStep);
        m_accumulatedTime -= m_config.fixedTimeStep;
    }
}

void PhysicsWorld::StepInternal(float dt)
{
#ifdef RVX_PHYSICS_JOLT
    // Step Jolt Physics
    // m_physicsSystem->Update(dt, ...);
#else
    // Built-in physics step
    
    // 1. Apply forces and integrate velocities
    for (auto& body : m_bodies)
    {
        if (!body || !body->IsDynamic() || body->IsSleeping())
            continue;

        // Apply gravity
        Vec3 gravity = m_config.gravity * body->GetGravityScale();
        Vec3 velocity = body->GetLinearVelocity();
        velocity += gravity * dt;
        
        // Apply damping
        float linearDamping = 1.0f - body->GetLinearDamping() * dt;
        float angularDamping = 1.0f - body->GetAngularDamping() * dt;
        linearDamping = std::max(0.0f, linearDamping);
        angularDamping = std::max(0.0f, angularDamping);
        
        velocity *= linearDamping;
        Vec3 angularVelocity = body->GetAngularVelocity() * angularDamping;
        
        body->SetLinearVelocity(velocity);
        body->SetAngularVelocity(angularVelocity);
    }

    // 2. Solve constraints
    for (int iter = 0; iter < m_config.velocitySteps; ++iter)
    {
        for (auto& constraint : m_constraints)
        {
            if (constraint && constraint->IsEnabled() && !constraint->IsBroken())
            {
                constraint->SolveVelocity(dt);
            }
        }
    }

    // 3. Integrate positions
    for (auto& body : m_bodies)
    {
        if (!body || !body->IsDynamic() || body->IsSleeping())
            continue;

        Vec3 position = body->GetPosition();
        Quat rotation = body->GetRotation();
        
        position += body->GetLinearVelocity() * dt;
        
        Vec3 angularVel = body->GetAngularVelocity();
        if (length(angularVel) > 0.0001f)
        {
            Quat wQuat(0.0f, angularVel.x, angularVel.y, angularVel.z);
            Quat dq = wQuat * rotation * 0.5f * dt;
            rotation = normalize(rotation + dq);
        }
        
        body->SetPosition(position);
        body->SetRotation(rotation);
        body->ClearForces();
    }

    // 4. Solve position constraints
    for (int iter = 0; iter < m_config.positionSteps; ++iter)
    {
        for (auto& constraint : m_constraints)
        {
            if (constraint && constraint->IsEnabled() && !constraint->IsBroken())
            {
                constraint->SolvePosition(dt);
            }
        }
    }
    
    // 5. Check for sleeping bodies
    UpdateSleepStates();
#endif
}

void PhysicsWorld::UpdateSleepStates()
{
    const float sleepThreshold = 0.1f;
    
    for (auto& body : m_bodies)
    {
        if (!body || !body->IsDynamic() || !body->CanSleep())
            continue;

        float linearSpeed = length(body->GetLinearVelocity());
        float angularSpeed = length(body->GetAngularVelocity());
        
        if (linearSpeed < sleepThreshold && angularSpeed < sleepThreshold)
        {
            // Would accumulate sleep timer here
            // body->SetSleeping(true);
        }
    }
}

void PhysicsWorld::SetGravity(const Vec3& gravity)
{
    m_config.gravity = gravity;
    // TODO: Update backend gravity
}

BodyHandle PhysicsWorld::CreateBody(const RigidBodyDesc& desc)
{
    auto body = std::make_unique<RigidBody>(desc);
    uint64 id = m_nextBodyId++;
    body->SetId(id);

    // TODO: Create backend body

    m_bodyLookup[id] = m_bodies.size();
    m_bodies.push_back(std::move(body));

    return BodyHandle(id);
}

void PhysicsWorld::DestroyBody(BodyHandle handle)
{
    auto it = m_bodyLookup.find(handle.GetId());
    if (it == m_bodyLookup.end()) return;

    size_t index = it->second;
    
    // TODO: Destroy backend body

    // Swap and pop
    if (index != m_bodies.size() - 1)
    {
        m_bodies[index] = std::move(m_bodies.back());
        m_bodyLookup[m_bodies[index]->GetId()] = index;
    }
    m_bodies.pop_back();
    m_bodyLookup.erase(it);
}

RigidBody* PhysicsWorld::GetBody(BodyHandle handle)
{
    auto it = m_bodyLookup.find(handle.GetId());
    if (it == m_bodyLookup.end()) return nullptr;
    return m_bodies[it->second].get();
}

const RigidBody* PhysicsWorld::GetBody(BodyHandle handle) const
{
    auto it = m_bodyLookup.find(handle.GetId());
    if (it == m_bodyLookup.end()) return nullptr;
    return m_bodies[it->second].get();
}

void PhysicsWorld::SetBodyPosition(BodyHandle body, const Vec3& position)
{
    if (auto* b = GetBody(body)) b->SetPosition(position);
}

Vec3 PhysicsWorld::GetBodyPosition(BodyHandle body) const
{
    if (auto* b = GetBody(body)) return b->GetPosition();
    return Vec3(0.0f);
}

void PhysicsWorld::SetBodyRotation(BodyHandle body, const Quat& rotation)
{
    if (auto* b = GetBody(body)) b->SetRotation(rotation);
}

Quat PhysicsWorld::GetBodyRotation(BodyHandle body) const
{
    if (auto* b = GetBody(body)) return b->GetRotation();
    return Quat(1, 0, 0, 0);
}

void PhysicsWorld::SetBodyVelocity(BodyHandle body, const Vec3& velocity)
{
    if (auto* b = GetBody(body)) b->SetLinearVelocity(velocity);
}

Vec3 PhysicsWorld::GetBodyVelocity(BodyHandle body) const
{
    if (auto* b = GetBody(body)) return b->GetLinearVelocity();
    return Vec3(0.0f);
}

void PhysicsWorld::SetBodyAngularVelocity(BodyHandle body, const Vec3& angularVelocity)
{
    if (auto* b = GetBody(body)) b->SetAngularVelocity(angularVelocity);
}

Vec3 PhysicsWorld::GetBodyAngularVelocity(BodyHandle body) const
{
    if (auto* b = GetBody(body)) return b->GetAngularVelocity();
    return Vec3(0.0f);
}

void PhysicsWorld::ApplyForce(BodyHandle body, const Vec3& force)
{
    if (auto* b = GetBody(body)) b->ApplyForce(force);
}

void PhysicsWorld::ApplyImpulse(BodyHandle body, const Vec3& impulse)
{
    if (auto* b = GetBody(body)) b->ApplyImpulse(impulse);
}

void PhysicsWorld::ApplyTorque(BodyHandle body, const Vec3& torque)
{
    if (auto* b = GetBody(body)) b->ApplyTorque(torque);
}

void PhysicsWorld::AddShape(BodyHandle body, std::shared_ptr<CollisionShape> shape,
                            const Vec3& offset, const Quat& rotation)
{
    if (auto* b = GetBody(body))
    {
        b->AddShape(std::move(shape), offset, rotation);
    }
}

uint64 PhysicsWorld::CreateConstraint(std::shared_ptr<Constraint> constraint)
{
    uint64 id = m_nextConstraintId++;
    m_constraints.push_back(std::move(constraint));
    // TODO: Create backend constraint
    return id;
}

void PhysicsWorld::DestroyConstraint(uint64 constraintId)
{
    // TODO: Find and destroy constraint
}

bool PhysicsWorld::Raycast(const Vec3& origin, const Vec3& direction, float maxDistance,
                           RaycastHit& hit, uint32 layerMask) const
{
    hit = RaycastHit{};
    
    // TODO: Implement raycast using Jolt backend
    // JPH::RayCast ray(ToJolt(origin), ToJolt(direction) * maxDistance);
    // ...

    return false;
}

size_t PhysicsWorld::RaycastAll(const Vec3& origin, const Vec3& direction, float maxDistance,
                                std::vector<RaycastHit>& hits, uint32 layerMask) const
{
    hits.clear();
    // TODO: Implement multi-hit raycast
    return 0;
}

bool PhysicsWorld::SphereCast(const Vec3& origin, float radius, const Vec3& direction,
                              float maxDistance, ShapeCastHit& hit, uint32 layerMask) const
{
    hit = ShapeCastHit{};
    // TODO: Implement sphere cast
    return false;
}

size_t PhysicsWorld::OverlapSphere(const Vec3& center, float radius,
                                   std::vector<BodyHandle>& bodies, uint32 layerMask) const
{
    bodies.clear();
    // TODO: Implement overlap query
    return 0;
}

void PhysicsWorld::GetDebugDrawData(std::vector<Vec3>& lines, std::vector<Vec4>& colors,
                                    const DebugDrawOptions& options) const
{
    lines.clear();
    colors.clear();

    if (!options.drawBodies && !options.drawShapes) return;

    // TODO: Generate debug geometry from bodies
}

} // namespace RVX::Physics
