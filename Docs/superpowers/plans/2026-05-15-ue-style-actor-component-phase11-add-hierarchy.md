# UE-Style Actor Component Phase 11 AddHierarchy Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement `SceneManager::AddHierarchy()` so a `Node` tree is converted into spawned `SceneEntity` actors with matching names, local transforms, active state, and parent-child relationships.

**Architecture:** `AddHierarchy()` becomes a compatibility bridge from the old `Node` graph to the UE-style actor/component scene. It should use `SceneManager::SpawnActor()` for every node, preserve the existing `Node` tree object, and recursively attach each child entity through `ActorSpawnParams::parent`. This phase only transfers hierarchy, transform, and active state; mesh/material conversion remains owned by `ModelResource::InstantiateActor()` because that path has the `ModelResource` dependency required by `ComponentFactory`.

**Tech Stack:** C++20, RenderVerseX Scene module, standalone validation executables.

---

## Scope

- Convert `Node` hierarchy into `SceneEntity` actors via `SpawnActor`.
- Preserve local position, rotation, scale, active flag, and parent-child structure.
- Keep `AddHierarchy(std::shared_ptr<Node>)` returning `void`.
- Ignore a null root without changing the scene.
- Do not move or mutate the input `Node` objects.
- Do not create mesh/material components from `Node::MeshComponent`; model resources already provide the correct resource-backed conversion path.

## Files

- Modify: `E:\WorkSpace\RenderVerseX\Scene\Private\SceneManager.cpp`
  - Replace the logging-only `AddHierarchy()` stub with recursive spawn logic.
- Modify: `E:\WorkSpace\RenderVerseX\Tests\ActorComponentValidation\main.cpp`
  - Add focused tests for null hierarchy and recursive hierarchy conversion.
- Modify: `E:\WorkSpace\RenderVerseX\Docs\superpowers\plans\2026-05-15-ue-style-actor-component-phase11-add-hierarchy.md`
  - Track completion status as the phase proceeds.

---

## Task 1: Add Failing Tests And Static Guard

**Files:**
- Modify: `E:\WorkSpace\RenderVerseX\Tests\ActorComponentValidation\main.cpp`

- [x] Add `#include "Scene/Node.h"` near the existing Scene includes:

```cpp
#include "Scene/Node.h"
```

- [x] Add `Test_SceneManagerAddHierarchyIgnoresNullRoot` near the existing `SceneManager` actor tests:

```cpp
bool Test_SceneManagerAddHierarchyIgnoresNullRoot()
{
    RVX::SceneManager sceneManager;
    sceneManager.Initialize();

    sceneManager.AddHierarchy(nullptr);

    TEST_ASSERT_EQ(static_cast<size_t>(0), sceneManager.GetEntityCount());

    sceneManager.Shutdown();
    return true;
}
```

- [x] Add `Test_SceneManagerAddHierarchySpawnsNodeTree` near the existing `SceneManager` actor tests:

```cpp
bool Test_SceneManagerAddHierarchySpawnsNodeTree()
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

    TEST_ASSERT_EQ(static_cast<size_t>(3), sceneManager.GetEntityCount());
    TEST_ASSERT_NOT_NULL(rootEntity);
    TEST_ASSERT_EQ(std::string("HierarchyRoot"), rootEntity->GetName());
    TEST_ASSERT_EQ(RVX::Vec3(1.0f, 2.0f, 3.0f), rootEntity->GetPosition());
    TEST_ASSERT_EQ(rootRotation, rootEntity->GetRotation());
    TEST_ASSERT_EQ(RVX::Vec3(2.0f, 2.0f, 2.0f), rootEntity->GetScale());
    TEST_ASSERT_EQ(static_cast<size_t>(1), rootEntity->GetChildren().size());

    RVX::SceneEntity* childEntity = rootEntity->GetChildren()[0];
    TEST_ASSERT_NOT_NULL(childEntity);
    TEST_ASSERT_EQ(std::string("HierarchyChild"), childEntity->GetName());
    TEST_ASSERT_EQ(rootEntity, childEntity->GetParent());
    TEST_ASSERT_EQ(RVX::Vec3(4.0f, 5.0f, 6.0f), childEntity->GetPosition());
    TEST_ASSERT_FALSE(childEntity->IsActive());
    TEST_ASSERT_EQ(static_cast<size_t>(1), childEntity->GetChildren().size());

    RVX::SceneEntity* grandChildEntity = childEntity->GetChildren()[0];
    TEST_ASSERT_NOT_NULL(grandChildEntity);
    TEST_ASSERT_EQ(std::string("HierarchyGrandChild"), grandChildEntity->GetName());
    TEST_ASSERT_EQ(childEntity, grandChildEntity->GetParent());
    TEST_ASSERT_EQ(RVX::Vec3(0.5f, 0.5f, 0.5f), grandChildEntity->GetScale());

    sceneManager.Shutdown();
    return true;
}
```

- [x] Add `Test_SceneManagerAddHierarchySkipsNodeCycles` to cover cyclic `Node` graphs and verify `visitedNodes` prevents duplicate entity spawning.

- [x] Register the hierarchy tests in `main()` after `SceneManagerSpawnActorRejectsForeignParent`:

```cpp
suite.AddTest("SceneManagerAddHierarchyIgnoresNullRoot",
              Test_SceneManagerAddHierarchyIgnoresNullRoot);
suite.AddTest("SceneManagerAddHierarchySpawnsNodeTree",
              Test_SceneManagerAddHierarchySpawnsNodeTree);
```

- [x] Build and run the focused validation:

```powershell
cmake --build build\Debug --config Debug --target ActorComponentValidation
.\build\Debug\Tests\Debug\ActorComponentValidation.exe
```

Expected result before implementation: `SceneManagerAddHierarchySpawnsNodeTree` fails because the current `AddHierarchy()` only logs and does not create entities.

- [x] Run this static guard and confirm it fails before implementation:

```powershell
rg -n "Node -> SceneEntity conversion|AddHierarchy: Added node" Scene\Private\SceneManager.cpp
```

Expected result before implementation: matches in `SceneManager::AddHierarchy()`.

---

## Task 2: Implement Recursive Node Hierarchy Spawning

**Files:**
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Private\SceneManager.cpp`

- [x] Replace the `AddHierarchy()` stub with this implementation:

```cpp
void SceneManager::AddHierarchy(std::shared_ptr<Node> rootNode)
{
    if (!rootNode)
        return;

    std::unordered_set<const Node*> visitedNodes;

    auto spawnNode = [this, &visitedNodes](const Node* node, SceneEntity* parent,
                                           auto&& spawnNodeRef) -> void {
        if (!node)
            return;

        if (visitedNodes.find(node) != visitedNodes.end())
            return;

        visitedNodes.insert(node);

        const Transform& transform = node->GetLocalTransform();

        ActorSpawnParams params;
        params.name = node->GetName();
        params.localPosition = transform.GetPosition();
        params.localRotation = transform.GetRotation();
        params.localScale = transform.GetScale();
        params.parent = parent;

        SceneEntity* entity = SpawnActor(params);
        if (!entity)
            return;

        entity->SetActive(node->IsActive());

        for (const auto& child : node->GetChildren())
        {
            spawnNodeRef(child.get(), entity, spawnNodeRef);
        }
    };

    spawnNode(rootNode.get(), nullptr, spawnNode);
}
```

- [x] Re-run the static guard:

```powershell
rg -n "Node -> SceneEntity conversion|AddHierarchy: Added node" Scene\Private\SceneManager.cpp
```

Expected result after implementation: command exits with code 1 and prints no matches.

- [x] Build and run the focused validation:

```powershell
cmake --build build\Debug --config Debug --target ActorComponentValidation
.\build\Debug\Tests\Debug\ActorComponentValidation.exe
```

Expected result after implementation: all actor component tests pass.

---

## Task 3: Full Validation, Review, And Commit

**Files:**
- No additional source files.

- [x] Build validation targets and ModelViewer:

```powershell
cmake --build build\Debug --config Debug --target ActorComponentValidation SpatialComponentValidation ResourceInstantiationValidation RenderSceneValidation SystemIntegrationTest MaterialSystemValidation RenderPassBindingValidation ModelViewer
```

Expected result: build succeeds.

- [x] Run validation executables:

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

- [x] Run ModelViewer smoke:

```powershell
$stdout = "build_codex\phase11_modelviewer_stdout.log"
$stderr = "build_codex\phase11_modelviewer_stderr.log"
Remove-Item -LiteralPath $stdout -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $stderr -ErrorAction SilentlyContinue
$process = Start-Process -FilePath ".\build\Debug\Samples\ModelViewer\Debug\ModelViewer.exe" -ArgumentList "C:\Users\yinzs\Desktop\DamagedHelmet.glb" -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
Start-Sleep -Seconds 8
if (-not $process.HasExited) { Stop-Process -Id $process.Id }
Get-Content -Path $stdout | Select-String -Pattern "Model loaded successfully|Model instantiated as SceneEntity|Scene entity ready|Uploaded mesh to GPU|RenderGraph stats"
Get-Content -Path $stderr
```

Expected result: stdout includes model loaded, model instantiated, scene entity ready, GPU mesh upload, and render graph stats; stderr is empty.

- [x] Request exactly one code review using Dalton with this context:

```text
Review Phase 11 of the UE-style Actor Component framework.

Requirements:
- SceneManager::AddHierarchy(nullptr) is a no-op.
- SceneManager::AddHierarchy(rootNode) recursively spawns SceneEntity actors with SpawnActor.
- Name, local position, rotation, scale, active state, and parent-child hierarchy are preserved.
- Input Node objects are not moved or mutated.
- Mesh/material component conversion remains in ModelResource::InstantiateActor and is not added here.

Please report Critical and Important issues first, with file/line references.
```

- [x] Fix any Critical or Important review findings before committing.

- [x] Update every checkbox in this plan that was completed.

- [x] Commit the implementation:

```powershell
git add Docs\superpowers\plans\2026-05-15-ue-style-actor-component-phase11-add-hierarchy.md Scene\Private\SceneManager.cpp Tests\ActorComponentValidation\main.cpp
git commit -m "Implement scene node hierarchy spawning"
```

---

## Self-Review

- Spec coverage: This closes the `SceneManager::AddHierarchy()` compatibility bridge left as an implementation stub.
- Placeholder scan: No placeholder tasks are left; every code change and verification command is explicit.
- Type consistency: `Node`, `SceneEntity`, `Transform`, `ActorSpawnParams`, and `SceneManager::SpawnActor()` match the current headers and implementation.
