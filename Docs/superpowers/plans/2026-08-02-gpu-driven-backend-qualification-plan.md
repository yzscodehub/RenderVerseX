# GPU-Driven Backend Qualification Plan

**Status:** Stage 3A-B implemented; Stage 3C external tests registered with
native execution pending; DX12 is Candidate rev2 and Auto remains quarantined
**Date:** 2026-08-02
**Scope:** Production qualification policy for DX12, Vulkan, and Metal; DX11
and OpenGL remain fallback-oriented

## Objective

GPU-driven is an engine-owned runtime policy, not a user-facing quality toggle.
`Auto` is the shipping default. It selects GPU-driven only when both conditions
are true:

1. the active device exposes the required runtime capabilities and the compute
   pipeline is ready;
2. the backend has a reviewed, versioned qualification record with every
   required production gate closed.

`ForceEnabled` and `ForceDisabled` remain development and diagnosis overrides.
Forced enable bypasses backend qualification, but never bypasses device
capabilities or pipeline readiness.

## Qualification Levels

| Level | Meaning | Auto behavior |
|---|---|---|
| `Unqualified` | No reviewed backend evidence | Direct path |
| `Candidate` | Core path works, but one or more production gates are open | Direct path |
| `Qualified` | Every required gate is recorded as passed | GPU-driven when runtime-ready |

The level is derived from the evidence masks. It is not an independently
editable boolean, so a manifest cannot claim `Qualified` while a required gate
is missing.

## Required Gates

1. `RHIContractConformance`
2. `ShaderPipelineContracts`
3. `DescriptorIntegrity`
4. `ResourceStateValidation`
5. `MultiBatchMaterialRouting`
6. `IndirectExecutionSmoke`
7. `DirectFallbackSmoke`
8. `DeterministicVisualGolden`
9. `GPUBasedValidation`
10. `RepeatedFrameResize`
11. `CrossPathImageParity`
12. `RealAssetRegression`
13. `AdapterDriverMatrix`

Capability probing is intentionally not a qualification gate: it is evaluated
for every device at runtime and can still force a qualified backend to the
direct path.

## Current Matrix

| Backend | Revision | Level | Open gates |
|---|---:|---|---|
| DX12 | 2 | Candidate | Real-asset regression, adapter/driver matrix |
| Vulkan | 0 | Unqualified | All |
| Metal | 0 | Unqualified | All |
| DX11 | 0 | Unqualified | All |
| OpenGL | 0 | Unqualified | All |

DX12 revision 2 records the completed M2 correctness evidence: shared RHI
contracts, shader/PSO contracts, descriptor integrity, resource-state checks,
multi-batch/material routing tests, indirect and fallback smokes, deterministic
golden, dedicated GPU-Based Validation, repeated-frame/resize coverage, and
zero-tolerance cross-path image parity for an identical visible set.
The R7 triangle is not treated as evidence that the full renderer is
production-ready.

## Promotion Workflow

### Stage 1: Qualification infrastructure - complete

- Add a versioned backend qualification manifest.
- Derive `Unqualified`/`Candidate`/`Qualified` from gate masks.
- Carry level, revision, passed, required, and missing masks through the runtime
  policy decision.
- Export human-readable missing gate names in tool diagnostics.
- Add unit coverage and a native DX12 Auto-policy smoke.

### Stage 2: Cross-path parity - complete

- Build a deterministic visible-set fixture where direct and GPU-driven paths
  must render the same visible geometry.
- Compare captures at a documented tolerance and emit a diff artifact.
- Keep the existing distance-culling test: it proves culling affects submitted
  work, while the new fixture proves it does not affect visible output.
- Evidence: forced GPU-driven and forced direct 320x180 captures reported zero
  different pixels, MSE 0, and PSNR 100 under the DX12 Debug Layer.

### Stage 3: Real-asset regression

Stage 3 is deliberately split so a large developer asset library does not
silently become a mandatory source checkout or a false qualification signal.

#### Stage 3A: Asset intake and capability audit - complete

- Inventory model size, dependency completeness, mesh/material/primitive
  counts, alpha modes, skin/animation use, glTF extensions, and license terms.
- Treat parse success, scene instantiation, view visibility, and submitted draws
  as separate gates. A model that logs `Loaded successfully` but contributes no
  visible object is a failure, not a successful smoke.
- Keep source assets immutable during qualification; derived/cooked artifacts
  must live outside the source asset directory.

Audit evidence from the developer asset library on 2026-08-02:

| Asset | Size | Structure | License / capability result |
|---|---:|---|---|
| DamagedHelmet | 3.6 MiB | 1 mesh, 1 material | Existing direct/GPU baseline; no adjacent license metadata, so it is not qualification evidence |
| Spartan Armour | 129.9 MiB | 10 meshes, 9 materials, 9/10 mesh nodes skinned | CC-BY-4.0 and standard metallic-roughness, but blocked on skeletal asset support for the production gate |
| 1975 Porsche 911 | 70.8 MiB | 75 meshes, 14 materials, 75 static mesh nodes | CC-BY-4.0; selected developer-library real-asset candidate; optional clearcoat/specular/transmission features degrade to the supported base PBR path |
| Cyberpunk Hovercar / Sci-fi Girl variants | 23-89 MiB | Multi-mesh/material | Require `KHR_materials_pbrSpecularGlossiness`, which is outside the current base-PBR qualification scope |

The initial DX12 runs proved DamagedHelmet submits one direct draw. Porsche and
Spartan both parsed and instantiated, but produced identical background-only
captures and zero visible/direct draws because ModelViewer used a fixed camera
independent of model bounds. This is a viewer validation defect that must be
closed before GPU-driven conclusions are drawn from these assets.

#### Stage 3B: Deterministic asset visibility - implemented

- Add bounds-driven camera fitting with finite-bounds validation, aspect-aware
  distance, fit margin, and derived near/far planes.
- Preserve existing deterministic goldens: interactive viewing defaults to
  automatic fitting, while smoke mode remains fixed unless the command line
  explicitly selects automatic fitting.
- Add a final-frame visibility assertion based on immutable Render diagnostics.
  It must fail when loading succeeded but the model contributed zero visible
  objects.
- Log model bounds, primitive count, chosen camera target/distance, and clip
  planes so failures are diagnosable without inspecting a screenshot.

#### Stage 3C: Developer-library multi-batch gate - registered, execution pending

- Register an opt-in external-asset CTest layer, disabled by default and pointed
  at a developer/CI asset root. Do not hard-code a machine-local absolute path.
- Run the Porsche candidate through forced direct and forced GPU-driven DX12
  captures with the Debug Layer enabled.
- Require multiple visible objects, more than one material-grouped indirect
  batch, more than one indirect draw, zero unexpected direct fallback, and
  direct/GPU image parity at a documented tolerance.
- Record capture hashes, adapter, driver, model content hash, and attribution in
  the generated report. External-library success improves confidence but does
  not close `RealAssetRegression` by itself.

#### Stage 3D: Hermetic qualification asset

- Add a redistributable, checked-in and attribution-complete multi-mesh,
  multi-material asset that is small enough for normal CI. Prefer an authored
  or explicitly optimized derivative rather than committing the 70-130 MiB
  source downloads unchanged.
- Exercise opaque and masked groups, textures, nested transforms, and more than
  one indirect batch. Transparent materials remain a direct/transparent-pass
  invariant and are not silently counted as GPU-driven opaque evidence.
- Run forced-on and direct captures with Debug Layer and GPU-Based Validation,
  record a stable golden/parity result, and only then mark
  `RealAssetRegression` passed in the qualification manifest.

### Stage 4: Adapter and driver matrix

- Run the bounded gate suite on at least two independent DX12 vendor families.
- Record adapter, driver, capability snapshot, validation mode, and artifact
  hashes in CI output.
- Treat a driver-specific exception as a scoped deny-list entry, never as a
  global qualification bypass.

### Stage 5: DX12 promotion

- Mark the two remaining evidence bits passed and increment the DX12
  qualification revision.
- Verify the existing Auto-policy smoke changes from direct fallback to actual
  compute culling and indirect submission without changing its command line.
- Retain forced-on/off paths for regression and diagnosis.

### Stage 6: Vulkan and Metal

- Reuse the same gates and policy contract.
- Maintain independent backend records and revisions; DX12 qualification must
  not imply Vulkan or Metal qualification.

## Stage 1-2 Validation

```powershell
cmake --build build\win_x64_debug --config Debug `
  --target GPUDrivenValidation RenderPassValidation ModelViewer -- /m:1

ctest --test-dir build\win_x64_debug -C Debug `
  -R "^(GPUDrivenValidationFixture\.|RenderPassValidationFixture\.(DepthPrepassConsumesGPUDrivenMultiMeshIndirectStreams|OpaquePassFallsBackToDirectDrawWhenGPUDrivenPipelineFails|OpaquePassConsumesGPUDrivenMaterialGroupedIndirectStreams)$)" `
  --output-on-failure

ctest --test-dir build\win_x64_debug -C Debug `
  -R "^ModelViewerGPUDrivenAutoPolicySmoke$" --output-on-failure

ctest --test-dir build\win_x64_debug -C Debug `
  -R "^(ModelViewerGPUDrivenParityGPUSmoke|ModelViewerGPUDrivenParityDirectSmoke|GPUDrivenCrossPathVisualParityValidation)$" `
  --output-on-failure

ctest --test-dir build\win_x64_debug -C Debug `
  -R "^(ModelViewerGPUDrivenSmoke|ModelViewerGPUDrivenGBVSmoke|GPUDrivenVisualGoldenValidation|ModelViewerGPUDrivenDisabledSmoke|GPUDrivenDisabledVisualDiffValidation)$" `
  --output-on-failure
```

## Promotion Rule

Never promote a backend by changing `Auto` directly. Land the missing automated
evidence first, update the reviewed manifest second, then let the unchanged
policy derive `Qualified` and select GPU-driven.
