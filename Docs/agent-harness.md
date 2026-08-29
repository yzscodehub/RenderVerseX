# Agent harness contract

This document defines the repository-specific control plane used by coding
agents. It adapts the five harness subsystems—instructions, state,
verification, scope, and lifecycle—to RenderVerseX. The external harness
course and templates are design inputs, not repository facts; this repository
and its generated evidence remain authoritative.

## Artifact ownership

| Concern | Canonical artifact | Must not duplicate |
| --- | --- | --- |
| Agent routing and invariants | `AGENTS.md` | Build manuals or historical plans |
| Current agent work | `feature_list.json` | Product capability status |
| Work-state schema | `feature_list.schema.json` | CMake or test registration rules |
| Recent decisions/outcomes | `progress.md` | Full chat transcripts or test logs |
| Incomplete-session restart | `session-handoff.md` | Completed history |
| Cold-start orchestration | `init.ps1`, with `init.sh` as a wrapper | Build Truth validation logic |
| Product capability | `Docs/engine-capabilities.json` | Agent task status |
| Runtime verification | `Scripts/run_required_suite.ps1`, `Scripts/run_build_truth.ps1` | Hand-written pass claims |
| Architecture receipt | `.agent-harness/receipts/Architecture.<run-id>.json` | Mutable prose or ignored build output |

The state files are intentionally at the repository root so a cold agent sees
the same minimal artifacts as the reference package. Project-specific detail
lives here to keep `AGENTS.md` a map rather than an encyclopedia.

## State model

Each feature represents one bounded unit of agent work, normally small enough
to finish in one focused session or hand off at a clean checkpoint. It contains
four inseparable parts:

1. observable behavior and acceptance criteria;
2. included and excluded scope, owner, and dependencies;
3. exact verification commands and expected artifacts;
4. state, evidence, blocker, and next step.

Allowed transitions are:

```text
not_started -> active -> passing
                    |
                    v
                 blocked -> active

not_started | active | blocked -> cancelled
```

`passing` means the required commands actually returned success and their
evidence is recorded. It is deliberately not called `done`: completion is a
validator-owned outcome, not an agent's impression. The harness rejects a
`passing` feature whose required verification lacks a matching passed evidence
record.

There is at most one unresolved feature (`active` or `blocked`) per worktree.
An active feature may cross a session boundary only with a matching handoff;
blocked means there is a genuine external or technical blocker and additionally
requires the blocker plus an executable next step. Independent worktrees may
have independent work. Multiple agents sharing one worktree operate beneath
one coordinator-owned feature and must use disjoint file ownership.

The ledger is a current-work queue, not a forever-growing roadmap. Keep only
the active/blocked work and a small number of recent passing entries. Durable
product history belongs in Git, issue tracking, or an explicitly linked task
document.

## Initialization modes

`init.ps1` is a thin state machine and router. It never implements an alternate
test framework.

### Inspect

`-Mode Inspect` is the default read-only preflight. It validates required
artifacts and work-state invariants, checks PowerShell/CMake/CTest/Git, validates
the selected configure and test presets, and checks the latest harness-owned
Architecture receipt against the exact requested preset, configuration, build
tree, underlying report, JUnit outcome, commit, and product-source digest.

Its readiness states are intentionally distinct:

- `verified`: the receipt and its report match the request, current commit, and
  product-source digest;
- a `stale_*` or `context_mismatch` state: evidence exists but does not prove
  the current request or source;
- `failed`: receipt or report semantics are invalid;
- `missing`: no harness-owned Architecture receipt exists.

A successful preflight with stale evidence means the agent may investigate; it
does not authorize a claim that the current tree builds.

### Verify

`-Mode Verify` delegates to `Scripts/run_required_suite.ps1`, building
`RVXValidationArchitecture` and running `lane.architecture`. The required-suite
runner owns inventory checks, JUnit validation, source fingerprints, failure
classification, and the final JSON report. The harness then validates the full
report contract and writes an immutable receipt named by its run id plus a
`latest` pointer used only for current-readiness discovery. These compact
receipts are versionable repository control-plane evidence; the much larger
raw report and JUnit output remain under the ignored build tree.

The default uses an existing configured tree and passes `-SkipConfigure`.
`-AllowConfigure` explicitly permits CMake configuration. Neither path uses
`-Fresh`, installs dependencies, or enables network access implicitly.

### Close

`-Mode Close` validates the end-of-session transaction. With no unresolved
feature, it requires current verified product evidence and reports completion.
One `active` or `blocked` feature may instead close as an explicit handoff when
the marker names that exact feature; this succeeds as a non-complete readiness
even when the blocker is failed or unavailable verification. Both paths require
valid evidence identities plus unstaged and staged diff hygiene. Close does not
replace task-specific verification.

## Verification and observability

The harness has two observation layers:

- Process observability: the feature contract explains what is being changed,
  why it is acceptable, what is excluded, and what runs next.
- Runtime observability: Build Truth and required-suite reports expose what
  actually ran through JSON inventories, JUnit results, native lifecycle data,
  adapter identity, fingerprints, timings, and failure categories.

Feature evidence stores the exact command and immutable receipt identity—path,
SHA-256, run id, source commit, and product digest—rather than copying test
output into prose. For Architecture verification, the receipt additionally
binds the canonical required-suite report and JUnit hash. Re-running validation
creates a new receipt instead of silently changing old evidence. A typical
ladder is:

Architecture gates declare the immutable path template
`.agent-harness/receipts/Architecture.{runId}.json`; the evidence record supplies
the concrete run id and matching receipt hash. Historical passing entries
validate their own versionable receipt, while
`.agent-harness/receipts/Architecture.latest.json` is used only to decide
whether the current product source is verified. Inspect does not require the
ignored build tree to survive after the receipt has been created.

1. static syntax/schema/diff checks;
2. focused target and test;
3. Architecture required suite for policy/public-boundary changes;
4. complete Debug Build Truth lanes for cross-module or native work;
5. qualification, Shipping, TSAN, compatibility, Nightly, or soak gates when
   their product boundary is touched.

The implementation agent must not be the only evaluator for risky work. Use
deterministic tests as the primary independent judge; add a separately scoped
reviewer for architecture, security, native-backend, or broad multi-file work.

## Session protocol

At session start:

1. run `Inspect`;
2. resume the named handoff or claim one bounded feature;
3. establish a verified baseline for non-trivial implementation;
4. load only the contracts linked by the feature scope.

During work:

1. keep work in scope and WIP at one;
2. record meaningful decisions, not a command transcript;
3. serialize shared writes and isolate parallel workers;
4. treat every failed validator as feedback about what to change next.

At session end:

1. finalize product files, run required validation, and obtain its receipt;
2. record the receipt and transition to `passing`, or keep ordinary continuing
   work `active`; use `blocked` only for a truthful blocker;
3. update the bounded progress log and exact handoff marker;
4. remove session-created temporary artifacts;
5. run `Close` and report anything that remains unverified.

`feature_list.json`, `progress.md`, `session-handoff.md`, and the persistent
receipt directory form the control plane. They remain schema-, hash-, and
diff-checked as applicable, but are excluded from the product-source digest so
recording a receipt does not invalidate that same receipt. Harness
implementation, contracts, source, tests, and product docs are not excluded.

Do not add automatic loops or graph orchestration until repeatable tasks,
machine-checkable stop conditions, isolation, recovery, and review capacity
justify the coordination cost. The reliable single-work loop is the foundation.
