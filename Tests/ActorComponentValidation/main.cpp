#include "Core/Core.h"
#include "Scene/Actor.h"
#include "Scene/ActorComponent.h"
#include "Scene/ActorFactory.h"
#include "Scene/Component.h"
#include "Scene/ComponentFactory.h"
#include "Scene/Components/StaticMeshComponent.h"
#include "Scene/Node.h"
#include "Scene/PrimitiveComponent.h"
#include "Scene/SceneComponent.h"
#include "Scene/SceneEntity.h"
#include "Scene/SceneManager.h"
#include "World/World.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace
{
    class LogEnvironment : public ::testing::Environment
    {
    public:
        void SetUp() override
        {
            RVX::Log::Initialize();
            RVX_CORE_INFO("Actor Component Validation Tests");
        }

        void TearDown() override
        {
            RVX::Log::Shutdown();
        }
    };

    [[maybe_unused]] const auto* g_logEnvironment =
        ::testing::AddGlobalTestEnvironment(new LogEnvironment);

    class TestComponent : public RVX::ActorComponent
    {
    public:
        const char* GetClassName() const override { return "TestComponent"; }

        void OnRegister() override { registered = true; }
        void InitializeComponent() override { initialized = true; }
        void BeginPlay() override { beganPlay = true; }
        void TickComponent(float deltaTime) override
        {
            ticked = true;
            lastDeltaTime = deltaTime;
        }
        void EndPlay() override { endedPlay = true; }
        void OnUnregister() override { unregistered = true; }

        bool registered = false;
        bool initialized = false;
        bool beganPlay = false;
        bool ticked = false;
        bool endedPlay = false;
        bool unregistered = false;
        float lastDeltaTime = 0.0f;
    };

    TEST(ActorComponentValidation, ActorComponentLifecycleDefaults)
    {
        TestComponent component;
        EXPECT_TRUE(component.IsEnabled());
        EXPECT_FALSE(component.IsRegistered());
        EXPECT_FALSE(component.CanEverTick());
        EXPECT_FALSE(component.IsTickEnabled());

        component.SetCanEverTick(true);
        component.SetTickEnabled(true);
        EXPECT_TRUE(component.CanEverTick());
        EXPECT_TRUE(component.IsTickEnabled());
        EXPECT_EQ(std::string("TestComponent"), std::string(component.GetClassName()));
    }

    TEST(ActorComponentValidation, ActorAssignsUniqueDefaultComponentNames)
    {
        RVX::Actor actor("NamedActor");

        auto* first = actor.AddComponent<TestComponent>();
        auto* second = actor.AddComponent<TestComponent>();

        ASSERT_NE(nullptr, first);
        ASSERT_NE(nullptr, second);
        EXPECT_EQ(std::string("TestComponent"), first->GetName());
        EXPECT_EQ(std::string("TestComponent_1"), second->GetName());
    }

    TEST(ActorComponentValidation, ActorAddOwnedComponentUniquifiesExplicitComponentNames)
    {
        RVX::Actor actor("ExplicitNameActor");

        auto firstComponent = std::make_unique<TestComponent>();
        firstComponent->SetName("SharedName");
        auto* first = static_cast<TestComponent*>(
            actor.AddOwnedComponent(std::move(firstComponent)));

        auto secondComponent = std::make_unique<TestComponent>();
        secondComponent->SetName("SharedName");
        auto* second = static_cast<TestComponent*>(
            actor.AddOwnedComponent(std::move(secondComponent)));

        ASSERT_NE(nullptr, first);
        ASSERT_NE(nullptr, second);
        EXPECT_EQ(std::string("SharedName"), first->GetName());
        EXPECT_EQ(std::string("SharedName_1"), second->GetName());
    }

    TEST(ActorComponentValidation, SceneEntityCompatibilityRootGetsDefaultComponentName)
    {
        RVX::SceneEntity entity("NamedSceneEntity");

        ASSERT_NE(nullptr, entity.GetRootComponent());
        EXPECT_EQ(std::string("SceneComponent"), entity.GetRootComponent()->GetName());
    }

    class CountingComponent : public RVX::ActorComponent
    {
    public:
        const char* GetClassName() const override { return "CountingComponent"; }

        void OnComponentCreated() override { ++created; }
        void OnRegister() override { ++registered; }
        void InitializeComponent() override { ++initialized; }
        void BeginPlay() override { ++beganPlay; }
        void TickComponent(float) override { ++ticked; }
        void EndPlay() override { ++endedPlay; }
        void OnUnregister() override { ++unregistered; }
        void OnComponentDestroyed() override { ++destroyed; }

        int created = 0;
        int registered = 0;
        int initialized = 0;
        int beganPlay = 0;
        int ticked = 0;
        int endedPlay = 0;
        int unregistered = 0;
        int destroyed = 0;
    };

    bool IsNear(float a, float b, float epsilon = 0.0001f)
    {
        return std::abs(a - b) <= epsilon;
    }

    struct RuntimeLifecycleCounters
    {
        int beganPlay = 0;
        int ticked = 0;
        int endedPlay = 0;
        int unregistered = 0;
        float lastDeltaTime = 0.0f;
        std::vector<std::string> events;
    };

    struct LegacyLifecycleCounters
    {
        int created = 0;
        int attached = 0;
        int registered = 0;
        int initialized = 0;
        int beganPlay = 0;
        int ticked = 0;
        int endedPlay = 0;
        int unregistered = 0;
        int detached = 0;
        int destroyed = 0;
        float lastDeltaTime = 0.0f;
        std::vector<std::string> events;
    };

    class SceneManagedActorComponent : public RVX::ActorComponent
    {
    public:
        explicit SceneManagedActorComponent(RuntimeLifecycleCounters* counters)
            : m_counters(counters)
        {
            SetCanEverTick(true);
            SetTickEnabled(true);
        }

        const char* GetClassName() const override { return "SceneManagedActorComponent"; }

        void BeginPlay() override
        {
            if (m_counters)
            {
                ++m_counters->beganPlay;
                m_counters->events.push_back("BeginPlay");
            }
        }

        void TickComponent(float deltaTime) override
        {
            if (m_counters)
            {
                ++m_counters->ticked;
                m_counters->lastDeltaTime = deltaTime;
                m_counters->events.push_back("Tick");
            }
        }

        void EndPlay() override
        {
            if (m_counters)
            {
                ++m_counters->endedPlay;
                m_counters->events.push_back("EndPlay");
            }
        }

        void OnUnregister() override
        {
            if (m_counters)
            {
                ++m_counters->unregistered;
            }
        }

    private:
        RuntimeLifecycleCounters* m_counters = nullptr;
    };

    class SceneManagedLegacyTickComponent : public RVX::Component
    {
    public:
        explicit SceneManagedLegacyTickComponent(int* externalTickCount = nullptr)
            : m_externalTickCount(externalTickCount)
        {
        }

        const char* GetTypeName() const override { return "SceneManagedLegacyTickComponent"; }

        void Tick(float deltaTime) override
        {
            ++tickCount;
            if (m_externalTickCount)
            {
                ++(*m_externalTickCount);
            }
            lastDeltaTime = deltaTime;
        }

        int tickCount = 0;
        float lastDeltaTime = 0.0f;

    private:
        int* m_externalTickCount = nullptr;
    };

    class SceneManagedLegacyLifecycleComponent : public RVX::Component
    {
    public:
        explicit SceneManagedLegacyLifecycleComponent(LegacyLifecycleCounters* counters)
            : m_counters(counters)
        {
        }

        const char* GetTypeName() const override { return "SceneManagedLegacyLifecycleComponent"; }

        void OnComponentCreated() override
        {
            if (m_counters)
            {
                ++m_counters->created;
                m_counters->events.push_back("Created");
            }
        }

        void OnAttach() override
        {
            if (m_counters)
            {
                ++m_counters->attached;
                m_counters->events.push_back("Attach");
            }
        }

        void OnRegister() override
        {
            if (m_counters)
            {
                ++m_counters->registered;
                m_counters->events.push_back("Register");
            }
        }

        void InitializeComponent() override
        {
            if (m_counters)
            {
                ++m_counters->initialized;
                m_counters->events.push_back("Initialize");
            }
        }

        void BeginPlay() override
        {
            if (m_counters)
            {
                ++m_counters->beganPlay;
                m_counters->events.push_back("BeginPlay");
            }
        }

        void Tick(float deltaTime) override
        {
            if (m_counters)
            {
                ++m_counters->ticked;
                m_counters->lastDeltaTime = deltaTime;
                m_counters->events.push_back("Tick");
            }
        }

        void EndPlay() override
        {
            if (m_counters)
            {
                ++m_counters->endedPlay;
                m_counters->events.push_back("EndPlay");
            }
        }

        void OnUnregister() override
        {
            if (m_counters)
            {
                ++m_counters->unregistered;
                m_counters->events.push_back("Unregister");
            }
        }

        void OnDetach() override
        {
            if (m_counters)
            {
                ++m_counters->detached;
                m_counters->events.push_back("Detach");
            }
        }

        void OnComponentDestroyed() override
        {
            if (m_counters)
            {
                ++m_counters->destroyed;
                m_counters->events.push_back("Destroy");
            }
        }

    private:
        LegacyLifecycleCounters* m_counters = nullptr;
    };

    class AddLegacyDuringTickComponent : public RVX::Component
    {
    public:
        explicit AddLegacyDuringTickComponent(int* addedTickCount)
            : m_addedTickCount(addedTickCount)
        {
        }

        const char* GetTypeName() const override { return "AddLegacyDuringTickComponent"; }

        void Tick(float) override
        {
            if (!m_added && GetOwner())
            {
                GetOwner()->AddComponent<SceneManagedLegacyTickComponent>(m_addedTickCount);
                m_added = true;
            }
        }

    private:
        int* m_addedTickCount = nullptr;
        bool m_added = false;
    };

    class SelfRemovingLegacyTickComponent : public RVX::Component
    {
    public:
        explicit SelfRemovingLegacyTickComponent(int* tickCount)
            : m_tickCount(tickCount)
        {
        }

        const char* GetTypeName() const override { return "SelfRemovingLegacyTickComponent"; }

        void Tick(float) override
        {
            if (m_tickCount)
            {
                ++(*m_tickCount);
            }
            if (GetOwner())
            {
                GetOwner()->RemoveComponent<SelfRemovingLegacyTickComponent>();
            }
        }

    private:
        int* m_tickCount = nullptr;
    };

    class VictimLegacyTickComponent : public RVX::Component
    {
    public:
        explicit VictimLegacyTickComponent(int* tickCount)
            : m_tickCount(tickCount)
        {
        }

        const char* GetTypeName() const override { return "VictimLegacyTickComponent"; }

        void Tick(float) override
        {
            if (m_tickCount)
            {
                ++(*m_tickCount);
            }
        }

    private:
        int* m_tickCount = nullptr;
    };

    class RemoveVictimLegacyDuringTickComponent : public RVX::Component
    {
    public:
        const char* GetTypeName() const override { return "RemoveVictimLegacyDuringTickComponent"; }

        void Tick(float) override
        {
            if (GetOwner())
            {
                GetOwner()->RemoveComponent<VictimLegacyTickComponent>();
            }
        }
    };

    class SelfDestroyingActorComponent : public RVX::ActorComponent
    {
    public:
        SelfDestroyingActorComponent(RVX::SceneManager* sceneManager,
                                     RVX::SceneEntity::Handle ownerHandle,
                                     int* tickCount)
            : m_sceneManager(sceneManager)
            , m_ownerHandle(ownerHandle)
            , m_tickCount(tickCount)
        {
            SetCanEverTick(true);
            SetTickEnabled(true);
        }

        const char* GetClassName() const override { return "SelfDestroyingActorComponent"; }

        void TickComponent(float) override
        {
            if (m_tickCount)
            {
                ++(*m_tickCount);
            }
            if (m_sceneManager)
            {
                m_sceneManager->DestroyEntity(m_ownerHandle);
            }
        }

    private:
        RVX::SceneManager* m_sceneManager = nullptr;
        RVX::SceneEntity::Handle m_ownerHandle = RVX::SceneEntity::InvalidHandle;
        int* m_tickCount = nullptr;
    };

    class SpawnableSceneActor : public RVX::SceneEntity
    {
    public:
        explicit SpawnableSceneActor(const std::string& name)
            : SceneEntity(name)
        {
        }

        const char* GetClassName() const override { return "SpawnableSceneActor"; }
    };

    class DestroyOwnerActorComponent : public RVX::ActorComponent
    {
    public:
        DestroyOwnerActorComponent(RVX::SceneManager* sceneManager,
                                   bool* firstResult,
                                   bool* secondResult = nullptr)
            : m_sceneManager(sceneManager)
            , m_firstResult(firstResult)
            , m_secondResult(secondResult)
        {
            SetCanEverTick(true);
            SetTickEnabled(true);
        }

        const char* GetClassName() const override { return "DestroyOwnerActorComponent"; }

        void TickComponent(float) override
        {
            if (m_hasTicked || !m_sceneManager)
                return;

            m_hasTicked = true;
            if (m_firstResult)
            {
                *m_firstResult = m_sceneManager->DestroyActor(GetOwner());
            }
            if (m_secondResult)
            {
                *m_secondResult = m_sceneManager->DestroyActor(GetOwner());
            }
        }

    private:
        RVX::SceneManager* m_sceneManager = nullptr;
        bool* m_firstResult = nullptr;
        bool* m_secondResult = nullptr;
        bool m_hasTicked = false;
    };

    class WorldManagedPureActor : public RVX::Actor
    {
    public:
        explicit WorldManagedPureActor(const std::string& name)
            : Actor(name)
        {
            auto* root = AddComponent<RVX::SceneComponent>();
            SetRootComponent(root);
        }

        const char* GetClassName() const override { return "WorldManagedPureActor"; }
    };

    struct FactoryLifecycleCounters
    {
        static inline int registered = 0;
        static inline int initialized = 0;
        static inline int beganPlay = 0;
        static inline int ticked = 0;
        static inline int unregistered = 0;

        static void Reset()
        {
            registered = 0;
            initialized = 0;
            beganPlay = 0;
            ticked = 0;
            unregistered = 0;
        }
    };

    class FactoryLifecycleComponent : public RVX::ActorComponent
    {
    public:
        FactoryLifecycleComponent()
        {
            SetCanEverTick(true);
            SetTickEnabled(true);
        }

        const char* GetClassName() const override { return "FactoryLifecycleComponent"; }
        void OnRegister() override { ++FactoryLifecycleCounters::registered; }
        void InitializeComponent() override { ++FactoryLifecycleCounters::initialized; }
        void BeginPlay() override { ++FactoryLifecycleCounters::beganPlay; }
        void TickComponent(float) override { ++FactoryLifecycleCounters::ticked; }
        void OnUnregister() override { ++FactoryLifecycleCounters::unregistered; }
    };

    class FactorySpawnSceneActor : public RVX::SceneEntity
    {
    public:
        FactorySpawnSceneActor()
            : SceneEntity("FactorySpawnSceneActorDefault")
        {
            AddComponent<FactoryLifecycleComponent>();
        }

        const char* GetClassName() const override { return "FactorySpawnSceneActor"; }
    };

    class FactorySpawnPureActor : public RVX::Actor
    {
    public:
        FactorySpawnPureActor()
            : Actor("FactorySpawnPureActorDefault")
        {
            auto* root = AddComponent<RVX::SceneComponent>();
            SetRootComponent(root);
            AddComponent<FactoryLifecycleComponent>();
        }

        const char* GetClassName() const override { return "FactorySpawnPureActor"; }
    };

    struct WorldActorLifecycleCounters
    {
        int registered = 0;
        int initialized = 0;
        int beganPlay = 0;
        int ticked = 0;
        int endedPlay = 0;
        int unregistered = 0;
        int destroyed = 0;
    };

    class WorldActorLifecycleComponent : public RVX::ActorComponent
    {
    public:
        explicit WorldActorLifecycleComponent(WorldActorLifecycleCounters* counters)
            : m_counters(counters)
        {
            SetCanEverTick(true);
            SetTickEnabled(true);
        }

        const char* GetClassName() const override { return "WorldActorLifecycleComponent"; }
        void OnRegister() override { if (m_counters) ++m_counters->registered; }
        void InitializeComponent() override { if (m_counters) ++m_counters->initialized; }
        void BeginPlay() override { if (m_counters) ++m_counters->beganPlay; }
        void TickComponent(float) override { if (m_counters) ++m_counters->ticked; }
        void EndPlay() override { if (m_counters) ++m_counters->endedPlay; }
        void OnUnregister() override { if (m_counters) ++m_counters->unregistered; }
        void OnComponentDestroyed() override { if (m_counters) ++m_counters->destroyed; }

    private:
        WorldActorLifecycleCounters* m_counters = nullptr;
    };

    class WorldActorSelfDestroyComponent : public RVX::ActorComponent
    {
    public:
        WorldActorSelfDestroyComponent(RVX::World* world, bool* firstResult, bool* secondResult = nullptr)
            : m_world(world)
            , m_firstResult(firstResult)
            , m_secondResult(secondResult)
        {
            SetCanEverTick(true);
            SetTickEnabled(true);
        }

        const char* GetClassName() const override { return "WorldActorSelfDestroyComponent"; }

        void TickComponent(float) override
        {
            if (m_hasTicked || !m_world)
                return;

            m_hasTicked = true;
            if (m_firstResult)
            {
                *m_firstResult = m_world->DestroyActor(GetOwner());
            }
            if (m_secondResult)
            {
                *m_secondResult = m_world->DestroyActor(GetOwner());
            }
        }

    private:
        RVX::World* m_world = nullptr;
        bool* m_firstResult = nullptr;
        bool* m_secondResult = nullptr;
        bool m_hasTicked = false;
    };

    class TickCounterComponent : public RVX::ActorComponent
    {
    public:
        explicit TickCounterComponent(int* tickCount = nullptr)
            : m_tickCount(tickCount)
        {
            SetCanEverTick(true);
            SetTickEnabled(true);
        }

        const char* GetClassName() const override { return "TickCounterComponent"; }

        void BeginPlay() override
        {
            if (beginCount)
            {
                ++(*beginCount);
            }
        }

        void TickComponent(float) override
        {
            if (m_tickCount)
            {
                ++(*m_tickCount);
            }
        }

        int* beginCount = nullptr;

    private:
        int* m_tickCount = nullptr;
    };

    class AddComponentDuringTickComponent : public RVX::ActorComponent
    {
    public:
        explicit AddComponentDuringTickComponent(int* addedTickCount)
            : m_addedTickCount(addedTickCount)
        {
            SetCanEverTick(true);
            SetTickEnabled(true);
        }

        const char* GetClassName() const override { return "AddComponentDuringTickComponent"; }

        void TickComponent(float) override
        {
            if (!m_added && GetOwner())
            {
                GetOwner()->AddComponent<TickCounterComponent>(m_addedTickCount);
                m_added = true;
            }
        }

    private:
        int* m_addedTickCount = nullptr;
        bool m_added = false;
    };

    class AddComponentDuringBeginPlayComponent : public RVX::ActorComponent
    {
    public:
        explicit AddComponentDuringBeginPlayComponent(int* addedBeginCount)
            : m_addedBeginCount(addedBeginCount)
        {
        }

        const char* GetClassName() const override { return "AddComponentDuringBeginPlayComponent"; }

        void BeginPlay() override
        {
            if (!m_added && GetOwner())
            {
                auto* added = GetOwner()->AddComponent<TickCounterComponent>();
                added->beginCount = m_addedBeginCount;
                m_added = true;
            }
        }

    private:
        int* m_addedBeginCount = nullptr;
        bool m_added = false;
    };

    class SelfRemovingTickComponent : public RVX::ActorComponent
    {
    public:
        explicit SelfRemovingTickComponent(int* tickCount, int* endPlayCount = nullptr)
            : m_tickCount(tickCount)
            , m_endPlayCount(endPlayCount)
        {
            SetCanEverTick(true);
            SetTickEnabled(true);
        }

        const char* GetClassName() const override { return "SelfRemovingTickComponent"; }

        void TickComponent(float) override
        {
            if (m_tickCount)
            {
                ++(*m_tickCount);
            }
            if (GetOwner())
            {
                GetOwner()->RemoveComponent<SelfRemovingTickComponent>();
            }
        }

        void EndPlay() override
        {
            if (m_endPlayCount)
            {
                ++(*m_endPlayCount);
            }
        }

    private:
        int* m_tickCount = nullptr;
        int* m_endPlayCount = nullptr;
    };

    class VictimTickComponent : public RVX::ActorComponent
    {
    public:
        explicit VictimTickComponent(int* tickCount)
            : m_tickCount(tickCount)
        {
            SetCanEverTick(true);
            SetTickEnabled(true);
        }

        const char* GetClassName() const override { return "VictimTickComponent"; }

        void TickComponent(float) override
        {
            if (m_tickCount)
            {
                ++(*m_tickCount);
            }
        }

    private:
        int* m_tickCount = nullptr;
    };

    class RemoveVictimDuringTickComponent : public RVX::ActorComponent
    {
    public:
        RemoveVictimDuringTickComponent()
        {
            SetCanEverTick(true);
            SetTickEnabled(true);
        }

        const char* GetClassName() const override { return "RemoveVictimDuringTickComponent"; }

        void TickComponent(float) override
        {
            if (GetOwner())
            {
                GetOwner()->RemoveComponent<VictimTickComponent>();
            }
        }
    };

    struct EndPlayMutationCounters
    {
        int removerEndPlay = 0;
        int victimEndPlay = 0;
        int victimDestroyed = 0;
    };

    class EndPlayVictimComponent : public RVX::ActorComponent
    {
    public:
        explicit EndPlayVictimComponent(EndPlayMutationCounters* counters)
            : m_counters(counters)
        {
        }

        const char* GetClassName() const override { return "EndPlayVictimComponent"; }

        void EndPlay() override
        {
            if (m_counters)
            {
                ++m_counters->victimEndPlay;
            }
        }

        void OnComponentDestroyed() override
        {
            if (m_counters)
            {
                ++m_counters->victimDestroyed;
            }
        }

    private:
        EndPlayMutationCounters* m_counters = nullptr;
    };

    class SelfRemovingEndPlayComponent : public RVX::ActorComponent
    {
    public:
        explicit SelfRemovingEndPlayComponent(int* endPlayCount)
            : m_endPlayCount(endPlayCount)
        {
        }

        const char* GetClassName() const override { return "SelfRemovingEndPlayComponent"; }

        void EndPlay() override
        {
            if (m_endPlayCount)
            {
                ++(*m_endPlayCount);
            }
            if (GetOwner())
            {
                GetOwner()->RemoveComponent<SelfRemovingEndPlayComponent>();
            }
        }

    private:
        int* m_endPlayCount = nullptr;
    };

    class RemoveVictimDuringEndPlayComponent : public RVX::ActorComponent
    {
    public:
        explicit RemoveVictimDuringEndPlayComponent(EndPlayMutationCounters* counters)
            : m_counters(counters)
        {
        }

        const char* GetClassName() const override { return "RemoveVictimDuringEndPlayComponent"; }

        void EndPlay() override
        {
            if (m_counters)
            {
                ++m_counters->removerEndPlay;
            }
            if (GetOwner())
            {
                GetOwner()->RemoveComponent<EndPlayVictimComponent>();
            }
        }

    private:
        EndPlayMutationCounters* m_counters = nullptr;
    };

    class EndPlayThenRemoveVictimDuringTickComponent : public RVX::ActorComponent
    {
    public:
        EndPlayThenRemoveVictimDuringTickComponent()
        {
            SetCanEverTick(true);
            SetTickEnabled(true);
        }

        const char* GetClassName() const override { return "EndPlayThenRemoveVictimDuringTickComponent"; }

        void TickComponent(float) override
        {
            if (!m_ran && GetOwner())
            {
                m_ran = true;
                GetOwner()->EndPlay();
                GetOwner()->RemoveComponent<EndPlayVictimComponent>();
            }
        }

    private:
        bool m_ran = false;
    };

    class QueueVictimThenEndPlayDuringTickComponent : public RVX::ActorComponent
    {
    public:
        QueueVictimThenEndPlayDuringTickComponent()
        {
            SetCanEverTick(true);
            SetTickEnabled(true);
        }

        const char* GetClassName() const override { return "QueueVictimThenEndPlayDuringTickComponent"; }

        void TickComponent(float) override
        {
            if (!m_ran && GetOwner())
            {
                m_ran = true;
                GetOwner()->RemoveComponent<EndPlayVictimComponent>();
                GetOwner()->EndPlay();
            }
        }

    private:
        bool m_ran = false;
    };

    class DestructionPendingRemovalComponent : public RVX::ActorComponent
    {
    public:
        explicit DestructionPendingRemovalComponent(int* destroyedCount)
            : m_destroyedCount(destroyedCount)
        {
            SetCanEverTick(true);
            SetTickEnabled(true);
        }

        const char* GetClassName() const override { return "DestructionPendingRemovalComponent"; }

        void TickComponent(float) override
        {
            if (GetOwner())
            {
                GetOwner()->RemoveComponent<DestructionPendingRemovalComponent>();
            }
        }

        void OnComponentDestroyed() override
        {
            if (m_destroyedCount)
            {
                ++(*m_destroyedCount);
            }
        }

    private:
        int* m_destroyedCount = nullptr;
    };

    TEST(ActorComponentValidation, ActorOwnsComponentsAndDispatchesLifecycle)
    {
        RVX::Actor actor("LifecycleActor");
        auto* component = actor.AddComponent<CountingComponent>();
        component->SetCanEverTick(true);
        component->SetTickEnabled(true);

        EXPECT_EQ(&actor, component->GetOwner());
        EXPECT_EQ(1, component->created);

        actor.RegisterAllComponents();
        EXPECT_TRUE(component->IsRegistered());
        EXPECT_EQ(1, component->registered);
        EXPECT_EQ(1, component->initialized);

        actor.BeginPlay();
        actor.Tick(0.25f);
        actor.EndPlay();
        actor.UnregisterAllComponents();

        EXPECT_EQ(1, component->beganPlay);
        EXPECT_EQ(1, component->ticked);
        EXPECT_EQ(1, component->endedPlay);
        EXPECT_EQ(1, component->unregistered);
    }

    TEST(ActorComponentValidation, SceneManagerUpdateDispatchesActorComponentLifecycle)
    {
        RVX::SceneManager sceneManager;
        sceneManager.Initialize();

        const auto handle = sceneManager.CreateEntity("RuntimeLifecycleEntity");
        auto* entity = sceneManager.GetEntity(handle);
        ASSERT_NE(nullptr, entity);

        RuntimeLifecycleCounters counters;
        auto component = std::make_unique<SceneManagedActorComponent>(&counters);
        auto* raw = component.get();
        EXPECT_EQ(raw, static_cast<RVX::Actor*>(entity)->AddOwnedComponent(std::move(component)));

        sceneManager.Update(0.5f);
        sceneManager.Update(0.25f);

        EXPECT_TRUE(raw->HasBegunPlay());
        EXPECT_EQ(1, counters.beganPlay);
        EXPECT_EQ(2, counters.ticked);
        EXPECT_TRUE(IsNear(0.25f, counters.lastDeltaTime));
        EXPECT_EQ(static_cast<size_t>(3), counters.events.size());
        EXPECT_EQ(std::string("BeginPlay"), counters.events[0]);
        EXPECT_EQ(std::string("Tick"), counters.events[1]);
        EXPECT_EQ(std::string("Tick"), counters.events[2]);

        sceneManager.Shutdown();
    }

    TEST(ActorComponentValidation, SceneManagerBeginsComponentsAddedAfterActorBeginsPlay)
    {
        RVX::SceneManager sceneManager;
        sceneManager.Initialize();

        const auto handle = sceneManager.CreateEntity("LateComponentEntity");
        auto* entity = sceneManager.GetEntity(handle);
        ASSERT_NE(nullptr, entity);

        sceneManager.Update(0.016f);
        EXPECT_TRUE(static_cast<RVX::Actor*>(entity)->HasBegunPlay());

        RuntimeLifecycleCounters counters;
        auto component = std::make_unique<SceneManagedActorComponent>(&counters);
        auto* raw = component.get();
        EXPECT_EQ(raw, static_cast<RVX::Actor*>(entity)->AddOwnedComponent(std::move(component)));
        EXPECT_FALSE(raw->HasBegunPlay());

        sceneManager.Update(0.033f);

        EXPECT_TRUE(raw->HasBegunPlay());
        EXPECT_EQ(1, counters.beganPlay);
        EXPECT_EQ(1, counters.ticked);
        EXPECT_TRUE(IsNear(0.033f, counters.lastDeltaTime));
        EXPECT_EQ(static_cast<size_t>(2), counters.events.size());
        EXPECT_EQ(std::string("BeginPlay"), counters.events[0]);
        EXPECT_EQ(std::string("Tick"), counters.events[1]);

        sceneManager.Shutdown();
    }

    TEST(ActorComponentValidation, SceneManagerUpdateKeepsLegacyComponentTicking)
    {
        RVX::SceneManager sceneManager;
        sceneManager.Initialize();

        const auto handle = sceneManager.CreateEntity("LegacyTickEntity");
        auto* entity = sceneManager.GetEntity(handle);
        ASSERT_NE(nullptr, entity);

        auto* component = entity->AddComponent<SceneManagedLegacyTickComponent>();
        ASSERT_NE(nullptr, component);

        sceneManager.Update(0.125f);
        EXPECT_EQ(1, component->tickCount);
        EXPECT_TRUE(IsNear(0.125f, component->lastDeltaTime));

        entity->SetActive(false);
        sceneManager.Update(0.5f);
        EXPECT_EQ(1, component->tickCount);

        entity->SetActive(true);
        sceneManager.Update(0.25f);
        EXPECT_EQ(2, component->tickCount);
        EXPECT_TRUE(IsNear(0.25f, component->lastDeltaTime));

        sceneManager.Shutdown();
    }

    TEST(ActorComponentValidation, SceneManagerDispatchesLegacyComponentFullLifecycle)
    {
        RVX::SceneManager sceneManager;
        sceneManager.Initialize();

        auto entity = std::make_shared<RVX::SceneEntity>("LegacyLifecycleEntity");
        LegacyLifecycleCounters counters;
        auto* component = entity->AddComponent<SceneManagedLegacyLifecycleComponent>(&counters);
        ASSERT_NE(nullptr, component);

        sceneManager.AddEntity(entity);
        EXPECT_TRUE(component->IsRegistered());
        EXPECT_TRUE(component->IsInitialized());
        EXPECT_EQ(1, counters.registered);
        EXPECT_EQ(1, counters.initialized);

        sceneManager.Update(0.125f);
        EXPECT_TRUE(component->HasBegunPlay());
        EXPECT_EQ(1, counters.beganPlay);
        EXPECT_EQ(1, counters.ticked);
        EXPECT_TRUE(IsNear(0.125f, counters.lastDeltaTime));

        sceneManager.DestroyEntity(entity->GetHandle());
        EXPECT_EQ(1, counters.endedPlay);
        EXPECT_EQ(1, counters.unregistered);

        entity.reset();
        EXPECT_EQ(1, counters.detached);
        EXPECT_EQ(1, counters.destroyed);

        sceneManager.Shutdown();
    }

    TEST(ActorComponentValidation, SceneManagerBeginsLegacyComponentAddedAfterEntityBeginsPlay)
    {
        RVX::SceneManager sceneManager;
        sceneManager.Initialize();

        const auto handle = sceneManager.CreateEntity("LateLegacyLifecycleEntity");
        auto* entity = sceneManager.GetEntity(handle);
        ASSERT_NE(nullptr, entity);

        sceneManager.Update(0.016f);
        EXPECT_TRUE(entity->HasBegunPlay());

        LegacyLifecycleCounters counters;
        auto* component = entity->AddComponent<SceneManagedLegacyLifecycleComponent>(&counters);
        ASSERT_NE(nullptr, component);
        EXPECT_TRUE(component->IsRegistered());
        EXPECT_TRUE(component->IsInitialized());
        EXPECT_FALSE(component->HasBegunPlay());

        sceneManager.Update(0.033f);
        EXPECT_TRUE(component->HasBegunPlay());
        EXPECT_EQ(1, counters.beganPlay);
        EXPECT_EQ(1, counters.ticked);
        EXPECT_TRUE(IsNear(0.033f, counters.lastDeltaTime));

        sceneManager.Shutdown();
    }

    TEST(ActorComponentValidation, SceneEntityActorPointerTickDispatchesLegacyComponents)
    {
        RVX::SceneEntity entity("LegacyActorPointerTickEntity");
        RVX::Actor* actor = &entity;

        LegacyLifecycleCounters counters;
        auto* component = entity.AddComponent<SceneManagedLegacyLifecycleComponent>(&counters);
        ASSERT_NE(nullptr, component);

        actor->Tick(0.5f);
        EXPECT_EQ(1, counters.ticked);
        EXPECT_TRUE(IsNear(0.5f, counters.lastDeltaTime));
    }

    TEST(ActorComponentValidation, SceneEntityLegacyRemovalDispatchesLifecycleCleanup)
    {
        RVX::SceneManager sceneManager;
        sceneManager.Initialize();

        const auto handle = sceneManager.CreateEntity("LegacyRemovalLifecycleEntity");
        auto* entity = sceneManager.GetEntity(handle);
        ASSERT_NE(nullptr, entity);

        LegacyLifecycleCounters counters;
        auto* component = entity->AddComponent<SceneManagedLegacyLifecycleComponent>(&counters);
        ASSERT_NE(nullptr, component);
        sceneManager.Update(0.016f);

        EXPECT_TRUE(entity->RemoveComponent<SceneManagedLegacyLifecycleComponent>());
        EXPECT_EQ(nullptr, entity->GetComponent<SceneManagedLegacyLifecycleComponent>());
        EXPECT_EQ(1, counters.endedPlay);
        EXPECT_EQ(1, counters.unregistered);
        EXPECT_EQ(1, counters.detached);
        EXPECT_EQ(1, counters.destroyed);

        sceneManager.Shutdown();
    }

    TEST(ActorComponentValidation, SceneEntityLegacyTickSnapshotsComponentsAddedDuringTick)
    {
        RVX::SceneEntity entity("LegacyAddDuringTickEntity");
        int addedTickCount = 0;

        auto* spawner = entity.AddComponent<AddLegacyDuringTickComponent>(&addedTickCount);
        ASSERT_NE(nullptr, spawner);

        entity.TickComponents(0.016f);
        EXPECT_EQ(0, addedTickCount);
        EXPECT_EQ(static_cast<size_t>(2), entity.GetComponentCount());

        entity.TickComponents(0.016f);
        EXPECT_EQ(1, addedTickCount);
    }

    TEST(ActorComponentValidation, SceneEntityLegacyDefersSelfRemovalDuringTick)
    {
        RVX::SceneEntity entity("LegacySelfRemoveEntity");
        int tickCount = 0;

        auto* component = entity.AddComponent<SelfRemovingLegacyTickComponent>(&tickCount);
        ASSERT_NE(nullptr, component);

        entity.TickComponents(0.016f);

        EXPECT_EQ(1, tickCount);
        EXPECT_EQ(static_cast<size_t>(0), entity.GetComponentCount());

        entity.TickComponents(0.016f);
        EXPECT_EQ(1, tickCount);
    }

    TEST(ActorComponentValidation, SceneEntityLegacySkipsComponentRemovedEarlierInTick)
    {
        RVX::SceneEntity entity("LegacyRemoveOtherEntity");
        int victimTickCount = 0;

        auto* remover = entity.AddComponent<RemoveVictimLegacyDuringTickComponent>();
        ASSERT_NE(nullptr, remover);
        auto* victim = entity.AddComponent<VictimLegacyTickComponent>(&victimTickCount);
        ASSERT_NE(nullptr, victim);

        entity.TickComponents(0.016f);

        EXPECT_EQ(0, victimTickCount);
        EXPECT_EQ(nullptr, entity.GetComponent<VictimLegacyTickComponent>());
    }

    TEST(ActorComponentValidation, SceneEntityActiveStateControlsActorTickThroughActorPointer)
    {
        RVX::SceneEntity entity("DirectActorTickEntity");
        RVX::Actor* actor = &entity;

        RuntimeLifecycleCounters counters;
        auto component = std::make_unique<SceneManagedActorComponent>(&counters);
        auto* raw = component.get();
        EXPECT_EQ(raw, actor->AddOwnedComponent(std::move(component)));

        entity.SetActive(false);
        actor->Tick(0.25f);
        EXPECT_EQ(0, counters.ticked);

        actor->SetActive(true);
        actor->Tick(0.5f);
        EXPECT_EQ(1, counters.ticked);
        EXPECT_TRUE(IsNear(0.5f, counters.lastDeltaTime));
    }

    TEST(ActorComponentValidation, SceneManagerDefersDestroyRequestsDuringLifecycleDispatch)
    {
        RVX::SceneManager sceneManager;
        sceneManager.Initialize();

        const auto handle = sceneManager.CreateEntity("SelfDestroyingEntity");
        auto* entity = sceneManager.GetEntity(handle);
        ASSERT_NE(nullptr, entity);

        int tickCount = 0;
        auto component = std::make_unique<SelfDestroyingActorComponent>(&sceneManager, handle, &tickCount);
        ASSERT_NE(nullptr, static_cast<RVX::Actor*>(entity)->AddOwnedComponent(std::move(component)));
        int legacyTickCount = 0;
        auto* legacyComponent = entity->AddComponent<SceneManagedLegacyTickComponent>(&legacyTickCount);
        ASSERT_NE(nullptr, legacyComponent);

        sceneManager.Update(0.016f);

        EXPECT_EQ(1, tickCount);
        EXPECT_EQ(0, legacyTickCount);
        EXPECT_EQ(nullptr, sceneManager.GetEntity(handle));

        sceneManager.Shutdown();
    }

    TEST(ActorComponentValidation, SceneManagerDestroyEntityDispatchesEndPlayBeforeUnregister)
    {
        RVX::SceneManager sceneManager;
        sceneManager.Initialize();

        const auto handle = sceneManager.CreateEntity("DestroyLifecycleEntity");
        auto* entity = sceneManager.GetEntity(handle);
        ASSERT_NE(nullptr, entity);

        RuntimeLifecycleCounters counters;
        auto component = std::make_unique<SceneManagedActorComponent>(&counters);
        ASSERT_NE(nullptr, static_cast<RVX::Actor*>(entity)->AddOwnedComponent(std::move(component)));

        sceneManager.Update(0.016f);
        EXPECT_EQ(1, counters.beganPlay);

        sceneManager.DestroyEntity(handle);

        EXPECT_EQ(1, counters.endedPlay);
        EXPECT_EQ(1, counters.unregistered);
        sceneManager.Shutdown();
    }

    TEST(ActorComponentValidation, SceneManagerDestroyEntityBeforeBeginPlayDoesNotDispatchEndPlay)
    {
        RVX::SceneManager sceneManager;
        sceneManager.Initialize();

        const auto handle = sceneManager.CreateEntity("NeverBegunLifecycleEntity");
        auto* entity = sceneManager.GetEntity(handle);
        ASSERT_NE(nullptr, entity);

        RuntimeLifecycleCounters counters;
        auto component = std::make_unique<SceneManagedActorComponent>(&counters);
        ASSERT_NE(nullptr, static_cast<RVX::Actor*>(entity)->AddOwnedComponent(std::move(component)));

        entity->SetActive(false);
        sceneManager.DestroyEntity(handle);

        EXPECT_EQ(0, counters.beganPlay);
        EXPECT_EQ(0, counters.ticked);
        EXPECT_EQ(0, counters.endedPlay);
        EXPECT_EQ(1, counters.unregistered);
        sceneManager.Shutdown();
    }

    TEST(ActorComponentValidation, ActorTickSnapshotsComponentsAddedDuringTick)
    {
        RVX::Actor actor("AddDuringTickActor");
        int addedTickCount = 0;
        actor.AddComponent<AddComponentDuringTickComponent>(&addedTickCount);

        actor.Tick(0.016f);
        EXPECT_EQ(0, addedTickCount);
        EXPECT_EQ(static_cast<size_t>(2), actor.GetActorComponentCount());

        actor.Tick(0.016f);
        EXPECT_EQ(1, addedTickCount);
    }

    TEST(ActorComponentValidation, ActorDefersSelfRemovalDuringTick)
    {
        RVX::Actor actor("SelfRemoveActor");
        int tickCount = 0;
        actor.AddComponent<SelfRemovingTickComponent>(&tickCount);

        actor.Tick(0.016f);

        EXPECT_EQ(1, tickCount);
        EXPECT_EQ(static_cast<size_t>(0), actor.GetActorComponentCount());

        actor.Tick(0.016f);
        EXPECT_EQ(1, tickCount);
    }

    TEST(ActorComponentValidation, ActorDispatchesEndPlayForBegunComponentRemovedDuringTick)
    {
        RVX::Actor actor("BegunSelfRemoveActor");
        int tickCount = 0;
        int endPlayCount = 0;
        actor.AddComponent<SelfRemovingTickComponent>(&tickCount, &endPlayCount);

        actor.BeginPlay();
        actor.Tick(0.016f);

        EXPECT_EQ(1, tickCount);
        EXPECT_EQ(1, endPlayCount);
        EXPECT_EQ(static_cast<size_t>(0), actor.GetActorComponentCount());
    }

    TEST(ActorComponentValidation, ActorSkipsComponentRemovedEarlierInTick)
    {
        RVX::Actor actor("RemoveOtherActor");
        int victimTickCount = 0;
        actor.AddComponent<RemoveVictimDuringTickComponent>();
        actor.AddComponent<VictimTickComponent>(&victimTickCount);

        actor.Tick(0.016f);

        EXPECT_EQ(0, victimTickCount);
        EXPECT_EQ(nullptr, actor.GetComponent<VictimTickComponent>());
    }

    TEST(ActorComponentValidation, ActorBeginPlaySnapshotsComponentsAddedDuringBeginPlay)
    {
        RVX::Actor actor("AddDuringBeginPlayActor");
        int addedBeginCount = 0;
        actor.AddComponent<AddComponentDuringBeginPlayComponent>(&addedBeginCount);

        actor.BeginPlay();
        EXPECT_EQ(0, addedBeginCount);
        EXPECT_EQ(static_cast<size_t>(2), actor.GetActorComponentCount());

        actor.BeginPlay();
        EXPECT_EQ(1, addedBeginCount);
    }

    TEST(ActorComponentValidation, ActorDefersSelfRemovalDuringEndPlay)
    {
        RVX::Actor actor("SelfRemoveEndPlayActor");
        int endPlayCount = 0;
        actor.AddComponent<SelfRemovingEndPlayComponent>(&endPlayCount);

        actor.BeginPlay();
        actor.EndPlay();

        EXPECT_EQ(1, endPlayCount);
        EXPECT_EQ(static_cast<size_t>(0), actor.GetActorComponentCount());
    }

    TEST(ActorComponentValidation, ActorDirectRemovalHandlesSelfRemovalDuringEndPlay)
    {
        RVX::Actor actor("DirectSelfRemoveEndPlayActor");
        int endPlayCount = 0;
        actor.AddComponent<SelfRemovingEndPlayComponent>(&endPlayCount);

        actor.BeginPlay();
        EXPECT_TRUE(actor.RemoveComponent<SelfRemovingEndPlayComponent>());

        EXPECT_EQ(1, endPlayCount);
        EXPECT_EQ(static_cast<size_t>(0), actor.GetActorComponentCount());
    }

    TEST(ActorComponentValidation, ActorSkipsComponentRemovedEarlierInEndPlay)
    {
        RVX::Actor actor("RemoveOtherEndPlayActor");
        EndPlayMutationCounters counters;
        actor.AddComponent<EndPlayVictimComponent>(&counters);
        actor.AddComponent<RemoveVictimDuringEndPlayComponent>(&counters);

        actor.BeginPlay();
        actor.EndPlay();

        EXPECT_EQ(1, counters.removerEndPlay);
        EXPECT_EQ(0, counters.victimEndPlay);
        EXPECT_EQ(1, counters.victimDestroyed);
        EXPECT_EQ(nullptr, actor.GetComponent<EndPlayVictimComponent>());
    }

    TEST(ActorComponentValidation, ActorKeepsEndPlaySuppressedAfterNestedEndPlayReturnsToTick)
    {
        RVX::Actor actor("NestedEndPlayRemoveActor");
        EndPlayMutationCounters counters;
        actor.AddComponent<EndPlayVictimComponent>(&counters);
        actor.AddComponent<RemoveVictimDuringEndPlayComponent>(&counters);
        actor.AddComponent<EndPlayThenRemoveVictimDuringTickComponent>();

        actor.BeginPlay();
        actor.Tick(0.016f);

        EXPECT_EQ(1, counters.removerEndPlay);
        EXPECT_EQ(0, counters.victimEndPlay);
        EXPECT_EQ(1, counters.victimDestroyed);
        EXPECT_EQ(nullptr, actor.GetComponent<EndPlayVictimComponent>());
    }

    TEST(ActorComponentValidation, ActorDispatchesEndPlayForComponentQueuedBeforeNestedEndPlay)
    {
        RVX::Actor actor("QueueBeforeNestedEndPlayActor");
        EndPlayMutationCounters counters;
        actor.AddComponent<EndPlayVictimComponent>(&counters);
        actor.AddComponent<RemoveVictimDuringEndPlayComponent>(&counters);
        actor.AddComponent<QueueVictimThenEndPlayDuringTickComponent>();

        actor.BeginPlay();
        actor.Tick(0.016f);

        EXPECT_EQ(1, counters.removerEndPlay);
        EXPECT_EQ(1, counters.victimEndPlay);
        EXPECT_EQ(1, counters.victimDestroyed);
        EXPECT_EQ(nullptr, actor.GetComponent<EndPlayVictimComponent>());
    }

    TEST(ActorComponentValidation, ActorDestructionClearsPendingRemovalWithoutDoubleDestroy)
    {
        int destroyedCount = 0;
        {
            RVX::Actor actor("DestroyPendingRemovalActor");
            actor.AddComponent<DestructionPendingRemovalComponent>(&destroyedCount);
            actor.Tick(0.016f);
        }

        EXPECT_EQ(1, destroyedCount);
    }

    TEST(ActorComponentValidation, SceneComponentAttachmentComputesWorldTransform)
    {
        RVX::Actor actor("TransformActor");
        auto* root = actor.AddComponent<RVX::SceneComponent>();
        auto* child = actor.AddComponent<RVX::SceneComponent>();

        actor.SetRootComponent(root);
        root->SetRelativeLocation(RVX::Vec3(10.0f, 0.0f, 0.0f));
        child->SetRelativeLocation(RVX::Vec3(2.0f, 0.0f, 0.0f));
        EXPECT_TRUE(child->AttachToComponent(root));

        EXPECT_EQ(RVX::Vec3(12.0f, 0.0f, 0.0f), child->GetWorldLocation());
        EXPECT_EQ(root, child->GetAttachParent());
        EXPECT_EQ(static_cast<size_t>(1), root->GetAttachChildren().size());
    }

    TEST(ActorComponentValidation, PrimitiveComponentTracksVisibilityLayerAndBounds)
    {
        RVX::PrimitiveComponent primitive;
        primitive.SetVisible(false);
        primitive.SetLayerMask(0x04);
        primitive.SetLocalBounds(RVX::AABB(RVX::Vec3(-1.0f), RVX::Vec3(1.0f)));

        EXPECT_FALSE(primitive.IsVisible());
        EXPECT_EQ(static_cast<RVX::uint32>(0x04), primitive.GetLayerMask());
        EXPECT_TRUE(primitive.GetLocalBounds().IsValid());
        EXPECT_TRUE(primitive.GetWorldBounds().IsValid());
    }

    TEST(ActorComponentValidation, StaticMeshComponentIsPrimitiveSceneComponent)
    {
        EXPECT_TRUE((std::is_base_of_v<RVX::PrimitiveComponent, RVX::StaticMeshComponent>));

        RVX::StaticMeshComponent component;
        EXPECT_EQ(std::string("StaticMeshComponent"), std::string(component.GetClassName()));
        EXPECT_FALSE(component.HasRenderData());
        EXPECT_FALSE(component.HasValidMesh());
        EXPECT_TRUE(component.IsVisible());
    }

    TEST(ActorComponentValidation, StaticMeshComponentRegistersWithSceneManager)
    {
        RVX::SceneManager sceneManager;
        sceneManager.Initialize();

        auto entity = std::make_shared<RVX::SceneEntity>("PreOwnedPrimitive");
        auto* primitive = static_cast<RVX::Actor*>(entity.get())->AddComponent<RVX::StaticMeshComponent>();
        ASSERT_NE(nullptr, primitive);
        EXPECT_FALSE(primitive->IsRegistered());

        const auto handle = entity->GetHandle();
        sceneManager.AddEntity(entity);

        EXPECT_TRUE(primitive->IsRegistered());
        EXPECT_EQ(static_cast<size_t>(1), sceneManager.GetPrimitives().size());
        EXPECT_EQ(primitive, sceneManager.GetPrimitives()[0]);

        sceneManager.DestroyEntity(handle);

        EXPECT_FALSE(primitive->IsRegistered());
        EXPECT_TRUE(sceneManager.GetPrimitives().empty());
        sceneManager.Shutdown();
    }

    TEST(ActorComponentValidation, RuntimeStaticMeshComponentRemovalUnregistersPrimitive)
    {
        RVX::SceneManager sceneManager;
        sceneManager.Initialize();

        const auto handle = sceneManager.CreateEntity("RuntimePrimitive");
        auto* entity = sceneManager.GetEntity(handle);
        ASSERT_NE(nullptr, entity);

        auto* primitive = static_cast<RVX::Actor*>(entity)->AddComponent<RVX::StaticMeshComponent>();
        ASSERT_NE(nullptr, primitive);
        EXPECT_TRUE(primitive->IsRegistered());
        EXPECT_EQ(static_cast<size_t>(1), sceneManager.GetPrimitives().size());

        EXPECT_TRUE(static_cast<RVX::Actor*>(entity)->RemoveComponent<RVX::StaticMeshComponent>());
        EXPECT_TRUE(sceneManager.GetPrimitives().empty());

        sceneManager.Shutdown();
    }

    TEST(ActorComponentValidation, SceneEntityAddComponentAutoRegistersStaticMeshPrimitive)
    {
        RVX::SceneManager sceneManager;
        sceneManager.Initialize();

        const auto handle = sceneManager.CreateEntity("LegacyAddComponentPrimitive");
        auto* entity = sceneManager.GetEntity(handle);
        ASSERT_NE(nullptr, entity);

        auto* primitive = entity->AddComponent<RVX::StaticMeshComponent>();
        ASSERT_NE(nullptr, primitive);
        EXPECT_TRUE(primitive->IsRegistered());
        EXPECT_EQ(static_cast<size_t>(1), sceneManager.GetPrimitives().size());
        EXPECT_EQ(primitive, sceneManager.GetPrimitives()[0]);

        sceneManager.Shutdown();
    }

    class TestLegacyComponent : public RVX::Component
    {
    public:
        const char* GetTypeName() const override { return "TestLegacyComponent"; }
    };

    TEST(ActorComponentValidation, SceneEntityAndComponentAreActorCompatible)
    {
        EXPECT_TRUE((std::is_base_of_v<RVX::Actor, RVX::SceneEntity>));
        EXPECT_TRUE((std::is_base_of_v<RVX::ActorComponent, RVX::Component>));

        RVX::SceneEntity entity("CompatEntity");
        auto* component = entity.AddComponent<TestLegacyComponent>();
        auto* actorComponent = static_cast<RVX::ActorComponent*>(component);

        ASSERT_NE(nullptr, component);
        EXPECT_EQ(&entity, component->GetOwner());
        EXPECT_EQ(static_cast<RVX::Actor*>(&entity), actorComponent->GetOwner());
    }

    TEST(ActorComponentValidation, ActorAddComponentRejectsLegacyComponentToAvoidContainerSplit)
    {
        RVX::SceneEntity entity("ActorLegacyAddGuardEntity");
        RVX::Actor* actor = &entity;
        const size_t initialActorComponents = actor->GetActorComponentCount();

        auto* component = actor->AddComponent<TestLegacyComponent>();

        EXPECT_EQ(nullptr, component);
        EXPECT_EQ(initialActorComponents, actor->GetActorComponentCount());
        EXPECT_EQ(static_cast<size_t>(0), entity.GetComponentCount());
        EXPECT_EQ(nullptr, entity.GetComponent<TestLegacyComponent>());
    }

    TEST(ActorComponentValidation, ActorAndComponentFactoriesCreateRegisteredTypes)
    {
        RVX::ActorFactory::ClearAll();
        RVX::ActorFactory::Register<RVX::Actor>("Actor");
        auto actor = RVX::ActorFactory::Create("Actor");
        ASSERT_NE(nullptr, actor.get());
        EXPECT_EQ(std::string("Actor"), std::string(actor->GetClassName()));

        RVX::ComponentFactory::ClearComponentClasses();
        RVX::ComponentFactory::RegisterComponentClass<RVX::SceneComponent>("SceneComponent");
        auto component = RVX::ComponentFactory::CreateComponentByClassName("SceneComponent");
        ASSERT_NE(nullptr, component.get());
        EXPECT_EQ(std::string("SceneComponent"), std::string(component->GetClassName()));
    }

    TEST(ActorComponentValidation, ActorTransformForwardsToRootComponent)
    {
        RVX::Actor actor("RootTransformActor");
        auto* root = actor.AddComponent<RVX::SceneComponent>();
        actor.SetRootComponent(root);

        actor.SetPosition(RVX::Vec3(3.0f, 4.0f, 5.0f));
        actor.SetScale(RVX::Vec3(2.0f, 3.0f, 4.0f));

        EXPECT_EQ(RVX::Vec3(3.0f, 4.0f, 5.0f), root->GetRelativeLocation());
        EXPECT_EQ(RVX::Vec3(3.0f, 4.0f, 5.0f), actor.GetWorldPosition());
        EXPECT_EQ(RVX::Vec3(2.0f, 3.0f, 4.0f), root->GetRelativeScale3D());
        EXPECT_EQ(RVX::Vec3(2.0f, 3.0f, 4.0f), actor.GetWorldScale());
        EXPECT_EQ(root->GetWorldTransform(), actor.GetWorldMatrix());
    }

    TEST(ActorComponentValidation, SceneEntityCreatesRootAndForwardsTransform)
    {
        RVX::SceneEntity entity("CompatTransformEntity");
        auto* root = entity.GetRootComponent();
        ASSERT_NE(nullptr, root);

        entity.SetPosition(RVX::Vec3(7.0f, 8.0f, 9.0f));
        entity.SetScale(RVX::Vec3(2.0f, 2.0f, 2.0f));

        EXPECT_EQ(RVX::Vec3(7.0f, 8.0f, 9.0f), entity.GetPosition());
        EXPECT_EQ(RVX::Vec3(7.0f, 8.0f, 9.0f), root->GetRelativeLocation());
        EXPECT_EQ(RVX::Vec3(7.0f, 8.0f, 9.0f), entity.GetWorldPosition());
        EXPECT_EQ(RVX::Vec3(2.0f, 2.0f, 2.0f), entity.GetScale());
        EXPECT_EQ(root->GetWorldTransform(), entity.GetWorldMatrix());
    }

    TEST(ActorComponentValidation, SceneEntityHierarchyAttachesRootComponents)
    {
        RVX::SceneEntity parent("ParentEntity");
        RVX::SceneEntity child("ChildEntity");

        parent.SetPosition(RVX::Vec3(10.0f, 0.0f, 0.0f));
        child.SetPosition(RVX::Vec3(2.0f, 0.0f, 0.0f));

        parent.AddChild(&child);

        EXPECT_EQ(parent.GetRootComponent(), child.GetRootComponent()->GetAttachParent());
        EXPECT_EQ(RVX::Vec3(12.0f, 0.0f, 0.0f), child.GetWorldPosition());

        parent.SetPosition(RVX::Vec3(20.0f, 0.0f, 0.0f));
        EXPECT_EQ(RVX::Vec3(22.0f, 0.0f, 0.0f), child.GetWorldPosition());

        EXPECT_TRUE(parent.RemoveChild(&child));
        EXPECT_EQ(nullptr, child.GetRootComponent()->GetAttachParent());
        EXPECT_EQ(RVX::Vec3(2.0f, 0.0f, 0.0f), child.GetWorldPosition());
    }

    TEST(ActorComponentValidation, SceneEntityTransformStaysSyncedThroughActorAndRootPaths)
    {
        RVX::SceneEntity entity("SyncEntity");
        RVX::Actor* actor = &entity;

        actor->SetPosition(RVX::Vec3(3.0f, 0.0f, 0.0f));
        entity.Translate(RVX::Vec3(2.0f, 0.0f, 0.0f));
        EXPECT_EQ(RVX::Vec3(5.0f, 0.0f, 0.0f), entity.GetPosition());

        entity.GetRootComponent()->SetRelativeLocation(RVX::Vec3(4.0f, 0.0f, 0.0f));
        entity.Translate(RVX::Vec3(1.0f, 0.0f, 0.0f));
        EXPECT_EQ(RVX::Vec3(5.0f, 0.0f, 0.0f), entity.GetPosition());

        entity.GetRootComponent()->SetRelativeLocation(RVX::Vec3(9.0f, 0.0f, 0.0f));
        entity.SetPosition(RVX::Vec3(5.0f, 0.0f, 0.0f));
        EXPECT_EQ(RVX::Vec3(5.0f, 0.0f, 0.0f), entity.GetRootComponent()->GetRelativeLocation());
    }

    TEST(ActorComponentValidation, SceneEntityDestructionMaintainsHierarchy)
    {
        RVX::SceneEntity parent("PersistentParent");
        {
            auto child = std::make_unique<RVX::SceneEntity>("TemporaryChild");
            parent.AddChild(child.get());
            EXPECT_EQ(static_cast<size_t>(1), parent.GetChildCount());
        }
        EXPECT_EQ(static_cast<size_t>(0), parent.GetChildCount());

        auto child = std::make_unique<RVX::SceneEntity>("PersistentChild");
        {
            auto temporaryParent = std::make_unique<RVX::SceneEntity>("TemporaryParent");
            temporaryParent->AddChild(child.get());
            EXPECT_EQ(temporaryParent.get(), child->GetParent());
        }

        EXPECT_EQ(nullptr, child->GetParent());
        EXPECT_EQ(nullptr, child->GetRootComponent()->GetAttachParent());
    }

    TEST(ActorComponentValidation, SceneEntityCompatRootCannotBeRemovedThroughActorAPI)
    {
        RVX::SceneEntity entity("RootProtectedEntity");
        RVX::Actor* actor = &entity;
        auto* root = entity.GetRootComponent();

        ASSERT_NE(nullptr, root);
        EXPECT_FALSE(actor->RemoveComponent<RVX::SceneComponent>());
        EXPECT_EQ(root, entity.GetRootComponent());

        entity.SetPosition(RVX::Vec3(6.0f, 0.0f, 0.0f));
        EXPECT_EQ(RVX::Vec3(6.0f, 0.0f, 0.0f), entity.GetWorldPosition());
    }

    TEST(ActorComponentValidation, SceneEntityRootReplacementKeepsCompatibility)
    {
        RVX::SceneEntity parent("ReplacementParent");
        RVX::SceneEntity child("ReplacementChild");
        RVX::Actor* childActor = &child;

        parent.SetPosition(RVX::Vec3(10.0f, 0.0f, 0.0f));
        child.SetPosition(RVX::Vec3(2.0f, 0.0f, 0.0f));
        parent.AddChild(&child);

        auto* originalRoot = child.GetRootComponent();
        childActor->SetRootComponent(nullptr);
        EXPECT_EQ(originalRoot, child.GetRootComponent());

        auto* replacementRoot = childActor->AddComponent<RVX::SceneComponent>();
        replacementRoot->SetRelativeLocation(RVX::Vec3(3.0f, 0.0f, 0.0f));

        childActor->SetRootComponent(replacementRoot);

        EXPECT_EQ(replacementRoot, child.GetRootComponent());
        EXPECT_EQ(parent.GetRootComponent(), replacementRoot->GetAttachParent());
        EXPECT_EQ(RVX::Vec3(3.0f, 0.0f, 0.0f), child.GetPosition());
        EXPECT_EQ(RVX::Vec3(13.0f, 0.0f, 0.0f), child.GetWorldPosition());
    }

    TEST(ActorComponentValidation, RootComponentTransformMarksSceneEntityDirty)
    {
        RVX::SceneEntity entity("RootDirtyEntity");
        entity.SetLocalBounds(RVX::AABB(RVX::Vec3(-1.0f), RVX::Vec3(1.0f)));

        auto initialBounds = entity.GetWorldBounds();
        EXPECT_EQ(RVX::Vec3(0.0f), initialBounds.GetCenter());

        entity.ClearSpatialDirty();
        EXPECT_FALSE(entity.IsSpatialDirty());

        entity.GetRootComponent()->SetRelativeLocation(RVX::Vec3(10.0f, 0.0f, 0.0f));

        EXPECT_TRUE(entity.IsSpatialDirty());
        auto updatedBounds = entity.GetWorldBounds();
        EXPECT_EQ(RVX::Vec3(10.0f, 0.0f, 0.0f), updatedBounds.GetCenter());
    }

    TEST(ActorComponentValidation, SceneEntityRootReplacementRejectsInvalidAttachment)
    {
        RVX::SceneEntity parent("CycleParent");
        RVX::SceneEntity child("CycleChild");
        RVX::Actor* childActor = &child;

        parent.AddChild(&child);
        auto* originalRoot = child.GetRootComponent();
        auto* replacementRoot = childActor->AddComponent<RVX::SceneComponent>();

        EXPECT_TRUE(parent.GetRootComponent()->AttachToComponent(replacementRoot));

        childActor->SetRootComponent(replacementRoot);

        EXPECT_EQ(originalRoot, child.GetRootComponent());
        EXPECT_EQ(parent.GetRootComponent(), originalRoot->GetAttachParent());

        parent.GetRootComponent()->DetachFromComponent();
    }

    TEST(ActorComponentValidation, SceneEntityRootReplacementDetachesExternalParentWhenRootEntity)
    {
        RVX::SceneEntity externalParent("ExternalParent");
        RVX::SceneEntity entity("RootReplacementEntity");
        RVX::Actor* actor = &entity;

        externalParent.SetPosition(RVX::Vec3(100.0f, 0.0f, 0.0f));
        auto* replacementRoot = actor->AddComponent<RVX::SceneComponent>();
        replacementRoot->SetRelativeLocation(RVX::Vec3(5.0f, 0.0f, 0.0f));

        EXPECT_TRUE(replacementRoot->AttachToComponent(externalParent.GetRootComponent()));

        actor->SetRootComponent(replacementRoot);

        EXPECT_EQ(replacementRoot, entity.GetRootComponent());
        EXPECT_EQ(nullptr, replacementRoot->GetAttachParent());
        EXPECT_EQ(RVX::Vec3(5.0f, 0.0f, 0.0f), entity.GetWorldPosition());
    }

    TEST(ActorComponentValidation, SceneEntityAddChildRejectsInconsistentComponentCycle)
    {
        RVX::SceneEntity parent("CycleAttachmentParent");
        RVX::SceneEntity child("CycleAttachmentChild");

        EXPECT_TRUE(parent.GetRootComponent()->AttachToComponent(child.GetRootComponent()));

        parent.AddChild(&child);

        EXPECT_EQ(nullptr, child.GetParent());
        EXPECT_EQ(static_cast<size_t>(0), parent.GetChildCount());
        EXPECT_EQ(child.GetRootComponent(), parent.GetRootComponent()->GetAttachParent());

        parent.GetRootComponent()->DetachFromComponent();
    }

    TEST(ActorComponentValidation, SceneManagerSpawnActorCreatesSceneOwnedActor)
    {
        RVX::SceneManager sceneManager;
        sceneManager.Initialize();

        RVX::ActorSpawnParams params;
        params.name = "SpawnedActor";
        params.localPosition = RVX::Vec3(3.0f, 4.0f, 5.0f);
        params.localScale = RVX::Vec3(2.0f, 3.0f, 4.0f);

        auto* actor = sceneManager.SpawnActor<SpawnableSceneActor>(params);
        ASSERT_NE(nullptr, actor);
        EXPECT_EQ(actor, sceneManager.GetEntity(actor->GetHandle()));
        EXPECT_EQ(std::string("SpawnedActor"), actor->GetName());
        EXPECT_EQ(params.localPosition, actor->GetPosition());
        EXPECT_EQ(params.localPosition, actor->GetWorldPosition());
        EXPECT_EQ(params.localScale, actor->GetScale());
        EXPECT_EQ(params.localScale, actor->GetWorldScale());
        EXPECT_EQ(static_cast<size_t>(1), sceneManager.GetEntityCount());

        sceneManager.Shutdown();
    }

    TEST(ActorComponentValidation, SceneManagerSpawnActorAttachesParent)
    {
        RVX::SceneManager sceneManager;
        sceneManager.Initialize();

        RVX::ActorSpawnParams parentParams;
        parentParams.name = "SpawnParent";
        parentParams.localPosition = RVX::Vec3(10.0f, 2.0f, 0.0f);
        parentParams.localRotation = RVX::Quat(0.9238795f, 0.0f, 0.3826834f, 0.0f);
        parentParams.localScale = RVX::Vec3(2.0f, 2.0f, 2.0f);
        auto* parent = sceneManager.SpawnActor<SpawnableSceneActor>(parentParams);
        ASSERT_NE(nullptr, parent);

        RVX::ActorSpawnParams childParams;
        childParams.name = "SpawnChild";
        childParams.localPosition = RVX::Vec3(1.0f, 2.0f, 3.0f);
        childParams.localRotation = RVX::Quat(0.9659258f, 0.2588190f, 0.0f, 0.0f);
        childParams.localScale = RVX::Vec3(0.5f, 0.75f, 1.25f);
        childParams.parent = parent;

        auto* child = sceneManager.SpawnActor<SpawnableSceneActor>(childParams);
        ASSERT_NE(nullptr, child);
        EXPECT_EQ(parent, child->GetParent());
        EXPECT_EQ(static_cast<size_t>(1), parent->GetChildCount());
        EXPECT_EQ(parent->GetRootComponent(), child->GetRootComponent()->GetAttachParent());
        EXPECT_EQ(childParams.localPosition, child->GetPosition());
        EXPECT_EQ(childParams.localScale, child->GetScale());
        EXPECT_EQ(parent->GetWorldMatrix() * child->GetLocalMatrix(), child->GetWorldMatrix());
        EXPECT_EQ(static_cast<size_t>(2), sceneManager.GetEntityCount());

        sceneManager.Shutdown();
    }

    TEST(ActorComponentValidation, SceneManagerSpawnActorRejectsForeignParent)
    {
        RVX::SceneManager sourceScene;
        RVX::SceneManager targetScene;
        sourceScene.Initialize();
        targetScene.Initialize();

        RVX::ActorSpawnParams parentParams;
        parentParams.name = "ForeignParent";
        auto* foreignParent = sourceScene.SpawnActor<SpawnableSceneActor>(parentParams);
        ASSERT_NE(nullptr, foreignParent);

        RVX::ActorSpawnParams childParams;
        childParams.name = "RejectedChild";
        childParams.parent = foreignParent;
        EXPECT_EQ(nullptr, targetScene.SpawnActor<SpawnableSceneActor>(childParams));
        EXPECT_EQ(static_cast<size_t>(0), targetScene.GetEntityCount());

        RVX::SceneEntity stackParent("StackParent");
        childParams.parent = &stackParent;
        EXPECT_EQ(nullptr, targetScene.SpawnActor<SpawnableSceneActor>(childParams));
        EXPECT_EQ(static_cast<size_t>(0), targetScene.GetEntityCount());

        sourceScene.Shutdown();
        targetScene.Shutdown();
    }

    TEST(ActorComponentValidation, SceneManagerSpawnActorByClassNameCreatesSceneOwnedActor)
    {
        RVX::ActorFactory::ClearAll();
        RVX::ActorFactory::Register("FactorySpawnSceneActor", []() {
            return std::make_unique<FactorySpawnSceneActor>();
        });
        FactoryLifecycleCounters::Reset();

        RVX::SceneManager sceneManager;
        sceneManager.Initialize();

        RVX::ActorSpawnParams parentParams;
        parentParams.name = "FactoryParent";
        RVX::SceneEntity* parent = sceneManager.SpawnActor(parentParams);
        ASSERT_NE(nullptr, parent);

        RVX::ActorSpawnParams params;
        params.name = "FactorySceneSpawn";
        params.localPosition = RVX::Vec3(3.0f, 4.0f, 5.0f);
        params.localRotation = RVX::Quat(0.9238795f, 0.0f, 0.3826834f, 0.0f);
        params.localScale = RVX::Vec3(2.0f, 2.0f, 2.0f);
        params.parent = parent;

        RVX::SceneEntity* spawned = sceneManager.SpawnActorByClassName("FactorySpawnSceneActor", params);

        ASSERT_NE(nullptr, spawned);
        ASSERT_NE(nullptr, dynamic_cast<FactorySpawnSceneActor*>(spawned));
        EXPECT_EQ(std::string("FactorySceneSpawn"), spawned->GetName());
        EXPECT_EQ(parent, spawned->GetParent());
        EXPECT_EQ(RVX::Vec3(3.0f, 4.0f, 5.0f), spawned->GetPosition());
        EXPECT_EQ(RVX::Quat(0.9238795f, 0.0f, 0.3826834f, 0.0f), spawned->GetRotation());
        EXPECT_EQ(RVX::Vec3(2.0f, 2.0f, 2.0f), spawned->GetScale());
        EXPECT_EQ(spawned, sceneManager.GetEntity(spawned->GetHandle()));
        EXPECT_EQ(static_cast<size_t>(2), sceneManager.GetEntityCount());
        EXPECT_EQ(1, FactoryLifecycleCounters::registered);
        EXPECT_EQ(1, FactoryLifecycleCounters::initialized);

        sceneManager.Update(0.016f);
        EXPECT_EQ(1, FactoryLifecycleCounters::beganPlay);
        EXPECT_EQ(1, FactoryLifecycleCounters::ticked);

        sceneManager.Shutdown();
        EXPECT_EQ(1, FactoryLifecycleCounters::unregistered);
        RVX::ActorFactory::ClearAll();
    }

    TEST(ActorComponentValidation, SceneManagerSpawnActorByClassNameRejectsInvalidClasses)
    {
        RVX::ActorFactory::ClearAll();
        RVX::ActorFactory::Register("FactorySpawnPureActor", []() {
            return std::make_unique<FactorySpawnPureActor>();
        });
        RVX::ActorFactory::Register("FactorySpawnSceneActor", []() {
            return std::make_unique<FactorySpawnSceneActor>();
        });

        RVX::SceneManager sceneManager;
        sceneManager.Initialize();
        RVX::SceneManager foreignSceneManager;
        foreignSceneManager.Initialize();

        RVX::ActorSpawnParams foreignParentParams;
        foreignParentParams.name = "ForeignParent";
        RVX::SceneEntity* foreignParent = foreignSceneManager.SpawnActor(foreignParentParams);
        ASSERT_NE(nullptr, foreignParent);

        EXPECT_EQ(nullptr, sceneManager.SpawnActorByClassName("MissingActor", {}));
        EXPECT_EQ(nullptr, sceneManager.SpawnActorByClassName("FactorySpawnPureActor", {}));

        RVX::ActorSpawnParams childParams;
        childParams.name = "RejectedChild";
        childParams.parent = foreignParent;
        EXPECT_EQ(nullptr, sceneManager.SpawnActorByClassName("FactorySpawnSceneActor", childParams));
        EXPECT_EQ(static_cast<size_t>(0), sceneManager.GetEntityCount());

        foreignSceneManager.Shutdown();
        sceneManager.Shutdown();
        RVX::ActorFactory::ClearAll();
    }

    TEST(ActorComponentValidation, SceneManagerAddHierarchyIgnoresNullRoot)
    {
        RVX::SceneManager sceneManager;
        sceneManager.Initialize();

        sceneManager.AddHierarchy(nullptr);

        EXPECT_EQ(static_cast<size_t>(0), sceneManager.GetEntityCount());

        sceneManager.Shutdown();
    }

    TEST(ActorComponentValidation, SceneManagerAddHierarchySpawnsNodeTree)
    {
        RVX::SceneManager sceneManager;
        sceneManager.Initialize();

        auto root = std::make_shared<RVX::Node>("HierarchyRoot");
        const RVX::Quat rootRotation(0.9238795f, 0.0f, 0.3826834f, 0.0f);
        root->GetLocalTransform().SetPosition(RVX::Vec3(1.0f, 2.0f, 3.0f));
        root->GetLocalTransform().SetRotation(rootRotation);
        root->GetLocalTransform().SetScale(RVX::Vec3(2.0f, 2.0f, 2.0f));

        auto child = std::make_shared<RVX::Node>("HierarchyChild");
        child->SetActive(false);
        child->GetLocalTransform().SetPosition(RVX::Vec3(4.0f, 5.0f, 6.0f));

        auto grandChild = std::make_shared<RVX::Node>("HierarchyGrandChild");
        grandChild->GetLocalTransform().SetScale(RVX::Vec3(0.5f, 0.5f, 0.5f));

        child->AddChild(grandChild);
        root->AddChild(child);

        sceneManager.AddHierarchy(root);

        RVX::SceneEntity* rootEntity = nullptr;
        sceneManager.ForEachEntity([&](RVX::SceneEntity* entity) {
            if (entity && !entity->GetParent())
            {
                rootEntity = entity;
            }
        });

        EXPECT_EQ(static_cast<size_t>(3), sceneManager.GetEntityCount());
        ASSERT_NE(nullptr, rootEntity);
        EXPECT_EQ(std::string("HierarchyRoot"), rootEntity->GetName());
        EXPECT_EQ(RVX::Vec3(1.0f, 2.0f, 3.0f), rootEntity->GetPosition());
        EXPECT_EQ(rootRotation, rootEntity->GetRotation());
        EXPECT_EQ(RVX::Vec3(2.0f, 2.0f, 2.0f), rootEntity->GetScale());
        EXPECT_EQ(static_cast<size_t>(1), rootEntity->GetChildren().size());

        RVX::SceneEntity* childEntity = rootEntity->GetChildren()[0];
        ASSERT_NE(nullptr, childEntity);
        EXPECT_EQ(std::string("HierarchyChild"), childEntity->GetName());
        EXPECT_EQ(rootEntity, childEntity->GetParent());
        EXPECT_EQ(RVX::Vec3(4.0f, 5.0f, 6.0f), childEntity->GetPosition());
        EXPECT_FALSE(childEntity->IsActive());
        EXPECT_EQ(static_cast<size_t>(1), childEntity->GetChildren().size());

        RVX::SceneEntity* grandChildEntity = childEntity->GetChildren()[0];
        ASSERT_NE(nullptr, grandChildEntity);
        EXPECT_EQ(std::string("HierarchyGrandChild"), grandChildEntity->GetName());
        EXPECT_EQ(childEntity, grandChildEntity->GetParent());
        EXPECT_EQ(RVX::Vec3(0.5f, 0.5f, 0.5f), grandChildEntity->GetScale());

        sceneManager.Shutdown();
    }

    TEST(ActorComponentValidation, SceneManagerAddHierarchySkipsNodeCycles)
    {
        RVX::SceneManager sceneManager;
        sceneManager.Initialize();

        auto root = std::make_shared<RVX::Node>("CycleRoot");
        auto child = std::make_shared<RVX::Node>("CycleChild");

        root->AddChild(child);
        child->AddChild(root);

        sceneManager.AddHierarchy(root);

        RVX::SceneEntity* rootEntity = nullptr;
        sceneManager.ForEachEntity([&](RVX::SceneEntity* entity) {
            if (entity && !entity->GetParent())
            {
                rootEntity = entity;
            }
        });

        EXPECT_EQ(static_cast<size_t>(2), sceneManager.GetEntityCount());
        ASSERT_NE(nullptr, rootEntity);
        EXPECT_EQ(std::string("CycleRoot"), rootEntity->GetName());
        EXPECT_EQ(static_cast<size_t>(1), rootEntity->GetChildren().size());

        RVX::SceneEntity* childEntity = rootEntity->GetChildren()[0];
        ASSERT_NE(nullptr, childEntity);
        EXPECT_EQ(std::string("CycleChild"), childEntity->GetName());
        EXPECT_EQ(rootEntity, childEntity->GetParent());
        EXPECT_EQ(static_cast<size_t>(0), childEntity->GetChildren().size());

        sceneManager.Shutdown();
    }

    TEST(ActorComponentValidation, SceneManagerDestroyActorUsesActorPointer)
    {
        RVX::SceneManager sceneManager;
        sceneManager.Initialize();

        RVX::ActorSpawnParams params;
        params.name = "DestroyMe";
        auto* actor = sceneManager.SpawnActor<SpawnableSceneActor>(params);
        ASSERT_NE(nullptr, actor);
        const auto handle = actor->GetHandle();

        EXPECT_TRUE(sceneManager.DestroyActor(static_cast<RVX::Actor*>(actor)));
        EXPECT_EQ(static_cast<size_t>(0), sceneManager.GetEntityCount());
        EXPECT_EQ(nullptr, sceneManager.GetEntity(handle));

        sceneManager.Shutdown();
    }

    TEST(ActorComponentValidation, SceneManagerDestroyActorDefersDuringLifecycleDispatch)
    {
        RVX::SceneManager sceneManager;
        sceneManager.Initialize();

        bool destroyAccepted = false;
        RVX::ActorSpawnParams params;
        params.name = "DeferredDestroy";
        auto* actor = sceneManager.SpawnActor<SpawnableSceneActor>(params);
        ASSERT_NE(nullptr, actor);
        actor->AddComponent<DestroyOwnerActorComponent>(&sceneManager, &destroyAccepted);

        sceneManager.Update(0.016f);

        EXPECT_TRUE(destroyAccepted);
        EXPECT_EQ(static_cast<size_t>(0), sceneManager.GetEntityCount());

        sceneManager.Shutdown();
    }

    TEST(ActorComponentValidation, SceneManagerDestroyActorRejectsDuplicatePendingDestroy)
    {
        RVX::SceneManager sceneManager;
        sceneManager.Initialize();

        bool firstDestroy = false;
        bool secondDestroy = true;
        RVX::ActorSpawnParams params;
        params.name = "DuplicateDestroy";
        auto* actor = sceneManager.SpawnActor<SpawnableSceneActor>(params);
        ASSERT_NE(nullptr, actor);
        actor->AddComponent<DestroyOwnerActorComponent>(&sceneManager, &firstDestroy, &secondDestroy);

        sceneManager.Update(0.016f);

        EXPECT_TRUE(firstDestroy);
        EXPECT_FALSE(secondDestroy);
        EXPECT_EQ(static_cast<size_t>(0), sceneManager.GetEntityCount());

        sceneManager.Shutdown();
    }

    TEST(ActorComponentValidation, WorldSpawnActorDelegatesToSceneManager)
    {
        RVX::World world;
        world.Initialize();

        RVX::ActorSpawnParams params;
        params.name = "WorldSpawned";
        auto* actor = world.SpawnActor<SpawnableSceneActor>(params);
        ASSERT_NE(nullptr, actor);
        ASSERT_NE(nullptr, world.GetSceneManager());
        EXPECT_EQ(actor, world.GetSceneManager()->GetEntity(actor->GetHandle()));
        EXPECT_TRUE(world.DestroyActor(actor));
        EXPECT_EQ(static_cast<size_t>(0), world.GetSceneManager()->GetEntityCount());

        world.Shutdown();
    }

    TEST(ActorComponentValidation, WorldSpawnActorByClassNameRoutesSceneAndPureActors)
    {
        RVX::ActorFactory::ClearAll();
        RVX::ActorFactory::Register("FactorySpawnSceneActor", []() {
            return std::make_unique<FactorySpawnSceneActor>();
        });
        RVX::ActorFactory::Register("FactorySpawnPureActor", []() {
            return std::make_unique<FactorySpawnPureActor>();
        });

        RVX::World world;
        world.Initialize();
        FactoryLifecycleCounters::Reset();

        RVX::ActorSpawnParams sceneParams;
        sceneParams.name = "WorldSceneFactoryActor";
        sceneParams.localRotation = RVX::Quat(0.9807853f, 0.1950903f, 0.0f, 0.0f);
        RVX::Actor* sceneActor = world.SpawnActorByClassName("FactorySpawnSceneActor", sceneParams);
        ASSERT_NE(nullptr, sceneActor);
        ASSERT_NE(nullptr, dynamic_cast<FactorySpawnSceneActor*>(sceneActor));
        EXPECT_EQ(sceneActor, world.GetActor(sceneActor->GetHandle()));
        auto* sceneEntity = dynamic_cast<RVX::SceneEntity*>(sceneActor);
        ASSERT_NE(nullptr, sceneEntity);
        EXPECT_EQ(sceneEntity, world.GetSceneManager()->GetEntity(sceneEntity->GetHandle()));
        EXPECT_EQ(RVX::Quat(0.9807853f, 0.1950903f, 0.0f, 0.0f), sceneEntity->GetRotation());
        EXPECT_EQ(static_cast<size_t>(1), world.GetSceneManager()->GetEntityCount());

        RVX::ActorSpawnParams pureParams;
        pureParams.name = "WorldPureFactoryActor";
        pureParams.localPosition = RVX::Vec3(7.0f, 8.0f, 9.0f);
        pureParams.localRotation = RVX::Quat(0.9659258f, 0.0f, 0.2588190f, 0.0f);
        RVX::Actor* pureActor = world.SpawnActorByClassName("FactorySpawnPureActor", pureParams);
        ASSERT_NE(nullptr, pureActor);
        ASSERT_NE(nullptr, dynamic_cast<FactorySpawnPureActor*>(pureActor));
        EXPECT_EQ(std::string("WorldPureFactoryActor"), pureActor->GetName());
        EXPECT_EQ(RVX::Vec3(7.0f, 8.0f, 9.0f), pureActor->GetWorldPosition());
        EXPECT_EQ(RVX::Quat(0.9659258f, 0.0f, 0.2588190f, 0.0f), pureActor->GetWorldRotation());
        EXPECT_EQ(pureActor, world.GetActor(pureActor->GetHandle()));
        EXPECT_EQ(nullptr, world.GetSceneManager()->GetEntity(pureActor->GetHandle()));
        EXPECT_EQ(static_cast<size_t>(1), world.GetActorCount());
        EXPECT_EQ(2, FactoryLifecycleCounters::registered);
        EXPECT_EQ(2, FactoryLifecycleCounters::initialized);

        world.Tick(0.016f);
        EXPECT_EQ(2, FactoryLifecycleCounters::beganPlay);
        EXPECT_EQ(2, FactoryLifecycleCounters::ticked);

        RVX::ActorSpawnParams parentedPureParams;
        parentedPureParams.name = "RejectedPureFactoryActor";
        parentedPureParams.parent = sceneEntity;
        EXPECT_EQ(nullptr, world.SpawnActorByClassName("FactorySpawnPureActor", parentedPureParams));

        RVX::SceneManager foreignSceneManager;
        foreignSceneManager.Initialize();
        RVX::ActorSpawnParams foreignParentParams;
        foreignParentParams.name = "ForeignWorldParent";
        RVX::SceneEntity* foreignParent = foreignSceneManager.SpawnActor(foreignParentParams);
        ASSERT_NE(nullptr, foreignParent);

        RVX::ActorSpawnParams foreignChildParams;
        foreignChildParams.name = "RejectedWorldSceneFactoryActor";
        foreignChildParams.parent = foreignParent;
        EXPECT_EQ(nullptr, world.SpawnActorByClassName("FactorySpawnSceneActor", foreignChildParams));

        EXPECT_EQ(nullptr, world.SpawnActorByClassName("MissingActor", {}));

        foreignSceneManager.Shutdown();
        world.Shutdown();
        EXPECT_EQ(2, FactoryLifecycleCounters::unregistered);
        RVX::ActorFactory::ClearAll();
    }

    TEST(ActorComponentValidation, WorldSpawnActorRejectsWhenUninitialized)
    {
        RVX::World world;
        RVX::ActorSpawnParams params;
        params.name = "RejectedWorldSpawn";
        RVX::Actor actor("StackActor");

        EXPECT_EQ(nullptr, world.SpawnActor<SpawnableSceneActor>(params));
        EXPECT_FALSE(world.DestroyActor(&actor));
    }

    TEST(ActorComponentValidation, DestroyActorRejectsForeignOrPureActor)
    {
        RVX::SceneManager sceneManager;
        RVX::SceneManager foreignScene;
        sceneManager.Initialize();
        foreignScene.Initialize();

        RVX::Actor pureActor("PureActor");
        RVX::SceneEntity stackEntity("StackEntity");
        RVX::ActorSpawnParams params;
        params.name = "ForeignActor";
        auto* foreignActor = foreignScene.SpawnActor<SpawnableSceneActor>(params);
        ASSERT_NE(nullptr, foreignActor);

        EXPECT_FALSE(sceneManager.DestroyActor(nullptr));
        EXPECT_FALSE(sceneManager.DestroyActor(&pureActor));
        EXPECT_FALSE(sceneManager.DestroyActor(&stackEntity));
        EXPECT_FALSE(sceneManager.DestroyActor(foreignActor));
        EXPECT_EQ(static_cast<size_t>(0), sceneManager.GetEntityCount());
        EXPECT_EQ(static_cast<size_t>(1), foreignScene.GetEntityCount());

        sceneManager.Shutdown();
        foreignScene.Shutdown();
    }

    TEST(ActorComponentValidation, WorldSpawnPureActorOwnsAndLooksUpActor)
    {
        RVX::World world;
        world.Initialize();

        RVX::ActorSpawnParams params;
        params.name = "PureActor";
        params.localPosition = RVX::Vec3(3.0f, 4.0f, 5.0f);
        params.localRotation = RVX::Quat(0.9238795f, 0.0f, 0.3826834f, 0.0f);
        params.localScale = RVX::Vec3(2.0f, 3.0f, 4.0f);

        auto* actor = world.SpawnActor<WorldManagedPureActor>(params);
        ASSERT_NE(nullptr, actor);
        EXPECT_EQ(actor, world.GetActor(actor->GetHandle()));
        EXPECT_EQ(static_cast<size_t>(1), world.GetActorCount());
        ASSERT_NE(nullptr, world.GetSceneManager());
        EXPECT_EQ(nullptr, world.GetSceneManager()->GetEntity(actor->GetHandle()));
        EXPECT_EQ(params.localPosition, actor->GetWorldPosition());
        EXPECT_EQ(params.localScale, actor->GetWorldScale());

        world.Shutdown();
    }

    TEST(ActorComponentValidation, WorldSpawnSceneEntityStillDelegatesToSceneManager)
    {
        RVX::World world;
        world.Initialize();

        RVX::ActorSpawnParams params;
        params.name = "SceneActorThroughWorld";
        auto* actor = world.SpawnActor<SpawnableSceneActor>(params);
        ASSERT_NE(nullptr, actor);
        EXPECT_EQ(static_cast<size_t>(0), world.GetActorCount());
        ASSERT_NE(nullptr, world.GetSceneManager());
        EXPECT_EQ(actor, world.GetSceneManager()->GetEntity(actor->GetHandle()));
        EXPECT_EQ(static_cast<RVX::Actor*>(actor), world.GetActor(actor->GetHandle()));

        world.Shutdown();
    }

    TEST(ActorComponentValidation, WorldPureActorLifecycleBeginsTicksAndDestroys)
    {
        RVX::World world;
        world.Initialize();

        WorldActorLifecycleCounters counters;
        RVX::ActorSpawnParams params;
        params.name = "LifecyclePureActor";
        auto* actor = world.SpawnActor<WorldManagedPureActor>(params);
        ASSERT_NE(nullptr, actor);
        actor->AddComponent<WorldActorLifecycleComponent>(&counters);

        world.Tick(0.016f);

        EXPECT_EQ(1, counters.registered);
        EXPECT_EQ(1, counters.initialized);
        EXPECT_EQ(1, counters.beganPlay);
        EXPECT_EQ(1, counters.ticked);

        EXPECT_TRUE(world.DestroyActor(actor));
        EXPECT_EQ(1, counters.endedPlay);
        EXPECT_EQ(1, counters.unregistered);
        EXPECT_EQ(1, counters.destroyed);
        EXPECT_EQ(static_cast<size_t>(0), world.GetActorCount());

        world.Shutdown();
    }

    TEST(ActorComponentValidation, WorldDestroyPureActorDefersDuringLifecycleDispatch)
    {
        RVX::World world;
        world.Initialize();

        bool destroyAccepted = false;
        RVX::ActorSpawnParams params;
        params.name = "DeferredPureDestroy";
        auto* actor = world.SpawnActor<WorldManagedPureActor>(params);
        ASSERT_NE(nullptr, actor);
        const auto handle = actor->GetHandle();
        actor->AddComponent<WorldActorSelfDestroyComponent>(&world, &destroyAccepted);

        world.Tick(0.016f);

        EXPECT_TRUE(destroyAccepted);
        EXPECT_EQ(static_cast<size_t>(0), world.GetActorCount());
        EXPECT_EQ(nullptr, world.GetActor(handle));

        world.Shutdown();
    }

    TEST(ActorComponentValidation, WorldDestroyPureActorRejectsDuplicatePendingDestroy)
    {
        RVX::World world;
        world.Initialize();

        bool firstDestroy = false;
        bool secondDestroy = true;
        RVX::ActorSpawnParams params;
        params.name = "DuplicatePureDestroy";
        auto* actor = world.SpawnActor<WorldManagedPureActor>(params);
        ASSERT_NE(nullptr, actor);
        actor->AddComponent<WorldActorSelfDestroyComponent>(&world, &firstDestroy, &secondDestroy);

        world.Tick(0.016f);

        EXPECT_TRUE(firstDestroy);
        EXPECT_FALSE(secondDestroy);
        EXPECT_EQ(static_cast<size_t>(0), world.GetActorCount());

        world.Shutdown();
    }

    TEST(ActorComponentValidation, WorldPureActorRejectsSceneParent)
    {
        RVX::World world;
        world.Initialize();

        RVX::ActorSpawnParams sceneParams;
        sceneParams.name = "SceneParent";
        auto* parent = world.SpawnActor<SpawnableSceneActor>(sceneParams);
        ASSERT_NE(nullptr, parent);

        RVX::ActorSpawnParams pureParams;
        pureParams.name = "RejectedPureChild";
        pureParams.parent = parent;
        EXPECT_EQ(nullptr, world.SpawnActor<WorldManagedPureActor>(pureParams));
        EXPECT_EQ(static_cast<size_t>(0), world.GetActorCount());

        world.Shutdown();
    }

    TEST(ActorComponentValidation, WorldUnloadClearsPureActorsAndSceneActors)
    {
        RVX::World world;
        world.Initialize();

        RVX::ActorSpawnParams pureParams;
        pureParams.name = "UnloadPureActor";
        auto* pureActor = world.SpawnActor<WorldManagedPureActor>(pureParams);
        ASSERT_NE(nullptr, pureActor);

        RVX::ActorSpawnParams sceneParams;
        sceneParams.name = "UnloadSceneActor";
        auto* sceneActor = world.SpawnActor<SpawnableSceneActor>(sceneParams);
        ASSERT_NE(nullptr, sceneActor);
        EXPECT_EQ(static_cast<size_t>(1), world.GetActorCount());
        ASSERT_NE(nullptr, world.GetSceneManager());
        EXPECT_EQ(static_cast<size_t>(1), world.GetSceneManager()->GetEntityCount());

        world.Unload();

        EXPECT_EQ(static_cast<size_t>(0), world.GetActorCount());
        ASSERT_NE(nullptr, world.GetSceneManager());
        EXPECT_EQ(static_cast<size_t>(0), world.GetSceneManager()->GetEntityCount());

        world.Shutdown();
    }

    TEST(ActorComponentValidation, WorldForEachActorVisitsPureAndSceneActors)
    {
        RVX::World world;
        world.Initialize();

        RVX::ActorSpawnParams pureParams;
        pureParams.name = "PureIterated";
        auto* pureActor = world.SpawnActor<WorldManagedPureActor>(pureParams);
        ASSERT_NE(nullptr, pureActor);

        RVX::ActorSpawnParams sceneParams;
        sceneParams.name = "SceneIterated";
        auto* sceneActor = world.SpawnActor<SpawnableSceneActor>(sceneParams);
        ASSERT_NE(nullptr, sceneActor);

        std::vector<RVX::Actor::Handle> visited;
        world.ForEachActor([&](RVX::Actor* actor) {
            if (actor)
            {
                visited.push_back(actor->GetHandle());
            }
        });

        EXPECT_EQ(static_cast<size_t>(2), visited.size());
        EXPECT_TRUE(std::find(visited.begin(), visited.end(), pureActor->GetHandle()) != visited.end());
        EXPECT_TRUE(std::find(visited.begin(), visited.end(), sceneActor->GetHandle()) != visited.end());

        world.Shutdown();
    }

    TEST(ActorComponentValidation, WorldForEachActorIgnoresEmptyCallback)
    {
        RVX::World world;
        world.Initialize();

        RVX::ActorSpawnParams params;
        params.name = "EmptyCallbackPureActor";
        auto* actor = world.SpawnActor<WorldManagedPureActor>(params);
        ASSERT_NE(nullptr, actor);

        world.ForEachActor({});
        EXPECT_EQ(static_cast<size_t>(1), world.GetActorCount());

        world.Shutdown();
    }

    TEST(ActorComponentValidation, WorldForEachActorAllowsPureActorDestroyDuringCallback)
    {
        RVX::World world;
        world.Initialize();

        RVX::ActorSpawnParams firstParams;
        firstParams.name = "DestroyPureDuringIteration";
        auto* first = world.SpawnActor<WorldManagedPureActor>(firstParams);
        ASSERT_NE(nullptr, first);
        const auto firstHandle = first->GetHandle();

        RVX::ActorSpawnParams secondParams;
        secondParams.name = "SurvivePureIteration";
        auto* second = world.SpawnActor<WorldManagedPureActor>(secondParams);
        ASSERT_NE(nullptr, second);
        const auto secondHandle = second->GetHandle();

        bool destroyAccepted = false;
        size_t visitCount = 0;
        world.ForEachActor([&](RVX::Actor* actor) {
            if (!actor)
                return;

            ++visitCount;
            if (actor->GetHandle() == firstHandle)
            {
                destroyAccepted = world.DestroyActor(actor);
            }
        });

        EXPECT_TRUE(destroyAccepted);
        EXPECT_EQ(nullptr, world.GetActor(firstHandle));
        EXPECT_EQ(second, world.GetActor(secondHandle));
        EXPECT_TRUE(visitCount >= static_cast<size_t>(1));

        world.Shutdown();
    }

    TEST(ActorComponentValidation, WorldForEachActorAllowsSceneActorDestroyDuringCallback)
    {
        RVX::World world;
        world.Initialize();

        RVX::ActorSpawnParams firstParams;
        firstParams.name = "DestroySceneDuringIteration";
        auto* first = world.SpawnActor<SpawnableSceneActor>(firstParams);
        ASSERT_NE(nullptr, first);
        const auto firstHandle = first->GetHandle();

        RVX::ActorSpawnParams secondParams;
        secondParams.name = "SurviveSceneIteration";
        auto* second = world.SpawnActor<SpawnableSceneActor>(secondParams);
        ASSERT_NE(nullptr, second);
        const auto secondHandle = second->GetHandle();

        bool destroyAccepted = false;
        size_t visitCount = 0;
        world.ForEachActor([&](RVX::Actor* actor) {
            if (!actor)
                return;

            ++visitCount;
            if (actor->GetHandle() == firstHandle)
            {
                destroyAccepted = world.DestroyActor(actor);
            }
        });

        EXPECT_TRUE(destroyAccepted);
        EXPECT_EQ(nullptr, world.GetActor(firstHandle));
        EXPECT_EQ(second, world.GetActor(secondHandle));
        EXPECT_TRUE(visitCount >= static_cast<size_t>(1));

        world.Shutdown();
    }
} // namespace
