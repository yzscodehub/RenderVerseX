/**
 * @file PhysicsSubsystem.cpp
 * @brief World-owned physics simulation subsystem implementation
 */

#include "World/PhysicsSubsystem.h"

#include "Core/Log.h"
#include "Scene/Components/RigidBodyComponent.h"
#include "Scene/SceneRuntime.h"
#include "World/World.h"

namespace RVX
{

void PhysicsSubsystem::Initialize()
{
    if (m_physicsWorld && m_physicsWorld->IsInitialized())
    {
        return;
    }

    m_physicsWorld = std::make_unique<Physics::PhysicsWorld>();
    if (!m_physicsWorld->Initialize(m_config))
    {
        RVX_CORE_ERROR("PhysicsSubsystem: failed to initialize PhysicsWorld");
        m_physicsWorld.reset();
        return;
    }

    RebindScene();

    RVX_CORE_INFO("PhysicsSubsystem initialized");
}

void PhysicsSubsystem::Deinitialize()
{
    if (m_registeredScene)
    {
        m_registeredScene->UnregisterSystem(m_sceneSystemHandle);
    }
    m_sceneSystemHandle = InvalidSceneSystemHandle;
    m_registeredScene = nullptr;

    DetachComponents();

    if (m_physicsWorld)
    {
        m_physicsWorld->Shutdown();
        m_physicsWorld.reset();
    }

    m_lastRegisteredBodyComponentCount = 0;
    m_lastPhysicsStepCount = 0;
    m_physicsAccumulatorSeconds = 0.0f;
    RVX_CORE_INFO("PhysicsSubsystem deinitialized");
}

void PhysicsSubsystem::Tick(float deltaTime)
{
    if (!m_physicsWorld || !m_physicsWorld->IsInitialized())
    {
        return;
    }

    std::vector<RigidBodyComponent*> components;
    GatherRigidBodyComponents(components);
    AttachComponents(components);

    for (RigidBodyComponent* component : components)
    {
        if (component)
        {
            component->SyncToPhysics();
        }
    }

    m_physicsWorld->Step(deltaTime);
    m_lastPhysicsStepCount = m_physicsWorld->GetLastStepCount();
    m_physicsAccumulatorSeconds = m_physicsWorld->GetAccumulatedTime();

    for (RigidBodyComponent* component : components)
    {
        if (component && component->IsDynamic())
        {
            component->SyncFromPhysics();
        }
    }

    m_lastRegisteredBodyComponentCount = components.size();
}

void PhysicsSubsystem::SetConfig(const Physics::PhysicsWorldConfig& config)
{
    if (m_physicsWorld && m_physicsWorld->IsInitialized())
    {
        RVX_CORE_WARN("PhysicsSubsystem: config changes after initialization are ignored");
        return;
    }

    m_config = config;
}

void PhysicsSubsystem::GatherRigidBodyComponents(std::vector<RigidBodyComponent*>& outComponents) const
{
    outComponents.clear();

    const World* world = GetWorld();
    const Scene* scene = world ? world->GetScene() : nullptr;
    if (!scene)
    {
        return;
    }

    outComponents = scene->GetComponents<RigidBodyComponent>();
}

void PhysicsSubsystem::RebindScene()
{
    World* world = GetWorld();
    Scene* scene = world ? world->GetScene() : nullptr;
    if (scene == m_registeredScene && m_sceneSystemHandle.IsValid())
        return;

    if (m_registeredScene)
        m_registeredScene->UnregisterSystem(m_sceneSystemHandle);

    m_registeredScene = scene;
    m_sceneSystemHandle = InvalidSceneSystemHandle;
    if (m_registeredScene)
    {
        m_sceneSystemHandle = m_registeredScene->RegisterSystem(
            "PhysicsSubsystem",
            SceneUpdatePhase::FixedPhysics,
            [this](float deltaTime) { Tick(deltaTime); });
    }
}

void PhysicsSubsystem::AttachComponents(std::vector<RigidBodyComponent*>& components)
{
    if (!m_physicsWorld)
    {
        return;
    }

    for (RigidBodyComponent* component : components)
    {
        if (!component)
        {
            continue;
        }

        if (component->GetPhysicsWorld() != m_physicsWorld.get() || !component->IsRegisteredWithPhysicsWorld())
        {
            component->SetPhysicsWorld(m_physicsWorld.get());
        }
    }
}

void PhysicsSubsystem::DetachComponents()
{
    std::vector<RigidBodyComponent*> components;
    GatherRigidBodyComponents(components);

    for (RigidBodyComponent* component : components)
    {
        if (component && component->GetPhysicsWorld() == m_physicsWorld.get())
        {
            component->SetPhysicsWorld(nullptr);
        }
    }
}

} // namespace RVX
