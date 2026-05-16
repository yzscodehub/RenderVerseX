# UE-Style Actor Component Phase 10 Spawn API Adoption Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `SpawnActor` the preferred runtime actor creation path for model, prefab, and ModelViewer fallback instantiation, while hardening mixed pure/scene actor iteration.

**Architecture:** `SceneManager::CreateEntity()` remains as a compatibility API and as useful test coverage for legacy behavior. Runtime systems that instantiate scene content should call `SceneManager::SpawnActor()` so new actor construction, parent validation, and future spawn hooks share one entry point. `World::ForEachActor()` should be safe to call with an empty callback, should enumerate both world-owned pure actors and scene-owned actors, and should snapshot actor handles before invoking callbacks so callback-side actor destruction cannot invalidate the active iteration.

**Tech Stack:** C++20, RenderVerseX Scene/Resource/World modules, standalone CMake validation executables.

---

## Scope

This phase intentionally changes only runtime instantiation call sites and `World::ForEachActor()` callback/iteration safety.

- Keep `SceneManager::CreateEntity()` and `CreateEntity<T>()` behavior unchanged.
- Do not rewrite existing tests that use `CreateEntity()` to protect compatibility behavior.
- Do not add generic prefab class spawning; prefab data still instantiates `SceneEntity` compatibility actors.
- Do not change model, prefab, material, or render behavior.
- Define `World::ForEachActor()` mutation semantics: actors present at snapshot time are considered for visitation; actors destroyed before their turn are skipped; actors spawned from a callback are not guaranteed to be visited until a later `ForEachActor()` call.

## Files

- Modify: `E:\WorkSpace\RenderVerseX\World\Private\World.cpp`
  - Keep the existing empty-callback guard and add handle snapshots for pure actor and scene actor iteration.
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Include\Scene\SceneManager.h`
  - Expose a read-only `IsDestroyPending()` query so `World::ForEachActor()` can skip scene actors queued for deferred destruction.
- Modify: `E:\WorkSpace\RenderVerseX\Resource\Private\Types\ModelResource.cpp`
  - Replace direct `SceneManager::CreateEntity()` usage with `SceneManager::SpawnActor<SceneEntity>()`.
  - Pass local transform and parent through `ActorSpawnParams`.
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Private\Prefab.cpp`
  - Replace direct `SceneManager::CreateEntity()` usage with `SceneManager::SpawnActor()`.
  - Preserve the current two-pass hierarchy/property setup.
- Modify: `E:\WorkSpace\RenderVerseX\Samples\ModelViewer\main.cpp`
  - Replace fallback entity creation with `SceneManager::SpawnActor()`.
- Modify: `E:\WorkSpace\RenderVerseX\Tests\ActorComponentValidation\main.cpp`
  - Add mixed pure/scene actor iteration coverage.
  - Add empty-callback regression coverage.
  - Add callback-side pure/scene actor destruction coverage.
- Modify: `E:\WorkSpace\RenderVerseX\Tests\ResourceInstantiationValidation\main.cpp`
  - Add model hierarchy coverage for spawned child entities.
  - Add prefab child instantiation coverage through the spawn path.

---

## Task 1: Add Failing Coverage And Static Migration Guard

**Files:**
- Modify: `E:\WorkSpace\RenderVerseX\Tests\ActorComponentValidation\main.cpp`
- Modify: `E:\WorkSpace\RenderVerseX\Tests\ResourceInstantiationValidation\main.cpp`

- [ ] Add this helper include to `Tests\ActorComponentValidation\main.cpp` if it is not already present:

```cpp
#include <algorithm>
```

- [ ] Add `Test_WorldForEachActorVisitsPureAndSceneActors` near the existing world actor tests:

```cpp
bool Test_WorldForEachActorVisitsPureAndSceneActors()
{
    RVX::World world;
    world.Initialize();

    RVX::ActorSpawnParams pureParams;
    pureParams.name = "PureIterated";
    auto* pureActor = world.SpawnActor<WorldManagedPureActor>(pureParams);
    TEST_ASSERT_NOT_NULL(pureActor);

    RVX::ActorSpawnParams sceneParams;
    sceneParams.name = "SceneIterated";
    auto* sceneActor = world.SpawnActor<SpawnableSceneActor>(sceneParams);
    TEST_ASSERT_NOT_NULL(sceneActor);

    std::vector<RVX::Actor::Handle> visited;
    world.ForEachActor([&](RVX::Actor* actor) {
        if (actor)
        {
            visited.push_back(actor->GetHandle());
        }
    });

    TEST_ASSERT_EQ(static_cast<size_t>(2), visited.size());
    TEST_ASSERT_TRUE(std::find(visited.begin(), visited.end(), pureActor->GetHandle()) != visited.end());
    TEST_ASSERT_TRUE(std::find(visited.begin(), visited.end(), sceneActor->GetHandle()) != visited.end());

    world.Shutdown();
    return true;
}
```

- [ ] Add `Test_WorldForEachActorIgnoresEmptyCallback` near the existing world actor tests:

```cpp
bool Test_WorldForEachActorIgnoresEmptyCallback()
{
    RVX::World world;
    world.Initialize();

    RVX::ActorSpawnParams params;
    params.name = "EmptyCallbackPureActor";
    auto* actor = world.SpawnActor<WorldManagedPureActor>(params);
    TEST_ASSERT_NOT_NULL(actor);

    world.ForEachActor({});
    TEST_ASSERT_EQ(static_cast<size_t>(1), world.GetActorCount());

    world.Shutdown();
    return true;
}
```

- [ ] Add `Test_WorldForEachActorAllowsPureActorDestroyDuringCallback` near the existing world actor tests:

```cpp
bool Test_WorldForEachActorAllowsPureActorDestroyDuringCallback()
{
    RVX::World world;
    world.Initialize();

    RVX::ActorSpawnParams firstParams;
    firstParams.name = "DestroyPureDuringIteration";
    auto* first = world.SpawnActor<WorldManagedPureActor>(firstParams);
    TEST_ASSERT_NOT_NULL(first);
    const auto firstHandle = first->GetHandle();

    RVX::ActorSpawnParams secondParams;
    secondParams.name = "SurvivePureIteration";
    auto* second = world.SpawnActor<WorldManagedPureActor>(secondParams);
    TEST_ASSERT_NOT_NULL(second);
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

    TEST_ASSERT_TRUE(destroyAccepted);
    TEST_ASSERT_EQ(nullptr, world.GetActor(firstHandle));
    TEST_ASSERT_EQ(second, world.GetActor(secondHandle));
    TEST_ASSERT_TRUE(visitCount >= static_cast<size_t>(1));

    world.Shutdown();
    return true;
}
```

- [ ] Add `Test_WorldForEachActorAllowsSceneActorDestroyDuringCallback` near the existing world actor tests:

```cpp
bool Test_WorldForEachActorAllowsSceneActorDestroyDuringCallback()
{
    RVX::World world;
    world.Initialize();

    RVX::ActorSpawnParams firstParams;
    firstParams.name = "DestroySceneDuringIteration";
    auto* first = world.SpawnActor<SpawnableSceneActor>(firstParams);
    TEST_ASSERT_NOT_NULL(first);
    const auto firstHandle = first->GetHandle();

    RVX::ActorSpawnParams secondParams;
    secondParams.name = "SurviveSceneIteration";
    auto* second = world.SpawnActor<SpawnableSceneActor>(secondParams);
    TEST_ASSERT_NOT_NULL(second);
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

    TEST_ASSERT_TRUE(destroyAccepted);
    TEST_ASSERT_EQ(nullptr, world.GetActor(firstHandle));
    TEST_ASSERT_EQ(second, world.GetActor(secondHandle));
    TEST_ASSERT_TRUE(visitCount >= static_cast<size_t>(1));

    world.Shutdown();
    return true;
}
```

- [ ] Register all four iteration tests in `main()` after `WorldUnloadClearsPureActorsAndSceneActors`:

```cpp
suite.AddTest("WorldForEachActorVisitsPureAndSceneActors",
              Test_WorldForEachActorVisitsPureAndSceneActors);
suite.AddTest("WorldForEachActorIgnoresEmptyCallback",
              Test_WorldForEachActorIgnoresEmptyCallback);
suite.AddTest("WorldForEachActorAllowsPureActorDestroyDuringCallback",
              Test_WorldForEachActorAllowsPureActorDestroyDuringCallback);
suite.AddTest("WorldForEachActorAllowsSceneActorDestroyDuringCallback",
              Test_WorldForEachActorAllowsSceneActorDestroyDuringCallback);
```

- [ ] Add `FillHierarchicalIndexedModel()` to `Tests\ResourceInstantiationValidation\main.cpp` after `FillIndexedModel()`:

```cpp
void FillHierarchicalIndexedModel(ModelResource& model)
{
    model.AddMesh(MakeMeshResource(1101));
    model.AddMaterial(MakeMaterialResource(2101));

    auto root = std::make_shared<Node>("RootNode");
    root->GetLocalTransform().SetPosition(Vec3(1.0f, 2.0f, 3.0f));

    auto child = std::make_shared<Node>("ChildNode");
    child->SetMeshIndex(0);
    child->SetMaterialIndices({0});
    child->GetLocalTransform().SetPosition(Vec3(4.0f, 5.0f, 6.0f));
    root->AddChild(child);

    model.SetRootNode(root);
}
```

- [ ] Add `Test_ModelResourceInstantiateActorBuildsSpawnedHierarchy` after `Test_LegacyInstantiateDelegatesToActorPath`:

```cpp
bool Test_ModelResourceInstantiateActorBuildsSpawnedHierarchy()
{
    ComponentFactory::RegisterDefaults();

    SceneManager sceneManager;
    sceneManager.Initialize();

    ModelResource model;
    FillHierarchicalIndexedModel(model);

    Actor* actor = model.InstantiateActor(&sceneManager);
    TEST_ASSERT_NOT_NULL(actor);

    auto* root = dynamic_cast<SceneEntity*>(actor);
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_EQ(std::string("RootNode"), root->GetName());
    TEST_ASSERT_EQ(Vec3(1.0f, 2.0f, 3.0f), root->GetPosition());
    TEST_ASSERT_EQ(static_cast<size_t>(1), root->GetChildren().size());
    TEST_ASSERT_EQ(static_cast<size_t>(2), sceneManager.GetEntityCount());

    auto* child = root->GetChildren()[0];
    TEST_ASSERT_NOT_NULL(child);
    TEST_ASSERT_EQ(std::string("ChildNode"), child->GetName());
    TEST_ASSERT_EQ(root, child->GetParent());
    TEST_ASSERT_EQ(Vec3(4.0f, 5.0f, 6.0f), child->GetPosition());
    TEST_ASSERT_NOT_NULL(static_cast<Actor*>(child)->GetComponent<StaticMeshComponent>());

    sceneManager.Shutdown();
    return true;
}
```

- [ ] Add `Test_PrefabInstantiateAsChildBuildsSpawnedHierarchy` after `Test_PrefabInstantiatesRegisteredActorComponentClasses`:

```cpp
bool Test_PrefabInstantiateAsChildBuildsSpawnedHierarchy()
{
    ComponentFactory::ClearComponentClasses();
    ComponentFactory::RegisterComponentClass<StaticMeshComponent>("StaticMeshComponent");

    PrefabEntityData rootData;
    rootData.name = "PrefabRoot";
    rootData.position = Vec3(1.0f, 0.0f, 0.0f);
    rootData.actorComponentData.push_back({"StaticMeshComponent", ""});

    PrefabEntityData childData;
    childData.name = "PrefabChild";
    childData.parentIndex = 0;
    childData.position = Vec3(0.0f, 2.0f, 0.0f);

    auto prefab = Prefab::CreateFromData({rootData, childData});

    SceneManager sceneManager;
    sceneManager.Initialize();

    ActorSpawnParams parentParams;
    parentParams.name = "PrefabParent";
    auto* parent = sceneManager.SpawnActor(parentParams);
    TEST_ASSERT_NOT_NULL(parent);

    SceneEntity* root = prefab->InstantiateAsChild(parent);
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_EQ(parent, root->GetParent());
    TEST_ASSERT_EQ(static_cast<size_t>(1), root->GetChildren().size());
    TEST_ASSERT_EQ(static_cast<size_t>(3), sceneManager.GetEntityCount());
    TEST_ASSERT_NOT_NULL(static_cast<Actor*>(root)->GetComponent<StaticMeshComponent>());

    auto* child = root->GetChildren()[0];
    TEST_ASSERT_NOT_NULL(child);
    TEST_ASSERT_EQ(std::string("PrefabChild"), child->GetName());
    TEST_ASSERT_EQ(root, child->GetParent());
    TEST_ASSERT_EQ(Vec3(0.0f, 2.0f, 0.0f), child->GetPosition());

    sceneManager.Shutdown();
    return true;
}
```

- [ ] Register both resource tests in `main()`:

```cpp
suite.AddTest("ModelResourceInstantiateActorBuildsSpawnedHierarchy",
              Test_ModelResourceInstantiateActorBuildsSpawnedHierarchy);
suite.AddTest("PrefabInstantiateAsChildBuildsSpawnedHierarchy",
              Test_PrefabInstantiateAsChildBuildsSpawnedHierarchy);
```

- [ ] Run the focused build and test to verify the mutation-safety tests expose the current direct-iteration risk before implementation:

```powershell
cmake --build build --config Debug --target ActorComponentValidation
.\build\Tests\Debug\ActorComponentValidation.exe
```

Expected result before production changes: the empty-callback regression should pass, while at least one callback-side destroy test may fail or the executable may terminate because the current implementation erases from `m_actors` or `SceneManager::GetEntities()` while iterating directly.

- [ ] Run the focused resource build and test:

```powershell
cmake --build build --config Debug --target ResourceInstantiationValidation
.\build\Tests\Debug\ResourceInstantiationValidation.exe
```

Expected result before production changes: new resource tests may pass because they assert existing behavior, and they protect the migration from changing hierarchy/component semantics.

- [ ] Run this static migration guard and confirm it fails before production changes:

```powershell
rg -n "CreateEntity" Resource\Private\Types\ModelResource.cpp Scene\Private\Prefab.cpp Samples\ModelViewer\main.cpp
```

Expected result before production changes: matches in all three runtime files.

- [ ] Run this static iteration-safety guard and confirm it fails before production changes:

```powershell
rg -n "for \\(auto& \\[handle, actor\\] : m_actors\\)|callback\\(entity\\.get\\(\\)\\)" World\Private\World.cpp
```

Expected result before production changes: matches in `World::ForEachActor()` showing direct container iteration.

---

## Task 2: Harden World Actor Iteration

**Files:**
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Include\Scene\SceneManager.h`
- Modify: `E:\WorkSpace\RenderVerseX\World\Private\World.cpp`

- [ ] In `SceneManager.h`, move `IsDestroyPending(SceneEntity::Handle handle) const` from `private` to the public Entity Management section:

```cpp
/// Check whether an entity is queued for deferred destruction.
bool IsDestroyPending(SceneEntity::Handle handle) const;
```

- [ ] Remove the old private declaration of `IsDestroyPending(SceneEntity::Handle handle) const` from the helper list in `SceneManager.h` so the class has one declaration.

- [ ] Update `World::ForEachActor()` to keep the empty callback guard and snapshot handles before invoking user callbacks:

```cpp
void World::ForEachActor(const std::function<void(Actor*)>& callback)
{
    if (!callback)
        return;

    std::vector<Actor::Handle> pureActorHandles;
    pureActorHandles.reserve(m_actors.size());
    for (const auto& [handle, actor] : m_actors)
    {
        if (actor)
        {
            pureActorHandles.push_back(handle);
        }
    }

    for (Actor::Handle handle : pureActorHandles)
    {
        Actor* actor = GetActor(handle);
        if (actor && m_actors.find(handle) != m_actors.end() && !IsActorDestroyPending(handle))
        {
            callback(actor);
        }
    }

    if (!m_sceneManager)
        return;

    std::vector<SceneEntity::Handle> sceneActorHandles;
    sceneActorHandles.reserve(m_sceneManager->GetEntityCount());
    for (const auto& [handle, entity] : m_sceneManager->GetEntities())
    {
        if (entity)
        {
            sceneActorHandles.push_back(handle);
        }
    }

    for (SceneEntity::Handle handle : sceneActorHandles)
    {
        SceneEntity* entity = m_sceneManager->GetEntity(handle);
        if (entity && !m_sceneManager->IsDestroyPending(handle))
        {
            callback(static_cast<Actor*>(entity));
        }
    }
}
```

- [ ] Re-run the focused actor validation:

```powershell
cmake --build build --config Debug --target ActorComponentValidation
.\build\Tests\Debug\ActorComponentValidation.exe
```

Expected result: all `ActorComponentValidation` tests pass.

- [ ] Re-run the static iteration-safety guard and confirm it now passes with no matches:

```powershell
rg -n "for \\(auto& \\[handle, actor\\] : m_actors\\)|callback\\(entity\\.get\\(\\)\\)" World\Private\World.cpp
```

Expected result after production changes: command exits with code 1 and prints no matches.

---

## Task 3: Migrate Runtime Instantiation To SpawnActor

**Files:**
- Modify: `E:\WorkSpace\RenderVerseX\Resource\Private\Types\ModelResource.cpp`
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Private\Prefab.cpp`
- Modify: `E:\WorkSpace\RenderVerseX\Samples\ModelViewer\main.cpp`

- [ ] In `ModelResource::InstantiateActorNode()`, replace direct entity creation and manual parent attachment with `ActorSpawnParams`:

```cpp
const auto& transform = node->GetLocalTransform();

ActorSpawnParams params;
params.name = node->GetName();
params.localPosition = transform.GetPosition();
params.localRotation = transform.GetRotation();
params.localScale = transform.GetScale();
params.parent = parent;

SceneEntity* entity = scene->SpawnActor<SceneEntity>(params);
if (!entity)
    return nullptr;
```

- [ ] Remove the subsequent manual transform assignment and this manual parent block from `ModelResource::InstantiateActorNode()`:

```cpp
if (parent)
{
    parent->AddChild(entity);
}
```

- [ ] In `Prefab::InstantiateInternal()`, replace the creation loop with name-only spawn calls while keeping the existing second pass for hierarchy, transform, layer, active state, and component creation:

```cpp
for (const auto& entityData : m_entities)
{
    ActorSpawnParams params;
    params.name = entityData.name;

    SceneEntity* entity = sceneManager.SpawnActor(params);
    if (!entity)
    {
        for (auto h : createdHandles)
        {
            sceneManager.DestroyEntity(h);
        }
        return nullptr;
    }

    createdEntities.push_back(entity);
    createdHandles.push_back(entity->GetHandle());
}
```

- [ ] In `Samples\ModelViewer\main.cpp`, replace fallback `CreateEntity()` usage with `SpawnActor()`:

```cpp
ActorSpawnParams fallbackParams;
fallbackParams.name = "FallbackEntity";
modelEntity = sceneManager->SpawnActor(fallbackParams);
```

- [ ] Run the static migration guard and confirm it now passes with no matches:

```powershell
rg -n "CreateEntity" Resource\Private\Types\ModelResource.cpp Scene\Private\Prefab.cpp Samples\ModelViewer\main.cpp
```

Expected result after production changes: command exits with code 1 and prints no matches.

- [ ] Run focused resource validation:

```powershell
cmake --build build --config Debug --target ResourceInstantiationValidation
.\build\Tests\Debug\ResourceInstantiationValidation.exe
```

Expected result: all resource instantiation tests pass.

---

## Task 4: Full Validation And Runtime Smoke

**Files:**
- No new source files.

- [ ] Build validation targets and ModelViewer:

```powershell
cmake --build build --config Debug --target ActorComponentValidation SpatialComponentValidation ResourceInstantiationValidation RenderSceneValidation SystemIntegrationTest MaterialSystemValidation RenderPassBindingValidation ModelViewer
```

Expected result: build succeeds.

- [ ] Run validation executables:

```powershell
.\build\Tests\Debug\ActorComponentValidation.exe
.\build\Tests\Debug\SpatialComponentValidation.exe
.\build\Tests\Debug\ResourceInstantiationValidation.exe
.\build\Tests\Debug\RenderSceneValidation.exe
.\build\Tests\Debug\SystemIntegrationTest.exe
.\build\Tests\Debug\MaterialSystemValidation.exe
.\build\Tests\Debug\RenderPassBindingValidation.exe
```

Expected result: every executable returns exit code 0 and reports all tests passing.

- [ ] Run ModelViewer smoke with the known sample model and stop it after several seconds:

```powershell
$stdout = "build_codex\phase10_modelviewer_stdout.log"
$stderr = "build_codex\phase10_modelviewer_stderr.log"
Remove-Item -LiteralPath $stdout -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $stderr -ErrorAction SilentlyContinue
$process = Start-Process -FilePath ".\build\Debug\Samples\ModelViewer\Debug\ModelViewer.exe" -ArgumentList "C:\Users\yinzs\Desktop\DamagedHelmet.glb" -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
Start-Sleep -Seconds 8
if (-not $process.HasExited) { Stop-Process -Id $process.Id }
Get-Content -Path $stdout | Select-String -Pattern "Model loaded successfully|Model instantiated as SceneEntity|Scene entity ready|Uploaded mesh to GPU|RenderGraph stats"
Get-Content -Path $stderr
```

Expected result: stdout includes model loaded, model instantiated, scene entity ready, GPU mesh upload, and render graph stats; stderr is empty.

- [ ] Request exactly one code review using Dalton with this context:

```text
Review Phase 10 of the UE-style Actor Component framework.

Requirements:
- Runtime model, prefab, and ModelViewer fallback instantiation use SpawnActor instead of direct CreateEntity.
- CreateEntity remains available for compatibility and old tests may keep using it.
- World::ForEachActor safely ignores an empty callback.
- World::ForEachActor still visits both pure world actors and SceneManager-owned SceneEntity actors.
- Model/prefab hierarchy, transforms, component registration, and ModelViewer behavior are unchanged.

Please report Critical and Important issues first, with file/line references.
```

- [ ] Fix any Critical or Important review findings before continuing.

- [ ] Update every checkbox in this plan that was completed.

- [ ] Commit the plan and implementation:

```powershell
git add Docs\superpowers\plans\2026-05-15-ue-style-actor-component-phase10-spawn-api-adoption.md Scene\Include\Scene\SceneManager.h World\Private\World.cpp Resource\Private\Types\ModelResource.cpp Scene\Private\Prefab.cpp Samples\ModelViewer\main.cpp Tests\ActorComponentValidation\main.cpp Tests\ResourceInstantiationValidation\main.cpp
git commit -m "Adopt actor spawn API in runtime instantiation"
```

---

## Self-Review

- Spec coverage: The plan covers the next ECS framework gap discovered after Phase 9: new runtime actor creation should flow through `SpawnActor`, while legacy `CreateEntity` remains compatibility surface.
- Placeholder scan: No placeholder tasks are left; every code change and validation command is explicit.
- Type consistency: `ActorSpawnParams`, `SceneManager::SpawnActor()`, `World::ForEachActor()`, `SceneEntity`, and `Actor` signatures match the current headers.
