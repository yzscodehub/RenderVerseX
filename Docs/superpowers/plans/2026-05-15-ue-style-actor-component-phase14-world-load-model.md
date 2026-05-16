# UE-Style Actor Component Phase 14 World Load Model Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give `World::Load(path)` a first real runtime path by loading a `ModelResource` through `ResourceManager` and instantiating it as an actor hierarchy in the world scene.

**Architecture:** `World::Load()` remains a high-level world-content API. It validates the world, path, resource manager, and loaded resource before mutating current content; model data is first instantiated into a fresh `SceneManager`, and only a successful staged instantiation replaces the current scene. Full scene asset loading, prefab file loading, async streaming, and property deserialization are deliberately left for later phases with dedicated asset formats.

**Tech Stack:** C++20, RenderVerseX World/Resource/Scene modules, standalone `ResourceInstantiationValidation`.

---

## Scope

- Implement model-resource loading in `World::Load(const std::string& path)`.
- Preserve existing scene content when load cannot start or the loaded resource is not a model.
- Replace existing scene content when a valid model resource is loaded.
- Instantiate the model using `ModelResource::InstantiateActor()` so the UE-style actor/component path remains the single runtime route.
- Preserve existing scene content when the loaded model has no root node and therefore cannot instantiate.
- Preserve existing scene content if staged model instantiation fails for any reason.
- Do not introduce a scene file format.
- Do not implement prefab file loading in this phase.
- Do not make `World::Load()` implicitly initialize an uninitialized world.

## Files

- Modify: `E:\WorkSpace\RenderVerseX\World\Private\World.cpp`
  - Include `Resource/ResourceManager.h` and `Resource/Types/ModelResource.h`.
  - Replace the current logging-only stub in `World::Load`.
- Modify: `E:\WorkSpace\RenderVerseX\World\CMakeLists.txt`
  - Link `RVX::Resource` because `World::Load()` now calls `ResourceManager` and `ModelResource`.
- Modify: `E:\WorkSpace\RenderVerseX\Tests\ResourceInstantiationValidation\main.cpp`
  - Add `World/World.h`, `Resource/ResourceManager.h`, and a focused test loader.
  - Add tests for valid model load, invalid load preserving existing content, and non-instantiable model preserving existing content.
- Modify: `E:\WorkSpace\RenderVerseX\Docs\superpowers\plans\2026-05-15-ue-style-actor-component-phase14-world-load-model.md`
  - Track completion status.

---

## Task 1: Add Failing World Load Tests

**Files:**
- Modify: `E:\WorkSpace\RenderVerseX\Tests\ResourceInstantiationValidation\main.cpp`

- [ ] Add includes near existing includes:

```cpp
#include "Resource/ResourceManager.h"
#include "World/World.h"
```

- [ ] Add this test loader inside the anonymous namespace near `PrefabCustomSceneActor`:

```cpp
class WorldLoadModelLoader : public IResourceLoader
{
public:
    ResourceType GetResourceType() const override { return ResourceType::Model; }

    std::vector<std::string> GetSupportedExtensions() const override
    {
        return {".model"};
    }

    IResource* Load(const std::string&) override
    {
        auto* model = new ModelResource();

        auto root = std::make_shared<Node>("LoadedWorldRoot");
        root->GetLocalTransform().SetPosition(Vec3(1.0f, 2.0f, 3.0f));

        auto child = std::make_shared<Node>("LoadedWorldChild");
        child->GetLocalTransform().SetPosition(Vec3(4.0f, 5.0f, 6.0f));
        root->AddChild(child);

        model->SetRootNode(root);
        return model;
    }
};
```

- [ ] Add this empty model loader after `WorldLoadModelLoader`:

```cpp
class WorldLoadEmptyModelLoader : public IResourceLoader
{
public:
    ResourceType GetResourceType() const override { return ResourceType::Model; }

    std::vector<std::string> GetSupportedExtensions() const override
    {
        return {".model"};
    }

    IResource* Load(const std::string&) override
    {
        return new ModelResource();
    }
};
```

- [ ] Add this ResourceManager guard near the loaders. It shuts down any existing singleton state at construction and destruction, so fake loaders cannot leak between tests even if an assertion returns early:

```cpp
class ResourceManagerTestGuard
{
public:
    explicit ResourceManagerTestGuard(bool initialize)
    {
        auto& resourceManager = ResourceManager::Get();
        if (resourceManager.IsInitialized())
        {
            resourceManager.Shutdown();
        }

        if (initialize)
        {
            ResourceManagerConfig config;
            config.asyncThreadCount = 0;
            resourceManager.Initialize(config);
        }
    }

    ~ResourceManagerTestGuard()
    {
        auto& resourceManager = ResourceManager::Get();
        if (resourceManager.IsInitialized())
        {
            resourceManager.Shutdown();
        }
    }
};
```

- [ ] Add this helper near the loaders:

```cpp
SceneEntity* FindEntityByName(SceneManager& sceneManager, const std::string& name)
{
    SceneEntity* found = nullptr;
    sceneManager.ForEachEntity([&](SceneEntity* entity) {
        if (entity && entity->GetName() == name)
        {
            found = entity;
        }
    });
    return found;
}
```

- [ ] Add `Test_WorldLoadModelResourceReplacesSceneContent` near other prefab/model tests:

```cpp
bool Test_WorldLoadModelResourceReplacesSceneContent()
{
    ResourceManagerTestGuard resourceGuard(true);
    auto& resourceManager = ResourceManager::Get();
    resourceManager.RegisterLoader(ResourceType::Model, std::make_unique<WorldLoadModelLoader>());

    World world;
    world.Initialize();

    ActorSpawnParams existingParams;
    existingParams.name = "ExistingActor";
    TEST_ASSERT_NOT_NULL(world.SpawnActor(existingParams));
    TEST_ASSERT_EQ(static_cast<size_t>(1), world.GetSceneManager()->GetEntityCount());

    world.Load("fixture.model");

    SceneManager* sceneManager = world.GetSceneManager();
    TEST_ASSERT_NOT_NULL(sceneManager);
    TEST_ASSERT_EQ(static_cast<size_t>(2), sceneManager->GetEntityCount());

    auto* root = FindEntityByName(*sceneManager, "LoadedWorldRoot");
    auto* child = FindEntityByName(*sceneManager, "LoadedWorldChild");
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_NOT_NULL(child);
    TEST_ASSERT_EQ(nullptr, FindEntityByName(*sceneManager, "ExistingActor"));
    TEST_ASSERT_EQ(Vec3(1.0f, 2.0f, 3.0f), root->GetPosition());
    TEST_ASSERT_EQ(root, child->GetParent());
    TEST_ASSERT_EQ(Vec3(4.0f, 5.0f, 6.0f), child->GetPosition());
    TEST_ASSERT_EQ(static_cast<Actor*>(root), world.GetActor(root->GetHandle()));

    world.Shutdown();
    return true;
}
```

- [ ] Add `Test_WorldLoadInvalidRequestKeepsExistingContent` after the previous test:

```cpp
bool Test_WorldLoadInvalidRequestKeepsExistingContent()
{
    ResourceManagerTestGuard resourceGuard(false);

    World world;
    world.Initialize();

    ActorSpawnParams params;
    params.name = "PersistentActor";
    SceneEntity* existing = world.SpawnActor(params);
    TEST_ASSERT_NOT_NULL(existing);

    world.Load("missing.model");

    TEST_ASSERT_EQ(static_cast<size_t>(1), world.GetSceneManager()->GetEntityCount());
    TEST_ASSERT_EQ(existing, FindEntityByName(*world.GetSceneManager(), "PersistentActor"));

    world.Shutdown();
    return true;
}
```

- [ ] Add `Test_WorldLoadEmptyModelKeepsExistingContent` after the previous test:

```cpp
bool Test_WorldLoadEmptyModelKeepsExistingContent()
{
    ResourceManagerTestGuard resourceGuard(true);
    auto& resourceManager = ResourceManager::Get();
    resourceManager.RegisterLoader(ResourceType::Model, std::make_unique<WorldLoadEmptyModelLoader>());

    World world;
    world.Initialize();

    ActorSpawnParams params;
    params.name = "PersistentActor";
    SceneEntity* existing = world.SpawnActor(params);
    TEST_ASSERT_NOT_NULL(existing);

    world.Load("empty.model");

    TEST_ASSERT_EQ(static_cast<size_t>(1), world.GetSceneManager()->GetEntityCount());
    TEST_ASSERT_EQ(existing, FindEntityByName(*world.GetSceneManager(), "PersistentActor"));

    world.Shutdown();
    return true;
}
```

- [ ] Register the three tests in `main()`:

```cpp
suite.AddTest("WorldLoadModelResourceReplacesSceneContent",
              Test_WorldLoadModelResourceReplacesSceneContent);
suite.AddTest("WorldLoadInvalidRequestKeepsExistingContent",
              Test_WorldLoadInvalidRequestKeepsExistingContent);
suite.AddTest("WorldLoadEmptyModelKeepsExistingContent",
              Test_WorldLoadEmptyModelKeepsExistingContent);
```

- [ ] Build the focused target and confirm RED:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation
.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
```

Expected result before implementation: the executable builds, but `WorldLoadModelResourceReplacesSceneContent` fails because `World::Load()` is still a logging-only stub and leaves the existing actor in place.

---

## Task 2: Implement ModelResource World Load

**Files:**
- Modify: `E:\WorkSpace\RenderVerseX\World\Private\World.cpp`
- Modify: `E:\WorkSpace\RenderVerseX\World\CMakeLists.txt`

- [ ] In `World\CMakeLists.txt`, add `RVX::Resource` to `target_link_libraries(RVX_World ...)`:

```cmake
target_link_libraries(RVX_World
    PUBLIC
        RVX::Core
        RVX::Scene
        RVX::Resource
        Spatial
        RVX::Runtime
)
```

- [ ] In `World.cpp`, add includes after `Core/Log.h`:

```cpp
#include "Resource/ResourceManager.h"
#include "Resource/Types/ModelResource.h"
```

- [ ] Replace `World::Load` with:

```cpp
void World::Load(const std::string& path)
{
    RVX_CORE_INFO("Loading world from: {}", path);

    if (!m_initialized || !m_sceneManager)
    {
        RVX_CORE_WARN("Cannot load world content before World is initialized");
        return;
    }

    if (path.empty())
    {
        RVX_CORE_WARN("Cannot load world content from an empty path");
        return;
    }

    auto& resourceManager = Resource::ResourceManager::Get();
    if (!resourceManager.IsInitialized())
    {
        RVX_CORE_WARN("Cannot load world content because ResourceManager is not initialized");
        return;
    }

    Resource::IResource* resource = resourceManager.LoadResource(path);
    auto* model = dynamic_cast<Resource::ModelResource*>(resource);
    if (!model)
    {
        RVX_CORE_WARN("World::Load only supports ModelResource paths in this phase: {}", path);
        return;
    }

    auto newSceneManager = std::make_unique<SceneManager>();
    newSceneManager->Initialize();

    Actor* rootActor = model->InstantiateActor(newSceneManager.get());
    if (!rootActor)
    {
        newSceneManager->Shutdown();
        RVX_CORE_WARN("World::Load loaded a model but failed to instantiate it: {}", path);
        return;
    }

    ClearPureActors();
    if (m_sceneManager)
    {
        m_sceneManager->Shutdown();
    }
    m_sceneManager = std::move(newSceneManager);

    RVX_CORE_INFO("Loaded world model root actor: {}", rootActor->GetName());
}
```

- [ ] Build and run focused validation:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation
.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
```

Expected result: all resource instantiation tests pass.

---

## Task 3: Validate, Review, And Commit

**Files:**
- No additional source files.

- [ ] Build all validation targets and ModelViewer sequentially:

```powershell
foreach ($target in @('ActorComponentValidation','SpatialComponentValidation','ResourceInstantiationValidation','RenderSceneValidation','SystemIntegrationTest','MaterialSystemValidation','RenderPassBindingValidation','ModelViewer')) {
    cmake --build build\Debug --config Debug --target $target
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
```

Expected result: every target builds.

- [ ] Run validation executables:

```powershell
.\build\Debug\Tests\Debug\ActorComponentValidation.exe
.\build\Debug\Tests\Debug\SpatialComponentValidation.exe
.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
.\build\Debug\Tests\Debug\RenderSceneValidation.exe
.\build\Debug\Tests\Debug\SystemIntegrationTest.exe
.\build\Debug\Tests\Debug\MaterialSystemValidation.exe
.\build\Debug\Tests\Debug\RenderPassBindingValidation.exe
```

Expected result: every executable returns exit code 0 and reports all tests passing.

- [ ] Run ModelViewer smoke:

```powershell
$stdout = "build_codex\phase14_modelviewer_stdout.log"
$stderr = "build_codex\phase14_modelviewer_stderr.log"
New-Item -ItemType Directory -Force -Path "build_codex" | Out-Null
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
Review Phase 14 of the UE-style Actor Component framework.

Requirements:
- World::Load(path) loads ModelResource through ResourceManager.
- Invalid load requests do not clear existing scene content.
- Loaded models without root nodes do not clear existing scene content.
- Any staged model instantiation failure does not clear existing scene content.
- Valid model load replaces existing scene content.
- Loaded model instantiates through ModelResource::InstantiateActor.
- No implicit World initialization and no scene/prefab asset format is introduced.

Please report Critical and Important issues first, with file/line references.
```

- [ ] Fix any Critical or Important review findings before committing.

- [ ] Update every checkbox in this plan that was completed.

- [ ] Commit the implementation:

```powershell
git add Docs\superpowers\plans\2026-05-15-ue-style-actor-component-phase14-world-load-model.md World\CMakeLists.txt World\Private\World.cpp Tests\ResourceInstantiationValidation\main.cpp
git commit -m "Load model resources through World"
```

---

## Self-Review

- Spec coverage: This plan makes the existing `World::Load(path)` API useful for actor-based model content without inventing a broader asset format, and it covers invalid-resource plus non-instantiable-model preservation.
- Gap scan: Executable steps are concrete; scene files, prefab files, async streaming, and property deserialization are explicitly out of scope.
- Type consistency: `World::Load`, `ResourceManager::LoadResource`, `Resource::ModelResource`, `ModelResource::InstantiateActor`, `SceneManager::ForEachEntity`, `World\CMakeLists.txt`, and the validation test framework exist in the current codebase.
