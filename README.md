# RenderVerseX

RenderVerseX is a runtime-first cross-platform game-engine foundation written
in C++20. It provides an explicit Runtime/Developer/Programs separation, a
multi-backend RHI, ECS-owned World execution, rendering, cooked assets, and
fail-closed validation. It is a game-engine foundation under active
development, not a claim that every product-level game feature is complete.

The machine-readable capability contract is
[`Docs/engine-capabilities.json`](Docs/engine-capabilities.json). It classifies
features as `integrated`, `partial`, `experimental`, or `deferred` and links
each non-deferred claim to repository evidence. The documentation classes are
explained in [`Docs/README.md`](Docs/README.md).

## Implemented architecture

- Runtime modules contain Shipping-eligible engine code and platform/RHI
  backends.
- Developer modules own source asset import, shader compilation, diagnostics,
  and development composition.
- Programs contain developer command-line entry points such as `RVXCook`.
- Engine composes the Runtime graph through explicit move-only dependencies;
  Shipping has no importer or source-shader fallback.
- Resource loading is instance-owned and follows
  `Supports -> Prepare -> Validate -> Publish`.
- RenderGraph is the independent `RVX::RenderGraph` link target inside the
  `Render` Manifest module.
- Model products use only `RVX_MODEL_ARTIFACT_V2`; source FBX support is a
  development-only, feature-gated static subset.

Core/HAL/RHI/Render, Scene/ECS/World, Animation, Built-in Physics, Particle,
Terrain, Water, and cooked Shader/Resource paths are integrated. Audio,
Scripting, and UI are partial. AI, Networking, and the Jolt ECS path are
experimental and are not part of the Engine composition. Editor, PIE,
LocalPlayer, Replay, and Dedicated Server are deferred.

## Requirements

- CMake 3.25 or newer.
- A C++20 compiler: Visual Studio 2022, a current GCC/Clang toolchain, or Xcode.
- vcpkg with `VCPKG_ROOT` set; dependencies are resolved from `vcpkg.json`.
- Windows SDK for DX11/DX12, Vulkan tooling for Vulkan builds, and Xcode for
  Metal builds.
- PowerShell 7 (`pwsh`) for the cross-platform validation runners.

Example environment setup:

```powershell
$env:VCPKG_ROOT = "E:/WorkSpace/vcpkg"
```

```bash
export VCPKG_ROOT=/path/to/vcpkg
```

Do not maintain a separate manual dependency-install list. The vcpkg Manifest,
its feature selection, and the checked-in overlay ports are the dependency
contract.

## Configure and build

Use the checked-in Presets. Common developer configurations are:

```powershell
cmake --preset win_x64_debug
cmake --build --preset win_x64_debug --target RVXValidationAll
```

```bash
cmake --preset linux_x64_debug
cmake --build --preset linux_x64_debug --target RVXValidationAll
```

```bash
cmake --preset mac_arm64_debug
cmake --build --preset mac_arm64_debug --target RVXValidationAll
```

Release, PR smoke, qualification, Nightly compatibility, Weekly soak, TSAN,
Shipping host, and Shipping product Presets are declared in
`CMakePresets.json`. `Docs/engine-capabilities.json` mirrors every public
configure and test Preset; the architecture gate rejects drift.

The principal component options are:

| Option | Purpose |
| --- | --- |
| `RVX_ENABLE_DX11`, `RVX_ENABLE_DX12`, `RVX_ENABLE_VULKAN`, `RVX_ENABLE_METAL`, `RVX_ENABLE_OPENGL` | Select RHI backends supported by the host platform. |
| `RVX_BUILD_DEVELOPER` | Build source import, shader compilation, diagnostics, and development composition. |
| `RVX_BUILD_PROGRAMS` | Build developer command-line programs. |
| `RVX_ENABLE_FBX` | Enable verified development-only static FBX import through the vcpkg feature. |
| `RVX_BUILD_SAMPLES` | Build Runtime proof applications. |
| `RVX_BUILD_TESTS` | Build validation targets. |
| `RVX_BUILD_SHIPPING_PRODUCTS`, `RVX_BUILD_SHIPPING_TESTS` | Build the cooked-only product and its required validation. |
| `RVX_BUILD_EDITOR` | Deferred; Runtime Presets keep this disabled. |

Developer base Presets enable the `fbx` vcpkg feature. Shipping host and target
Presets explicitly disable it, so ufbx and AssetImport cannot enter the
Shipping closure.

## Validation

GoogleTest cases and direct script/product checks are registered through
CTest. Every test has exactly one primary lane label:

```text
lane.architecture
lane.unit
lane.integration
lane.native
lane.qualification
lane.shipping
```

For a complete local validation, use Build Truth v3. It configures, builds the
aggregate target, inventories CTest, runs required lanes with independent
JUnit files, validates semantic evidence, and rejects source changes during
the run:

```powershell
Scripts\run_build_truth.ps1 `
  -ConfigurePreset win_x64_debug `
  -BuildPreset win_x64_debug `
  -LaneTestPresets "win_x64_debug_architecture,win_x64_debug_unit,win_x64_debug_integration,win_x64_debug_native" `
  -BuildDir build\win_x64_debug `
  -Configuration Debug `
  -RequiredNativeBackend DX12 `
  -Fresh
```

For one required lane or suite, use the thin fail-closed runner over the same
Build Truth inventory and evidence implementation:

```powershell
Scripts\run_required_suite.ps1 `
  -ConfigurePreset win_x64_pr_debug `
  -BuildPreset win_x64_pr_debug `
  -TestPreset win_x64_pr_debug_smoke `
  -BuildDir build\win_x64_pr_debug `
  -Configuration Debug `
  -BuildTarget RVXValidationSuitePrSmoke `
  -RequiredLabel suite.pr-smoke `
  -ExpectedTestCount 3 `
  -Fresh
```

The fixed PR smoke is SceneLifecycle, Direct rendering, 320x180, Low quality,
eight frames, with image and JSON evidence. See
[`Docs/build-truth.md`](Docs/build-truth.md) and
[`Docs/shipping-validation.md`](Docs/shipping-validation.md) for the complete
contracts.

## Asset workflow

`RVXCook` is built only in a Developer/Programs host-tools configuration.
Runtime and Shipping read cooked or package artifacts through strict loaders.
Model cooking converges glTF/GLB and feature-enabled static FBX into the same
Model Artifact V2 tail. FBX deliberately rejects skin, animation, blend shape,
embedded texture, missing external texture, and source-root escape cases.

See [`Docs/rvxcook-workflow.md`](Docs/rvxcook-workflow.md) for commands,
formats, and deterministic cook/load gates.

## Repository layout

```text
Source/
  Runtime/       Shipping-eligible engine modules and RHI backends
  Developer/     Source import, compilation, diagnostics, dev composition
  Programs/      RVXCook and RVXAnimationDerive entry points
Editor/          Explicitly deferred authoring product
Samples/         Runtime proof applications
Tests/           CTest/GoogleTest and Shipping validation
Docs/            Current contracts, proposals, and historical evidence
Scripts/         Build Truth, required-suite, architecture, and product gates
cmake/           Module inventory and validation registration helpers
```

Public includes use logical module prefixes such as `Core/...`, `RHI/...`, and
`Render/...`; physical `Source/Runtime` or `Source/Developer` paths never form
part of the public include contract. The module policy is documented in
[`Docs/module-boundaries.md`](Docs/module-boundaries.md).

## Sample applications

`RenderVerseSamples` is the current Runtime product proof surface while Editor
is deferred. The sample registry includes model rendering/viewing, PBR
materials, GPU-driven rendering, lighting/shadows, asset gallery, render
pipeline, scene lifecycle, asset streaming, interior rendering, physics,
animation, and rendering stress scenarios. Samples use the same pure-ECS World,
resource publication, render extraction, diagnostics, and cleanup contracts as
the validated Runtime composition.

## Contributing

Keep Runtime independent of Developer, Programs, Editor, Tests, and Samples.
Do not add source-asset or source-shader fallbacks to Shipping. Register tests
through the validation helpers, assign exactly one primary lane, and update the
machine capability contract whenever a capability, public Preset, artifact
format, or required evidence changes.
