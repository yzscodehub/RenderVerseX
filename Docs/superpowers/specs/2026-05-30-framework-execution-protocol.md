# RenderVerseX Framework Execution Protocol

**Date:** 2026-05-30
**Scope:** UE5-style framework remediation and all dependent engine optimization phases.
**Status:** Mandatory execution protocol.

---

## 1. Purpose

This protocol prevents implementation drift during the UE5-style framework remediation work.
Every phase must start from the approved roadmap/spec documents, receive a Spark subagent plan review before code changes, receive a Spark subagent code review after implementation, and be committed before the next phase begins.

The goal is simple:

> Each phase does exactly the work approved for that phase, no less and no more.

---

## 2. Mandatory Phase Gate

Every phase uses this sequence:

```text
1. Re-read the roadmap/spec section for the current phase.
2. Write a phase implementation plan.
3. Ask Spark subagent to review the plan.
4. Revise the plan until Spark review has no blocking issues.
5. Implement only the approved phase scope.
6. Run the phase-specific tests and required smoke checks.
7. Ask Spark subagent to review the code implementation.
8. Fix all blocking review findings.
9. Commit the completed phase.
10. Move to the next phase only after the commit succeeds.
```

If any gate fails, the phase does not advance.

---

## 3. Required Phase Plan Format

Before implementation, each phase plan must include:

- **Source of truth:** The exact roadmap/spec sections being executed.
- **Goal:** What the phase must accomplish.
- **Non-goals:** Work explicitly out of scope for this phase.
- **Dependencies:** Prior phases, required contracts, and known blockers.
- **Files/modules touched:** Expected edit surface.
- **Implementation steps:** Ordered tasks with small reviewable boundaries.
- **Old-path handling:** Whether legacy paths are deleted, forwarded, disabled, or left untouched.
- **Tests:** New and existing validation commands.
- **Smoke checks:** Sample or runtime checks required for the phase.
- **Acceptance criteria:** Objective conditions for completion.
- **Rollback risk:** What can break and how to isolate it.
- **Spark plan review result:** Blocking findings and resolution notes.

No phase may begin code changes from memory alone.

---

## 4. Spark Review Requirements

Spark is used as an independent subagent reviewer at two points.

### Plan Review

Spark must check:

- The plan matches the approved roadmap/spec scope.
- No P2 or unrelated work is smuggled into P0/P1 phases.
- Dependencies and ordering are valid.
- Tests and smoke checks are sufficient for the risk.
- Legacy compatibility handling is explicit.
- Acceptance criteria are measurable.

### Code Review

Spark must check:

- The implementation matches the approved phase plan.
- No unrelated refactors or scope creep were introduced.
- Old paths were handled as planned.
- Tests cover the changed behavior.
- Runtime false-success paths were not introduced.
- The code follows RenderVerseX style and ownership rules.

Blocking Spark findings must be fixed before commit.

---

## 5. Commit Rule

Each phase ends in one or more focused commits before the next phase starts.

Commit messages should identify the phase, for example:

```text
phase-minus1: make runtime placeholder paths honest
phase0-b1: define RHI submit fence contract
phase1-object: add thin object class registry
```

Do not mix phases in one commit unless the approved phase plan explicitly says the work is inseparable.

---

## 6. Verification Rule

Every phase must run its own targeted validation.

Additionally:

- ECS/render extraction/asset/material/render-proxy phases must run a `ModelViewer` smoke check when the local environment supports it.
- Full `ModelViewer` validation is required after all phases complete.
- Rendering build-out phases must not proceed without at least a screenshot or golden-image smoke gate.
- If a smoke check cannot run in the current environment, the reason must be recorded in the phase completion notes.

---

## 7. Drift Prevention Checklist

Before starting implementation, answer these questions in the phase plan:

- Which exact phase am I executing?
- Which approved document section defines this work?
- What is explicitly out of scope?
- What old path will this phase remove, forward, or disable?
- What test will fail before the fix or prove the new behavior?
- What Spark plan-review findings were resolved?

Before committing, answer these questions in the completion notes:

- Did the implementation stay inside the approved scope?
- Which tests and smoke checks passed?
- What Spark code-review findings were resolved?
- Are there any remaining risks or deferred items?

---

## 8. Relationship To Roadmap

This protocol is binding for:

- `2026-05-30-engine-program-plan-v2.md` (authoritative program plan)
- `2026-05-30-ue5-style-engine-framework-design.md` (target architecture)
- All phase implementation plans derived from those documents.

`2026-05-30-engine-full-optimization-master-plan.md` is superseded by v2 and is not binding.
`2026-05-30-ecs-ue-migration-completion-design.md` is demoted to a stepping-stone folded into SP1, not a binding target.

If a roadmap/spec and this protocol conflict, stop and update the documents before implementation.

