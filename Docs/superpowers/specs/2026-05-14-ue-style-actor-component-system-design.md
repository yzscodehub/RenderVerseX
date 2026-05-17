# UE-Style Actor Component System Design

## Status

Design approved direction:

- Target architecture: full UE-style Actor Component System.
- Migration strategy: staged compatibility migration.
- First implementation phase must preserve existing samples, model loading, render extraction, and material routing.

Base checkpoint:

- Commit: `3751bed Complete material system routing`
- Current untracked local artifacts are intentionally out of scope: `.claude/`, `.codex`, `ShaderCache/`, `build_*`.

## Problem Statement

RenderVerseX currently has an object-oriented component model:

- `SceneEntity` owns identity, active state, layer mask, transform, bounds cache, hierarchy, and component ownership.
- `Component` is a virtual base class with owner pointer, lifecycle hooks, optional bounds, and per-frame `Tick()`.
- `SceneManager` owns `SceneEntity` instances through a handle map.
- `RenderSceneCollector` traverses the `SceneEntity` tree and pulls data from `MeshRendererComponent` and `LightComponent`.
- `SpatialSubsystem` and other systems use `SceneEntity*` as the spatial/runtime object identity.

This design works for small scenes, but it mixes responsibilities that should be separated in a UE-style engine:

- Transform and attachment belong to scene components, not to the actor itself.
- Renderable/spatial objects should register as primitives, not rely on actor traversal.
- Components need a richer registration lifecycle than attach/detach/tick.
- Future editor, prefab, scripting, and reflection support need stable class metadata and factories.
- Render and RHI should remain isolated from gameplay objects and consume extracted render snapshots.

## Goals

- Make `Actor` the long-term runtime object concept.
- Make `ActorComponent`, `SceneComponent`, and `PrimitiveComponent` the core component hierarchy.
- Move transform, attachment, bounds, render registration, and spatial registration out of the actor.
- Keep `RenderScene` as the renderer-facing snapshot boundary.
- Preserve existing sample behavior during migration.
- Provide lightweight reflection metadata and factory registration for future editor/prefab/script integration.
- Allow the old `SceneEntity` and `Component` names to exist only as temporary compatibility APIs.

## Non-Goals

- Do not introduce archetype ECS or sparse-set ECS storage.
- Do not make RHI, RenderGraph, or render passes aware of `Actor` or component pointers.
- Do not implement Blueprint, full property reflection, hot reload, or editor UI in the first phase.
- Do not migrate every component in one commit.
- Do not remove `SceneEntity` until the compatibility window is complete.

## Target Architecture

The long-term object model is:

```text
World
  owns Actor instances
  owns WorldSubsystem instances
  owns component registration lists through subsystems/managers

Actor
  owns ActorComponent instances
  has optional RootComponent
  manages lifecycle and component creation/destruction

ActorComponent
  non-spatial component base
  lifecycle, ticking, owner reference, metadata

SceneComponent : ActorComponent
  local/world transform
  attachment parent/children
  transform dirty propagation

PrimitiveComponent : SceneComponent
  bounds, visibility, layer mask
  render registration
  spatial registration
```

The runtime data flow is:

```text
World::Tick()
  -> Actor/component lifecycle and tick groups
  -> Transform update for SceneComponent hierarchy
  -> Primitive registration updates
  -> Render extraction from registered PrimitiveComponent instances
  -> RenderScene snapshot
  -> SceneRenderer / RenderGraph / RHI
```

The renderer-facing boundary remains:

```text
Actor/Component world data -> RenderSceneCollector -> RenderScene -> SceneRenderer
```

Render passes continue to consume `RenderScene`, `RenderDrawItem`, GPU resource managers, material systems, and RHI objects. They must not query actors or components directly.

## Core Types

### Actor

`Actor` replaces `SceneEntity` as the long-term object identity.

Responsibilities:

- Stable handle/id.
- Name.
- Active state.
- Component ownership.
- Root component ownership/reference.
- Lifecycle dispatch.
- Tick enablement at actor level.
- Compatibility forwarding for old transform methods during migration.

Not responsibilities:

- Own transform data directly.
- Own bounds cache directly.
- Own child actor hierarchy directly.
- Act as the spatial index entity directly in the final architecture.

Proposed public shape:

```cpp
class Actor
{
public:
    using Handle = Spatial::EntityHandle;

    explicit Actor(const std::string& name = "Actor");
    virtual ~Actor();

    virtual const char* GetClassName() const;

    Handle GetHandle() const;
    const std::string& GetName() const;
    void SetName(const std::string& name);

    bool IsActive() const;
    void SetActive(bool active);

    SceneComponent* GetRootComponent() const;
    void SetRootComponent(SceneComponent* component);

    template<typename T, typename... Args>
    T* AddComponent(Args&&... args);

    template<typename T>
    T* GetComponent() const;

    virtual void OnConstruction();
    virtual void BeginPlay();
    virtual void Tick(float deltaTime);
    virtual void EndPlay();

    // Compatibility only during migration.
    Vec3 GetWorldPosition() const;
    Mat4 GetWorldMatrix() const;
    void SetPosition(const Vec3& position);
};
```

### ActorComponent

`ActorComponent` replaces `Component` as the long-term base class.

Responsibilities:

- Owner pointer.
- Enabled/active state.
- Registration lifecycle.
- Tick configuration.
- Lightweight class metadata.
- Serialization hooks.

Proposed lifecycle:

```text
OnComponentCreated
OnRegister
InitializeComponent
BeginPlay
TickComponent
EndPlay
OnUnregister
OnComponentDestroyed
```

Proposed public shape:

```cpp
class ActorComponent
{
public:
    virtual const char* GetClassName() const;

    Actor* GetOwner() const;
    bool IsRegistered() const;
    bool IsEnabled() const;
    void SetEnabled(bool enabled);

    bool CanEverTick() const;
    bool IsTickEnabled() const;
    void SetTickEnabled(bool enabled);

    virtual void OnComponentCreated();
    virtual void OnRegister();
    virtual void InitializeComponent();
    virtual void BeginPlay();
    virtual void TickComponent(float deltaTime);
    virtual void EndPlay();
    virtual void OnUnregister();
    virtual void OnComponentDestroyed();

    virtual void Serialize(ComponentArchive& archive) const;
    virtual void Deserialize(ComponentArchive& archive);
};
```

### SceneComponent

`SceneComponent` owns transform and attachment.

Responsibilities:

- Local transform.
- Cached world transform.
- Parent/child attachment graph between scene components.
- Dirty propagation.
- Transform changed notification.

Attachment is component-to-component, not actor-to-actor. Actor hierarchy becomes an emergent relationship through root components.

Proposed public shape:

```cpp
class SceneComponent : public ActorComponent
{
public:
    const Vec3& GetRelativeLocation() const;
    const Quat& GetRelativeRotation() const;
    const Vec3& GetRelativeScale3D() const;

    void SetRelativeLocation(const Vec3& location);
    void SetRelativeRotation(const Quat& rotation);
    void SetRelativeScale3D(const Vec3& scale);

    Mat4 GetRelativeTransform() const;
    Mat4 GetWorldTransform() const;
    Vec3 GetWorldLocation() const;
    Quat GetWorldRotation() const;

    bool AttachToComponent(SceneComponent* parent);
    void DetachFromComponent();

    SceneComponent* GetAttachParent() const;
    const std::vector<SceneComponent*>& GetAttachChildren() const;

protected:
    virtual void OnTransformChanged();
};
```

### PrimitiveComponent

`PrimitiveComponent` is the base for objects that have spatial/render relevance.

Responsibilities:

- Local and world bounds.
- Visibility.
- Layer mask.
- Spatial registration.
- Render primitive registration.
- Render data extraction hook.

Proposed public shape:

```cpp
class PrimitiveComponent : public SceneComponent
{
public:
    bool IsVisible() const;
    void SetVisible(bool visible);

    uint32 GetLayerMask() const;
    void SetLayerMask(uint32 mask);

    virtual AABB GetLocalBounds() const;
    AABB GetWorldBounds() const;

    virtual bool HasRenderData() const;
    virtual void CollectRenderData(RenderScene& scene) const;

protected:
    void MarkRenderStateDirty();
    void MarkBoundsDirty();
};
```

## Component Migration Mapping

Current components migrate as follows:

| Current Type | Target Type | Notes |
| --- | --- | --- |
| `Component` | `ActorComponent` | Temporary alias/compat wrapper during migration. |
| `SceneEntity` transform | `SceneComponent` root | Actor forwards old transform API to root during migration. |
| `MeshRendererComponent` | `StaticMeshComponent : PrimitiveComponent` | Primary renderable primitive. |
| `LightComponent` | `LightComponent : SceneComponent` or `PrimitiveComponent` | Use `SceneComponent` if no bounds/spatial queries are needed; use `PrimitiveComponent` if light culling/spatial indexing is required. |
| `CameraComponent` | `CameraComponent : SceneComponent` | View extraction reads transform from component. |
| `DecalComponent` | `DecalComponent : PrimitiveComponent` | Decals have bounds and render registration. |
| `ReflectionProbeComponent` | `ReflectionProbeComponent : PrimitiveComponent` | Spatial and render relevant. |
| `AudioComponent` | `AudioComponent : SceneComponent` | Positional audio needs transform. |
| `RigidBodyComponent` | `ActorComponent` plus SceneComponent sync | Physics owns body state, syncs with scene component transform. |
| `SkeletonComponent` | `ActorComponent` | Bone attachments attach scene components to socket components or socket handles. |
| `AnimatorComponent` | `ActorComponent` | Updates animation/skeleton data; not spatial by itself. |
| `LODComponent` | `ActorComponent` or integrated into primitive | Prefer primitive-level LOD selection later. |

## Compatibility Strategy

The end state should not keep two permanent object models. Compatibility exists only to keep the engine working while migration proceeds.

Initial compatibility:

```cpp
class SceneEntity : public Actor
{
public:
    using Actor::Actor;

    // Old API forwards to Actor/root component.
    Mat4 GetWorldMatrix() const;
    AABB GetWorldBounds() const;
};

class Component : public ActorComponent
{
public:
    SceneEntity* GetOwner() const;
};
```

This allows existing call sites to compile while new code uses `Actor` and `ActorComponent`.

Compatibility rules:

- New features should use `Actor` naming.
- Existing samples may continue using `SceneEntity` during phase 1.
- Existing `Component::Tick()` should be forwarded to `TickComponent()` temporarily.
- `SceneEntity` transform APIs must be marked as migration compatibility in comments.
- No new subsystem should take `SceneEntity*` unless it is explicitly compatibility-only.

## Module Layout

Initial file placement should stay in the `Scene` module:

```text
Scene/
  Include/Scene/
    Actor.h
    ActorComponent.h
    SceneComponent.h
    PrimitiveComponent.h
    ActorFactory.h
    ComponentFactory.h
    SceneEntity.h       # compatibility
    Component.h         # compatibility
  Private/
    Actor.cpp
    ActorComponent.cpp
    SceneComponent.cpp
    PrimitiveComponent.cpp
```

Long term, if Scene grows too large, split into:

```text
ActorCore/
Scene/
World/
```

Do not split modules during phase 1. It adds build churn without improving the first migration step.

## World and SceneManager Changes

Long-term ownership should be:

```text
World
  owns ActorManager or SceneManager
  owns WorldSubsystems

ActorManager/SceneManager
  creates Actor
  destroys Actor
  owns actor handle map
  tracks registered components by category
```

Phase 1 should keep `SceneManager` and add `Actor` APIs:

```cpp
Actor::Handle CreateActor(const std::string& name = "Actor");
Actor* GetActor(Actor::Handle handle);
void DestroyActor(Actor::Handle handle);
```

Old APIs remain:

```cpp
SceneEntity::Handle CreateEntity(const std::string& name = "Entity");
SceneEntity* GetEntity(SceneEntity::Handle handle);
void DestroyEntity(SceneEntity::Handle handle);
```

Internally, `CreateEntity()` can create a `SceneEntity` compatibility actor. Later it can become an alias over `CreateActor()`.

## Transform and Attachment

Current `SceneEntity` hierarchy should migrate to `SceneComponent` attachment.

Rules:

- Actor transform equals root component transform.
- Actor parent/child relationships should be deprecated.
- Attachments are between `SceneComponent` instances.
- `RootComponent` is required for spatial actors, optional for non-spatial actors.
- A default root `SceneComponent` may be created for actors that use transform compatibility APIs.

Dirty propagation:

- Relative transform changes mark the component world transform dirty.
- Dirty state propagates to attached children.
- Bounds dirty propagates to primitive registration and spatial index.
- Transform update should be centralized, not recomputed recursively from arbitrary render extraction paths.

## Primitive Registration

`PrimitiveComponent` should register with world-level services when it becomes active:

```text
PrimitiveComponent::OnRegister()
  -> World primitive registry
  -> SpatialSubsystem registration if spatially queryable
  -> Render extraction list registration

PrimitiveComponent::OnUnregister()
  -> Remove from primitive registry
  -> Remove from spatial index
  -> Mark render scene dirty
```

The first phase can implement registration lists simply:

```cpp
std::vector<PrimitiveComponent*> m_registeredPrimitives;
```

Later optimization can introduce stable handles and dense arrays.

## Render Extraction

Current `RenderSceneCollector` recursively walks `SceneEntity` and checks components. Long-term it should iterate registered primitives:

```cpp
void RenderSceneCollector::Collect(RenderScene& outScene, World* world)
{
    outScene.Clear();

    for (PrimitiveComponent* primitive : world->GetRegisteredPrimitives())
    {
        if (!primitive || !primitive->IsVisible())
            continue;

        primitive->CollectRenderData(outScene);
    }
}
```

`StaticMeshComponent::CollectRenderData()` creates `RenderObject` from:

- world transform from `SceneComponent`
- bounds from `PrimitiveComponent`
- mesh resource id
- submesh material ids
- render flags
- owner actor handle

This keeps the material system and render pass routing stable because they already consume `RenderObject` and `RenderDrawItem`.

## Spatial System

The current spatial system uses `SceneEntity*` as user data. Long-term spatial indexing should use `PrimitiveComponent` or a stable primitive handle.

Phase 1:

- Add compatibility support for both `SceneEntity*` and `PrimitiveComponent*` if needed.
- Prefer `PrimitiveComponent*` for new registrations.

Target:

```cpp
struct SpatialProxy
{
    PrimitiveComponent* primitive = nullptr;
    Actor::Handle owner = Actor::InvalidHandle;
};
```

Picking result should eventually return:

```cpp
struct HitResult
{
    Actor* actor = nullptr;
    PrimitiveComponent* component = nullptr;
    Vec3 location;
    Vec3 normal;
    float distance = 0.0f;
};
```

This mirrors UE's actor/component hit model.

## Lightweight Reflection and Factories

First phase reflection should be intentionally small.

Required metadata:

- `GetClassName()`
- base class relationship known by C++ type
- factory registration by string
- serialization hooks

Avoid macro-heavy property reflection in phase 1.

Proposed factories:

```cpp
class ActorFactory
{
public:
    template<typename T>
    static void Register(const std::string& className);

    static std::unique_ptr<Actor> Create(const std::string& className);
};

class ComponentFactory
{
public:
    template<typename T>
    static void Register(const std::string& className);

    static std::unique_ptr<ActorComponent> Create(const std::string& className);
};
```

Serialization contract:

```cpp
class ComponentArchive
{
public:
    template<typename T>
    void Field(const char* name, T& value);
};
```

The first implementation may stub archive support if prefab serialization is not migrated in the same phase, but the virtual hooks should exist so the API shape is stable.

## Tick Model

UE-style tick should be explicit and opt-in.

Proposed tick groups:

```cpp
enum class TickGroup : uint8
{
    PrePhysics,
    DuringPhysics,
    PostPhysics,
    PreRender,
};
```

Rules:

- Components do not tick just because they exist.
- `ActorComponent` declares whether it can ever tick.
- Tick registration is centralized in world/component manager lists.
- Actor tick and component tick are separate.
- Render extraction should run after transform/animation/physics relevant ticks.

Phase 1 can preserve existing `Component::Tick()` through compatibility forwarding, but the target is `TickComponent(float deltaTime)`.

## Resource and Model Instantiation

`ModelResource::Instantiate()` currently creates a `SceneEntity` tree and attaches `MeshRendererComponent`.

Target behavior:

- Create `Actor` for imported nodes that need an object identity.
- Create root `SceneComponent` for node transform.
- Attach child scene components to parent scene components.
- Attach `StaticMeshComponent` for mesh primitives.
- Assign material resource ids per submesh.

During compatibility:

- `Instantiate()` may continue returning `SceneEntity*`.
- Internally, that `SceneEntity` should be an `Actor` with a root scene component.
- A future API should be added:

```cpp
Actor* InstantiateActor(SceneManager* scene) const;
```

The old `Instantiate()` can then call the new path and return a compatibility pointer where possible.

## Prefab Migration

Prefab should eventually serialize:

- actor class name
- actor name
- root component id
- component list
- scene component attachment graph
- per-component serialized fields

Phase 1 should not rewrite prefab serialization fully. It should:

- Keep old prefab behavior working.
- Add design-compatible hooks for component class names and serialization.
- Avoid creating new prefab file format until actor/component basics are stable.

## Render Module Impact

Expected render-side changes:

- `RenderSceneCollector` moves from entity traversal to primitive iteration.
- `MeshRendererComponent` render data path migrates to `StaticMeshComponent`.
- `RenderObject::entityId` may become `ownerActorId`.
- Material system, `RenderDrawItem`, pass routing, and RHI interfaces remain unchanged.

Render-side non-changes:

- RenderGraph does not know actors.
- RHI does not know actors.
- Passes do not query components.
- GPU resource managers continue using resource ids.

## Migration Plan

### Phase 1: Introduce Core UE-Style Types

- Add `Actor`, `ActorComponent`, `SceneComponent`, `PrimitiveComponent`.
- Add compatibility relationship for `SceneEntity` and `Component`.
- Add root component support.
- Add lifecycle hooks.
- Add lightweight class metadata.
- Add factory registration shape.
- Keep all existing samples compiling and running.

### Phase 2: Move Transform and Attachment

- Move transform storage from `SceneEntity` to root `SceneComponent`.
- Forward old `SceneEntity` transform APIs to root component.
- Migrate hierarchy operations to scene component attachment.
- Add tests for parent/child world transform propagation.

### Phase 3: Migrate Renderable Components

- Introduce `StaticMeshComponent : PrimitiveComponent`.
- Keep `MeshRendererComponent` as alias or adapter during migration.
- Move render data collection into primitive components.
- Update `RenderSceneCollector` to iterate registered primitives.
- Verify ModelViewer still renders DamagedHelmet.

### Phase 4: Migrate Spatial and Picking

- Register `PrimitiveComponent` with spatial subsystem.
- Update raycast/pick result to include actor and component.
- Keep compatibility methods returning `SceneEntity*` until call sites migrate.

### Phase 5: Migrate Resource/Prefab/Model Instantiation

- Add actor-based model instantiate path.
- Migrate prefab serialization to actor/component graph.
- Preserve old entry points until samples and tests are updated.

### Phase 6: Cleanup Old Names

- Deprecate `SceneEntity` and `Component`.
- Migrate all call sites to `Actor` and `ActorComponent`.
- Remove compatibility-only transform/hierarchy storage from actor.
- Update docs and samples.

## Testing Strategy

Phase 1 validation:

- Actor can own components.
- Component lifecycle order is deterministic.
- Actor root component can be set and queried.
- SceneEntity compatibility APIs still compile and behave.
- Existing ModelViewer still builds and renders.

Phase 2 validation:

- Parent/child scene component attachment produces correct world transforms.
- Dirty propagation updates children.
- Old `SceneEntity::SetPosition()` forwards correctly.

Phase 3 validation:

- `StaticMeshComponent` produces the same `RenderObject` fields as current `MeshRendererComponent`.
- Transparent/material routing remains unchanged.
- ModelViewer renders DamagedHelmet.

Phase 4 validation:

- Spatial queries return the expected primitive and actor.
- Picking returns both actor and component.

Regression validation to keep running:

- `MaterialSystemValidation`
- `RenderPassBindingValidation`
- `RHIDX12SourceValidation`
- `ModelViewer` smoke screenshot

## Risks

### Risk: Two object models linger permanently

Mitigation:

- Document `SceneEntity` and `Component` as compatibility-only after phase 1.
- Do not add new features to compatibility classes except forwarding.
- Track cleanup as explicit phase 6 work.

### Risk: Transform duplication creates divergent state

Mitigation:

- Make root `SceneComponent` the source of truth as soon as phase 2 begins.
- Actor transform APIs must forward, not store.

### Risk: Render extraction breaks material system work

Mitigation:

- Preserve `RenderScene` snapshot format through early phases.
- Add tests comparing `MeshRendererComponent` and `StaticMeshComponent` extraction.

### Risk: Spatial subsystem is tightly coupled to `SceneEntity*`

Mitigation:

- Introduce a primitive/spatial proxy compatibility layer.
- Migrate query output gradually to actor + component hit results.

### Risk: Component lifecycle becomes hard to reason about

Mitigation:

- Keep first lifecycle implementation small.
- Add tests for order.
- Avoid editor-time construction complexity until runtime lifecycle is stable.

## Open Decisions

The following should be decided during implementation planning, not in this design:

- Whether `SceneEntity` is implemented as `class SceneEntity : public Actor` or as a type alias after phase 1.
- Whether `Component` inherits from `ActorComponent` or becomes an alias after initial migration.
- Whether primitive registration lives in `SceneManager`, `World`, or a dedicated `ComponentRegistry`.
- Whether tick registration is owned by `World` or a new `TickTaskManager`.

Recommended defaults:

- Use `class SceneEntity : public Actor` first for source compatibility.
- Use `class Component : public ActorComponent` first for source compatibility.
- Put primitive registration in `SceneManager` initially, because render extraction already enters through scene/world.
- Keep tick registration in `World` or `SceneManager` until job-system integration is designed.

## Acceptance Criteria

The design is considered implemented when:

- New runtime code can create an `Actor` with a root `SceneComponent`.
- New runtime code can create `ActorComponent`, `SceneComponent`, and `PrimitiveComponent` derived components.
- Existing `SceneEntity` creation still works during compatibility phase.
- Existing ModelViewer renders after each migration phase.
- Render extraction no longer depends on recursive `SceneEntity` traversal in the target phase.
- RHI and RenderGraph remain actor-agnostic.
- Old names are documented as compatibility-only and eventually removed.
