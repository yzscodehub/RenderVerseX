# UE-Style Actor Component Phase 8 Spawn API Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add UE-style `SpawnActor`/`DestroyActor` entry points on `SceneManager` and `World`, while preserving the existing `SceneEntity` storage model and legacy `CreateEntity` API.

**Architecture:** This phase formalizes the current compatibility bridge: `SceneEntity` is the engine's spatial `Actor` subtype and remains the only actor type owned by `SceneManager`. `SpawnActor<T>()` is therefore constrained to `SceneEntity`-derived actor classes. Non-spatial pure `Actor` ownership is intentionally deferred to a later phase because it requires a separate actor store, lifetime rules, and query semantics.

**Tech Stack:** C++20, RenderVerseX Scene and World modules, custom validation executables, CMake.

## Compatibility Decisions

- Keep `SceneManager::CreateEntity()` and `CreateEntity<T>()` unchanged as legacy-compatible APIs.
- Add `SpawnActor` as the preferred UE-style API for new code.
- `SpawnActor<T>()` only supports `T : SceneEntity` in this phase.
- `ActorSpawnParams` includes name, local transform, and optional parent. When a parent is provided, transform values are local relative to that parent. When no parent is provided, local transform is also world transform.
- Spawn validates `parent` before construction. A parent must already be owned by the same `SceneManager`; cross-world or stack-only parent pointers reject the spawn and return null.
- The spawned actor is added to `SceneManager` ownership before parent/local transform setup so component auto-registration behavior remains consistent with existing `CreateEntity`.
- Parent attachment is applied before local transform values. This keeps `localPosition/localRotation/localScale` unambiguously relative to the parent and avoids world/local drift when the parent has a non-identity transform.
- `DestroyActor(Actor*)` only succeeds for `SceneEntity`-derived actors currently owned by the scene.
- `World::SpawnActor<T>()` and `World::DestroyActor()` are thin convenience wrappers over `SceneManager`.
- Existing resource/model/prefab instantiation can keep using `CreateEntity` in this phase; migration can be a later low-risk cleanup after the API is validated.

---

## File Structure

- Modify `Scene/Include/Scene/SceneManager.h`
  - Add `ActorSpawnParams`.
  - Add `SpawnActor<T>()`, `SpawnActor(const ActorSpawnParams&)`, and `DestroyActor(Actor*)`.

- Modify `Scene/Private/SceneManager.cpp`
  - Implement non-template `SpawnActor` overload and `DestroyActor`.

- Modify `World/Include/World/World.h`
  - Add `SpawnActor<T>()`, `SpawnActor(const ActorSpawnParams&)`, and `DestroyActor(Actor*)` wrappers.

- Modify `World/Private/World.cpp`
  - Implement non-template `World` spawn/destroy wrappers.

- Modify `Tests/ActorComponentValidation/main.cpp`
  - Add validation coverage for SceneManager and World spawn/destroy behavior.

- Modify `Tests/CMakeLists.txt`
  - Link `ActorComponentValidation` with `RVX::World` because this phase adds direct `World` API tests.

---

## Task 1: Add Failing Spawn API Tests

**Files:**
- Modify: `Tests/ActorComponentValidation/main.cpp`

- [x] Add a custom `SpawnableSceneActor : public RVX::SceneEntity` helper that can be created from a name.
- [x] Add `Test_SceneManagerSpawnActorCreatesSceneOwnedActor`:
  - `SceneManager::SpawnActor<SpawnableSceneActor>({ .name = "Spawned", ... })`
  - Assert returned pointer is non-null, `GetEntity(handle)` returns the same object, name and local/world transform match params, and entity count is 1.
- [x] Add `Test_SceneManagerSpawnActorAttachesParent`:
  - Create a parent actor with non-zero position, rotation, and scale, spawn child with parent in `ActorSpawnParams`, assert parent/child hierarchy is established.
  - Assert spawn params are interpreted as local transform: child local transform matches params and child world transform matches parent world transform applied to the local transform.
- [x] Add `Test_SceneManagerSpawnActorRejectsForeignParent`:
  - Create one `SceneManager` with a parent actor and another `SceneManager` for spawning.
  - Spawn with the foreign parent, assert null and no entity count change.
  - Also pass a stack-only `SceneEntity` as parent and assert null.
- [x] Add `Test_SceneManagerDestroyActorUsesActorPointer`:
  - Spawn actor, destroy via `DestroyActor(static_cast<Actor*>(actor))`, assert entity count becomes 0 and lookup by handle is null.
- [x] Add `Test_SceneManagerDestroyActorDefersDuringLifecycleDispatch`:
  - Spawn an actor with a component that calls `DestroyActor(owner)` during tick.
  - Run `SceneManager::Update()`, assert `DestroyActor` returned true from inside the callback and entity count is 0 after pending destroy flush.
- [x] Add `Test_SceneManagerDestroyActorRejectsDuplicatePendingDestroy`:
  - During lifecycle dispatch call `DestroyActor(owner)` twice.
  - Assert the first call returns true, the second returns false if the handle is already pending, and the actor is destroyed once after update.
- [x] Add `Test_WorldSpawnActorDelegatesToSceneManager`:
  - Initialize `World`, spawn actor via world wrapper, assert it is present in the world's scene manager, then destroy through world wrapper.
- [x] Add `Test_WorldSpawnActorRejectsWhenUninitialized`:
  - Use an uninitialized `World`, assert `SpawnActor` returns null and `DestroyActor` returns false.
- [x] Add `Test_DestroyActorRejectsForeignOrPureActor`:
  - Call `DestroyActor(nullptr)`, a stack `Actor`, and a stack `SceneEntity` not owned by the scene; all should return false and not mutate scene ownership.
- [x] Register the new tests in `main()`.
- [x] Run `ActorComponentValidation` and confirm the new tests fail to compile before API implementation.

---

## Task 2: Add SceneManager Spawn/Destroy API

**Files:**
- Modify: `Scene/Include/Scene/SceneManager.h`
- Modify: `Scene/Private/SceneManager.cpp`

- [x] Add `ActorSpawnParams` near the entity-management public API:

```cpp
struct ActorSpawnParams
{
    std::string name = "Actor";
    Vec3 localPosition{0.0f};
    Quat localRotation{};
    Vec3 localScale{1.0f};
    SceneEntity* parent = nullptr;
};
```

- [x] Add template `SpawnActor<T = SceneEntity>(const ActorSpawnParams& params = {})`.
- [x] Constrain `SpawnActor<T>()` with `static_assert(std::is_base_of_v<SceneEntity, T>)`.
- [x] Construct with `params.name`, add via `AddEntity`, then apply parent and transform.
- [x] Before construction, reject `params.parent` when `GetEntity(params.parent->GetHandle()) != params.parent`.
- [x] Construct with `params.name`, add via `AddEntity`, then attach parent first, then apply local transform.
- [x] Add non-template `SceneEntity* SpawnActor(const ActorSpawnParams& params = {})` forwarding to `SpawnActor<SceneEntity>()`.
- [x] Add `bool DestroyActor(Actor* actor)`:
  - Reject null.
  - `dynamic_cast<SceneEntity*>`.
  - Reject non-`SceneEntity` actors.
  - Verify the scene owns the handle before calling `DestroyEntity(handle)`.
  - Return false if the actor is already pending destroy.
  - Return true only when a destroy request is accepted.
- [x] Add a private `bool IsDestroyPending(SceneEntity::Handle handle) const` helper so `DestroyActor` and lifecycle update share the same pending-destroy semantics without duplicating scans.

---

## Task 3: Add World Spawn/Destroy Wrappers

**Files:**
- Modify: `World/Include/World/World.h`
- Modify: `World/Private/World.cpp`
- Modify: `Tests/CMakeLists.txt`

- [x] Include the minimum Scene headers needed for template wrappers.
- [x] Add `template<typename T = SceneEntity> T* SpawnActor(const ActorSpawnParams& params = {})`.
- [x] Add `SceneEntity* SpawnActor(const ActorSpawnParams& params = {})`.
- [x] Add `bool DestroyActor(Actor* actor)`.
- [x] Return null/false when `World` is not initialized or `m_sceneManager` is null.
- [x] Add `RVX::World` to the `ActorComponentValidation` target link libraries.

---

## Task 4: Verify Phase 8

**Files:**
- No source edits unless validation exposes issues.

- [x] Build:
  - `cmake --build build\Debug --config Debug --target ActorComponentValidation SpatialComponentValidation ResourceInstantiationValidation RenderSceneValidation SystemIntegrationTest ModelViewer`
- [x] Run:
  - `.\build\Debug\Tests\Debug\ActorComponentValidation.exe`
  - `.\build\Debug\Tests\Debug\SpatialComponentValidation.exe`
  - `.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe`
  - `.\build\Debug\Tests\Debug\RenderSceneValidation.exe`
  - `.\build\Debug\Tests\Debug\SystemIntegrationTest.exe`
- [x] Run ModelViewer smoke with `C:\Users\yinzs\Desktop\DamagedHelmet.glb`; confirm model loaded, instantiated, uploaded to GPU, RenderGraph stats emitted, stderr empty, and no error/critical/assert.

---

## Review Checkpoints

- [x] Before implementation, run one GPT-5.3-Codex-Spark review of this plan. Fix any Critical/Important plan issues before coding.
- [x] After implementation and validation, run one GPT-5.3-Codex-Spark code review. Fix any Critical/Important findings before committing.
