# UE-Style Actor Component Phase 4 Spatial Picking Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move spatial indexing and picking toward UE-style actor/component hits by indexing `PrimitiveComponent` through a compatibility proxy while preserving existing `SceneEntity*` query APIs.

**Architecture:** `SceneManager` remains the owner of spatial integration during this migration phase. Registered `PrimitiveComponent` instances get internal `ISpatialEntity` proxy objects with guarded, reserved-range stable handles; query resolution is handle-based and never depends on `QueryResult::userData`. `SpatialSubsystem` reuses `SceneManager` collection and resolution helpers so world picking and scene queries produce the same hit metadata. Entity fallback is explicit: an owner with at least one spatially indexable primitive is indexed by those primitives, while an owner with no indexable primitives continues to use the legacy `SceneEntity` spatial entry.

**Tech Stack:** C++20, RenderVerseX Scene/World/Spatial modules, custom validation executables, CMake.

---

## File Structure

- Modify `Scene/Include/Scene/PrimitiveComponent.h`
  - Move `SetLayerMask()` out of the inline body so layer changes can mark spatial state dirty.
  - Add `IsSpatialDirty()`, `ClearSpatialDirty()`, and private `MarkSpatialDirty()` for proxy-driven index updates.

- Modify `Scene/Private/PrimitiveComponent.cpp`
  - Mark primitive spatial state dirty when bounds, layer mask, or transform changes.
  - Notify the owning `SceneEntity` through `MarkBoundsDirty()` so `SceneManager::CollectDirtyEntities()` can detect the update.

- Modify `Scene/Include/Scene/SceneManager.h`
  - Extend `RaycastHit` with `Actor* actor` and `PrimitiveComponent* component`.
  - Add `SpatialQueryTarget` for resolving raw `Spatial::QueryResult` objects.
  - Add primitive query compatibility methods: `QueryVisiblePrimitives`, `QuerySpherePrimitives`, `QueryBoxPrimitives`.
  - Add `CollectSpatialEntities()` and `ResolveSpatialQueryTarget()` for `World::SpatialSubsystem`.
  - Add private primitive proxy storage, guarded handle allocation, and helper declarations.

- Modify `Scene/Private/SceneManager.cpp`
  - Define an internal `PrimitiveSpatialProxy : Spatial::ISpatialEntity`.
  - Create/destroy proxies in `RegisterPrimitive()` / `UnregisterPrimitive()`.
  - Build spatial indexes from valid primitive proxies first, then fallback `SceneEntity` entries for entities without spatial primitives.
  - Resolve query results to actor/entity/component targets and populate `RaycastHit`.
  - Rebuild the index when primitive-backed entities become dirty; keep legacy incremental updates for entity-only scenes.

- Modify `World/Private/SpatialSubsystem.cpp`
  - Build the subsystem index from `SceneManager::CollectSpatialEntities()`.
  - Resolve query results via `SceneManager::ResolveSpatialQueryTarget()` instead of casting `userData` to `SceneEntity*`.
  - Deduplicate `SceneEntity*` outputs for compatibility entity-list queries when an actor has multiple primitives.
  - Populate `RaycastHit::actor` and `RaycastHit::component`.

- Create `Tests/SpatialComponentValidation/main.cpp`
  - New validation target covering primitive spatial queries, raycasts, world spatial subsystem, and picking.

- Modify `Tests/CMakeLists.txt`
  - Add `SpatialComponentValidation` executable linked with `RVX_TestFramework`, `RVX::World`, `RVX::Scene`, and `RVX::Resource`.

- Modify `Tests/SystemIntegration/main.cpp`
  - Add assertions that legacy `RaycastHit` still sets `entity` while new fields remain compatible.

---

## Task 1: Add Failing Spatial Component Tests

**Files:**
- Create: `Tests/SpatialComponentValidation/main.cpp`
- Modify: `Tests/CMakeLists.txt`
- Modify: `Tests/SystemIntegration/main.cpp`

- [ ] **Step 1: Write the failing test target**

Create `Tests/SpatialComponentValidation/main.cpp` with these tests:

```cpp
#include "Core/Core.h"
#include "Scene/Actor.h"
#include "Scene/Components/StaticMeshComponent.h"
#include "Scene/PrimitiveComponent.h"
#include "Scene/SceneManager.h"
#include "TestFramework/TestRunner.h"
#include "World/PickingService.h"
#include "World/SpatialSubsystem.h"
#include "World/World.h"

using namespace RVX;
using namespace RVX::Test;

namespace
{
    struct PrimitiveFixture
    {
        SceneEntity* entity = nullptr;
        StaticMeshComponent* primitive = nullptr;
    };

    PrimitiveFixture CreatePrimitive(SceneManager& sceneManager,
                                     const char* name,
                                     const Vec3& location,
                                     uint32 layerMask = ~0u)
    {
        const auto handle = sceneManager.CreateEntity(name);
        auto* entity = sceneManager.GetEntity(handle);
        auto* primitive = static_cast<Actor*>(entity)->AddComponent<StaticMeshComponent>();
        primitive->AttachToComponent(entity->GetRootComponent());
        primitive->SetLocalBounds(AABB(Vec3(-1.0f), Vec3(1.0f)));
        primitive->SetLayerMask(layerMask);
        entity->SetPosition(location);
        return {entity, primitive};
    }

    bool Test_PrimitiveRaycastReturnsActorAndComponent()
    {
        SceneManager sceneManager;
        sceneManager.Initialize();

        auto fixture = CreatePrimitive(sceneManager, "PrimitiveHit", Vec3(0.0f));
        sceneManager.Update(0.0f);
        sceneManager.RebuildSpatialIndex();

        RaycastHit hit;
        const bool didHit = sceneManager.Raycast(Ray(Vec3(0.0f, 0.0f, 10.0f),
                                                     Vec3(0.0f, 0.0f, -1.0f)),
                                                 hit);

        TEST_ASSERT_TRUE(didHit);
        TEST_ASSERT_EQ(fixture.entity, hit.entity);
        TEST_ASSERT_EQ(static_cast<Actor*>(fixture.entity), hit.actor);
        TEST_ASSERT_EQ(static_cast<PrimitiveComponent*>(fixture.primitive), hit.component);
        TEST_ASSERT_TRUE(hit.distance > 0.0f);

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrimitiveBoxQueryReturnsPrimitiveAndDeduplicatedEntity()
    {
        SceneManager sceneManager;
        sceneManager.Initialize();

        auto fixture = CreatePrimitive(sceneManager, "PrimitiveQuery", Vec3(0.0f));
        fixture.entity->SetLocalBounds(AABB(Vec3(-5.0f), Vec3(5.0f)));
        sceneManager.Update(0.0f);
        sceneManager.RebuildSpatialIndex();

        std::vector<PrimitiveComponent*> primitives;
        sceneManager.QueryBoxPrimitives(AABB(Vec3(-2.0f), Vec3(2.0f)), primitives);
        TEST_ASSERT_EQ(static_cast<size_t>(1), primitives.size());
        TEST_ASSERT_EQ(static_cast<PrimitiveComponent*>(fixture.primitive), primitives[0]);

        std::vector<SceneEntity*> entities;
        sceneManager.QueryBox(AABB(Vec3(-2.0f), Vec3(2.0f)), entities);
        TEST_ASSERT_EQ(static_cast<size_t>(1), entities.size());
        TEST_ASSERT_EQ(fixture.entity, entities[0]);

        sceneManager.Shutdown();
        return true;
    }

    bool Test_LegacyEntitySpatialQueriesStillWork()
    {
        SceneManager sceneManager;
        sceneManager.Initialize();

        const auto handle = sceneManager.CreateEntity("LegacySpatial");
        auto* entity = sceneManager.GetEntity(handle);
        entity->SetLocalBounds(AABB(Vec3(-1.0f), Vec3(1.0f)));
        sceneManager.Update(0.0f);
        sceneManager.RebuildSpatialIndex();

        RaycastHit hit;
        const bool didHit = sceneManager.Raycast(Ray(Vec3(0.0f, 0.0f, 10.0f),
                                                     Vec3(0.0f, 0.0f, -1.0f)),
                                                 hit);

        TEST_ASSERT_TRUE(didHit);
        TEST_ASSERT_EQ(entity, hit.entity);
        TEST_ASSERT_EQ(static_cast<Actor*>(entity), hit.actor);
        TEST_ASSERT_EQ(nullptr, hit.component);

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrimitiveLayerFilterAffectsSpatialQueries()
    {
        SceneManager sceneManager;
        sceneManager.Initialize();

        auto fixture = CreatePrimitive(sceneManager, "LayeredPrimitive", Vec3(0.0f), 1u << 3);
        sceneManager.Update(0.0f);
        sceneManager.RebuildSpatialIndex();

        RaycastHit hit;
        TEST_ASSERT_FALSE(sceneManager.Raycast(Ray(Vec3(0.0f, 0.0f, 10.0f),
                                                Vec3(0.0f, 0.0f, -1.0f)),
                                            Spatial::QueryFilter::Layer(2),
                                            hit));

        TEST_ASSERT_TRUE(sceneManager.Raycast(Ray(Vec3(0.0f, 0.0f, 10.0f),
                                               Vec3(0.0f, 0.0f, -1.0f)),
                                           Spatial::QueryFilter::Layer(3),
                                           hit));
        TEST_ASSERT_EQ(static_cast<PrimitiveComponent*>(fixture.primitive), hit.component);

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrimitiveCustomAndTypeFiltersUseSpatialEntityContract()
    {
        SceneManager sceneManager;
        sceneManager.Initialize();

        auto fixture = CreatePrimitive(sceneManager, "FilteredPrimitive", Vec3(0.0f), 1u << 4);
        sceneManager.Update(0.0f);
        sceneManager.RebuildSpatialIndex();

        Spatial::QueryFilter filter = Spatial::QueryFilter::Type(
            static_cast<uint32_t>(EntityType::Node));
        filter.customFilter = [](const Spatial::ISpatialEntity* entity) {
            return entity && entity->GetLayerMask() == (1u << 4);
        };

        RaycastHit hit;
        TEST_ASSERT_TRUE(sceneManager.Raycast(Ray(Vec3(0.0f, 0.0f, 10.0f),
                                               Vec3(0.0f, 0.0f, -1.0f)),
                                           filter,
                                           hit));
        TEST_ASSERT_EQ(fixture.entity, hit.entity);
        TEST_ASSERT_EQ(static_cast<PrimitiveComponent*>(fixture.primitive), hit.component);

        sceneManager.Shutdown();
        return true;
    }

    bool Test_NonIndexablePrimitiveFallsBackToLegacyEntityBounds()
    {
        SceneManager sceneManager;
        sceneManager.Initialize();

        auto fixture = CreatePrimitive(sceneManager, "DisabledPrimitiveFallback", Vec3(0.0f));
        fixture.primitive->SetEnabled(false);
        fixture.entity->SetLocalBounds(AABB(Vec3(-2.0f), Vec3(2.0f)));
        sceneManager.Update(0.0f);
        sceneManager.RebuildSpatialIndex();

        std::vector<PrimitiveComponent*> primitives;
        sceneManager.QueryBoxPrimitives(AABB(Vec3(-3.0f), Vec3(3.0f)), primitives);
        TEST_ASSERT_TRUE(primitives.empty());

        RaycastHit hit;
        TEST_ASSERT_TRUE(sceneManager.Raycast(Ray(Vec3(0.0f, 0.0f, 10.0f),
                                               Vec3(0.0f, 0.0f, -1.0f)),
                                           hit));
        TEST_ASSERT_EQ(fixture.entity, hit.entity);
        TEST_ASSERT_EQ(static_cast<Actor*>(fixture.entity), hit.actor);
        TEST_ASSERT_EQ(nullptr, hit.component);

        sceneManager.Shutdown();
        return true;
    }

    bool Test_MultiplePrimitivesDeduplicateEntityQueries()
    {
        SceneManager sceneManager;
        sceneManager.Initialize();

        auto fixture = CreatePrimitive(sceneManager, "MultiPrimitiveActor", Vec3(0.0f));
        auto* secondPrimitive = static_cast<Actor*>(fixture.entity)->AddComponent<StaticMeshComponent>();
        secondPrimitive->AttachToComponent(fixture.entity->GetRootComponent());
        secondPrimitive->SetRelativeLocation(Vec3(3.0f, 0.0f, 0.0f));
        secondPrimitive->SetLocalBounds(AABB(Vec3(-1.0f), Vec3(1.0f)));

        sceneManager.Update(0.0f);
        sceneManager.RebuildSpatialIndex();

        std::vector<PrimitiveComponent*> primitives;
        sceneManager.QueryBoxPrimitives(AABB(Vec3(-2.0f), Vec3(5.0f)), primitives);
        TEST_ASSERT_EQ(static_cast<size_t>(2), primitives.size());

        std::vector<SceneEntity*> entities;
        sceneManager.QueryBox(AABB(Vec3(-2.0f), Vec3(5.0f)), entities);
        TEST_ASSERT_EQ(static_cast<size_t>(1), entities.size());
        TEST_ASSERT_EQ(fixture.entity, entities[0]);

        sceneManager.Shutdown();
        return true;
    }

    bool Test_WorldSpatialSubsystemAndPickingReturnComponentHit()
    {
        World world;
        world.Initialize();

        auto* sceneManager = world.GetSceneManager();
        auto fixture = CreatePrimitive(*sceneManager, "WorldPrimitive", Vec3(0.0f));

        auto* spatial = world.GetSpatial();
        TEST_ASSERT_NOT_NULL(spatial);
        spatial->RebuildIndex();

        Ray ray(Vec3(0.0f, 0.0f, 10.0f), Vec3(0.0f, 0.0f, -1.0f));
        RaycastHit spatialHit;
        TEST_ASSERT_TRUE(spatial->Raycast(ray, spatialHit));
        TEST_ASSERT_EQ(fixture.entity, spatialHit.entity);
        TEST_ASSERT_EQ(static_cast<Actor*>(fixture.entity), spatialHit.actor);
        TEST_ASSERT_EQ(static_cast<PrimitiveComponent*>(fixture.primitive), spatialHit.component);

        PickingService picking(&world);
        RaycastHit pickHit;
        TEST_ASSERT_TRUE(picking.Pick(ray, pickHit));
        TEST_ASSERT_EQ(fixture.entity, pickHit.entity);
        TEST_ASSERT_EQ(static_cast<PrimitiveComponent*>(fixture.primitive), pickHit.component);

        world.Shutdown();
        return true;
    }

    bool Test_WorldSpatialSubsystemLegacyEntityDoesNotRequireUserData()
    {
        World world;
        world.Initialize();

        auto* sceneManager = world.GetSceneManager();
        const auto handle = sceneManager->CreateEntity("WorldLegacySpatial");
        auto* entity = sceneManager->GetEntity(handle);
        entity->SetLocalBounds(AABB(Vec3(-1.0f), Vec3(1.0f)));
        entity->SetUserData(nullptr);

        auto* spatial = world.GetSpatial();
        TEST_ASSERT_NOT_NULL(spatial);
        spatial->RebuildIndex();

        RaycastHit hit;
        TEST_ASSERT_TRUE(spatial->Raycast(Ray(Vec3(0.0f, 0.0f, 10.0f),
                                             Vec3(0.0f, 0.0f, -1.0f)),
                                         hit));
        TEST_ASSERT_EQ(entity, hit.entity);
        TEST_ASSERT_EQ(static_cast<Actor*>(entity), hit.actor);
        TEST_ASSERT_EQ(nullptr, hit.component);

        world.Shutdown();
        return true;
    }

    bool Test_WorldEntityQueriesDeduplicateMultiPrimitiveActors()
    {
        World world;
        world.Initialize();

        auto* sceneManager = world.GetSceneManager();
        auto fixture = CreatePrimitive(*sceneManager, "WorldMultiPrimitiveActor", Vec3(0.0f));
        auto* secondPrimitive = static_cast<Actor*>(fixture.entity)->AddComponent<StaticMeshComponent>();
        secondPrimitive->AttachToComponent(fixture.entity->GetRootComponent());
        secondPrimitive->SetRelativeLocation(Vec3(3.0f, 0.0f, 0.0f));
        secondPrimitive->SetLocalBounds(AABB(Vec3(-1.0f), Vec3(1.0f)));

        auto* spatial = world.GetSpatial();
        TEST_ASSERT_NOT_NULL(spatial);
        spatial->RebuildIndex();

        std::vector<SceneEntity*> entities;
        spatial->QueryBox(AABB(Vec3(-2.0f), Vec3(5.0f)), entities);
        TEST_ASSERT_EQ(static_cast<size_t>(1), entities.size());
        TEST_ASSERT_EQ(fixture.entity, entities[0]);

        world.Shutdown();
        return true;
    }
} // namespace

int main()
{
    Log::Initialize();
    RVX_CORE_INFO("Spatial Component Validation Tests");

    TestSuite suite;
    suite.AddTest("PrimitiveRaycastReturnsActorAndComponent",
                  Test_PrimitiveRaycastReturnsActorAndComponent);
    suite.AddTest("PrimitiveBoxQueryReturnsPrimitiveAndDeduplicatedEntity",
                  Test_PrimitiveBoxQueryReturnsPrimitiveAndDeduplicatedEntity);
    suite.AddTest("LegacyEntitySpatialQueriesStillWork",
                  Test_LegacyEntitySpatialQueriesStillWork);
    suite.AddTest("PrimitiveLayerFilterAffectsSpatialQueries",
                  Test_PrimitiveLayerFilterAffectsSpatialQueries);
    suite.AddTest("PrimitiveCustomAndTypeFiltersUseSpatialEntityContract",
                  Test_PrimitiveCustomAndTypeFiltersUseSpatialEntityContract);
    suite.AddTest("NonIndexablePrimitiveFallsBackToLegacyEntityBounds",
                  Test_NonIndexablePrimitiveFallsBackToLegacyEntityBounds);
    suite.AddTest("MultiplePrimitivesDeduplicateEntityQueries",
                  Test_MultiplePrimitivesDeduplicateEntityQueries);
    suite.AddTest("WorldSpatialSubsystemAndPickingReturnComponentHit",
                  Test_WorldSpatialSubsystemAndPickingReturnComponentHit);
    suite.AddTest("WorldSpatialSubsystemLegacyEntityDoesNotRequireUserData",
                  Test_WorldSpatialSubsystemLegacyEntityDoesNotRequireUserData);
    suite.AddTest("WorldEntityQueriesDeduplicateMultiPrimitiveActors",
                  Test_WorldEntityQueriesDeduplicateMultiPrimitiveActors);

    auto results = suite.Run();
    suite.PrintResults(results);

    Log::Shutdown();

    for (const auto& result : results)
    {
        if (!result.passed)
            return 1;
    }
    return 0;
}
```

- [ ] **Step 2: Add the failing test target to CMake**

Add this block after `ActorComponentValidation` in `Tests/CMakeLists.txt`:

```cmake
# Spatial component validation tests
add_executable(SpatialComponentValidation
    SpatialComponentValidation/main.cpp
)
target_link_libraries(SpatialComponentValidation PRIVATE
    RVX_TestFramework
    RVX::World
    RVX::Scene
    RVX::Resource
)
target_compile_features(SpatialComponentValidation PRIVATE cxx_std_20)
```

- [ ] **Step 3: Add compatibility assertions to SystemIntegration**

In `Tests/SystemIntegration/main.cpp`, after each successful `sceneManager.Raycast(...)` assertion, add:

```cpp
assert(hit.actor == static_cast<Actor*>(hit.entity));
assert(hit.component == nullptr);
```

- [ ] **Step 4: Run tests and verify RED**

Run:

```powershell
cmake --build build\Debug --config Debug --target SpatialComponentValidation SystemIntegrationTest
.\build\Debug\Tests\SpatialComponentValidation\Debug\SpatialComponentValidation.exe
```

Expected before implementation:

- Build fails because `RaycastHit::actor`, `RaycastHit::component`, and `SceneManager::QueryBoxPrimitives()` do not exist.

---

## Task 2: Add Primitive Spatial Proxy and SceneManager Query Resolution

**Files:**
- Modify: `Scene/Include/Scene/PrimitiveComponent.h`
- Modify: `Scene/Private/PrimitiveComponent.cpp`
- Modify: `Scene/Include/Scene/SceneManager.h`
- Modify: `Scene/Private/SceneManager.cpp`

- [ ] **Step 1: Extend PrimitiveComponent spatial dirty state**

In `Scene/Include/Scene/PrimitiveComponent.h`, replace the inline layer setter with declarations and add spatial dirty accessors:

```cpp
uint32 GetLayerMask() const { return m_layerMask; }
void SetLayerMask(uint32 layerMask);

bool IsSpatialDirty() const { return m_spatialDirty; }
void ClearSpatialDirty() { m_spatialDirty = false; }
```

Add this private helper/member:

```cpp
void MarkSpatialDirty();

bool m_spatialDirty = true;
```

In `Scene/Private/PrimitiveComponent.cpp`, implement:

```cpp
void PrimitiveComponent::SetLayerMask(uint32 layerMask)
{
    if (m_layerMask == layerMask)
        return;

    m_layerMask = layerMask;
    MarkSpatialDirty();
}

void PrimitiveComponent::MarkSpatialDirty()
{
    m_spatialDirty = true;

    auto* entity = dynamic_cast<SceneEntity*>(GetOwner());
    if (entity)
    {
        entity->MarkBoundsDirty();
    }
}
```

Update `SetLocalBounds()` and `OnTransformChanged()` to call `MarkSpatialDirty()`.

- [ ] **Step 2: Extend hit/query types**

In `Scene/Include/Scene/SceneManager.h`, forward declare `class Actor;` and extend `RaycastHit`:

```cpp
struct RaycastHit
{
    SceneEntity* entity = nullptr;
    Actor* actor = nullptr;
    PrimitiveComponent* component = nullptr;
    float distance = 0.0f;
    Vec3 hitPoint{0.0f};
    Vec3 hitNormal{0.0f, 1.0f, 0.0f};

    bool IsValid() const { return actor != nullptr || entity != nullptr; }
};
```

Add:

```cpp
struct SpatialQueryTarget
{
    SceneEntity* entity = nullptr;
    Actor* actor = nullptr;
    PrimitiveComponent* component = nullptr;

    bool IsValid() const { return actor != nullptr || entity != nullptr; }
};
```

- [ ] **Step 3: Add SceneManager public compatibility APIs**

Add declarations:

```cpp
void QueryVisiblePrimitives(const Frustum& frustum,
                            std::vector<PrimitiveComponent*>& outPrimitives);
void QueryVisiblePrimitives(const Frustum& frustum,
                            const Spatial::QueryFilter& filter,
                            std::vector<PrimitiveComponent*>& outPrimitives);
void QuerySpherePrimitives(const Vec3& center,
                           float radius,
                           std::vector<PrimitiveComponent*>& outPrimitives);
void QueryBoxPrimitives(const AABB& box,
                        std::vector<PrimitiveComponent*>& outPrimitives);

void CollectSpatialEntities(std::vector<Spatial::ISpatialEntity*>& outEntities) const;
SpatialQueryTarget ResolveSpatialQueryTarget(const Spatial::QueryResult& result) const;
```

Add private declarations:

```cpp
class PrimitiveSpatialProxy;

static constexpr Spatial::EntityHandle s_primitiveSpatialHandleStart = 0x80000000u;

std::unordered_map<PrimitiveComponent*, std::unique_ptr<PrimitiveSpatialProxy>> m_primitiveSpatialProxies;
std::unordered_map<Spatial::EntityHandle, PrimitiveSpatialProxy*> m_spatialProxyByHandle;
Spatial::EntityHandle m_nextPrimitiveSpatialHandle = s_primitiveSpatialHandleStart;

Spatial::EntityHandle AllocatePrimitiveSpatialHandle();
PrimitiveSpatialProxy* GetPrimitiveSpatialProxy(PrimitiveComponent* primitive) const;
bool IsPrimitiveSpatiallyIndexable(const PrimitiveComponent* primitive) const;
bool HasDirtyPrimitiveSpatialProxy() const;
void AppendUniqueEntity(const SpatialQueryTarget& target,
                        std::vector<SceneEntity*>& outEntities) const;
void AppendUniquePrimitive(const SpatialQueryTarget& target,
                           std::vector<PrimitiveComponent*>& outPrimitives) const;
RaycastHit MakeRaycastHit(const Spatial::QueryResult& result, const Ray& ray) const;
```

- [ ] **Step 4: Define PrimitiveSpatialProxy**

At the top of `Scene/Private/SceneManager.cpp`, after includes and before `SceneManager::SceneManager()`, add:

```cpp
class SceneManager::PrimitiveSpatialProxy : public Spatial::ISpatialEntity
{
public:
    PrimitiveSpatialProxy(Spatial::EntityHandle handle, PrimitiveComponent* primitive)
        : m_handle(handle), m_primitive(primitive)
    {
    }

    Spatial::EntityHandle GetHandle() const override { return m_handle; }
    PrimitiveComponent* GetPrimitive() const { return m_primitive; }

    SceneEntity* GetOwnerEntity() const
    {
        return m_primitive ? dynamic_cast<SceneEntity*>(m_primitive->GetOwner()) : nullptr;
    }

    AABB GetWorldBounds() const override
    {
        return m_primitive ? m_primitive->GetWorldBounds() : AABB{};
    }

    uint32_t GetLayerMask() const override
    {
        auto* owner = GetOwnerEntity();
        const uint32 primitiveMask = m_primitive ? m_primitive->GetLayerMask() : 0u;
        const uint32 ownerMask = owner ? owner->GetLayerMask() : ~0u;
        return primitiveMask & ownerMask;
    }

    uint32_t GetTypeMask() const override
    {
        auto* owner = GetOwnerEntity();
        return owner ? owner->GetTypeMask() : 0u;
    }

    bool IsSpatialDirty() const override
    {
        auto* owner = GetOwnerEntity();
        return (m_primitive && m_primitive->IsSpatialDirty()) ||
               (owner && owner->IsSpatialDirty());
    }

    void ClearSpatialDirty() override
    {
        if (m_primitive)
            m_primitive->ClearSpatialDirty();
    }

    void* GetUserData() const override
    {
        return const_cast<PrimitiveSpatialProxy*>(this);
    }

private:
    Spatial::EntityHandle m_handle = Spatial::InvalidEntityHandle;
    PrimitiveComponent* m_primitive = nullptr;
};
```

- [ ] **Step 5: Create and destroy proxies with primitive registration**

Update `RegisterPrimitive()`:

```cpp
m_primitives.push_back(primitive);

const auto proxyHandle = AllocatePrimitiveSpatialHandle();
if (proxyHandle == Spatial::InvalidEntityHandle)
{
    RVX_SCENE_ERROR("Failed to allocate primitive spatial handle");
    m_registeredPrimitives.erase(primitive);
    m_primitives.erase(std::remove(m_primitives.begin(), m_primitives.end(), primitive), m_primitives.end());
    return;
}

auto proxy = std::make_unique<PrimitiveSpatialProxy>(proxyHandle, primitive);
auto* proxyPtr = proxy.get();
m_spatialProxyByHandle[proxyPtr->GetHandle()] = proxyPtr;
m_primitiveSpatialProxies[primitive] = std::move(proxy);
m_indexNeedsRebuild = true;
```

Update `UnregisterPrimitive()`:

```cpp
if (auto* proxy = GetPrimitiveSpatialProxy(primitive))
{
    m_spatialProxyByHandle.erase(proxy->GetHandle());
}
m_primitiveSpatialProxies.erase(primitive);
m_indexNeedsRebuild = true;
```

Also clear both proxy maps in `Shutdown()`.

Implement guarded handle allocation:

```cpp
Spatial::EntityHandle SceneManager::AllocatePrimitiveSpatialHandle()
{
    const auto invalid = Spatial::InvalidEntityHandle;
    const uint64 capacity = static_cast<uint64>(invalid) -
                            static_cast<uint64>(s_primitiveSpatialHandleStart);

    for (uint64 attempts = 0; attempts < capacity; ++attempts)
    {
        const Spatial::EntityHandle candidate = m_nextPrimitiveSpatialHandle;
        ++m_nextPrimitiveSpatialHandle;
        if (m_nextPrimitiveSpatialHandle == invalid ||
            m_nextPrimitiveSpatialHandle < s_primitiveSpatialHandleStart)
        {
            m_nextPrimitiveSpatialHandle = s_primitiveSpatialHandleStart;
        }

        if (candidate == invalid || candidate < s_primitiveSpatialHandleStart)
            continue;

        if (m_spatialProxyByHandle.find(candidate) == m_spatialProxyByHandle.end() &&
            m_entities.find(candidate) == m_entities.end())
        {
            return candidate;
        }
    }

    return Spatial::InvalidEntityHandle;
}
```

Reset `m_nextPrimitiveSpatialHandle` to `s_primitiveSpatialHandleStart` during `Initialize()` and `Shutdown()`.

- [ ] **Step 6: Build mixed primitive/entity spatial lists**

Implement `CollectSpatialEntities()`:

```cpp
void SceneManager::CollectSpatialEntities(std::vector<Spatial::ISpatialEntity*>& outEntities) const
{
    std::unordered_set<const SceneEntity*> primitiveOwners;

    for (const auto& [primitive, proxy] : m_primitiveSpatialProxies)
    {
        if (!proxy || !IsPrimitiveSpatiallyIndexable(primitive))
            continue;

        outEntities.push_back(proxy.get());
        primitiveOwners.insert(proxy->GetOwnerEntity());
    }

    for (const auto& [handle, entity] : m_entities)
    {
        (void)handle;
        if (!entity || !entity->IsActive())
            continue;

        if (primitiveOwners.find(entity.get()) != primitiveOwners.end())
            continue;

        outEntities.push_back(entity.get());
    }
}
```

`IsPrimitiveSpatiallyIndexable()` must return true only when:

```cpp
primitive &&
primitive->IsRegistered() &&
primitive->IsEnabled() &&
ownerEntity &&
ownerEntity->IsActive() &&
primitive->GetWorldBounds().IsValid()
```

This locks the mixed-owner policy: a valid primitive suppresses owner entity fallback for that owner, preventing duplicate hits; a disabled/unregistered/invalid primitive does not suppress fallback, so legacy manual entity bounds keep working.

Update `RebuildSpatialIndex()` to call `CollectSpatialEntities(entities)`, then clear dirty flags on all active entities and all proxies.

- [ ] **Step 7: Resolve query results and fill hit metadata**

Implement:

```cpp
SpatialQueryTarget SceneManager::ResolveSpatialQueryTarget(const Spatial::QueryResult& result) const
{
    SpatialQueryTarget target;

    if (auto proxyIt = m_spatialProxyByHandle.find(result.handle);
        proxyIt != m_spatialProxyByHandle.end() && proxyIt->second)
    {
        auto* primitive = proxyIt->second->GetPrimitive();
        auto* entity = proxyIt->second->GetOwnerEntity();
        target.entity = entity;
        target.actor = entity;
        target.component = primitive;
        return target;
    }

    if (auto* entity = GetEntity(result.handle))
    {
        target.entity = entity;
        target.actor = entity;
    }

    return target;
}
```

Implement `MakeRaycastHit()` so both scene and world paths use the same fields:

```cpp
RaycastHit SceneManager::MakeRaycastHit(const Spatial::QueryResult& result, const Ray& ray) const
{
    RaycastHit hit;
    auto target = ResolveSpatialQueryTarget(result);
    hit.entity = target.entity;
    hit.actor = target.actor;
    hit.component = target.component;
    hit.distance = result.distance;
    hit.hitPoint = ray.At(result.distance);
    return hit;
}
```

- [ ] **Step 8: Update SceneManager query methods**

Update `QueryVisible`, `Raycast`, `RaycastAll`, `QuerySphere`, and `QueryBox` to call `ResolveSpatialQueryTarget()` / `MakeRaycastHit()` instead of only `GetEntity(result.handle)`.

Implement primitive query methods by running the same spatial query and appending unique `target.component` values.

- [ ] **Step 9: Update dirty-index handling**

Use this lifecycle:

- Mark dirty: `PrimitiveComponent::SetLocalBounds()`, `SetLayerMask()`, and `OnTransformChanged()` set primitive dirty and call owner `MarkBoundsDirty()`.
- Collect dirty: `SceneManager::CollectDirtyEntities()` continues to collect owner entities; `HasDirtyPrimitiveSpatialProxy()` scans proxy dirty state.
- Consume dirty: if any primitive proxy exists and either entity or primitive dirty state is present, `UpdateDirtyEntities()` performs a full rebuild instead of a legacy entity-only incremental update.
- Clear dirty: `RebuildSpatialIndex()` clears every active entity spatial dirty flag and every primitive proxy dirty flag after `Build()`.

Update `UpdateDirtyEntities()`:

```cpp
const bool primitiveDirty = HasDirtyPrimitiveSpatialProxy();
const bool hasPrimitiveSpatial = !m_primitiveSpatialProxies.empty();

if (hasPrimitiveSpatial && (m_indexNeedsRebuild || primitiveDirty || !m_dirtyEntities.empty()))
{
    RebuildSpatialIndex();
    return;
}
```

Keep the current incremental update branch for legacy entity-only scenes.

- [ ] **Step 10: Run tests and verify GREEN for Scene**

Run:

```powershell
cmake --build build\Debug --config Debug --target SpatialComponentValidation ActorComponentValidation SystemIntegrationTest
.\build\Debug\Tests\SpatialComponentValidation\Debug\SpatialComponentValidation.exe
.\build\Debug\Tests\ActorComponentValidation\Debug\ActorComponentValidation.exe
.\build\Debug\Tests\SystemIntegrationTest\Debug\SystemIntegrationTest.exe
```

Expected:

- `SpatialComponentValidation` compiles and passes its scene-level tests except the world subsystem test may still fail until Task 3.
- `ActorComponentValidation` remains green.
- `SystemIntegrationTest` remains green.

---

## Task 3: Migrate World SpatialSubsystem and Picking Resolution

**Files:**
- Modify: `World/Private/SpatialSubsystem.cpp`

- [ ] **Step 1: Build World spatial index from SceneManager collection**

Add the include needed for compatibility output dedupe:

```cpp
#include <unordered_set>
```

Replace the entity-only collection in `SpatialSubsystem::RebuildIndex()`:

```cpp
std::vector<Spatial::ISpatialEntity*> entities;
scene->CollectSpatialEntities(entities);
m_index->Build(entities);
```

- [ ] **Step 2: Resolve query results through SceneManager**

In each query method, fetch `SceneManager* scene = GetWorld()->GetSceneManager();` and resolve results by handle. Do not read or cast `result.userData`; `SceneEntity::GetUserData()` is not guaranteed to point to a scene entity.

```cpp
auto target = scene->ResolveSpatialQueryTarget(result);
if (target.entity)
{
    outEntities.push_back(target.entity);
}
```

For entity-list compatibility methods (`QueryVisible`, `QuerySphere`, `QueryBox`), use a local set so a multi-primitive actor appears once:

```cpp
std::unordered_set<SceneEntity*> seenEntities;
for (const auto& result : results)
{
    auto target = scene->ResolveSpatialQueryTarget(result);
    if (target.entity && seenEntities.insert(target.entity).second)
    {
        outEntities.push_back(target.entity);
    }
}
```

For raycasts:

```cpp
auto target = scene->ResolveSpatialQueryTarget(result);
outHit.entity = target.entity;
outHit.actor = target.actor;
outHit.component = target.component;
outHit.distance = result.distance;
outHit.hitPoint = ray.At(result.distance);
return outHit.IsValid();
```

For `RaycastAll`, fill the same fields for each hit.

`RaycastAll` intentionally remains per-hit rather than deduplicated, because it now carries component identity. Callers that need a unique entity list should use `QueryVisible`, `QuerySphere`, or `QueryBox`.

- [ ] **Step 3: Preserve compatibility behavior**

Keep public `SpatialSubsystem` signatures unchanged. Do not expose primitive-only query APIs from `World` in this phase; scene-level primitive query APIs are enough for migration and tests.

- [ ] **Step 4: Run tests and verify GREEN**

Run:

```powershell
cmake --build build\Debug --config Debug --target SpatialComponentValidation SystemIntegrationTest RenderSceneValidation
.\build\Debug\Tests\SpatialComponentValidation\Debug\SpatialComponentValidation.exe
.\build\Debug\Tests\SystemIntegrationTest\Debug\SystemIntegrationTest.exe
.\build\Debug\Tests\RenderSceneValidation\Debug\RenderSceneValidation.exe
```

Expected:

- All three executables pass.
- `PickingService::Pick()` returns the same actor/component metadata as `SpatialSubsystem::Raycast()`.

---

## Task 4: Full Validation, ModelViewer Smoke, Review, and Commit

**Files:**
- No new source changes unless validation uncovers issues.

- [ ] **Step 1: Build the validation set**

Run:

```powershell
cmake --build build\Debug --config Debug --target ActorComponentValidation SpatialComponentValidation RenderSceneValidation SystemIntegrationTest MaterialSystemValidation RenderPassBindingValidation ModelViewer
```

Expected: build succeeds.

- [ ] **Step 2: Run validation executables**

Run:

```powershell
.\build\Debug\Tests\ActorComponentValidation\Debug\ActorComponentValidation.exe
.\build\Debug\Tests\SpatialComponentValidation\Debug\SpatialComponentValidation.exe
.\build\Debug\Tests\RenderSceneValidation\Debug\RenderSceneValidation.exe
.\build\Debug\Tests\SystemIntegrationTest\Debug\SystemIntegrationTest.exe
.\build\Debug\Tests\MaterialSystemValidation\Debug\MaterialSystemValidation.exe
.\build\Debug\Tests\RenderPassBindingValidation\Debug\RenderPassBindingValidation.exe
```

Expected: every executable exits `0`.

- [ ] **Step 3: Run ModelViewer smoke**

Run hidden for a short smoke window and capture logs:

```powershell
Start-Process -FilePath ".\build\Debug\Samples\ModelViewer\Debug\ModelViewer.exe" -RedirectStandardOutput "build\Debug\modelviewer_phase4_smoke.log" -RedirectStandardError "build\Debug\modelviewer_phase4_smoke.err.log" -WindowStyle Hidden -PassThru
```

Stop after the log shows scene/model/rendergraph activity or after eight seconds. Expected log signals:

- `Model loaded successfully`
- `Model instantiated as SceneEntity`
- `Uploaded mesh to GPU`
- `RenderGraph stats`
- no critical/assertion lines

- [ ] **Step 4: Request one code review**

Use one reviewer with:

```text
DESCRIPTION: Implemented UE-style actor component Phase 4 spatial and picking migration. PrimitiveComponent now uses internal spatial proxies, SceneManager resolves spatial hits to actor/entity/component, World SpatialSubsystem and PickingService preserve old signatures while returning component-aware RaycastHit metadata.
PLAN_OR_REQUIREMENTS: Docs/superpowers/plans/2026-05-15-ue-style-actor-component-phase4-spatial-picking.md
BASE_SHA: commit before Task 1 implementation
HEAD_SHA: current HEAD
```

Fix Critical and Important issues before committing.

- [ ] **Step 5: Commit implementation**

Run:

```powershell
git status --short
git add Scene/Include/Scene/PrimitiveComponent.h Scene/Private/PrimitiveComponent.cpp Scene/Include/Scene/SceneManager.h Scene/Private/SceneManager.cpp World/Private/SpatialSubsystem.cpp Tests/SpatialComponentValidation/main.cpp Tests/CMakeLists.txt Tests/SystemIntegration/main.cpp
git commit -m "Migrate spatial picking to primitive components"
```

Expected commit includes only Phase 4 implementation and tests.

---

## Self-Review

- Spec coverage: Phase 4 requires primitive spatial registration, actor/component hit results, and compatibility methods returning `SceneEntity*`. Tasks 2 and 3 implement primitive proxy registration and result resolution. Existing `SceneManager` and `SpatialSubsystem` public entity-returning APIs remain unchanged. New primitive query APIs support migration without breaking callers.
- Placeholder scan: No unresolved placeholders or unspecified "add tests" steps remain. Test code, command lines, and expected outcomes are explicit.
- Type consistency: `RaycastHit::actor` is `Actor*`; `RaycastHit::component` is `PrimitiveComponent*`; `SpatialQueryTarget` uses the same fields; `SceneManager::ResolveSpatialQueryTarget()` is the single shared resolver for Scene and World.
- Risk check: The plan keeps `RenderGraph`, RHI, and render passes actor-agnostic. It avoids a permanent second spatial path by using one resolver and one spatial entity collection helper. Primitive proxy handles use a guarded reserved range, dirty state has an explicit mark/consume/clear lifecycle, mixed primitive/entity fallback behavior is tested, entity-list queries deduplicate multi-primitive actors, and custom filters are documented to operate on `ISpatialEntity` instead of downcasting to `SceneEntity`.
