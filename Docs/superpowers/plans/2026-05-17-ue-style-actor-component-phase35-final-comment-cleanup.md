# UE Style Actor Component Phase 35 Final Comment Cleanup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the last stale prefab/component test comment left behind by the UE-style Actor/Component migration.

**Architecture:** This is a narrow consistency pass. Runtime code is already covered by previous phases, so this phase only updates a misleading test comment that still says named single-property targets are deferred even though named payload and binding behavior is now implemented and validated.

**Tech Stack:** C++20, standalone validation executables, CMake Debug build.

---

## Files

- Modify: `Tests/ResourceInstantiationValidation/main.cpp`

## Plan Review

- Spec coverage: The current user request is to continue until the framework conversion is complete. A scan found no remaining Critical/Important runtime gaps in the UE-style Actor/Component path; this plan addresses the only stale local statement found during the final audit.
- Placeholder scan: This plan has exact files, edits, and validation commands.
- Risk check: Do not touch runtime component dispatch, prefab binding, or scene lifecycle code in this phase.

---

### Task 1: Update Stale Prefab Named-Target Comment

**Files:**
- Modify: `Tests/ResourceInstantiationValidation/main.cpp`

- [ ] **Step 1: Replace the stale comment**

Change the comment inside `Test_PrefabInstanceRevertAllRestoresNamedDuplicateActorComponentPayloads()` from:

```cpp
// Named single-property targets are intentionally deferred; this phase
// only guarantees name-aware full payload restoration through RevertAll.
```

to:

```cpp
// RevertAll must restore each named duplicate through runtime name binding
// even when multiple components share the same class.
```

- [ ] **Step 2: Run targeted validation**

Run:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation
build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
```

Expected: build exit code `0`; validation reports all tests passed.

- [ ] **Step 3: Run whitespace check**

Run:

```powershell
git diff --check
```

Expected: exit code `0`, allowing existing line-ending warnings only if Git reports them without failing.

- [ ] **Step 4: Request one code review**

Ask one review agent to compare this phase against the plan and flag Critical/Important issues only.

- [ ] **Step 5: Commit**

Run:

```powershell
git add Docs/superpowers/plans/2026-05-17-ue-style-actor-component-phase35-final-comment-cleanup.md Tests/ResourceInstantiationValidation/main.cpp
git commit -m "Clean up prefab named target comment"
```
