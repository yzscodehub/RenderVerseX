#include "Core/Core.h"
#include "Scene/Actor.h"
#include "Scene/ActorComponent.h"
#include "Scene/ActorFactory.h"
#include "Scene/Component.h"
#include "Scene/ComponentFactory.h"
#include "Scene/Components/StaticMeshComponent.h"
#include "Scene/PrimitiveComponent.h"
#include "Scene/SceneComponent.h"
#include "Scene/SceneEntity.h"
#include "Scene/SceneManager.h"
#include "TestFramework/TestRunner.h"
#include "World/World.h"

#include <cmath>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

using namespace RVX::Test;

namespace
{
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

    bool Test_ActorComponentLifecycleDefaults()
    {
        TestComponent component;
        TEST_ASSERT_TRUE(component.IsEnabled());
        TEST_ASSERT_FALSE(component.IsRegistered());
        TEST_ASSERT_FALSE(component.CanEverTick());
        TEST_ASSERT_FALSE(component.IsTickEnabled());

        component.SetCanEverTick(true);
        component.SetTickEnabled(true);
        TEST_ASSERT_TRUE(component.CanEverTick());
        TEST_ASSERT_TRUE(component.IsTickEnabled());
        TEST_ASSERT_EQ(std::string("TestComponent"), std::string(component.GetClassName()));
        return true;
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

    bool Test_ActorOwnsComponentsAndDispatchesLifecycle()
    {
        RVX::Actor actor("LifecycleActor");
        auto* component = actor.AddComponent<CountingComponent>();
        component->SetCanEverTick(true);
        component->SetTickEnabled(true);

        TEST_ASSERT_EQ(&actor, component->GetOwner());
        TEST_ASSERT_EQ(1, component->created);

        actor.RegisterAllComponents();
        TEST_ASSERT_TRUE(component->IsRegistered());
        TEST_ASSERT_EQ(1, component->registered);
        TEST_ASSERT_EQ(1, component->initialized);

        actor.BeginPlay();
        actor.Tick(0.25f);
        actor.EndPlay();
        actor.UnregisterAllComponents();

        TEST_ASSERT_EQ(1, component->beganPlay);
        TEST_ASSERT_EQ(1, component->ticked);
        TEST_ASSERT_EQ(1, component->endedPlay);
        TEST_ASSERT_EQ(1, component->unregistered);
        return true;
    }

    bool Test_SceneManagerUpdateDispatchesActorComponentLifecycle()
    {
        RVX::SceneManager sceneManager;
        sceneManager.Initialize();

        const auto handle = sceneManager.CreateEntity("RuntimeLifecycleEntity");
        auto* entity = sceneManager.GetEntity(handle);
        TEST_ASSERT_NOT_NULL(entity);

        RuntimeLifecycleCounters counters;
        auto component = std::make_unique<SceneManagedActorComponent>(&counters);
        auto* raw = component.get();
        TEST_ASSERT_EQ(raw, static_cast<RVX::Actor*>(entity)->AddOwnedComponent(std::move(component)));

        sceneManager.Update(0.5f);
        sceneManager.Update(0.25f);

        TEST_ASSERT_TRUE(raw->HasBegunPlay());
        TEST_ASSERT_EQ(1, counters.beganPlay);
        TEST_ASSERT_EQ(2, counters.ticked);
        TEST_ASSERT_TRUE(IsNear(0.25f, counters.lastDeltaTime));
        TEST_ASSERT_EQ(static_cast<size_t>(3), counters.events.size());
        TEST_ASSERT_EQ(std::string("BeginPlay"), counters.events[0]);
        TEST_ASSERT_EQ(std::string("Tick"), counters.events[1]);
        TEST_ASSERT_EQ(std::string("Tick"), counters.events[2]);

        sceneManager.Shutdown();
        return true;
    }

    bool Test_SceneManagerBeginsComponentsAddedAfterActorBeginsPlay()
    {
        RVX::SceneManager sceneManager;
        sceneManager.Initialize();

        const auto handle = sceneManager.CreateEntity("LateComponentEntity");
        auto* entity = sceneManager.GetEntity(handle);
        TEST_ASSERT_NOT_NULL(entity);

        sceneManager.Update(0.016f);
        TEST_ASSERT_TRUE(static_cast<RVX::Actor*>(entity)->HasBegunPlay());

        RuntimeLifecycleCounters counters;
        auto component = std::make_unique<SceneManagedActorComponent>(&counters);
        auto* raw = component.get();
        TEST_ASSERT_EQ(raw, static_cast<RVX::Actor*>(entity)->AddOwnedComponent(std::move(component)));
        TEST_ASSERT_FALSE(raw->HasBegunPlay());

        sceneManager.Update(0.033f);

        TEST_ASSERT_TRUE(raw->HasBegunPlay());
        TEST_ASSERT_EQ(1, counters.beganPlay);
        TEST_ASSERT_EQ(1, counters.ticked);
        TEST_ASSERT_TRUE(IsNear(0.033f, counters.lastDeltaTime));
        TEST_ASSERT_EQ(static_cast<size_t>(2), counters.events.size());
        TEST_ASSERT_EQ(std::string("BeginPlay"), counters.events[0]);
        TEST_ASSERT_EQ(std::string("Tick"), counters.events[1]);

        sceneManager.Shutdown();
        return true;
    }

    bool Test_SceneManagerUpdateKeepsLegacyComponentTicking()
    {
        RVX::SceneManager sceneManager;
        sceneManager.Initialize();

        const auto handle = sceneManager.CreateEntity("LegacyTickEntity");
        auto* entity = sceneManager.GetEntity(handle);
        TEST_ASSERT_NOT_NULL(entity);

        auto* component = entity->AddComponent<SceneManagedLegacyTickComponent>();
        TEST_ASSERT_NOT_NULL(component);

        sceneManager.Update(0.125f);
        TEST_ASSERT_EQ(1, component->tickCount);
        TEST_ASSERT_TRUE(IsNear(0.125f, component->lastDeltaTime));

        entity->SetActive(false);
        sceneManager.Update(0.5f);
        TEST_ASSERT_EQ(1, component->tickCount);

        entity->SetActive(true);
        sceneManager.Update(0.25f);
        TEST_ASSERT_EQ(2, component->tickCount);
        TEST_ASSERT_TRUE(IsNear(0.25f, component->lastDeltaTime));

        sceneManager.Shutdown();
        return true;
    }

    bool Test_SceneEntityActiveStateControlsActorTickThroughActorPointer()
    {
        RVX::SceneEntity entity("DirectActorTickEntity");
        RVX::Actor* actor = &entity;

        RuntimeLifecycleCounters counters;
        auto component = std::make_unique<SceneManagedActorComponent>(&counters);
        auto* raw = component.get();
        TEST_ASSERT_EQ(raw, actor->AddOwnedComponent(std::move(component)));

        entity.SetActive(false);
        actor->Tick(0.25f);
        TEST_ASSERT_EQ(0, counters.ticked);

        actor->SetActive(true);
        actor->Tick(0.5f);
        TEST_ASSERT_EQ(1, counters.ticked);
        TEST_ASSERT_TRUE(IsNear(0.5f, counters.lastDeltaTime));
        return true;
    }

    bool Test_SceneManagerDefersDestroyRequestsDuringLifecycleDispatch()
    {
        RVX::SceneManager sceneManager;
        sceneManager.Initialize();

        const auto handle = sceneManager.CreateEntity("SelfDestroyingEntity");
        auto* entity = sceneManager.GetEntity(handle);
        TEST_ASSERT_NOT_NULL(entity);

        int tickCount = 0;
        auto component = std::make_unique<SelfDestroyingActorComponent>(&sceneManager, handle, &tickCount);
        TEST_ASSERT_NOT_NULL(static_cast<RVX::Actor*>(entity)->AddOwnedComponent(std::move(component)));
        int legacyTickCount = 0;
        auto* legacyComponent = entity->AddComponent<SceneManagedLegacyTickComponent>(&legacyTickCount);
        TEST_ASSERT_NOT_NULL(legacyComponent);

        sceneManager.Update(0.016f);

        TEST_ASSERT_EQ(1, tickCount);
        TEST_ASSERT_EQ(0, legacyTickCount);
        TEST_ASSERT_EQ(nullptr, sceneManager.GetEntity(handle));

        sceneManager.Shutdown();
        return true;
    }

    bool Test_SceneManagerDestroyEntityDispatchesEndPlayBeforeUnregister()
    {
        RVX::SceneManager sceneManager;
        sceneManager.Initialize();

        const auto handle = sceneManager.CreateEntity("DestroyLifecycleEntity");
        auto* entity = sceneManager.GetEntity(handle);
        TEST_ASSERT_NOT_NULL(entity);

        RuntimeLifecycleCounters counters;
        auto component = std::make_unique<SceneManagedActorComponent>(&counters);
        TEST_ASSERT_NOT_NULL(static_cast<RVX::Actor*>(entity)->AddOwnedComponent(std::move(component)));

        sceneManager.Update(0.016f);
        TEST_ASSERT_EQ(1, counters.beganPlay);

        sceneManager.DestroyEntity(handle);

        TEST_ASSERT_EQ(1, counters.endedPlay);
        TEST_ASSERT_EQ(1, counters.unregistered);
        sceneManager.Shutdown();
        return true;
    }

    bool Test_SceneManagerDestroyEntityBeforeBeginPlayDoesNotDispatchEndPlay()
    {
        RVX::SceneManager sceneManager;
        sceneManager.Initialize();

        const auto handle = sceneManager.CreateEntity("NeverBegunLifecycleEntity");
        auto* entity = sceneManager.GetEntity(handle);
        TEST_ASSERT_NOT_NULL(entity);

        RuntimeLifecycleCounters counters;
        auto component = std::make_unique<SceneManagedActorComponent>(&counters);
        TEST_ASSERT_NOT_NULL(static_cast<RVX::Actor*>(entity)->AddOwnedComponent(std::move(component)));

        entity->SetActive(false);
        sceneManager.DestroyEntity(handle);

        TEST_ASSERT_EQ(0, counters.beganPlay);
        TEST_ASSERT_EQ(0, counters.ticked);
        TEST_ASSERT_EQ(0, counters.endedPlay);
        TEST_ASSERT_EQ(1, counters.unregistered);
        sceneManager.Shutdown();
        return true;
    }

    bool Test_ActorTickSnapshotsComponentsAddedDuringTick()
    {
        RVX::Actor actor("AddDuringTickActor");
        int addedTickCount = 0;
        actor.AddComponent<AddComponentDuringTickComponent>(&addedTickCount);

        actor.Tick(0.016f);
        TEST_ASSERT_EQ(0, addedTickCount);
        TEST_ASSERT_EQ(static_cast<size_t>(2), actor.GetActorComponentCount());

        actor.Tick(0.016f);
        TEST_ASSERT_EQ(1, addedTickCount);
        return true;
    }

    bool Test_ActorDefersSelfRemovalDuringTick()
    {
        RVX::Actor actor("SelfRemoveActor");
        int tickCount = 0;
        actor.AddComponent<SelfRemovingTickComponent>(&tickCount);

        actor.Tick(0.016f);

        TEST_ASSERT_EQ(1, tickCount);
        TEST_ASSERT_EQ(static_cast<size_t>(0), actor.GetActorComponentCount());

        actor.Tick(0.016f);
        TEST_ASSERT_EQ(1, tickCount);
        return true;
    }

    bool Test_ActorDispatchesEndPlayForBegunComponentRemovedDuringTick()
    {
        RVX::Actor actor("BegunSelfRemoveActor");
        int tickCount = 0;
        int endPlayCount = 0;
        actor.AddComponent<SelfRemovingTickComponent>(&tickCount, &endPlayCount);

        actor.BeginPlay();
        actor.Tick(0.016f);

        TEST_ASSERT_EQ(1, tickCount);
        TEST_ASSERT_EQ(1, endPlayCount);
        TEST_ASSERT_EQ(static_cast<size_t>(0), actor.GetActorComponentCount());
        return true;
    }

    bool Test_ActorSkipsComponentRemovedEarlierInTick()
    {
        RVX::Actor actor("RemoveOtherActor");
        int victimTickCount = 0;
        actor.AddComponent<RemoveVictimDuringTickComponent>();
        actor.AddComponent<VictimTickComponent>(&victimTickCount);

        actor.Tick(0.016f);

        TEST_ASSERT_EQ(0, victimTickCount);
        TEST_ASSERT_EQ(nullptr, actor.GetComponent<VictimTickComponent>());
        return true;
    }

    bool Test_ActorBeginPlaySnapshotsComponentsAddedDuringBeginPlay()
    {
        RVX::Actor actor("AddDuringBeginPlayActor");
        int addedBeginCount = 0;
        actor.AddComponent<AddComponentDuringBeginPlayComponent>(&addedBeginCount);

        actor.BeginPlay();
        TEST_ASSERT_EQ(0, addedBeginCount);
        TEST_ASSERT_EQ(static_cast<size_t>(2), actor.GetActorComponentCount());

        actor.BeginPlay();
        TEST_ASSERT_EQ(1, addedBeginCount);
        return true;
    }

    bool Test_ActorDefersSelfRemovalDuringEndPlay()
    {
        RVX::Actor actor("SelfRemoveEndPlayActor");
        int endPlayCount = 0;
        actor.AddComponent<SelfRemovingEndPlayComponent>(&endPlayCount);

        actor.BeginPlay();
        actor.EndPlay();

        TEST_ASSERT_EQ(1, endPlayCount);
        TEST_ASSERT_EQ(static_cast<size_t>(0), actor.GetActorComponentCount());
        return true;
    }

    bool Test_ActorDirectRemovalHandlesSelfRemovalDuringEndPlay()
    {
        RVX::Actor actor("DirectSelfRemoveEndPlayActor");
        int endPlayCount = 0;
        actor.AddComponent<SelfRemovingEndPlayComponent>(&endPlayCount);

        actor.BeginPlay();
        TEST_ASSERT_TRUE(actor.RemoveComponent<SelfRemovingEndPlayComponent>());

        TEST_ASSERT_EQ(1, endPlayCount);
        TEST_ASSERT_EQ(static_cast<size_t>(0), actor.GetActorComponentCount());
        return true;
    }

    bool Test_ActorSkipsComponentRemovedEarlierInEndPlay()
    {
        RVX::Actor actor("RemoveOtherEndPlayActor");
        EndPlayMutationCounters counters;
        actor.AddComponent<EndPlayVictimComponent>(&counters);
        actor.AddComponent<RemoveVictimDuringEndPlayComponent>(&counters);

        actor.BeginPlay();
        actor.EndPlay();

        TEST_ASSERT_EQ(1, counters.removerEndPlay);
        TEST_ASSERT_EQ(0, counters.victimEndPlay);
        TEST_ASSERT_EQ(1, counters.victimDestroyed);
        TEST_ASSERT_EQ(nullptr, actor.GetComponent<EndPlayVictimComponent>());
        return true;
    }

    bool Test_ActorKeepsEndPlaySuppressedAfterNestedEndPlayReturnsToTick()
    {
        RVX::Actor actor("NestedEndPlayRemoveActor");
        EndPlayMutationCounters counters;
        actor.AddComponent<EndPlayVictimComponent>(&counters);
        actor.AddComponent<RemoveVictimDuringEndPlayComponent>(&counters);
        actor.AddComponent<EndPlayThenRemoveVictimDuringTickComponent>();

        actor.BeginPlay();
        actor.Tick(0.016f);

        TEST_ASSERT_EQ(1, counters.removerEndPlay);
        TEST_ASSERT_EQ(0, counters.victimEndPlay);
        TEST_ASSERT_EQ(1, counters.victimDestroyed);
        TEST_ASSERT_EQ(nullptr, actor.GetComponent<EndPlayVictimComponent>());
        return true;
    }

    bool Test_ActorDispatchesEndPlayForComponentQueuedBeforeNestedEndPlay()
    {
        RVX::Actor actor("QueueBeforeNestedEndPlayActor");
        EndPlayMutationCounters counters;
        actor.AddComponent<EndPlayVictimComponent>(&counters);
        actor.AddComponent<RemoveVictimDuringEndPlayComponent>(&counters);
        actor.AddComponent<QueueVictimThenEndPlayDuringTickComponent>();

        actor.BeginPlay();
        actor.Tick(0.016f);

        TEST_ASSERT_EQ(1, counters.removerEndPlay);
        TEST_ASSERT_EQ(1, counters.victimEndPlay);
        TEST_ASSERT_EQ(1, counters.victimDestroyed);
        TEST_ASSERT_EQ(nullptr, actor.GetComponent<EndPlayVictimComponent>());
        return true;
    }

    bool Test_ActorDestructionClearsPendingRemovalWithoutDoubleDestroy()
    {
        int destroyedCount = 0;
        {
            RVX::Actor actor("DestroyPendingRemovalActor");
            actor.AddComponent<DestructionPendingRemovalComponent>(&destroyedCount);
            actor.Tick(0.016f);
        }

        TEST_ASSERT_EQ(1, destroyedCount);
        return true;
    }

    bool Test_SceneComponentAttachmentComputesWorldTransform()
    {
        RVX::Actor actor("TransformActor");
        auto* root = actor.AddComponent<RVX::SceneComponent>();
        auto* child = actor.AddComponent<RVX::SceneComponent>();

        actor.SetRootComponent(root);
        root->SetRelativeLocation(RVX::Vec3(10.0f, 0.0f, 0.0f));
        child->SetRelativeLocation(RVX::Vec3(2.0f, 0.0f, 0.0f));
        TEST_ASSERT_TRUE(child->AttachToComponent(root));

        TEST_ASSERT_EQ(RVX::Vec3(12.0f, 0.0f, 0.0f), child->GetWorldLocation());
        TEST_ASSERT_EQ(root, child->GetAttachParent());
        TEST_ASSERT_EQ(static_cast<size_t>(1), root->GetAttachChildren().size());
        return true;
    }

    bool Test_PrimitiveComponentTracksVisibilityLayerAndBounds()
    {
        RVX::PrimitiveComponent primitive;
        primitive.SetVisible(false);
        primitive.SetLayerMask(0x04);
        primitive.SetLocalBounds(RVX::AABB(RVX::Vec3(-1.0f), RVX::Vec3(1.0f)));

        TEST_ASSERT_FALSE(primitive.IsVisible());
        TEST_ASSERT_EQ(static_cast<RVX::uint32>(0x04), primitive.GetLayerMask());
        TEST_ASSERT_TRUE(primitive.GetLocalBounds().IsValid());
        TEST_ASSERT_TRUE(primitive.GetWorldBounds().IsValid());
        return true;
    }

    bool Test_StaticMeshComponentIsPrimitiveSceneComponent()
    {
        TEST_ASSERT_TRUE((std::is_base_of_v<RVX::PrimitiveComponent, RVX::StaticMeshComponent>));

        RVX::StaticMeshComponent component;
        TEST_ASSERT_EQ(std::string("StaticMeshComponent"), std::string(component.GetClassName()));
        TEST_ASSERT_FALSE(component.HasRenderData());
        TEST_ASSERT_FALSE(component.HasValidMesh());
        TEST_ASSERT_TRUE(component.IsVisible());
        return true;
    }

    bool Test_StaticMeshComponentRegistersWithSceneManager()
    {
        RVX::SceneManager sceneManager;
        sceneManager.Initialize();

        auto entity = std::make_shared<RVX::SceneEntity>("PreOwnedPrimitive");
        auto* primitive = static_cast<RVX::Actor*>(entity.get())->AddComponent<RVX::StaticMeshComponent>();
        TEST_ASSERT_NOT_NULL(primitive);
        TEST_ASSERT_FALSE(primitive->IsRegistered());

        const auto handle = entity->GetHandle();
        sceneManager.AddEntity(entity);

        TEST_ASSERT_TRUE(primitive->IsRegistered());
        TEST_ASSERT_EQ(static_cast<size_t>(1), sceneManager.GetPrimitives().size());
        TEST_ASSERT_EQ(primitive, sceneManager.GetPrimitives()[0]);

        sceneManager.DestroyEntity(handle);

        TEST_ASSERT_FALSE(primitive->IsRegistered());
        TEST_ASSERT_TRUE(sceneManager.GetPrimitives().empty());
        sceneManager.Shutdown();
        return true;
    }

    bool Test_RuntimeStaticMeshComponentRemovalUnregistersPrimitive()
    {
        RVX::SceneManager sceneManager;
        sceneManager.Initialize();

        const auto handle = sceneManager.CreateEntity("RuntimePrimitive");
        auto* entity = sceneManager.GetEntity(handle);
        TEST_ASSERT_NOT_NULL(entity);

        auto* primitive = static_cast<RVX::Actor*>(entity)->AddComponent<RVX::StaticMeshComponent>();
        TEST_ASSERT_NOT_NULL(primitive);
        TEST_ASSERT_TRUE(primitive->IsRegistered());
        TEST_ASSERT_EQ(static_cast<size_t>(1), sceneManager.GetPrimitives().size());

        TEST_ASSERT_TRUE(static_cast<RVX::Actor*>(entity)->RemoveComponent<RVX::StaticMeshComponent>());
        TEST_ASSERT_TRUE(sceneManager.GetPrimitives().empty());

        sceneManager.Shutdown();
        return true;
    }

    bool Test_SceneEntityAddComponentAutoRegistersStaticMeshPrimitive()
    {
        RVX::SceneManager sceneManager;
        sceneManager.Initialize();

        const auto handle = sceneManager.CreateEntity("LegacyAddComponentPrimitive");
        auto* entity = sceneManager.GetEntity(handle);
        TEST_ASSERT_NOT_NULL(entity);

        auto* primitive = entity->AddComponent<RVX::StaticMeshComponent>();
        TEST_ASSERT_NOT_NULL(primitive);
        TEST_ASSERT_TRUE(primitive->IsRegistered());
        TEST_ASSERT_EQ(static_cast<size_t>(1), sceneManager.GetPrimitives().size());
        TEST_ASSERT_EQ(primitive, sceneManager.GetPrimitives()[0]);

        sceneManager.Shutdown();
        return true;
    }

    class TestLegacyComponent : public RVX::Component
    {
    public:
        const char* GetTypeName() const override { return "TestLegacyComponent"; }
    };

    bool Test_SceneEntityAndComponentAreActorCompatible()
    {
        TEST_ASSERT_TRUE((std::is_base_of_v<RVX::Actor, RVX::SceneEntity>));
        TEST_ASSERT_TRUE((std::is_base_of_v<RVX::ActorComponent, RVX::Component>));

        RVX::SceneEntity entity("CompatEntity");
        auto* component = entity.AddComponent<TestLegacyComponent>();
        auto* actorComponent = static_cast<RVX::ActorComponent*>(component);

        TEST_ASSERT_NOT_NULL(component);
        TEST_ASSERT_EQ(&entity, component->GetOwner());
        TEST_ASSERT_EQ(static_cast<RVX::Actor*>(&entity), actorComponent->GetOwner());
        return true;
    }

    bool Test_ActorAndComponentFactoriesCreateRegisteredTypes()
    {
        RVX::ActorFactory::ClearAll();
        RVX::ActorFactory::Register<RVX::Actor>("Actor");
        auto actor = RVX::ActorFactory::Create("Actor");
        TEST_ASSERT_NOT_NULL(actor.get());
        TEST_ASSERT_EQ(std::string("Actor"), std::string(actor->GetClassName()));

        RVX::ComponentFactory::ClearComponentClasses();
        RVX::ComponentFactory::RegisterComponentClass<RVX::SceneComponent>("SceneComponent");
        auto component = RVX::ComponentFactory::CreateComponentByClassName("SceneComponent");
        TEST_ASSERT_NOT_NULL(component.get());
        TEST_ASSERT_EQ(std::string("SceneComponent"), std::string(component->GetClassName()));
        return true;
    }

    bool Test_ActorTransformForwardsToRootComponent()
    {
        RVX::Actor actor("RootTransformActor");
        auto* root = actor.AddComponent<RVX::SceneComponent>();
        actor.SetRootComponent(root);

        actor.SetPosition(RVX::Vec3(3.0f, 4.0f, 5.0f));
        actor.SetScale(RVX::Vec3(2.0f, 3.0f, 4.0f));

        TEST_ASSERT_EQ(RVX::Vec3(3.0f, 4.0f, 5.0f), root->GetRelativeLocation());
        TEST_ASSERT_EQ(RVX::Vec3(3.0f, 4.0f, 5.0f), actor.GetWorldPosition());
        TEST_ASSERT_EQ(RVX::Vec3(2.0f, 3.0f, 4.0f), root->GetRelativeScale3D());
        TEST_ASSERT_EQ(RVX::Vec3(2.0f, 3.0f, 4.0f), actor.GetWorldScale());
        TEST_ASSERT_EQ(root->GetWorldTransform(), actor.GetWorldMatrix());
        return true;
    }

    bool Test_SceneEntityCreatesRootAndForwardsTransform()
    {
        RVX::SceneEntity entity("CompatTransformEntity");
        auto* root = entity.GetRootComponent();
        TEST_ASSERT_NOT_NULL(root);

        entity.SetPosition(RVX::Vec3(7.0f, 8.0f, 9.0f));
        entity.SetScale(RVX::Vec3(2.0f, 2.0f, 2.0f));

        TEST_ASSERT_EQ(RVX::Vec3(7.0f, 8.0f, 9.0f), entity.GetPosition());
        TEST_ASSERT_EQ(RVX::Vec3(7.0f, 8.0f, 9.0f), root->GetRelativeLocation());
        TEST_ASSERT_EQ(RVX::Vec3(7.0f, 8.0f, 9.0f), entity.GetWorldPosition());
        TEST_ASSERT_EQ(RVX::Vec3(2.0f, 2.0f, 2.0f), entity.GetScale());
        TEST_ASSERT_EQ(root->GetWorldTransform(), entity.GetWorldMatrix());
        return true;
    }

    bool Test_SceneEntityHierarchyAttachesRootComponents()
    {
        RVX::SceneEntity parent("ParentEntity");
        RVX::SceneEntity child("ChildEntity");

        parent.SetPosition(RVX::Vec3(10.0f, 0.0f, 0.0f));
        child.SetPosition(RVX::Vec3(2.0f, 0.0f, 0.0f));

        parent.AddChild(&child);

        TEST_ASSERT_EQ(parent.GetRootComponent(), child.GetRootComponent()->GetAttachParent());
        TEST_ASSERT_EQ(RVX::Vec3(12.0f, 0.0f, 0.0f), child.GetWorldPosition());

        parent.SetPosition(RVX::Vec3(20.0f, 0.0f, 0.0f));
        TEST_ASSERT_EQ(RVX::Vec3(22.0f, 0.0f, 0.0f), child.GetWorldPosition());

        TEST_ASSERT_TRUE(parent.RemoveChild(&child));
        TEST_ASSERT_EQ(nullptr, child.GetRootComponent()->GetAttachParent());
        TEST_ASSERT_EQ(RVX::Vec3(2.0f, 0.0f, 0.0f), child.GetWorldPosition());
        return true;
    }

    bool Test_SceneEntityTransformStaysSyncedThroughActorAndRootPaths()
    {
        RVX::SceneEntity entity("SyncEntity");
        RVX::Actor* actor = &entity;

        actor->SetPosition(RVX::Vec3(3.0f, 0.0f, 0.0f));
        entity.Translate(RVX::Vec3(2.0f, 0.0f, 0.0f));
        TEST_ASSERT_EQ(RVX::Vec3(5.0f, 0.0f, 0.0f), entity.GetPosition());

        entity.GetRootComponent()->SetRelativeLocation(RVX::Vec3(4.0f, 0.0f, 0.0f));
        entity.Translate(RVX::Vec3(1.0f, 0.0f, 0.0f));
        TEST_ASSERT_EQ(RVX::Vec3(5.0f, 0.0f, 0.0f), entity.GetPosition());

        entity.GetRootComponent()->SetRelativeLocation(RVX::Vec3(9.0f, 0.0f, 0.0f));
        entity.SetPosition(RVX::Vec3(5.0f, 0.0f, 0.0f));
        TEST_ASSERT_EQ(RVX::Vec3(5.0f, 0.0f, 0.0f), entity.GetRootComponent()->GetRelativeLocation());
        return true;
    }

    bool Test_SceneEntityDestructionMaintainsHierarchy()
    {
        RVX::SceneEntity parent("PersistentParent");
        {
            auto child = std::make_unique<RVX::SceneEntity>("TemporaryChild");
            parent.AddChild(child.get());
            TEST_ASSERT_EQ(static_cast<size_t>(1), parent.GetChildCount());
        }
        TEST_ASSERT_EQ(static_cast<size_t>(0), parent.GetChildCount());

        auto child = std::make_unique<RVX::SceneEntity>("PersistentChild");
        {
            auto temporaryParent = std::make_unique<RVX::SceneEntity>("TemporaryParent");
            temporaryParent->AddChild(child.get());
            TEST_ASSERT_EQ(temporaryParent.get(), child->GetParent());
        }

        TEST_ASSERT_EQ(nullptr, child->GetParent());
        TEST_ASSERT_EQ(nullptr, child->GetRootComponent()->GetAttachParent());
        return true;
    }

    bool Test_SceneEntityCompatRootCannotBeRemovedThroughActorAPI()
    {
        RVX::SceneEntity entity("RootProtectedEntity");
        RVX::Actor* actor = &entity;
        auto* root = entity.GetRootComponent();

        TEST_ASSERT_NOT_NULL(root);
        TEST_ASSERT_FALSE(actor->RemoveComponent<RVX::SceneComponent>());
        TEST_ASSERT_EQ(root, entity.GetRootComponent());

        entity.SetPosition(RVX::Vec3(6.0f, 0.0f, 0.0f));
        TEST_ASSERT_EQ(RVX::Vec3(6.0f, 0.0f, 0.0f), entity.GetWorldPosition());
        return true;
    }

    bool Test_SceneEntityRootReplacementKeepsCompatibility()
    {
        RVX::SceneEntity parent("ReplacementParent");
        RVX::SceneEntity child("ReplacementChild");
        RVX::Actor* childActor = &child;

        parent.SetPosition(RVX::Vec3(10.0f, 0.0f, 0.0f));
        child.SetPosition(RVX::Vec3(2.0f, 0.0f, 0.0f));
        parent.AddChild(&child);

        auto* originalRoot = child.GetRootComponent();
        childActor->SetRootComponent(nullptr);
        TEST_ASSERT_EQ(originalRoot, child.GetRootComponent());

        auto* replacementRoot = childActor->AddComponent<RVX::SceneComponent>();
        replacementRoot->SetRelativeLocation(RVX::Vec3(3.0f, 0.0f, 0.0f));

        childActor->SetRootComponent(replacementRoot);

        TEST_ASSERT_EQ(replacementRoot, child.GetRootComponent());
        TEST_ASSERT_EQ(parent.GetRootComponent(), replacementRoot->GetAttachParent());
        TEST_ASSERT_EQ(RVX::Vec3(3.0f, 0.0f, 0.0f), child.GetPosition());
        TEST_ASSERT_EQ(RVX::Vec3(13.0f, 0.0f, 0.0f), child.GetWorldPosition());
        return true;
    }

    bool Test_RootComponentTransformMarksSceneEntityDirty()
    {
        RVX::SceneEntity entity("RootDirtyEntity");
        entity.SetLocalBounds(RVX::AABB(RVX::Vec3(-1.0f), RVX::Vec3(1.0f)));

        auto initialBounds = entity.GetWorldBounds();
        TEST_ASSERT_EQ(RVX::Vec3(0.0f), initialBounds.GetCenter());

        entity.ClearSpatialDirty();
        TEST_ASSERT_FALSE(entity.IsSpatialDirty());

        entity.GetRootComponent()->SetRelativeLocation(RVX::Vec3(10.0f, 0.0f, 0.0f));

        TEST_ASSERT_TRUE(entity.IsSpatialDirty());
        auto updatedBounds = entity.GetWorldBounds();
        TEST_ASSERT_EQ(RVX::Vec3(10.0f, 0.0f, 0.0f), updatedBounds.GetCenter());
        return true;
    }

    bool Test_SceneEntityRootReplacementRejectsInvalidAttachment()
    {
        RVX::SceneEntity parent("CycleParent");
        RVX::SceneEntity child("CycleChild");
        RVX::Actor* childActor = &child;

        parent.AddChild(&child);
        auto* originalRoot = child.GetRootComponent();
        auto* replacementRoot = childActor->AddComponent<RVX::SceneComponent>();

        TEST_ASSERT_TRUE(parent.GetRootComponent()->AttachToComponent(replacementRoot));

        childActor->SetRootComponent(replacementRoot);

        TEST_ASSERT_EQ(originalRoot, child.GetRootComponent());
        TEST_ASSERT_EQ(parent.GetRootComponent(), originalRoot->GetAttachParent());

        parent.GetRootComponent()->DetachFromComponent();
        return true;
    }

    bool Test_SceneEntityRootReplacementDetachesExternalParentWhenRootEntity()
    {
        RVX::SceneEntity externalParent("ExternalParent");
        RVX::SceneEntity entity("RootReplacementEntity");
        RVX::Actor* actor = &entity;

        externalParent.SetPosition(RVX::Vec3(100.0f, 0.0f, 0.0f));
        auto* replacementRoot = actor->AddComponent<RVX::SceneComponent>();
        replacementRoot->SetRelativeLocation(RVX::Vec3(5.0f, 0.0f, 0.0f));

        TEST_ASSERT_TRUE(replacementRoot->AttachToComponent(externalParent.GetRootComponent()));

        actor->SetRootComponent(replacementRoot);

        TEST_ASSERT_EQ(replacementRoot, entity.GetRootComponent());
        TEST_ASSERT_EQ(nullptr, replacementRoot->GetAttachParent());
        TEST_ASSERT_EQ(RVX::Vec3(5.0f, 0.0f, 0.0f), entity.GetWorldPosition());
        return true;
    }

    bool Test_SceneEntityAddChildRejectsInconsistentComponentCycle()
    {
        RVX::SceneEntity parent("CycleAttachmentParent");
        RVX::SceneEntity child("CycleAttachmentChild");

        TEST_ASSERT_TRUE(parent.GetRootComponent()->AttachToComponent(child.GetRootComponent()));

        parent.AddChild(&child);

        TEST_ASSERT_EQ(nullptr, child.GetParent());
        TEST_ASSERT_EQ(static_cast<size_t>(0), parent.GetChildCount());
        TEST_ASSERT_EQ(child.GetRootComponent(), parent.GetRootComponent()->GetAttachParent());

        parent.GetRootComponent()->DetachFromComponent();
        return true;
    }

    bool Test_SceneManagerSpawnActorCreatesSceneOwnedActor()
    {
        RVX::SceneManager sceneManager;
        sceneManager.Initialize();

        RVX::ActorSpawnParams params;
        params.name = "SpawnedActor";
        params.localPosition = RVX::Vec3(3.0f, 4.0f, 5.0f);
        params.localScale = RVX::Vec3(2.0f, 3.0f, 4.0f);

        auto* actor = sceneManager.SpawnActor<SpawnableSceneActor>(params);
        TEST_ASSERT_NOT_NULL(actor);
        TEST_ASSERT_EQ(actor, sceneManager.GetEntity(actor->GetHandle()));
        TEST_ASSERT_EQ(std::string("SpawnedActor"), actor->GetName());
        TEST_ASSERT_EQ(params.localPosition, actor->GetPosition());
        TEST_ASSERT_EQ(params.localPosition, actor->GetWorldPosition());
        TEST_ASSERT_EQ(params.localScale, actor->GetScale());
        TEST_ASSERT_EQ(params.localScale, actor->GetWorldScale());
        TEST_ASSERT_EQ(static_cast<size_t>(1), sceneManager.GetEntityCount());

        sceneManager.Shutdown();
        return true;
    }

    bool Test_SceneManagerSpawnActorAttachesParent()
    {
        RVX::SceneManager sceneManager;
        sceneManager.Initialize();

        RVX::ActorSpawnParams parentParams;
        parentParams.name = "SpawnParent";
        parentParams.localPosition = RVX::Vec3(10.0f, 2.0f, 0.0f);
        parentParams.localRotation = RVX::Quat(0.9238795f, 0.0f, 0.3826834f, 0.0f);
        parentParams.localScale = RVX::Vec3(2.0f, 2.0f, 2.0f);
        auto* parent = sceneManager.SpawnActor<SpawnableSceneActor>(parentParams);
        TEST_ASSERT_NOT_NULL(parent);

        RVX::ActorSpawnParams childParams;
        childParams.name = "SpawnChild";
        childParams.localPosition = RVX::Vec3(1.0f, 2.0f, 3.0f);
        childParams.localRotation = RVX::Quat(0.9659258f, 0.2588190f, 0.0f, 0.0f);
        childParams.localScale = RVX::Vec3(0.5f, 0.75f, 1.25f);
        childParams.parent = parent;

        auto* child = sceneManager.SpawnActor<SpawnableSceneActor>(childParams);
        TEST_ASSERT_NOT_NULL(child);
        TEST_ASSERT_EQ(parent, child->GetParent());
        TEST_ASSERT_EQ(static_cast<size_t>(1), parent->GetChildCount());
        TEST_ASSERT_EQ(parent->GetRootComponent(), child->GetRootComponent()->GetAttachParent());
        TEST_ASSERT_EQ(childParams.localPosition, child->GetPosition());
        TEST_ASSERT_EQ(childParams.localScale, child->GetScale());
        TEST_ASSERT_EQ(parent->GetWorldMatrix() * child->GetLocalMatrix(), child->GetWorldMatrix());
        TEST_ASSERT_EQ(static_cast<size_t>(2), sceneManager.GetEntityCount());

        sceneManager.Shutdown();
        return true;
    }

    bool Test_SceneManagerSpawnActorRejectsForeignParent()
    {
        RVX::SceneManager sourceScene;
        RVX::SceneManager targetScene;
        sourceScene.Initialize();
        targetScene.Initialize();

        RVX::ActorSpawnParams parentParams;
        parentParams.name = "ForeignParent";
        auto* foreignParent = sourceScene.SpawnActor<SpawnableSceneActor>(parentParams);
        TEST_ASSERT_NOT_NULL(foreignParent);

        RVX::ActorSpawnParams childParams;
        childParams.name = "RejectedChild";
        childParams.parent = foreignParent;
        TEST_ASSERT_EQ(nullptr, targetScene.SpawnActor<SpawnableSceneActor>(childParams));
        TEST_ASSERT_EQ(static_cast<size_t>(0), targetScene.GetEntityCount());

        RVX::SceneEntity stackParent("StackParent");
        childParams.parent = &stackParent;
        TEST_ASSERT_EQ(nullptr, targetScene.SpawnActor<SpawnableSceneActor>(childParams));
        TEST_ASSERT_EQ(static_cast<size_t>(0), targetScene.GetEntityCount());

        sourceScene.Shutdown();
        targetScene.Shutdown();
        return true;
    }

    bool Test_SceneManagerDestroyActorUsesActorPointer()
    {
        RVX::SceneManager sceneManager;
        sceneManager.Initialize();

        RVX::ActorSpawnParams params;
        params.name = "DestroyMe";
        auto* actor = sceneManager.SpawnActor<SpawnableSceneActor>(params);
        TEST_ASSERT_NOT_NULL(actor);
        const auto handle = actor->GetHandle();

        TEST_ASSERT_TRUE(sceneManager.DestroyActor(static_cast<RVX::Actor*>(actor)));
        TEST_ASSERT_EQ(static_cast<size_t>(0), sceneManager.GetEntityCount());
        TEST_ASSERT_EQ(nullptr, sceneManager.GetEntity(handle));

        sceneManager.Shutdown();
        return true;
    }

    bool Test_SceneManagerDestroyActorDefersDuringLifecycleDispatch()
    {
        RVX::SceneManager sceneManager;
        sceneManager.Initialize();

        bool destroyAccepted = false;
        RVX::ActorSpawnParams params;
        params.name = "DeferredDestroy";
        auto* actor = sceneManager.SpawnActor<SpawnableSceneActor>(params);
        TEST_ASSERT_NOT_NULL(actor);
        actor->AddComponent<DestroyOwnerActorComponent>(&sceneManager, &destroyAccepted);

        sceneManager.Update(0.016f);

        TEST_ASSERT_TRUE(destroyAccepted);
        TEST_ASSERT_EQ(static_cast<size_t>(0), sceneManager.GetEntityCount());

        sceneManager.Shutdown();
        return true;
    }

    bool Test_SceneManagerDestroyActorRejectsDuplicatePendingDestroy()
    {
        RVX::SceneManager sceneManager;
        sceneManager.Initialize();

        bool firstDestroy = false;
        bool secondDestroy = true;
        RVX::ActorSpawnParams params;
        params.name = "DuplicateDestroy";
        auto* actor = sceneManager.SpawnActor<SpawnableSceneActor>(params);
        TEST_ASSERT_NOT_NULL(actor);
        actor->AddComponent<DestroyOwnerActorComponent>(&sceneManager, &firstDestroy, &secondDestroy);

        sceneManager.Update(0.016f);

        TEST_ASSERT_TRUE(firstDestroy);
        TEST_ASSERT_FALSE(secondDestroy);
        TEST_ASSERT_EQ(static_cast<size_t>(0), sceneManager.GetEntityCount());

        sceneManager.Shutdown();
        return true;
    }

    bool Test_WorldSpawnActorDelegatesToSceneManager()
    {
        RVX::World world;
        world.Initialize();

        RVX::ActorSpawnParams params;
        params.name = "WorldSpawned";
        auto* actor = world.SpawnActor<SpawnableSceneActor>(params);
        TEST_ASSERT_NOT_NULL(actor);
        TEST_ASSERT_NOT_NULL(world.GetSceneManager());
        TEST_ASSERT_EQ(actor, world.GetSceneManager()->GetEntity(actor->GetHandle()));
        TEST_ASSERT_TRUE(world.DestroyActor(actor));
        TEST_ASSERT_EQ(static_cast<size_t>(0), world.GetSceneManager()->GetEntityCount());

        world.Shutdown();
        return true;
    }

    bool Test_WorldSpawnActorRejectsWhenUninitialized()
    {
        RVX::World world;
        RVX::ActorSpawnParams params;
        params.name = "RejectedWorldSpawn";
        RVX::Actor actor("StackActor");

        TEST_ASSERT_EQ(nullptr, world.SpawnActor<SpawnableSceneActor>(params));
        TEST_ASSERT_FALSE(world.DestroyActor(&actor));
        return true;
    }

    bool Test_DestroyActorRejectsForeignOrPureActor()
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
        TEST_ASSERT_NOT_NULL(foreignActor);

        TEST_ASSERT_FALSE(sceneManager.DestroyActor(nullptr));
        TEST_ASSERT_FALSE(sceneManager.DestroyActor(&pureActor));
        TEST_ASSERT_FALSE(sceneManager.DestroyActor(&stackEntity));
        TEST_ASSERT_FALSE(sceneManager.DestroyActor(foreignActor));
        TEST_ASSERT_EQ(static_cast<size_t>(0), sceneManager.GetEntityCount());
        TEST_ASSERT_EQ(static_cast<size_t>(1), foreignScene.GetEntityCount());

        sceneManager.Shutdown();
        foreignScene.Shutdown();
        return true;
    }
} // namespace

int main()
{
    RVX::Log::Initialize();
    RVX_CORE_INFO("Actor Component Validation Tests");

    TestSuite suite;
    suite.AddTest("ActorComponentLifecycleDefaults", Test_ActorComponentLifecycleDefaults);
    suite.AddTest("ActorOwnsComponentsAndDispatchesLifecycle",
                  Test_ActorOwnsComponentsAndDispatchesLifecycle);
    suite.AddTest("SceneManagerUpdateDispatchesActorComponentLifecycle",
                  Test_SceneManagerUpdateDispatchesActorComponentLifecycle);
    suite.AddTest("SceneManagerBeginsComponentsAddedAfterActorBeginsPlay",
                  Test_SceneManagerBeginsComponentsAddedAfterActorBeginsPlay);
    suite.AddTest("SceneManagerUpdateKeepsLegacyComponentTicking",
                  Test_SceneManagerUpdateKeepsLegacyComponentTicking);
    suite.AddTest("SceneEntityActiveStateControlsActorTickThroughActorPointer",
                  Test_SceneEntityActiveStateControlsActorTickThroughActorPointer);
    suite.AddTest("SceneManagerDefersDestroyRequestsDuringLifecycleDispatch",
                  Test_SceneManagerDefersDestroyRequestsDuringLifecycleDispatch);
    suite.AddTest("SceneManagerDestroyEntityDispatchesEndPlayBeforeUnregister",
                  Test_SceneManagerDestroyEntityDispatchesEndPlayBeforeUnregister);
    suite.AddTest("SceneManagerDestroyEntityBeforeBeginPlayDoesNotDispatchEndPlay",
                  Test_SceneManagerDestroyEntityBeforeBeginPlayDoesNotDispatchEndPlay);
    suite.AddTest("ActorTickSnapshotsComponentsAddedDuringTick",
                  Test_ActorTickSnapshotsComponentsAddedDuringTick);
    suite.AddTest("ActorDefersSelfRemovalDuringTick",
                  Test_ActorDefersSelfRemovalDuringTick);
    suite.AddTest("ActorDispatchesEndPlayForBegunComponentRemovedDuringTick",
                  Test_ActorDispatchesEndPlayForBegunComponentRemovedDuringTick);
    suite.AddTest("ActorSkipsComponentRemovedEarlierInTick",
                  Test_ActorSkipsComponentRemovedEarlierInTick);
    suite.AddTest("ActorBeginPlaySnapshotsComponentsAddedDuringBeginPlay",
                  Test_ActorBeginPlaySnapshotsComponentsAddedDuringBeginPlay);
    suite.AddTest("ActorDefersSelfRemovalDuringEndPlay",
                  Test_ActorDefersSelfRemovalDuringEndPlay);
    suite.AddTest("ActorDirectRemovalHandlesSelfRemovalDuringEndPlay",
                  Test_ActorDirectRemovalHandlesSelfRemovalDuringEndPlay);
    suite.AddTest("ActorSkipsComponentRemovedEarlierInEndPlay",
                  Test_ActorSkipsComponentRemovedEarlierInEndPlay);
    suite.AddTest("ActorKeepsEndPlaySuppressedAfterNestedEndPlayReturnsToTick",
                  Test_ActorKeepsEndPlaySuppressedAfterNestedEndPlayReturnsToTick);
    suite.AddTest("ActorDispatchesEndPlayForComponentQueuedBeforeNestedEndPlay",
                  Test_ActorDispatchesEndPlayForComponentQueuedBeforeNestedEndPlay);
    suite.AddTest("ActorDestructionClearsPendingRemovalWithoutDoubleDestroy",
                  Test_ActorDestructionClearsPendingRemovalWithoutDoubleDestroy);
    suite.AddTest("SceneComponentAttachmentComputesWorldTransform",
                  Test_SceneComponentAttachmentComputesWorldTransform);
    suite.AddTest("PrimitiveComponentTracksVisibilityLayerAndBounds",
                  Test_PrimitiveComponentTracksVisibilityLayerAndBounds);
    suite.AddTest("StaticMeshComponentIsPrimitiveSceneComponent",
                  Test_StaticMeshComponentIsPrimitiveSceneComponent);
    suite.AddTest("StaticMeshComponentRegistersWithSceneManager",
                  Test_StaticMeshComponentRegistersWithSceneManager);
    suite.AddTest("RuntimeStaticMeshComponentRemovalUnregistersPrimitive",
                  Test_RuntimeStaticMeshComponentRemovalUnregistersPrimitive);
    suite.AddTest("SceneEntityAddComponentAutoRegistersStaticMeshPrimitive",
                  Test_SceneEntityAddComponentAutoRegistersStaticMeshPrimitive);
    suite.AddTest("SceneEntityAndComponentAreActorCompatible",
                  Test_SceneEntityAndComponentAreActorCompatible);
    suite.AddTest("ActorAndComponentFactoriesCreateRegisteredTypes",
                  Test_ActorAndComponentFactoriesCreateRegisteredTypes);
    suite.AddTest("ActorTransformForwardsToRootComponent",
                  Test_ActorTransformForwardsToRootComponent);
    suite.AddTest("SceneEntityCreatesRootAndForwardsTransform",
                  Test_SceneEntityCreatesRootAndForwardsTransform);
    suite.AddTest("SceneEntityHierarchyAttachesRootComponents",
                  Test_SceneEntityHierarchyAttachesRootComponents);
    suite.AddTest("SceneEntityTransformStaysSyncedThroughActorAndRootPaths",
                  Test_SceneEntityTransformStaysSyncedThroughActorAndRootPaths);
    suite.AddTest("SceneEntityDestructionMaintainsHierarchy",
                  Test_SceneEntityDestructionMaintainsHierarchy);
    suite.AddTest("SceneEntityCompatRootCannotBeRemovedThroughActorAPI",
                  Test_SceneEntityCompatRootCannotBeRemovedThroughActorAPI);
    suite.AddTest("SceneEntityRootReplacementKeepsCompatibility",
                  Test_SceneEntityRootReplacementKeepsCompatibility);
    suite.AddTest("RootComponentTransformMarksSceneEntityDirty",
                  Test_RootComponentTransformMarksSceneEntityDirty);
    suite.AddTest("SceneEntityRootReplacementRejectsInvalidAttachment",
                  Test_SceneEntityRootReplacementRejectsInvalidAttachment);
    suite.AddTest("SceneEntityRootReplacementDetachesExternalParentWhenRootEntity",
                  Test_SceneEntityRootReplacementDetachesExternalParentWhenRootEntity);
    suite.AddTest("SceneEntityAddChildRejectsInconsistentComponentCycle",
                  Test_SceneEntityAddChildRejectsInconsistentComponentCycle);
    suite.AddTest("SceneManagerSpawnActorCreatesSceneOwnedActor",
                  Test_SceneManagerSpawnActorCreatesSceneOwnedActor);
    suite.AddTest("SceneManagerSpawnActorAttachesParent",
                  Test_SceneManagerSpawnActorAttachesParent);
    suite.AddTest("SceneManagerSpawnActorRejectsForeignParent",
                  Test_SceneManagerSpawnActorRejectsForeignParent);
    suite.AddTest("SceneManagerDestroyActorUsesActorPointer",
                  Test_SceneManagerDestroyActorUsesActorPointer);
    suite.AddTest("SceneManagerDestroyActorDefersDuringLifecycleDispatch",
                  Test_SceneManagerDestroyActorDefersDuringLifecycleDispatch);
    suite.AddTest("SceneManagerDestroyActorRejectsDuplicatePendingDestroy",
                  Test_SceneManagerDestroyActorRejectsDuplicatePendingDestroy);
    suite.AddTest("WorldSpawnActorDelegatesToSceneManager",
                  Test_WorldSpawnActorDelegatesToSceneManager);
    suite.AddTest("WorldSpawnActorRejectsWhenUninitialized",
                  Test_WorldSpawnActorRejectsWhenUninitialized);
    suite.AddTest("DestroyActorRejectsForeignOrPureActor",
                  Test_DestroyActorRejectsForeignOrPureActor);

    auto results = suite.Run();
    suite.PrintResults(results);

    RVX::Log::Shutdown();

    for (const auto& result : results)
    {
        if (!result.passed)
            return 1;
    }

    return 0;
}
