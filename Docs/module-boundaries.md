# Module boundary policy

`module-boundaries.json` is the policy source of truth for RenderVerseX module
ownership and allowed module edges. It uses schema
`RVX.ModuleBoundaryManifest` version 2. Every module has one path, domain,
layer, public include-prefix set, allowed-module list, shipping/deferred state,
and owner.

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
