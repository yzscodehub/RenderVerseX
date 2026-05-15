# UE-Style Actor Component Phase 3: Renderable Primitive Migration

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:executing-plans` to execute this plan. Use `superpowers:test-driven-development` for every behavior change. After implementation, use `superpowers:requesting-code-review` with two review subagents before committing.

**Goal:** Introduce a UE-style `StaticMeshComponent : PrimitiveComponent` renderable path while preserving the current `MeshRendererComponent`/ModelViewer behavior.

**Architecture:** Keep `RenderScene` as the render-thread snapshot boundary. New renderables should be extracted through `PrimitiveComponent::CollectRenderData(RenderScene&)`. `SceneManager` owns a duplicate-safe primitive registry for UE-style actor components. During this migration phase, `RenderSceneCollector` reads registered primitives first and keeps the existing legacy `MeshRendererComponent` traversal as compatibility fallback. Legacy collection is skipped only for entities that actually emitted at least one render object through the primitive path, so an incomplete or disabled `StaticMeshComponent` cannot suppress the existing renderer.

**Tech Stack:** C++20, CMake, Scene module, Render module, Resource module, standalone validation executables.

## Current Findings

- `PrimitiveComponent` already derives from `SceneComponent` and has `HasRenderData()` / `CollectRenderData(RenderScene&)`, but no concrete mesh primitive uses it yet.
- `MeshRendererComponent` still derives from legacy `Component`, not `PrimitiveComponent`, and its `CollectRenderData()` implementation is a placeholder.
- `RenderSceneCollector` currently recurses `SceneEntity` hierarchy and directly reads legacy `MeshRendererComponent`.
- `ComponentFactory` and `ModelResource::InstantiateNode()` still create `MeshRendererComponent` through `SceneEntity::AddComponent<T>()`, so changing `MeshRendererComponent` to inherit `StaticMeshComponent` would break existing factory and sample code.
- `SceneManager` currently stores entities and spatial data only; it has no primitive registry.

## Scope

### In Scope

1. Add `StaticMeshComponent : PrimitiveComponent`.
2. Register and unregister UE-style primitive components in `SceneManager`.
3. Update `RenderSceneCollector` to collect registered primitives through `CollectRenderData()`.
4. Preserve legacy `MeshRendererComponent` collection so existing model loading and ModelViewer continue to work.
5. Add validation coverage for the new primitive path and registry lifecycle.

### Out of Scope

- Removing `MeshRendererComponent`.
- Changing `ComponentFactory` to return actor components.
- Migrating spatial queries from entity bounds to primitive bounds.
- Serialization/prefab migration.
- RHI or RenderGraph changes.

## Files To Create

- `Scene/Include/Scene/Components/StaticMeshComponent.h`
- `Scene/Private/Components/StaticMeshComponent.cpp`

## Files To Modify

- `Scene/CMakeLists.txt`
- `Scene/Include/Scene/Actor.h`
- `Scene/Private/Actor.cpp`
- `Scene/Include/Scene/PrimitiveComponent.h`
- `Scene/Private/PrimitiveComponent.cpp`
- `Scene/Include/Scene/SceneManager.h`
- `Scene/Private/SceneManager.cpp`
- `Scene/Include/Scene/Scene.h`
- `Render/Private/Renderer/RenderSceneCollector.h`
- `Render/Private/Renderer/RenderSceneCollector.cpp`
- `Tests/ActorComponentValidation/main.cpp`
- `Tests/RenderSceneValidation/main.cpp`

## Design Details

### StaticMeshComponent

`StaticMeshComponent` is the new long-term mesh renderable component:

```cpp
class StaticMeshComponent : public PrimitiveComponent
{
public:
    const char* GetClassName() const override { return "StaticMeshComponent"; }

    bool HasRenderData() const override;
    void CollectRenderData(RenderScene& scene) const override;

    void SetMesh(Resource::ResourceHandle<Resource::MeshResource> mesh);
    Resource::ResourceHandle<Resource::MeshResource> GetMesh() const { return m_mesh; }
    bool HasValidMesh() const;

    void SetMaterial(size_t submeshIndex, Resource::ResourceHandle<Resource::MaterialResource> material);
    Resource::ResourceHandle<Resource::MaterialResource> GetMaterial(size_t submeshIndex) const;
    size_t GetSubmeshCount() const;

    bool CastsShadow() const { return m_castsShadow; }
    void SetCastsShadow(bool castsShadow) { m_castsShadow = castsShadow; }
    bool ReceivesShadow() const { return m_receivesShadow; }
    void SetReceivesShadow(bool receivesShadow) { m_receivesShadow = receivesShadow; }

private:
    Resource::ResourceHandle<Resource::MeshResource> m_mesh;
    std::vector<Resource::ResourceHandle<Resource::MaterialResource>> m_materialOverrides;
    bool m_castsShadow = true;
    bool m_receivesShadow = true;
};
```

Extraction fills the same `RenderObject` fields as the legacy collector:

```cpp
RenderObject obj;
obj.worldMatrix = GetWorldTransform();
obj.normalMatrix = glm::inverseTranspose(Mat4(Mat3(obj.worldMatrix)));
obj.bounds = GetWorldBounds();
obj.meshResource = m_mesh.Get();
obj.meshId = m_mesh.GetId();
obj.entityId = owner ? owner->GetHandle() : 0;
obj.castsShadow = m_castsShadow;
obj.receivesShadow = m_receivesShadow;
obj.visible = IsVisible();
```

### Primitive Registry

`SceneManager` stores UE-style primitives:

```cpp
void RegisterPrimitive(PrimitiveComponent* primitive);
void UnregisterPrimitive(PrimitiveComponent* primitive);
const std::vector<PrimitiveComponent*>& GetPrimitives() const;
```

`PrimitiveComponent::OnRegister()` registers itself when its owner is a `SceneEntity` with a `SceneManager`. `OnUnregister()` unregisters it.

Important compatibility rule: when `SceneManager::AddEntity()` sets the entity scene manager, it should call `entity->RegisterAllComponents()` so existing actor components become registered. `DestroyEntity()` / `Shutdown()` should unregister before clearing or detaching scene manager references.

Runtime-created actor components need the same behavior. Add a narrow virtual hook in `Actor` so `SceneEntity` can request automatic registration once it is owned by a scene:

```cpp
class Actor
{
protected:
    virtual bool ShouldAutoRegisterComponent(ActorComponent* component) const;
};
```

`Actor::AddComponent<T>()` should call this hook after `OnComponentCreated()`. If it returns true, `Actor` sets the component registered, calls `OnRegister()`, and initializes it if needed. `SceneEntity::ShouldAutoRegisterComponent()` returns true only when `m_sceneManager != nullptr`. This keeps non-scene actors unchanged while allowing:

```cpp
auto handle = sceneManager.CreateEntity("Mesh");
auto* entity = sceneManager.GetEntity(handle);
auto* primitive = static_cast<RVX::Actor*>(entity)->AddComponent<RVX::StaticMeshComponent>();
```

to appear in `SceneManager::GetPrimitives()` immediately.

### RenderSceneCollector

Collector flow:

1. Clear `RenderScene`.
2. Ask `SceneManager` for registered primitives.
3. For each enabled, visible, render-data primitive, call `CollectRenderData(outScene)`.
4. Track entity handles for primitives that actually appended render objects to `outScene`.
5. Collect lights through the existing entity hierarchy.
6. Collect legacy `MeshRendererComponent` only for entities not present in the primitive-emitted handle set, preventing duplicate objects without dropping valid legacy fallback draws.

Primitive collection returns a set of entity handles that produced render objects:

```cpp
static std::unordered_set<SceneEntity::Handle> CollectRegisteredPrimitives(
    RenderScene& outScene,
    SceneManager* sceneManager)
{
    std::unordered_set<SceneEntity::Handle> primitiveRenderedEntities;
    for (PrimitiveComponent* primitive : sceneManager->GetPrimitives())
    {
        if (!primitive || !primitive->IsEnabled() || !primitive->IsVisible() || !primitive->HasRenderData())
            continue;

        auto* ownerEntity = dynamic_cast<SceneEntity*>(primitive->GetOwner());
        const size_t before = outScene.GetObjectCount();
        primitive->CollectRenderData(outScene);
        if (ownerEntity && outScene.GetObjectCount() > before)
        {
            primitiveRenderedEntities.insert(ownerEntity->GetHandle());
        }
    }
    return primitiveRenderedEntities;
}
```

This keeps ModelViewer stable while allowing new code to use:

```cpp
auto* mesh = static_cast<RVX::Actor*>(entity)->AddComponent<RVX::StaticMeshComponent>();
mesh->SetMesh(meshHandle);
mesh->AttachToComponent(entity->GetRootComponent());
```

## Implementation Tasks

### Task 1: TDD For StaticMeshComponent Basics

Add failing tests in `Tests/ActorComponentValidation/main.cpp`:

- `StaticMeshComponentIsPrimitiveSceneComponent`
- `StaticMeshComponentTracksMeshBoundsAndMaterials`
- `StaticMeshComponentRegistrationFollowsSceneManagerLifecycle`

Expected assertions:

- `std::is_base_of_v<PrimitiveComponent, StaticMeshComponent>` is true.
- `GetClassName()` returns `StaticMeshComponent`.
- `HasRenderData()` is false without a valid mesh.
- Registering an entity with a `SceneManager` registers the component and adds it to `SceneManager::GetPrimitives()`.
- Destroying the entity unregisters the primitive.

Run:

```powershell
cmake --build build\Debug --config Debug --target ActorComponentValidation
build\Debug\Tests\Debug\ActorComponentValidation.exe
```

The new tests should fail before implementation.

### Task 2: Implement StaticMeshComponent And Primitive Registration

Implement:

- `StaticMeshComponent` mesh/material/shadow API.
- `PrimitiveComponent::OnRegister()` / `OnUnregister()` registry hooks.
- `SceneManager` primitive registry with duplicate-safe register/unregister. Use a vector for stable iteration order and an `std::unordered_set<PrimitiveComponent*>` membership set for O(1) duplicate checks.
- `SceneManager::AddEntity()` calls `RegisterAllComponents()` after `SetSceneManager(this)`.
- `Actor::AddComponent<T>()` auto-registers components only when `ShouldAutoRegisterComponent()` returns true.
- `SceneEntity::ShouldAutoRegisterComponent()` returns true after the entity belongs to a `SceneManager`.
- Runtime removal through `Actor::RemoveComponent<T>()` relies on existing `OnUnregister()` dispatch for registered components. Add a test that removing a scene-owned `StaticMeshComponent` removes it from `SceneManager::GetPrimitives()`.
- `SceneManager::DestroyEntity()` calls `UnregisterAllComponents()` before `SetSceneManager(nullptr)`.
- `SceneManager::Shutdown()` unregisters components before clearing entities.

Run ActorComponentValidation until green.

### Task 3: TDD For RenderScene Collection Through Primitives

Add failing tests in `Tests/RenderSceneValidation/main.cpp`:

- `StaticMeshComponentCollectsRenderObjectFromWorld`
- `RenderSceneCollectorKeepsLegacyMeshRendererFallback`
- `RenderSceneCollectorFallsBackToLegacyWhenStaticMeshPrimitiveHasNoRenderData`
- `RenderSceneCollectorDoesNotDuplicateWhenStaticMeshPrimitiveRenders`

Use source-level assertions only if constructing `MeshResource` handles is impractical. Prefer object-level tests when feasible:

- Create `World`, `SceneManager`, entity, `StaticMeshComponent`.
- Register components.
- Give the component a minimal valid mesh handle if resource APIs allow it.
- Call `RenderScene::CollectFromWorld()`.
- Assert one `RenderObject` appears with matching entity id, visibility, shadow flags, and world transform.
- Assert a valid legacy `MeshRendererComponent` still contributes one object when the same entity has an empty `StaticMeshComponent`.
- Assert a valid `StaticMeshComponent` and valid legacy `MeshRendererComponent` on the same entity still produce only one object.

Run:

```powershell
cmake --build build\Debug --config Debug --target RenderSceneValidation
build\Debug\Tests\Debug\RenderSceneValidation.exe
```

The new primitive collector test should fail before collector changes.

### Task 4: Update RenderSceneCollector

Refactor collector:

- Include `Scene/PrimitiveComponent.h` and `Scene/Components/StaticMeshComponent.h`.
- Add `CollectRegisteredPrimitives(RenderScene&, SceneManager*)` returning `std::unordered_set<SceneEntity::Handle>`.
- Keep `CollectEntity()` for lights and legacy mesh renderers.
- Avoid duplicate collection only when an entity already emitted a primitive render object.

Suggested private helpers:

```cpp
static std::unordered_set<SceneEntity::Handle> CollectRegisteredPrimitives(
    RenderScene& outScene,
    SceneManager* sceneManager);
static void CollectLegacyEntity(
    RenderScene& outScene,
    SceneEntity* entity,
    const Mat4& parentMatrix,
    const std::unordered_set<SceneEntity::Handle>& primitiveRenderedEntities);
```

Run RenderSceneValidation until green.

### Task 5: Regression Validation

Run:

```powershell
cmake --build build\Debug --config Debug --target ActorComponentValidation RenderSceneValidation SystemIntegrationTest MaterialSystemValidation RenderPassBindingValidation ModelViewer
build\Debug\Tests\Debug\ActorComponentValidation.exe
build\Debug\Tests\Debug\RenderSceneValidation.exe
build\Debug\Tests\Debug\SystemIntegrationTest.exe
build\Debug\Tests\Debug\MaterialSystemValidation.exe
build\Debug\Tests\Debug\RenderPassBindingValidation.exe
```

Run ModelViewer smoke:

```powershell
build\Debug\Samples\ModelViewer\Debug\ModelViewer.exe > build\Debug\modelviewer_phase3_smoke.log 2>&1
```

Because DX12 screenshot capture is currently unreliable for flip swapchain readback, use the log smoke as the primary validation. Required log evidence:

- Model loaded successfully.
- Model instantiated as SceneEntity.
- Uploaded mesh to GPU.
- RenderGraph stats show the normal frame passes.
- ModelViewer Sample Complete.

### Task 6: Review And Commit

Use one GPT-5.3-Codex-Spark review subagent. Ask it to review both primitive registration lifecycle/ownership safety and render collection duplicate or missing draw risks.

Fix any blocking findings, rerun relevant validations, then commit with message:

```text
Migrate static mesh rendering to primitive components
```

## Risks

- Calling `RegisterAllComponents()` from `SceneManager::AddEntity()` may change lifecycle timing for existing actor components. Mitigation: keep it idempotent and validate actor lifecycle tests.
- Runtime auto-registration may change behavior for actor components added to scene-owned entities. Mitigation: scope the hook to `SceneEntity` with a non-null `SceneManager` and validate both pre-add and post-add component registration.
- `PrimitiveComponent::OnRegister()` needs a `SceneEntity` owner. Non-entity `Actor` owners should be ignored safely.
- Mixed legacy/new renderables can duplicate draw objects or accidentally suppress legacy fallback. Mitigation: collector skips legacy mesh renderer only when a primitive for the same entity actually emitted a render object; add tests for empty primitive fallback and valid primitive de-duplication.
- If tests cannot create valid `ResourceHandle<MeshResource>` instances, use source-level validation for collector wiring and keep object-level tests for registry/lifecycle.

## Completion Criteria

- New `StaticMeshComponent` exists and derives from `PrimitiveComponent`.
- `SceneManager` exposes a primitive registry and lifecycle keeps it clean.
- `RenderSceneCollector` can collect UE-style primitives without removing legacy mesh renderer fallback.
- Actor, render scene, system integration, material, render pass binding validations pass.
- ModelViewer log smoke still proves model load, GPU upload, frame graph execution, and clean shutdown.
