# Module boundary policy

`module-boundaries.json` is the policy source of truth for RenderVerseX module
ownership and allowed module edges. It uses schema
`RVX.ModuleBoundaryManifest` version 2. Every module has one path, domain,
layer, public include-prefix set, allowed-module list, shipping/deferred state,
and owner.

`owner: unassigned` means that no team ownership model has been established for
that module. It records an explicit governance gap; it must not be interpreted
as an existing review, approval, escalation, or service-ownership process.

The generated CMake inventory is the implementation source of truth. It is
written below the build tree as
`Architecture/RVXModuleInventory-<configuration>.json`, uses schema
`RVX.ModuleTargetInventory` version 1, and records concrete target aliases,
sources, link libraries, and include directories. Architecture gates must read
both files; they do not infer dependencies by parsing CMake or guessing target
names.

CMake does not expose a global enumeration of aliases. Every first-party alias
must therefore be declared with its canonical target in the same
`rvx_register_module_target(ALIASES ...)` call. Any unregistered `RVX` alias
used by the concrete link graph fails configuration; inert aliases are not
claimed to be discoverable by CMake.

`Editor` remains explicitly deferred and shipping-ineligible. Deferred modules
must be declared in the inventory's `disabledModules` with a reason; Runtime
targets may not link them. Backend module ids deliberately differ from their
public prefixes: `RHI_DX12` exposes `DX12`, `RHI_Vulkan` exposes `Vulkan`, and
so on. Prefixes are case-insensitively unique.

`RuntimeServices` is the narrow application-runtime service module. Its
canonical target is `RVX_RuntimeServices`, its public alias is
`RVX::RuntimeServices`, and its only first-party dependencies are Core and
HAL. HAL owns the backend-neutral native-surface value (platform, handles,
extent, scale, and generation). RHI owns format, present-mode, and vsync policy
through `RHISwapChainConfig`; those policy fields must not be added to the HAL
surface contract.

Developer command-line entry points use the `programs` domain and live under
`Source/Programs`. `RVXCook` and `RVXAnimationDerive` contain only their
executable entry points and depend on the `AssetPipeline` developer library.
They are disabled whenever `RVX_BUILD_PROGRAMS` is off and are never shipping
dependencies.

The physical source layout is final for this architecture release:

```text
Source/Runtime/     shipping-capable contracts, features, composition, and backends
Source/Developer/   source import, shader compilation, diagnostics, and dev wiring
Source/Programs/    developer command-line entry points
Editor/             explicitly deferred authoring product
Samples/ Tests/ Docs/ Scripts/ cmake/   repository-level products and infrastructure
```

The canonical developer diagnostics target is `RVX_DebugTools`, exposed as
`RVX::DebugTools`. The old `RVX_Debug` / `RVX::Debug` identity is forbidden.
Likewise, the retired monolith identities `RVX::Runtime`, `RVX::Resource`, and
`RVX::Tools` must not reappear.

RenderGraph is a narrow independent link target (`RVX_RenderGraph`, public
alias `RVX::RenderGraph`) registered inside the `Render` Manifest module. It
does not have a separate Manifest id, path policy, or ownership record.

`RVX::UI` is a CPU-only producer. It records layout results into
`RenderContracts/UIRenderPacket.h` using backend-neutral vertices, indices,
draw commands, generational texture handles, and an owned font-atlas payload.
It must not expose or link RHI objects. `Render/Passes/UIPass.h` is the sole
GPU consumer: Render resolves texture handles and owns buffers, textures,
descriptors, pipelines, command contexts, and render-target policy.

## Dependency-aware validation planning

`Scripts/plan_validation.py` consumes the Manifest plus the configured target
inventory and computes a conservative reverse module closure for each change.
It records recommendations for portable Unit coverage and the DX11/OpenGL
compatibility compile closures. It never removes Architecture, Integration,
DX12, Vulkan, Metal, Linux TSAN, or any platform's Shipping gate.

PR CI runs the planner in `observation` mode and still executes the complete
required matrix. Selection may only be enabled explicitly after evidence shows
at least 10 consecutive code PRs and 3 consecutive Nightly runs with zero
planner misses. The evidence document is strict and versioned; absent, stale,
or malformed evidence cannot enable reduction. An empty change set, an unknown
path, or a change to CMake, presets, CI, dependency metadata, the module
Manifest, or architecture/build policy always falls back to the full matrix.

The generated `RVX.ValidationSelectionPlan` JSON is uploaded with architecture
evidence. It records changed and affected modules, proposed platform/backend
coverage, the immutable required-job set, observation thresholds, the reason
for the enforcement decision, and a deterministic plan hash. This makes future
selection auditable without making the current PR gate dependent on the
planner's recommendation.
