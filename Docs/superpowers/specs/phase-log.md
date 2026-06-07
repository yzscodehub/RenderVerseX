# RenderVerseX Phase Log

This log records execution of `2026-05-30-render-program-plan-v1.md`.

Every R-SP or sub-stage must append one entry before commit.

---

### R-SP: R0 Documentation and Scope Lock

**Date:** 2026-05-31
**Commit:** `4a50af8`
**Spark plan review agent:** `019e7994-2192-7940-8c23-c7b37ba169ac`
**Spark code review agent:** N/A - documentation plan review only

**Plan source:**

- Document: `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`
- Section: `4. R0 - Documentation and Scope Lock`
- Lines checked: R0 section and execution rules checked before this entry

**Prerequisite status:** PASS

- Previous R-SP: N/A
- Evidence: R0 is the first render-first program stage

**Approved scope:**

- Add the render-first authoritative roadmap.
- Add a durable phase-log template.
- Record strict serial execution, Spark review gates, and final ModelViewer validation.
- Keep full-engine roadmap as reference-only for current render work.

**Out of scope:**

- Code implementation.
- Build/test target changes.
- RenderGraph, RHI, Material, Asset, or SceneRenderer fixes.

**Files changed:**

- `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`
- `Docs/superpowers/specs/phase-log.md`

**Validation commands:**

```powershell
rg -n "R-HS|R7 - Visual Gate|R8 - RenderProxy|R12 - Final ModelViewer|strict serial|Spark" Docs\superpowers\specs\2026-05-30-render-program-plan-v1.md
rg -n "Prerequisite status|Visual gate: PASS" Docs\superpowers\specs\phase-log.md
```

**Validation result:**

- Build: N/A - documentation only
- Tests: N/A - documentation only
- Visual gate: N/A

**Artifacts:**

- Logs: terminal `rg` output confirmed key gates and phase-log fields
- Screenshots: N/A
- Diffs: working tree docs

**Spark plan review result:**

- Verdict: PASS - `2026-05-30-render-program-plan-v1.md` and `phase-log.md` can serve as the R0 documentation plan.
- Blockers resolved: Added "create/ensure tests exist" wording, prerequisite status, and enumerated visual gate status.

**Spark code review result:**

- Verdict: N/A - R0 produced documents only and received Spark document/plan review.
- Blockers resolved: N/A

**Notes / follow-ups:**

- Next stage is `R-HS - Render Honesty Sprint`.
- R-HS must create its own implementation plan and pass Spark plan review before code changes.

---

### R-SP: R-HS Render Honesty Sprint

**Date:** 2026-05-31
**Commit:** `b460f9e`
**Spark plan review agent:** `019e79aa-9968-7011-ad6c-3a8e719b884e`
**Spark code review agent:** `019e7bf0-4b96-78b2-92e2-25469de9e566`

**Plan source:**

- Document: `Docs/superpowers/specs/2026-05-31-r-hs-render-honesty-sprint-plan.md`
- Source roadmap: `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`
- Section: `5. R-HS - Render Honesty Sprint`
- Lines checked: R-HS plan sections 1-7 checked before implementation; sections 6-7 checked again before review/commit

**Prerequisite status:** PASS

- Previous R-SP: R0 Documentation and Scope Lock
- Evidence: R0 committed as `4a50af8`; R-HS implementation plan passed Spark plan review before code changes

**Approved scope:**

- Add `RenderHonestyValidation` and make render-facing false-success paths test-visible.
- Make invalid JSON parsing, placeholder importers, asset database persistence/load, material compile/bind, profiler timestamp support, and texture fallback states honest.
- Make RenderGraph invalid compile/execution-order, async fallback, and aliasing unsupported states visible.
- Expose backend query capability honesty, especially Vulkan query/timestamp unsupported paths.
- Mark disconnected post-process, sky/atmosphere, and particle simulation/rendering paths as unsupported/disabled instead of successful.

**Out of scope:**

- Full RenderGraph async/copy scheduling or aliasing implementation.
- Vulkan query/timestamp implementation.
- Production material pipelines.
- Full post-process, sky/atmosphere, or particle implementation.
- RenderProxy work and ModelViewer visual validation.

**Files changed:**

- `Docs/superpowers/specs/2026-05-31-r-hs-render-honesty-sprint-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `Core/Include/Core/Serialization/Serialization.h`
- `Core/Private/Serialization/Serialization.cpp`
- `Tools/Private/AssetDatabase.cpp`
- `Tools/Private/AssetPipeline.cpp`
- `Resource/Include/Resource/Loader/TextureLoader.h`
- `Resource/Private/Loader/TextureLoader.cpp`
- `Render/Include/Render/Debug/GPUProfiler.h`
- `Render/Private/Debug/GPUProfiler.cpp`
- `Render/Include/Render/Graph/RenderGraph.h`
- `Render/Private/Graph/RenderGraph.cpp`
- `Render/Private/Graph/RenderGraphCompiler.cpp`
- `Render/Private/Graph/RenderGraphExecutor.cpp`
- `Render/Private/Graph/RenderGraphInternal.h`
- `Render/Include/Render/Material/MaterialBinder.h`
- `Render/Private/Material/MaterialBinder.cpp`
- `Render/Include/Render/Material/MaterialTemplate.h`
- `Render/Private/Material/MaterialTemplate.cpp`
- `RHI/Include/RHI/RHICapabilities.h`
- `RHI_DX11/Private/DX11Device.cpp`
- `RHI_DX12/Private/DX12Device.cpp`
- `RHI_OpenGL/Private/OpenGLDevice.cpp`
- `RHI_Vulkan/Private/VulkanCommandContext.cpp`
- `RHI_Vulkan/Private/VulkanDevice.cpp`
- `Render/Include/Render/PostProcess/PostProcessStack.h`
- `Render/Private/PostProcess/PostProcessStack.cpp`
- `Render/Include/Render/PostProcess/TAA.h`
- `Render/Private/PostProcess/TAA.cpp`
- `Render/Include/Render/PostProcess/SSAO.h`
- `Render/Private/PostProcess/SSAO.cpp`
- `Render/Include/Render/PostProcess/SSR.h`
- `Render/Private/PostProcess/SSR.cpp`
- `Render/Private/PostProcess/ToneMapping.cpp`
- `Render/Private/PostProcess/Bloom.cpp`
- `Render/Private/PostProcess/FXAA.cpp`
- `Render/Private/PostProcess/ColorGrading.cpp`
- `Render/Private/PostProcess/Vignette.cpp`
- `Render/Private/PostProcess/ChromaticAberration.cpp`
- `Render/Private/PostProcess/FilmGrain.cpp`
- `Render/Private/PostProcess/MotionBlur.cpp`
- `Render/Private/PostProcess/DOF.cpp`
- `Render/Private/PostProcess/VolumetricLighting.cpp`
- `Render/Include/Render/Passes/SkyboxPass.h`
- `Render/Private/Passes/SkyboxPass.cpp`
- `Render/Include/Render/Sky/AtmosphericScattering.h`
- `Render/Private/Sky/AtmosphericScattering.cpp`
- `Particle/Include/Particle/ParticleSystemInstance.h`
- `Particle/Private/ParticleSystemInstance.cpp`
- `Particle/Private/ParticleSubsystem.cpp`
- `Particle/Include/Particle/Rendering/ParticleRenderer.h`
- `Particle/Private/Rendering/ParticleRenderer.cpp`
- `Particle/Include/Particle/Rendering/ParticlePass.h`
- `Particle/Private/Rendering/ParticlePass.cpp`
- `Tests/CMakeLists.txt`
- `Tests/RenderHonestyValidation/main.cpp`
- `Tests/RenderGraphValidation/main.cpp`
- `Tests/VulkanValidation/main.cpp`

**Validation commands:**

```powershell
cmake --build build/win_x64_debug --config Debug --target RenderHonestyValidation
build\win_x64_debug\Tests\Debug\RenderHonestyValidation.exe
cmake --build build/win_x64_debug --config Debug --target RenderGraphValidation
build\win_x64_debug\Tests\Debug\RenderGraphValidation.exe
cmake --build build/win_x64_debug --config Debug --target MaterialSystemValidation
build\win_x64_debug\Tests\Debug\MaterialSystemValidation.exe
cmake --build build/win_x64_debug --config Debug --target VulkanValidation
build\win_x64_debug\Tests\Debug\VulkanValidation.exe
cmake --build build/win_x64_debug --config Debug --target DX12Validation
build\win_x64_debug\Tests\Debug\DX12Validation.exe
cmake --build build/win_x64_debug --config Debug --target DX11Validation
build\win_x64_debug\Tests\Debug\DX11Validation.exe
cmake --build build/win_x64_debug --config Debug --target GPUUploadServiceValidation
build\win_x64_debug\Tests\Debug\GPUUploadServiceValidation.exe
cmake --build build/win_x64_debug --config Debug --target GPUResourceManagerValidation
build\win_x64_debug\Tests\Debug\GPUResourceManagerValidation.exe
cmake --build build/win_x64_debug --config Debug --target ResourceInstantiationValidation
build\win_x64_debug\Tests\Debug\ResourceInstantiationValidation.exe
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "RenderHonestyValidation|RenderGraphValidation|MaterialSystemValidation|GPUUploadServiceValidation|GPUResourceManagerValidation|ResourceInstantiationValidation|VulkanValidation|DX12Validation|DX11Validation"
```

**Validation result:**

- Build: PASS
- Tests: PASS
  - `RenderHonestyValidation`: 14/14
  - `RenderGraphValidation`: 21/21
  - `MaterialSystemValidation`: 1/1
  - `VulkanValidation`: 17/17
  - `DX12Validation`: 14/14
  - `DX11Validation`: 12/12
  - `GPUUploadServiceValidation`: 7/7
  - `GPUResourceManagerValidation`: 21/21
  - `ResourceInstantiationValidation`: 76/76
  - Filtered CTest: 183/183
- Visual gate: N/A

**Artifacts:**

- Logs: terminal build/test output; Spark plan and code review messages
- Screenshots: N/A
- Diffs: R-HS working tree diff before commit

**Spark plan review result:**

- Verdict: PASS on second review.
- Blockers resolved: first review flagged missing AssetDatabase coverage and oversized/broad pass scope; R-HS plan was narrowed to explicit files/tasks and passed review.

**Spark code review result:**

- Verdict: PASS after blocker fixes.
- Blockers resolved: fixed TextureLoader cache hit status reporting; replaced JsonArchive heuristic validation with recursive JSON syntax validation; added regression coverage for both.

**Notes / follow-ups:**

- Vulkan validation still emits pre-existing backend validation-layer messages in some tests, but the validation targets pass.
- Old untracked framework/spec documents and `vulkan_pipeline_cache.bin` are intentionally excluded from the R-HS commit.
- Next stage must reread the render-first plan and create its own implementation plan before code changes.

---

### R-SP: R1a RHI Submit/Fence Contract

**Date:** 2026-05-31
**Commit:** `4a441d1`
**Spark plan review agent:** `019e7c3c-06a7-71c0-8a0d-b662be0ec7cc`
**Spark code review agent:** `019e7cae-8987-73a3-ba9f-0afd6cbd2072`

**Plan source:**

- Document: `Docs/superpowers/specs/2026-05-31-r1a-rhi-submit-fence-contract-plan.md`
- Source roadmap: `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`
- Section: `6. R1 - RHI Core Contract`
- Lines checked: R1 section and R1a plan sections 1-8 checked before implementation; plan rechecked before review/commit

**Prerequisite status:** PASS

- Previous R-SP: R-HS Render Honesty Sprint
- Evidence: R-HS committed as `b460f9e`; R1a implementation plan passed Spark plan review before code changes

**Approved scope:**

- Define the RHI submit return contract: submitted fence value on queued work, `0` for no fence or failed/no-work submission.
- Add explicit synchronization capability bits for host fence signal, default queue signal, explicit queue signal, queue wait, mixed queue batch submit, and emulated fences.
- Update DX12, Vulkan, DX11, OpenGL, and Metal submit/fence implementations to report honest capabilities and return monotonic submitted fence values.
- Move command-context fence operations to backend capability semantics: unsupported paths are visible and do not fake GPU queue support.
- Update RenderContext, FrameSynchronizer, GPUUploadService, and validation fakes to consume the returned submitted fence value.
- Add `supportsExplicitHeapManagement` so explicit heap/placed-resource support is capability-gated instead of assumed by cross-backend tests.

**Out of scope:**

- Descriptor/barrier expansion, Vulkan query implementation, Linux shader pipeline, Metal runtime validation, or Vulkan bindless parity.
- Full multi-queue scheduling beyond the declared submit contract.
- Fixing pre-existing Vulkan validation-layer messages in legacy command-context tests.
- RenderGraph, Material, Asset, RenderProxy, or ModelViewer visual work.

**Files changed:**

- `Docs/superpowers/specs/2026-05-31-r1a-rhi-submit-fence-contract-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `RHI/Include/RHI/RHICapabilities.h`
- `RHI/Include/RHI/RHICommandContext.h`
- `RHI/Include/RHI/RHIDevice.h`
- `RHI/Include/RHI/RHISynchronization.h`
- `RHI_DX11/Private/DX11CommandContext.cpp`
- `RHI_DX11/Private/DX11Device.cpp`
- `RHI_DX11/Private/DX11Device.h`
- `RHI_DX11/Private/DX11Resources.cpp`
- `RHI_DX11/Private/DX11Resources.h`
- `RHI_DX12/Private/DX12CommandContext.cpp`
- `RHI_DX12/Private/DX12CommandContext.h`
- `RHI_DX12/Private/DX12Device.cpp`
- `RHI_DX12/Private/DX12Device.h`
- `RHI_DX12/Private/DX12Resources.cpp`
- `RHI_DX12/Private/DX12Resources.h`
- `RHI_Metal/Private/MetalCommandContext.h`
- `RHI_Metal/Private/MetalCommandContext.mm`
- `RHI_Metal/Private/MetalDevice.h`
- `RHI_Metal/Private/MetalDevice.mm`
- `RHI_Metal/Private/MetalSynchronization.h`
- `RHI_Metal/Private/MetalSynchronization.mm`
- `RHI_OpenGL/Private/OpenGLCommandContext.cpp`
- `RHI_OpenGL/Private/OpenGLDevice.cpp`
- `RHI_OpenGL/Private/OpenGLDevice.h`
- `RHI_OpenGL/Private/OpenGLSync.cpp`
- `RHI_OpenGL/Private/OpenGLSync.h`
- `RHI_Vulkan/Private/VulkanCommandContext.cpp`
- `RHI_Vulkan/Private/VulkanCommandContext.h`
- `RHI_Vulkan/Private/VulkanDevice.cpp`
- `RHI_Vulkan/Private/VulkanDevice.h`
- `Render/Include/Render/Context/FrameSynchronizer.h`
- `Render/Private/Context/FrameSynchronizer.cpp`
- `Render/Private/Context/RenderContext.cpp`
- `Render/Private/GPUUploadService.cpp`
- `Tests/CrossBackendValidation/main.cpp`
- `Tests/DX11Validation/main.cpp`
- `Tests/DX12Validation/main.cpp`
- `Tests/GPUResourceManagerValidation/main.cpp`
- `Tests/GPUUploadServiceValidation/main.cpp`
- `Tests/RenderHonestyValidation/main.cpp`
- `Tests/ResourceViewCacheValidation/main.cpp`
- `Tests/VulkanValidation/main.cpp`

**Validation commands:**

```powershell
cmake --build build/win_x64_debug --config Debug --target CrossBackendValidation
build\win_x64_debug\Tests\Debug\CrossBackendValidation.exe
cmake --build build/win_x64_debug --config Debug --target GPUUploadServiceValidation GPUResourceManagerValidation ResourceViewCacheValidation RenderHonestyValidation DX12Validation VulkanValidation DX11Validation
build\win_x64_debug\Tests\Debug\GPUUploadServiceValidation.exe
build\win_x64_debug\Tests\Debug\CrossBackendValidation.exe --gtest_filter=CrossBackendValidation.SubmitFenceValueConsistency
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "DX12Validation|VulkanValidation|DX11Validation|CrossBackendValidation|RenderHonestyValidation|GPUUploadServiceValidation|GPUResourceManagerValidation|ResourceViewCacheValidation"
```

**Validation result:**

- Build: PASS
- Tests: PASS
  - `DX12Validation`: 16/16
  - `VulkanValidation`: 19/19
  - `DX11Validation`: 14/14
  - `CrossBackendValidation`: 8/8
  - `GPUUploadServiceValidation`: 7/7
  - Filtered CTest: 101/101
- Visual gate: N/A

**Artifacts:**

- Logs: terminal build/test output; Spark plan and code review messages
- Screenshots: N/A
- Diffs: R1a working tree diff before commit

**Spark plan review result:**

- Verdict: PASS after plan corrections.
- Blockers resolved: split DX12 host/default/explicit queue fence semantics, declared DX12 mixed-queue batch unsupported, included all backend/test fake signature updates, and kept Vulkan queue waits unsupported.

**Spark code review result:**

- Verdict: PASS after blocker fixes.
- Blockers resolved: OpenGL null submit and OpenGL/DX11 empty/all-null batch submit now return `0` without signaling; Metal command-context `SignalFence`/`WaitFence` now log unsupported instead of performing host fence operations; cross-backend tests cover no-work submit semantics.

**Notes / follow-ups:**

- Vulkan validation still emits pre-existing validation-layer messages in legacy command-context tests, but the R1a validation targets pass.
- Metal code was updated to the same interface contract but could not be runtime-validated on this Windows machine.
- Next stage must reread the render-first plan and create its own implementation plan before code changes.

---

### R-SP: R1b RHI Descriptor/Barrier Base Contract

**Date:** 2026-05-31
**Commit:** `d82cd1b`
**Spark plan review agent:** `019e7e90-629e-7533-b55d-323d7aba8f3e`
**Spark code review agent:** `019e83a1-69fa-7b30-a5f6-0831a7e401bf`

**Plan source:**

- Document: `Docs/superpowers/specs/2026-05-31-r1b-rhi-descriptor-barrier-contract-plan.md`
- Source roadmap: `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`
- Section: `6. R1 - RHI Core Contract`
- Lines checked: R1 section and R1b plan sections 1-7 checked before implementation; plan rechecked before review/commit

**Prerequisite status:** PASS

- Previous R-SP: R1a RHI Submit/Fence Contract
- Evidence: R1a committed as `4a441d1`; R1b implementation plan passed Spark plan review before code changes

**Approved scope:**

- Add public descriptor layout inspection and make `RHIDescriptorSet::Update()` return `bool`.
- Add shared descriptor validation helpers for layout, pipeline layout, descriptor set, and update inputs.
- Make DX12, Vulkan, DX11, OpenGL, and Metal descriptor factories return `nullptr` for invalid descriptions.
- Make descriptor update failures return `false` and preserve the previous valid state.
- Add descriptor/barrier capability fields with honest backend values.
- Add null/same-state barrier no-work guards and capability-consistent split-barrier behavior.

**Out of scope:**

- Bindless descriptor parity, descriptor indexing, update-after-bind, and descriptor heap residency work.
- RenderGraph aliasing or transient resource aliasing.
- Shader reflection-driven descriptor layout generation.
- Material binding model changes.
- ModelViewer visual validation.

**Files changed:**

- `Docs/superpowers/specs/2026-05-31-r1b-rhi-descriptor-barrier-contract-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `RHI/Include/RHI/RHICapabilities.h`
- `RHI/Include/RHI/RHICommandContext.h`
- `RHI/Include/RHI/RHIDescriptor.h`
- `RHI_DX11/Private/DX11Device.cpp`
- `RHI_DX11/Private/DX11Pipeline.cpp`
- `RHI_DX11/Private/DX11Pipeline.h`
- `RHI_DX12/Private/DX12CommandContext.cpp`
- `RHI_DX12/Private/DX12Device.cpp`
- `RHI_DX12/Private/DX12Pipeline.cpp`
- `RHI_DX12/Private/DX12Pipeline.h`
- `RHI_Metal/Private/MetalCommandContext.mm`
- `RHI_Metal/Private/MetalDevice.mm`
- `RHI_Metal/Private/MetalResources.h`
- `RHI_Metal/Private/MetalResources.mm`
- `RHI_OpenGL/Private/OpenGLCommandContext.cpp`
- `RHI_OpenGL/Private/OpenGLDescriptor.cpp`
- `RHI_OpenGL/Private/OpenGLDescriptor.h`
- `RHI_OpenGL/Private/OpenGLDevice.cpp`
- `RHI_OpenGL/Private/OpenGLPipeline.h`
- `RHI_Vulkan/Private/VulkanCommandContext.cpp`
- `RHI_Vulkan/Private/VulkanDevice.cpp`
- `RHI_Vulkan/Private/VulkanPipeline.cpp`
- `RHI_Vulkan/Private/VulkanPipeline.h`
- `Tests/CrossBackendValidation/main.cpp`
- `Tests/DX11Validation/main.cpp`
- `Tests/DX12Validation/main.cpp`
- `Tests/RenderHonestyValidation/main.cpp`
- `Tests/VulkanValidation/main.cpp`

**Validation commands:**

```powershell
cmake --build build/win_x64_debug --config Debug --target RenderHonestyValidation DX12Validation VulkanValidation DX11Validation CrossBackendValidation
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "RenderHonestyValidation|DX12Validation|VulkanValidation|DX11Validation|CrossBackendValidation"
```

**Validation result:**

- Build: PASS
- Tests: PASS
  - Filtered CTest: 82/82
- Visual gate: N/A

**Artifacts:**

- Logs: terminal build/test output; Spark plan and code review messages
- Screenshots: N/A
- Diffs: R1b working tree diff before commit

**Spark plan review result:**

- Verdict: PASS on second review.
- Blockers resolved: first Spark plan review agent hung and was closed; second review approved the narrowed descriptor/barrier contract plan.

**Spark code review result:**

- Verdict: PASS.
- Blockers resolved: N/A
- Non-blocking follow-ups: consider adding observable tests that failed descriptor updates preserve old bindings; keep descriptor array/range validation for a later API extension.

**Notes / follow-ups:**

- Vulkan split barriers are now reported unsupported until event-based split barriers are implemented.
- Metal code was updated to the same descriptor/barrier contract but could not be runtime-validated on this Windows machine.
- Old untracked framework/spec documents and `vulkan_pipeline_cache.bin` are intentionally excluded from the R1b commit.
- Next stage must reread the render-first plan and create its own implementation plan before code changes.

---

### R-SP: R2 ShaderCompiler and Reflection

**Date:** 2026-05-31
**Commit:** `91b2ddb`
**Spark plan review agent:** `019e83b0-fc20-7cf1-ab15-8bf0499974e1`
**Spark code review agent:** `019e83c4-60f0-71c1-8ad4-f8cbc4b368e1`

**Plan source:**

- Document: `Docs/superpowers/specs/2026-05-31-r2-shadercompiler-reflection-plan.md`
- Source roadmap: `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`
- Section: `7. R2 - ShaderCompiler and Reflection`
- Lines checked: R2 roadmap section and R2 plan sections 1-7 checked before implementation; plan rechecked before review/commit

**Prerequisite status:** PASS

- Previous R-SP: R1b RHI Descriptor/Barrier Base Contract
- Evidence: R1b committed as `d82cd1b`; R2 implementation plan passed Spark plan review before code changes

**Approved scope:**

- Make async shader compile requests own stable copies of source, entry point, source path, target profile, and defines.
- Add `ShaderSourceInfo` to compile results and wire DXC include tracking into DX12/SPIR-V compile paths.
- Populate compile-result reflection metadata for DX11, DX12, Vulkan, and OpenGL paths where supported.
- Preserve source dependency metadata through shader cache save/load and validate memory-cache entries on load.
- Ensure OpenGL hot reload, shader manager, and permutation paths use generated GLSL source instead of SPIR-V bytes.
- Add `ShaderCompilerValidation` coverage for invalid inputs, async lifetime, include invalidation, reflection/layout metadata, OpenGL GLSL output, cache invalidation, and permutation source selection.

**Out of scope:**

- Pipeline/PSO creation, pipeline cache serialization redesign, or render pass integration.
- Material binding behavior beyond consuming existing shader compile results.
- Full shader hot-reload watcher redesign.
- Linux shader compiler replacement beyond honest unsupported behavior.
- Metal runtime validation on Windows.
- Visual rendering or ModelViewer validation.

**Files changed:**

- `Docs/superpowers/specs/2026-05-31-r2-shadercompiler-reflection-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `ShaderCompiler/Include/ShaderCompiler/ShaderCompileService.h`
- `ShaderCompiler/Include/ShaderCompiler/ShaderCompiler.h`
- `ShaderCompiler/Include/ShaderCompiler/ShaderHotReloader.h`
- `ShaderCompiler/Private/DXCCompiler.cpp`
- `ShaderCompiler/Private/ShaderCacheManager.cpp`
- `ShaderCompiler/Private/ShaderCompileService.cpp`
- `ShaderCompiler/Private/ShaderHotReloader.cpp`
- `ShaderCompiler/Private/ShaderManager.cpp`
- `ShaderCompiler/Private/ShaderPermutation.cpp`
- `Tests/CMakeLists.txt`
- `Tests/ShaderCompilerValidation/main.cpp`

**Validation commands:**

```powershell
cmake --preset local_win_x64_debug
cmake --build build/win_x64_debug --config Debug --target ShaderCompilerValidation RenderHonestyValidation MaterialSystemValidation DX12Validation VulkanValidation
build\win_x64_debug\Tests\Debug\ShaderCompilerValidation.exe
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "ShaderCompilerValidation|RenderHonestyValidation|MaterialSystemValidation|DX12Validation|VulkanValidation"
git diff --check
```

**Validation result:**

- Build: PASS
- Tests: PASS
  - `ShaderCompilerValidation`: 8/8
  - Filtered CTest: 65/65
- Visual gate: N/A

**Artifacts:**

- Logs: terminal CMake/build/test output; Spark plan and code review messages
- Screenshots: N/A
- Diffs: R2 working tree diff before commit

**Spark plan review result:**

- Verdict: PASS after plan clarification.
- Blockers resolved: first plan review agent ran out of context; second review treated current implementation gaps as plan blockers; plan was clarified so review judged scope/testability rather than pre-existing gaps, then passed.

**Spark code review result:**

- Verdict: PASS after final review.
- Blockers resolved: N/A
- Non-blocking follow-ups: async load path still does not persist compile results to cache or re-register hot-reload dependency metadata; Vulkan/Metal positive reflection/source-selection tests are still limited by local platform coverage; disk-cache include invalidation lacks a dedicated end-to-end test.

**Notes / follow-ups:**

- Legacy shader cache is intentionally bypassed when the dependency-aware `ShaderCacheManager` exists, to avoid stale include-blind hits.
- OpenGL and permutation paths now select GLSL source; Metal source selection is handled similarly but not runtime-validated on this Windows machine.
- Old untracked framework/spec documents and `vulkan_pipeline_cache.bin` are intentionally excluded from the R2 commit.
- Next stage must reread the render-first plan and create its own implementation plan before code changes.

---

### R-SP: R3a Pipeline and PSO Foundation Core

**Date:** 2026-06-02
**Commit:** `17204a9`
**Spark plan review agent:** `019e88bb-d3c1-76d2-8a0a-748afeca43a8`
**Spark code review agent:** `019e88c4-2f57-75f2-9051-bfc67229ae28`

**Plan source:**

- Document: `Docs/superpowers/specs/2026-06-02-r3-pipeline-pso-foundation-plan.md`
- Source roadmap: `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`
- Section: `8. R3 - Pipeline and PSO Foundation`
- Lines checked: R3 roadmap section and R3a/R3b split in the R3 plan checked before implementation; R3a scope rechecked before review/commit

**Prerequisite status:** PASS

- Previous R-SP: R2 ShaderCompiler and Reflection
- Evidence: R2 committed as `91b2ddb`; R3 implementation plan passed Spark plan review before code changes

**Approved scope:**

- Add `PipelineCacheValidation` and deterministic fake RHI coverage for PipelineCache behavior.
- Add PipelineCache diagnostics/config surface, `GetLastError()`, pipeline state hash stats, and visible initialization failure reasons.
- Build DefaultLit descriptor set layouts from shader reflection metadata and validate the required set/binding contract.
- Convert object and material constant-buffer bindings to dynamic uniform bindings for runtime dynamic-offset usage.
- Route opaque, masked, and transparent default-lit pipeline variants through a keyed `GetOrCreateDefaultLitPipeline()` helper.
- Compute deterministic PSO hashes covering backend, shader inputs/compiled outputs, variant, formats, fixed-function state, topology, and input layout.

**Out of scope:**

- PipelineCache manifest save/load/invalidation.
- Switching the runtime depth baseline to D32F or enabling reverse-Z by default.
- SceneRenderer depth target changes.
- Native backend pipeline cache blobs.
- Material binding redesign, RenderGraph pass completion, RenderProxy, visual golden validation, or ModelViewer final validation.

**Files changed:**

- `Docs/superpowers/specs/2026-06-02-r3-pipeline-pso-foundation-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `Render/Include/Render/PipelineCache.h`
- `Render/Private/PipelineCache.cpp`
- `Tests/CMakeLists.txt`
- `Tests/PipelineCacheValidation/main.cpp`

**Validation commands:**

```powershell
cmake --preset local_win_x64_debug
cmake --build build/win_x64_debug --config Debug --target PipelineCacheValidation
build\win_x64_debug\Tests\Debug\PipelineCacheValidation.exe
cmake --build build/win_x64_debug --config Debug --target PipelineCacheValidation MaterialSystemValidation DX12Validation
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "PipelineCacheValidation|MaterialSystemValidation|DX12Validation"
git diff --check
```

**Validation result:**

- Build: PASS
- Tests: PASS
  - `PipelineCacheValidation`: 7/7
  - Filtered CTest: 27/27
- Visual gate: N/A

**Artifacts:**

- Logs: terminal CMake/build/test output; Spark plan and code review messages
- Screenshots: N/A
- Diffs: R3a working tree diff before commit

**Spark plan review result:**

- Verdict: PASS after R3 was split into R3a/R3b.
- Blockers resolved: clarified required vs optional reflection bindings, hash canonicalization, corrupt/missing manifest behavior, projection migration deferral to R7, and the R3a/R3b split.

**Spark code review result:**

- Verdict: PASS.
- Blockers resolved: N/A
- Non-blocking follow-ups adopted: `Initialize()` no longer clears stats/cache on the already-initialized early-return path; shader hash now includes permutation hash and compiled bytecode/source outputs in addition to source dependency hash; R3 plan now marks D32F/reverse-Z defaults as R3b delivery.
- Remaining non-blocking follow-ups: add explicit malformed-reflection tests and a direct cache-hit regression once a public same-state creation path exists.

**Notes / follow-ups:**

- `PipelineCacheConfig` exposes manifest and depth/reverse-Z fields in R3a, but manifest behavior and D32F/reverse-Z default switching remain R3b scope.
- Old untracked framework/spec documents and `vulkan_pipeline_cache.bin` are intentionally excluded from the R3a commit.
- Next sub-stage is R3b and must reread the R3 plan, pass Spark plan review, implement, pass validation, pass Spark code review, update this log, and commit before R4.

---

### R-SP: R3b Pipeline Manifest and Depth Baseline

**Date:** 2026-06-02
**Commit:** `d99214b`
**Spark plan review agent:** `019e88cb-0cf5-7f62-94ed-e24e456ba6cc`
**Spark code review agent:** `019e88d2-5b3d-7e72-8163-33e744f2ded2`

**Plan source:**

- Document: `Docs/superpowers/specs/2026-06-02-r3b-pipeline-manifest-depth-plan.md`
- Parent plan: `Docs/superpowers/specs/2026-06-02-r3-pipeline-pso-foundation-plan.md`
- Source roadmap: `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`
- Section: `8. R3 - Pipeline and PSO Foundation`
- Lines checked: R3b plan sections 1-7 checked before implementation; scope and validation sections checked again before review/commit

**Prerequisite status:** PASS

- Previous R-SP: R3a Pipeline and PSO Foundation Core
- Evidence: R3a committed as `17204a9`; R3b implementation plan passed Spark plan review before code changes

**Approved scope:**

- Add strict PipelineCache engine-side manifest save/load/invalidation metadata.
- Persist only engine-visible PSO inputs and state hashes, not native backend pipeline blobs.
- Treat missing manifests as cold init; treat stale, corrupt, malformed, or version/config mismatched manifests as visible invalidation.
- Use temp/backup/restore manifest writes and keep manifest write failure non-fatal after successful runtime PSO creation.
- Switch PipelineCache and SceneRenderer default depth format to `RHIFormat::D32_FLOAT`.
- Add explicit forward-Z/reverse-Z depth-state and clear-depth helpers.
- Keep forward-Z as the runtime default; make reverse-Z opt-in and test-covered until R7 projection migration.
- Route OpaquePass and DepthPrepass scene clear-depth values through PipelineCache clear-depth convention.

**Out of scope:**

- Native backend pipeline cache blobs.
- Reverse-Z runtime default switch or projection/camera migration.
- Shadow-map depth convention migration.
- Visual golden validation or ModelViewer final validation.
- RenderGraph pass completion, RenderProxy, Material binding redesign, or Asset pipeline work.

**Files changed:**

- `Docs/superpowers/specs/2026-06-02-r3b-pipeline-manifest-depth-plan.md`
- `Docs/superpowers/specs/2026-06-02-r3-pipeline-pso-foundation-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `Render/Include/Render/PipelineCache.h`
- `Render/Private/PipelineCache.cpp`
- `Render/Private/Renderer/SceneRenderer.cpp`
- `Render/Private/Passes/OpaquePass.cpp`
- `Render/Private/Passes/DepthPrepass.cpp`
- `Tests/PipelineCacheValidation/main.cpp`

**Validation commands:**

```powershell
cmake --build build/win_x64_debug --config Debug --target PipelineCacheValidation
build\win_x64_debug\Tests\Debug\PipelineCacheValidation.exe
cmake --build build/win_x64_debug --config Debug --target PipelineCacheValidation MaterialSystemValidation DX12Validation RenderHonestyValidation VulkanValidation
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "PipelineCacheValidation|MaterialSystemValidation|DX12Validation"
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "RenderHonestyValidation|VulkanValidation"
git diff --check
```

**Validation result:**

- Build: PASS
- Tests: PASS
  - `PipelineCacheValidation`: 15/15
  - Required filtered CTest: 35/35
  - Optional filtered CTest: 37/37
- Visual gate: N/A

**Artifacts:**

- Logs: terminal build/test output; Spark plan and code review messages
- Screenshots: N/A
- Diffs: R3b working tree diff before commit

**Spark plan review result:**

- Verdict: PASS.
- Blockers resolved: N/A
- Non-blocking suggestions adopted: atomic-style manifest writes with backup/restore, non-fatal manifest write failure coverage, and explicit phase-log evidence for manifest validity/invalidation.

**Spark code review result:**

- Verdict: PASS after final delta review.
- Blockers resolved: N/A
- Non-blocking follow-ups adopted: manifest replacement now preserves a `.bak` backup until new manifest rename succeeds; parser rejects non-boolean `reverseZ` values and tests cover that invalidation path.
- Remaining non-blocking follow-ups: add diagnostics if backup restoration itself fails; replace string-based manifest mutation in tests with a field parser if the manifest format evolves.

**Notes / follow-ups:**

- Reverse-Z remains opt-in because the current camera/projection path is still forward-Z; R7 owns projection migration and visual acceptance.
- ShadowPass keeps its existing shadow depth clear convention until the shadow projection path is migrated deliberately.
- Old untracked framework/spec documents and `vulkan_pipeline_cache.bin` are intentionally excluded from the R3b commit.
- Next stage must reread the render-first plan, create/confirm its implementation plan, pass Spark plan review, implement, pass validation, pass Spark code review, update this log, and commit before moving on.

---

### R-SP: R4 Asset GPU Upload

**Date:** 2026-06-02
**Commit:** `de1118c`
**Spark plan review agent:** `019e88da-7932-72c2-85c4-532cf7e223b8`
**Spark code review agent:** `019e9319-2b15-7cf3-97ae-ef73c55cb940`

**Plan source:**

- Document: `Docs/superpowers/specs/2026-06-02-r4-asset-gpu-upload-plan.md`
- Section: R4 - Asset GPU Upload
- Lines checked: R4 plan sections 1-7 checked before implementation; scope, validation, and done criteria rechecked before review/commit

**Prerequisite status:** PASS

- Previous R-SP: R3b Pipeline Manifest and Depth Baseline
- Evidence: R3b committed as `d99214b`; R4 implementation plan passed Spark plan review before code changes

**Approved scope:**

- Harden `GPUResourceManager` texture upload preparation for 2D single-mip/single-layer texture resources.
- Expand `TextureFormat::RGB8` CPU data to RGBA8 before upload.
- Reject unsupported, inconsistent, cubemap, array, 3D, mip-chain, and compressed texture uploads visibly without placeholder GPU textures.
- Keep GPU texture cache identity keyed by `ResourceId`; invalidate dependent views and account memory/pending uploads on success, failure, replacement, and eviction.
- Prove `MaterialResource` texture handles resolve through `GPUResourceManager` + `ResourceViewCache` to resident GPU texture views, and fall back for non-resident or failed textures.

**Out of scope:**

- Full material binder/template compile wiring and SceneRenderer material binding integration.
- Compressed, cubemap, array, 3D, and mip-chain texture upload support.
- RenderProxy work.
- Visual golden or final ModelViewer validation.

**Files changed:**

- `Render/Include/Render/GPUResourceManager.h`
- `Render/Private/GPUResourceManager.cpp`
- `Tests/CMakeLists.txt`
- `Tests/GPUResourceManagerValidation/main.cpp`
- `Tests/MaterialSystemValidation/main.cpp`
- `Docs/superpowers/specs/2026-06-02-r4-asset-gpu-upload-plan.md`
- `Docs/superpowers/specs/phase-log.md`

**Validation commands:**

```powershell
cmake --build build/win_x64_debug --config Debug --target GPUResourceManagerValidation
build\win_x64_debug\Tests\Debug\GPUResourceManagerValidation.exe
cmake --build build/win_x64_debug --config Debug --target MaterialSystemValidation
build\win_x64_debug\Tests\Debug\MaterialSystemValidation.exe
cmake --build build/win_x64_debug --config Debug --target GPUUploadServiceValidation GPUResourceManagerValidation ResourceInstantiationValidation RenderHonestyValidation MaterialSystemValidation DX12Validation VulkanValidation
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "GPUUploadServiceValidation|GPUResourceManagerValidation|ResourceInstantiationValidation|RenderHonestyValidation|MaterialSystemValidation|DX12Validation|VulkanValidation"
git diff --check
```

**Validation result:**

- Build: PASS
- Tests: PASS
  - `GPUResourceManagerValidation`: 28/28
  - `MaterialSystemValidation`: 4/4
  - Required + optional filtered CTest: 171/171
- Visual gate: N/A

**Artifacts:**

- Logs: terminal build/test output; Spark plan and code review messages
- Screenshots: N/A
- Diffs: R4 working tree diff before commit

**Spark plan review result:**

- Verdict: PASS.
- Blockers resolved: N/A
- Non-blocking suggestions adopted: explicit same-ID replacement failure semantics, memory accounting for replacement/failure/release, and material resident/non-resident texture view tests.

**Spark code review result:**

- Verdict: PASS.
- Blockers resolved: N/A
- Non-blocking suggestions adopted: clear same-`ResourceId` queued texture uploads before immediate refresh, cover unsupported layout failures, cover failed texture material fallback, reject `isArray` textures even when `arrayLayers == 1`, and add texture upload size overflow protection.

**Notes / follow-ups:**

- `UploadImmediate(TextureResource*)` is now the explicit synchronous texture refresh path; `RequestUpload(TextureResource*)` still skips already-resident textures to avoid duplicate ordinary async uploads.
- R4 keeps unsupported texture classes honest instead of pretending upload support exists. Future compressed/mip/cubemap/array/3D support should add real upload paths and tests.
- `git diff --check` reports only LF-to-CRLF warnings for two test files, no whitespace errors.
- Old untracked framework/spec documents and `vulkan_pipeline_cache.bin` are intentionally excluded from the R4 commit.
- Next stage must reread the render-first plan, create/confirm its implementation plan, pass Spark plan review, implement, pass validation, pass Spark code review, update this log, and commit before moving on.

---

### R-SP: R5a Material Binder and Template Minimum Wiring

**Date:** 2026-06-02
**Commit:** `48c6d2b`
**Spark plan review agent:** `019e9321-0959-7723-9ba1-47b498f59336`
**Spark code review agent:** `019e9329-84a9-70c3-9002-2abec351b36d`

**Plan source:**

- Document: `Docs/superpowers/specs/2026-06-02-r5a-material-binder-template-plan.md`
- Section: R5a - Material Binder and Template Minimum Wiring
- Lines checked: R5a plan sections 1-7 checked before implementation; scope, validation, and done criteria rechecked before review/commit

**Prerequisite status:** PASS

- Previous R-SP: R4 Asset GPU Upload
- Evidence: R4 committed as `de1118c` with hash correction committed as `c07883d`; R5a implementation plan passed Spark plan review before code changes

**Approved scope:**

- Make `MaterialTemplate::Compile` report visible failure instead of placeholder compile success for missing device, missing vertex shader, missing pixel shader, and missing standalone pipeline integration.
- Make `MaterialBinder` initialization, material bind, default fallback bind, constant-buffer creation, and constant-buffer map failures expose inspectable status and messages.
- Convert real `Scene::Material` scalar/color/alpha/workflow/double-sided/texture-flag data into `MaterialGPUConstants`.
- Keep fallback material binding explicit and observable.
- Add tests for missing shader paths, missing pipeline integration, texture flag present/missing behavior, fallback status, create-buffer failure, and map failure.

**Out of scope:**

- SceneRenderer material-system wiring.
- Real material PSO creation or PipelineCache integration for material templates.
- Descriptor-table or backend-specific texture binding.
- RenderProxy work.
- Visual golden or final ModelViewer validation.

**Files changed:**

- `Render/Include/Render/Material/MaterialBinder.h`
- `Render/Private/Material/MaterialBinder.cpp`
- `Render/Private/Material/MaterialTemplate.cpp`
- `Tests/MaterialSystemValidation/main.cpp`
- `Tests/RenderHonestyValidation/main.cpp`
- `Docs/superpowers/specs/2026-06-02-r5a-material-binder-template-plan.md`
- `Docs/superpowers/specs/phase-log.md`

**Validation commands:**

```powershell
cmake --build build/win_x64_debug --config Debug --target MaterialSystemValidation
build\win_x64_debug\Tests\Debug\MaterialSystemValidation.exe
cmake --build build/win_x64_debug --config Debug --target MaterialSystemValidation RenderHonestyValidation
build\win_x64_debug\Tests\Debug\MaterialSystemValidation.exe
build\win_x64_debug\Tests\Debug\RenderHonestyValidation.exe
cmake --build build/win_x64_debug --config Debug --target MaterialSystemValidation RenderHonestyValidation PipelineCacheValidation DX12Validation VulkanValidation
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "MaterialSystemValidation|RenderHonestyValidation|PipelineCacheValidation|DX12Validation|VulkanValidation"
git diff --check
```

**Validation result:**

- Build: PASS
- Tests: PASS
  - `MaterialSystemValidation`: 11/11
  - `RenderHonestyValidation`: 15/15
  - Required + optional filtered CTest: 82/82
- Visual gate: N/A

**Artifacts:**

- Logs: terminal build/test output; Spark plan and code review messages
- Screenshots: N/A
- Diffs: R5a working tree diff before commit

**Spark plan review result:**

- Verdict: PASS.
- Blockers resolved: N/A
- Non-blocking suggestions adopted: independent missing-pipeline coverage, texture-flag present/missing coverage, bind status/message consistency checks, create/map failure coverage, and grouped compile-failure tests.

**Spark code review result:**

- Verdict: PASS.
- Blockers resolved: N/A
- Non-blocking suggestions deferred: clarify the `Bind(uint64)` fallback message in a later material-ID/resource resolution stage if it becomes useful; `RenderHonestyValidation` may add split missing-shader/missing-pipeline assertions later, while R5a already covers them in `MaterialSystemValidation`.

**Notes / follow-ups:**

- R5a intentionally leaves material PSO and descriptor binding as visible `Unsupported` until R5b wires SceneRenderer, PipelineCache, and material descriptor layout integration.
- `git diff --check` reports only LF-to-CRLF warnings for two test files, no whitespace errors.
- Old untracked framework/spec documents and `vulkan_pipeline_cache.bin` are intentionally excluded from the R5a commit.
- Next stage must reread the render-first plan, create/confirm its implementation plan, pass Spark plan review, implement, pass validation, pass Spark code review, update this log, and commit before moving on.

---

### R-SP: R5b-1 Material Draw Routing

**Date:** 2026-06-06
**Commit:** `1e61ee3`
**Spark plan review agent:** `019e9335-0c36-72c0-be27-bc6e3c2e1b25`
**Spark code review agent:** `019e9a99-6d07-7672-b69b-9bdba2e5c30b`

**Plan source:**

- Document: `Docs/superpowers/specs/2026-06-02-r5b-1-material-draw-routing-plan.md`
- Section: R5b-1 - Material Draw Routing
- Lines checked: parent R5b section and R5b-1 plan sections 1-8 checked before implementation; scope, validation, and done criteria rechecked before review/commit

**Prerequisite status:** PASS

- Previous R-SP: R5a Material Binder and Template Minimum Wiring
- Evidence: R5a committed as `48c6d2b` with hash correction committed as `200014a`; R5b-1 implementation plan passed Spark plan review after the first broader R5b plan was blocked and split

**Approved scope:**

- Extract a testable material draw-list helper from `SceneRenderer::BuildMaterialDrawLists`.
- Preserve material render mode classification, submesh identity, `materialResource`, `materialId`, sort keys, and transparent back-to-front ordering.
- Make `SceneRenderer::BuildMaterialDrawLists` delegate to the helper.
- Make `OpaquePass` bind opaque and masked material pipeline variants separately at actual execution time.
- Make missing opaque or masked variant pipelines skip only their corresponding draw group with visible logging.
- Keep `TransparentPass` on the transparent material variant contract.
- Add `RenderPassValidation` execution tests that assert `SetPipeline` order/identity and `DrawIndexed` count.

**Out of scope:**

- `MaterialSystem` status/result redesign and `UpdateMaterialConstants()` return contract; this remains R5b-2.
- Descriptor fallback status/result structs.
- RenderProxy work.
- ECS/Object/SceneEntity migration.
- RenderGraph hardening.
- Shader compiler, bindless, or new PBR shader feature work.
- Visual golden or final ModelViewer validation.

**Files changed:**

- `Render/Include/Render/Renderer/RenderDrawItem.h`
- `Render/Private/Renderer/RenderDrawItem.cpp`
- `Render/Private/Renderer/SceneRenderer.cpp`
- `Render/Private/Passes/OpaquePass.cpp`
- `Render/Private/Passes/TransparentPass.cpp`
- `Tests/RenderSceneValidation/main.cpp`
- `Tests/RenderPassValidation/main.cpp`
- `Tests/CMakeLists.txt`
- `Docs/superpowers/specs/2026-06-02-r5b-1-material-draw-routing-plan.md`
- `Docs/superpowers/specs/phase-log.md`

**Validation commands:**

```powershell
cmake -S . -B build/win_x64_debug
cmake --build build/win_x64_debug --config Debug --target RenderSceneValidation RenderPassValidation
build\win_x64_debug\Tests\Debug\RenderSceneValidation.exe
build\win_x64_debug\Tests\Debug\RenderPassValidation.exe
build\win_x64_debug\Tests\Debug\MaterialSystemValidation.exe
cmake --build build/win_x64_debug --config Debug --target RenderSceneValidation RenderPassValidation MaterialSystemValidation RenderHonestyValidation PipelineCacheValidation DX12Validation VulkanValidation
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "RenderSceneValidation|RenderPassValidation|MaterialSystemValidation|RenderHonestyValidation|PipelineCacheValidation|DX12Validation|VulkanValidation"
git diff --check
```

**Validation result:**

- Build: PASS
- Tests: PASS
  - `RenderSceneValidation`: 9/9
  - `RenderPassValidation`: 4/4
  - `MaterialSystemValidation`: 11/11
  - Required + optional filtered CTest: 95/95
- Visual gate: N/A

**Artifacts:**

- Logs: terminal configure/build/test output; Spark plan and code review messages
- Screenshots: N/A
- Diffs: R5b-1 working tree diff before commit

**Spark plan review result:**

- Verdict: PASS after split.
- Blockers resolved: first broader R5b plan review (`019e9332-800e-7873-85cd-56ec1f7eb9f0`) blocked on missing pass execution tests, vague `MaterialSystem` status contract, and oversized scope; R5b was split into R5b-1 draw routing and future R5b-2 material status/result contract.
- Non-blocking suggestions adopted: fixed `RenderPassValidation` target name, added missing opaque pipeline skip coverage, and added independent `RenderSceneValidation`/`RenderPassValidation` done criteria.

**Spark code review result:**

- Verdict: PASS.
- Blockers resolved: N/A
- Non-blocking suggestions: `#define private public` in `RenderPassValidation` is acceptable for this missing-variant simulation; a future test-only friend or `RVX_BUILD_TESTS` hook could replace it if desired.

**Notes / follow-ups:**

- R5b-1 intentionally does not change `MaterialSystem::UpdateMaterialConstants()` or descriptor fallback result semantics; that is the next R5b-2 sub-stage.
- `git diff --check` reports only LF-to-CRLF warnings for four touched files, no whitespace errors.
- Old untracked framework/spec documents and `vulkan_pipeline_cache.bin` are intentionally excluded from the R5b-1 commit.
- Next stage must reread the render-first plan and R5b-2 scope, create/confirm its implementation plan, pass Spark plan review, implement, pass validation, pass Spark code review, update this log, and commit before moving on.

---

### R5b-2: Material Binding Status

**Date:** 2026-06-06
**Commit:** `22e850b`
**Spark plan review agent:** `019e9a9f-2018-7333-826f-39b925787d6a`
**Spark code review agent:** `019e9aa8-20ff-7e02-a9df-fd1843d0483c`

**Plan source:**

- Document: `Docs/superpowers/specs/2026-05-30-engine-program-plan-v2.md`
- Section: R5b material draw contract, narrowed by R5b-1 follow-up notes
- Stage plan: `Docs/superpowers/specs/2026-06-06-r5b-2-material-binding-status-plan.md`
- Lines checked: current document text was reread before implementation; stage scope was limited to material binding status/result semantics and pass draw gating.

**Prerequisite status:** PASS

- Previous R-SP: R5b-1
- Evidence: R5b-1 commit `48c6d2b` plus log correction `200014a`; R5b-1 noted `MaterialSystem::UpdateMaterialConstants()` and descriptor fallback result semantics as the next sub-stage.

**Approved scope:**

- Add structured `MaterialBindingStatus` and `MaterialBindingResult`.
- Add `MaterialSystem::PrepareMaterialBinding()` as the atomic constants + descriptor + dynamic-offset contract.
- Make initialization failure, missing constant buffer, map failure, descriptor creation failure, and explicit fallback visible through status/message.
- Keep `UpdateMaterialConstants()` and `GetOrCreateMaterialSet()` compatibility while wiring them to observable last-result state.
- Change `OpaquePass` and `TransparentPass` to draw only when material binding is `Ready` or explicit `Fallback`.
- Add validation for error skip, fallback draw, and compatibility return semantics.

**Out of scope:**

- Further material shader feature work.
- RenderGraph or pass scheduling changes.
- RenderProxy work.
- ECS/Object/SceneEntity migration.
- Visual golden or final ModelViewer validation.

**Files changed:**

- `Render/Include/Render/Material/MaterialSystem.h`
- `Render/Private/Material/MaterialSystem.cpp`
- `Render/Private/Passes/OpaquePass.cpp`
- `Render/Private/Passes/TransparentPass.cpp`
- `Tests/MaterialSystemValidation/main.cpp`
- `Tests/RenderPassValidation/main.cpp`
- `Docs/superpowers/specs/2026-06-06-r5b-2-material-binding-status-plan.md`
- `Docs/superpowers/specs/phase-log.md`

**Validation commands:**

```powershell
cmake --build build/win_x64_debug --config Debug --target MaterialSystemValidation RenderPassValidation
build\win_x64_debug\Tests\Debug\MaterialSystemValidation.exe
build\win_x64_debug\Tests\Debug\RenderPassValidation.exe
cmake --build build/win_x64_debug --config Debug --target MaterialSystemValidation RenderPassValidation RenderSceneValidation RenderHonestyValidation PipelineCacheValidation DX12Validation VulkanValidation
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "MaterialSystemValidation|RenderPassValidation|RenderSceneValidation|RenderHonestyValidation|PipelineCacheValidation|DX12Validation|VulkanValidation"
git diff --check
```

**Validation result:**

- Build: PASS
- Tests: PASS
  - `MaterialSystemValidation`: 18/18
  - `RenderPassValidation`: 8/8
  - Required + optional filtered CTest: 106/106
- Visual gate: N/A

**Artifacts:**

- Logs: terminal build/test output; Spark plan and final code review messages
- Screenshots: N/A
- Diffs: R5b-2 working tree diff before commit

**Spark plan review result:**

- Verdict: PASS.
- Blockers resolved: plan adopted explicit `NotInitialized`/`Unavailable` semantics, pass error/fallback test coverage, and compatibility risk notes.

**Spark code review result:**

- Verdict: PASS.
- Blockers resolved: N/A
- Non-blocking suggestions adopted: added fallback-draw pass regressions and `UpdateMaterialConstants()` true/false compatibility semantics tests before final review.

**Notes / follow-ups:**

- `git diff --check` reports only LF-to-CRLF warnings for touched files, no whitespace errors.
- Old untracked framework/spec documents and `vulkan_pipeline_cache.bin` are intentionally excluded from the R5b-2 commit.
- Next stage must reread the render-first plan and the next documented stage scope, create/confirm its implementation plan, pass Spark plan review, implement, pass validation, pass Spark code review, update this log, and commit before moving on.

---

### R6a: RenderGraph Lifetime and Hazard Validation

**Date:** 2026-06-06
**Commit:** `c5985c2`
**Spark plan review agent:** `019e9ab1-3b1f-7501-bef4-a52b0c6510de`
**Spark code review agent:** `019e9ab8-846b-7490-9da7-9e33b0d8eabd`

**Plan source:**

- Document: `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`
- Section: `12. R6 - RenderGraph Hardening`
- Stage plan: `Docs/superpowers/specs/2026-06-06-r6a-rendergraph-lifetime-hazard-plan.md`
- Lines checked: current R6/R7/R8 sections were reread before implementation; R6 was split to keep lifetime hazards independent from aliasing and async scheduler work.

**Prerequisite status:** PASS

- Previous R-SP: R5b-2
- Evidence: R5b-2 commit `22e850b` plus log correction `e1bd252`; R6 is the next documented render-first stage.

**Approved scope:**

- Add visible RenderGraph compile stats for transient read-before-write hazards and uninitialized exports.
- Reject transient texture/buffer `Read` before any producing write.
- Reject transient texture/buffer `ReadWrite` before initialization.
- Reject exported transient resources that were never produced by a needed pass.
- Preserve imported-resource read-before-later-write behavior.
- Ensure culled hazard passes do not poison valid graphs.
- Ensure invalid lifetime graphs do not execute callbacks.

**Out of scope:**

- Full cycle construction APIs or external dependency graph API.
- Re-enabling memory aliasing or emitting native aliasing barriers.
- Per-subresource or per-buffer-range initialized-region tracking.
- Real async compute/copy scheduler or multi-queue submission.
- RenderGraph visual pass completion.
- Visual golden baseline, RenderProxy, material, asset, or ModelViewer work.

**Files changed:**

- `Render/Include/Render/Graph/RenderGraph.h`
- `Render/Private/Graph/RenderGraphCompiler.cpp`
- `Tests/RenderGraphValidation/main.cpp`
- `Docs/superpowers/specs/2026-06-06-r6a-rendergraph-lifetime-hazard-plan.md`
- `Docs/superpowers/specs/phase-log.md`

**Validation commands:**

```powershell
cmake --build build/win_x64_debug --config Debug --target RenderGraphValidation RenderHonestyValidation
build\win_x64_debug\Tests\Debug\RenderGraphValidation.exe
build\win_x64_debug\Tests\Debug\RenderHonestyValidation.exe
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "RenderGraphValidation|RenderHonestyValidation"
cmake --build build/win_x64_debug --config Debug --target RenderPassValidation RenderSceneValidation MaterialSystemValidation
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "RenderGraphValidation|RenderHonestyValidation|RenderPassValidation|RenderSceneValidation|MaterialSystemValidation"
git diff --check
```

**Validation result:**

- Build: PASS
- Tests: PASS
  - `RenderGraphValidation`: 27/27
  - `RenderHonestyValidation`: 15/15
  - Required filtered CTest: 42/42
  - Optional render regression CTest: 77/77
- Visual gate: N/A

**Artifacts:**

- Logs: terminal build/test output; Spark plan and final code review messages
- Screenshots: N/A
- Diffs: R6a working tree diff before commit

**Spark plan review result:**

- Verdict: PASS.
- Blockers resolved: N/A
- Non-blocking suggestions adopted: added `ReadWrite` coverage, stats reset coverage, imported/transient behavior distinction, and explicit uninitialized export semantics to the plan.

**Spark code review result:**

- Verdict: PASS.
- Blockers resolved: N/A
- Non-blocking suggestions adopted: added buffer invalid-graph no-execute coverage.

**Notes / follow-ups:**

- `git diff --check` reports only an LF-to-CRLF warning for `Tests/RenderGraphValidation/main.cpp`, no whitespace errors.
- Future R6 work should consider lifetime validation based on final `executionOrder` and de-duplicating repeated same-pass hazard counts.
- Old untracked framework/spec documents and `vulkan_pipeline_cache.bin` are intentionally excluded from the R6a commit.
- Next stage must reread the render-first plan and the next documented R6 sub-scope, create/confirm its implementation plan, pass Spark plan review, implement, pass validation, pass Spark code review, update this log, and commit before moving on.

---

### R7: Visual Gate Baseline

**Date:** 2026-06-06
**Commit:** `7a275bc`
**Spark plan review agent:** `019e9ac4-a24f-7380-8baf-451060bd510f`
**Spark code review agent:** `019e9ae6-0c95-74c0-ba46-385b09906636`

**Plan source:**

- Document: `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`
- Section: `13. R7 - Visual Gate Baseline`
- Stage plan: `Docs/superpowers/specs/2026-06-06-r7-visual-gate-baseline-plan.md`
- Lines checked: roadmap lines 374-399 were reread before implementation; R7 plan was rechecked and corrected to match the implemented capture order before review/commit.

**Prerequisite status:** PASS

- Previous R-SP: R6a RenderGraph Lifetime and Hazard Validation
- Evidence: R6a committed as `c5985c2` with hash correction committed as `a5446ba`; R7 implementation plan passed Spark plan review before code changes.

**Approved scope:**

- Add a repository-owned deterministic glTF fixture for ModelViewer.
- Add ModelViewer smoke mode with fixed DX11 backend, camera, time step, frame count, resolution, validation setting, and bounded render loop.
- Add DX11 backbuffer screenshot capture that writes a binary PPM before present and fails visibly on unsupported backends.
- Add PPM load/save/diff helpers and `VisualGoldenValidation`.
- Fix `ImageCompare` so any channel difference marks the pixel once, and add `ImageCompareValidation`.
- Add `ModelViewerSmoke` and make `VisualGoldenValidation` depend on it in CTest.
- Store the first DX11 golden PPM under `Tests/Golden/ModelViewer`.
- Fix DX11 issues required for the visual gate: non-structured vertex buffer stride handling, DX11 runtime-compatible register-space shader compilation, material sampler remap range, and immediate DX11 buffer upload fallback.

**Out of scope:**

- RenderProxy, ECS, Object, or SceneEntity work.
- DX12/Vulkan/Metal/OpenGL golden capture.
- PNG or external image dependencies.
- Broader renderer algorithm changes beyond bounded ModelViewer capture plumbing.

**Files changed:**

- `Docs/superpowers/specs/2026-06-06-r7-visual-gate-baseline-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `Samples/ModelViewer/main.cpp`
- `Tests/CMakeLists.txt`
- `Tests/Common/ImageCompare.cpp`
- `Tests/Common/ImageFile.h`
- `Tests/Common/ImageFile.cpp`
- `Tests/ImageCompareValidation/main.cpp`
- `Tests/VisualGoldenValidation/main.cpp`
- `Tests/Fixtures/ModelViewer/R7Triangle.gltf`
- `Tests/Golden/ModelViewer/R7_DX11_320x180.ppm`
- `RHI_DX11/Private/DX11Resources.cpp`
- `RHI_DX11/Private/DX11BindingRemapper.cpp`
- `Render/Private/GPUUploadService.cpp`
- `Render/Private/PipelineCache.cpp`
- `ShaderCompiler/Private/DXCCompiler.cpp`
- `Tests/DX11Validation/main.cpp`
- `Tests/ShaderCompilerValidation/main.cpp`

**Validation commands:**

```powershell
cmake --build build/win_x64_debug --config Debug --target DX11Validation ShaderCompilerValidation ModelViewer VisualGoldenValidation ImageCompareValidation
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation|DX11Validation|ShaderCompilerValidation"
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "RenderGraphValidation|RenderHonestyValidation|RenderSceneValidation|RenderPassValidation|MaterialSystemValidation|ShaderCompilerValidation|DX11Validation|PipelineCacheValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
git diff --check
```

**Validation result:**

- Build: PASS
- Tests: PASS
  - R7 and affected filtered CTest: 34/34
  - Broad render regression CTest: 126/126
- Visual gate: PASS
  - `ModelViewerSmoke`: PASS
  - `VisualGoldenValidation`: PASS
  - Actual and golden PPM size: 172815 bytes
  - Actual and golden color set: `(24, 7, 4)` and `(25, 25, 38)`

**Artifacts:**

- Logs: terminal build/test output; Spark plan and code review messages
- Screenshots: `build/win_x64_debug/Tests/VisualArtifacts/Debug/ModelViewer/R7_DX11_320x180.ppm`
- Golden: `Tests/Golden/ModelViewer/R7_DX11_320x180.ppm`
- Diffs: failure diff path configured as `build/win_x64_debug/Tests/VisualArtifacts/Debug/ModelViewer/R7_DX11_320x180.diff.ppm`

**Spark plan review result:**

- Verdict: PASS.
- Blockers resolved: N/A
- Non-blocking suggestions adopted: fixed camera/time explicitly documented, capture order made explicit, DX11/window prerequisite documented, CTest dependency added with `set_property`, ImageCompare regression tests added, and build directory parameterization documented.

**Spark code review result:**

- Verdict: PASS after providing a review bundle because the subagent filesystem sandbox could not read the workspace directly.
- Blockers resolved: N/A
- Non-blocking suggestions deferred: optionally reject non-DX11 smoke backends even without screenshot, optionally write diff artifacts for tolerated drift, and clarify tolerance units in future comments/tests.

**Notes / follow-ups:**

- DX12/Vulkan screenshot and golden expansion remains deferred until the RHI row-pitch/copy-readback contract is unified.
- DX11 buffer staged upload still needs a proper staging wrapper/copy contract; R7 forces immediate mapped buffer upload for DX11.
- DX11 set 3 has no default sampler range because D3D11 exposes only 16 sampler slots and set 2 uses `s8-s15`.
- `git diff --check` reports only LF-to-CRLF warnings for touched files, no whitespace errors.
- Old untracked framework/spec documents and `vulkan_pipeline_cache.bin` are intentionally excluded from the R7 commit.
- Next stage must reread the render-first plan and R8 scope, create/confirm its implementation plan, pass Spark plan review, implement, pass validation including the new visual gate, pass Spark code review, update this log, and commit before moving on.

---

### R8: RenderProxy-v1 Bridge and Main Path Switch

**Date:** 2026-06-06
**Commit:** `4785071`
**Spark plan review agent:** `019e9aef-8ddb-7c60-86fe-f436b466a22e`
**Spark code review agent:** `019e9afc-45d3-7d23-87a8-447444db3437`

**Plan source:**

- Document: `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`
- Section: `14. R8 - RenderProxy-v1 Bridge and Main Path Switch`
- Stage plan: `Docs/superpowers/specs/2026-06-06-r8-renderproxy-v1-plan.md`
- Lines checked: roadmap lines 403-481 were reread before implementation.

**Prerequisite status:** PASS

- Previous R-SP: R7 Visual Gate Baseline
- Evidence: R7 committed as `7a275bc` with hash correction committed as `9319c79`; R8 implementation plan passed Spark plan review before code changes.

**Approved scope:**

- Add render-only proxy data types for primitives, lights, snapshots, ids, and minimal commands.
- Extend `PrimitiveComponent` with proxy creation methods while retaining legacy `CollectRenderData`.
- Make `StaticMeshComponent` produce `RenderPrimitiveProxy` without embedding Scene/Component pointers in proxy data.
- Add a synchronous Scene-to-Render proxy bridge that builds a snapshot from scene primitives and lights.
- Add visible fallback reasons and owner ids when proxy collection cannot represent the current world.
- Add `RenderScene::ApplyProxySnapshot` to convert proxy snapshots into existing render object/light lists.
- Switch `SceneRenderer::SetupView` to prefer proxy snapshots, with logged and counted legacy fallback.
- Add RenderScene validation coverage for proxy creation, snapshot application, bridge fallback, light proxy capture, hidden/disabled behavior, creation failure, and transform updates.
- Keep the R7 visual gate in the required validation path.

**Out of scope:**

- Full ECS/Object migration or deletion of `SceneEntity`.
- Persistent proxy lifetime optimization.
- Multithreaded render command queue.
- Removal of `RenderSceneCollector`, `CollectFromWorld`, or `CollectRenderData`.
- Broad pass/draw-list rewrites beyond adapting collection to the proxy bridge.

**Files changed:**

- `Docs/superpowers/specs/2026-06-06-r8-renderproxy-v1-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `Render/CMakeLists.txt`
- `Render/Include/Render/Renderer/RenderProxy.h`
- `Render/Include/Render/Renderer/RenderScene.h`
- `Render/Include/Render/Renderer/SceneRenderer.h`
- `Render/Private/Renderer/RenderProxySceneBridge.h`
- `Render/Private/Renderer/RenderProxySceneBridge.cpp`
- `Render/Private/Renderer/RenderScene.cpp`
- `Render/Private/Renderer/RenderSceneCollector.h`
- `Render/Private/Renderer/SceneRenderer.cpp`
- `Scene/Include/Scene/PrimitiveComponent.h`
- `Scene/Include/Scene/Components/StaticMeshComponent.h`
- `Scene/Private/Components/StaticMeshComponent.cpp`
- `Tests/CMakeLists.txt`
- `Tests/RenderSceneValidation/main.cpp`

**Validation commands:**

```powershell
cmake --build build/win_x64_debug --config Debug --target RenderSceneValidation ResourceInstantiationValidation ModelViewer VisualGoldenValidation ImageCompareValidation
build\win_x64_debug\Tests\Debug\RenderSceneValidation.exe
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "RenderSceneValidation|ResourceInstantiationValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "RenderGraphValidation|RenderHonestyValidation|RenderSceneValidation|RenderPassValidation|MaterialSystemValidation|ResourceInstantiationValidation|PipelineCacheValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
git diff --check
```

**Validation result:**

- Build: PASS
- Tests: PASS
  - `RenderSceneValidation`: 17/17
  - Required R8 filtered CTest: 99/99
  - Broad render regression CTest: 182/182
- Visual gate: PASS
  - `ModelViewerSmoke`: PASS
  - `VisualGoldenValidation`: PASS

**Artifacts:**

- Logs: terminal build/test output; Spark plan, code review, and follow-up confirmation messages
- Screenshots: `build/win_x64_debug/Tests/VisualArtifacts/Debug/ModelViewer/R7_DX11_320x180.ppm`
- Golden: `Tests/Golden/ModelViewer/R7_DX11_320x180.ppm`
- Diffs: failure diff path configured as `build/win_x64_debug/Tests/VisualArtifacts/Debug/ModelViewer/R7_DX11_320x180.diff.ppm`

**Spark plan review result:**

- Verdict: PASS.
- Blockers resolved: N/A
- Non-blocking suggestions adopted: added a stable fallback reason enum, explicit fallback-only tests, and deterministic stats/test seam requirements to the stage plan.

**Spark code review result:**

- Verdict: PASS, confirmed again after adding the proxy creation failure test.
- Blockers resolved: N/A
- Non-blocking suggestions adopted: added `PrimitiveProxyCreationFailed` bridge coverage.
- Non-blocking suggestions deferred: direct `SceneRenderer::SetupView` stats tests and null-world stats/log tests, because constructing the full renderer/context is larger than the R8 bridge seam.

**Notes / follow-ups:**

- `RenderSceneCollector`, `RenderScene::CollectFromWorld`, and `PrimitiveComponent::CollectRenderData` are now legacy fallback paths, not the desired primary renderer collection path.
- Future stages should add direct `SceneRenderer` collection stats tests once a lightweight renderer harness exists.
- Persistent proxy ids, incremental proxy updates, and multithreaded command queues remain future work.
- Old untracked framework/spec documents and `vulkan_pipeline_cache.bin` are intentionally excluded from the R8 commit.
- Next stage must reread the render-first plan and the next documented scope, create/confirm its implementation plan, pass Spark plan review, implement, pass validation including the visual gate, pass Spark code review, update this log, and commit before moving on.

---

### R9a: Render Pass Chain Honesty

**Date:** 2026-06-06
**Commit:** `2f2c077`
**Spark plan review agent:** `019e9b05-8327-79c0-9d06-635963545420`
**Spark code review agent:** `019e9c6d-7f55-74d2-ba77-1c6ea4fc536b`

**Plan source:**

- Document: `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`
- Section: `15. R9 - Render Pass Completion`
- Stage plan: `Docs/superpowers/specs/2026-06-06-r9a-render-pass-chain-honesty-plan.md`
- Lines checked: roadmap lines 485-513 were reread before implementation.

**Prerequisite status:** PASS

- Previous R-SP: R8 RenderProxy-v1 Bridge and Main Path Switch
- Evidence: R8 committed as `4785071` with hash correction committed as `86e392d`; R9a implementation plan passed Spark plan review before code changes.

**Approved scope:**

- Add a uniform `IRenderPass` status contract for requested, supported, enabled, and unsupported reason.
- Add `RenderPassRegistry` status snapshots for registered main-chain passes.
- Expose `SceneRenderer` pass-chain stats for the last graph build.
- Make `SceneRenderer::BuildRenderGraph` add only enabled passes, skip requested unsupported passes visibly, and count disabled/unsupported skips.
- Make `DepthPrepass`, `SkyboxPass`, `ShadowPass`, and `TransparentPass` report honest status through the new contract.
- Register `ShadowPass` in the default main chain as a non-requested, unsupported future production pass.
- Keep `OpaquePass` and `TransparentPass` as supported implemented geometry passes.
- Add `RenderPassValidation` coverage for status semantics, registry snapshots, and built-in production pass honesty.
- Keep the R7 visual gate in the required validation path.

**Out of scope:**

- Completing shadow map rendering, PSSM matrices, or shadow sampling.
- Implementing clustered lighting shader/descriptors.
- Implementing ToneMapping, Bloom, TAA, or IBL algorithms.
- Adding HDR/post-process render targets or ping-pong buffers.
- Changing ModelViewer visuals beyond preserving the existing golden output.
- Removing legacy passes or rewriting draw submission.

**Files changed:**

- `Docs/superpowers/specs/2026-06-06-r9a-render-pass-chain-honesty-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `Render/Include/Render/Passes/IRenderPass.h`
- `Render/Include/Render/Passes/DepthPrepass.h`
- `Render/Include/Render/Passes/ShadowPass.h`
- `Render/Include/Render/Passes/SkyboxPass.h`
- `Render/Include/Render/Passes/TransparentPass.h`
- `Render/Include/Render/Renderer/SceneRenderer.h`
- `Render/Private/Passes/DepthPrepass.cpp`
- `Render/Private/Passes/ShadowPass.cpp`
- `Render/Private/Renderer/RenderFrameResourceBinder.h`
- `Render/Private/Renderer/RenderFrameResourceBinder.cpp`
- `Render/Private/Renderer/RenderPassRegistry.h`
- `Render/Private/Renderer/RenderPassRegistry.cpp`
- `Render/Private/Renderer/SceneRenderer.cpp`
- `Tests/RenderPassValidation/main.cpp`

**Validation commands:**

```powershell
cmake --build build/win_x64_debug --config Debug --target RenderPassValidation RenderHonestyValidation ModelViewer VisualGoldenValidation ImageCompareValidation RenderGraphValidation
build\win_x64_debug\Tests\Debug\RenderPassValidation.exe
build\win_x64_debug\Tests\Debug\RenderHonestyValidation.exe
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "RenderPassValidation|RenderHonestyValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "RenderGraphValidation|RenderHonestyValidation|RenderSceneValidation|RenderPassValidation|MaterialSystemValidation|ResourceInstantiationValidation|PipelineCacheValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
git diff --check
```

**Validation result:**

- Build: PASS
- Tests: PASS
  - `RenderPassValidation`: 12/12 by direct executable
  - `RenderHonestyValidation`: 15/15 by direct executable
  - Required R9a filtered CTest: 29/29
  - Broad render regression CTest: 182/182
  - Post-review spot CTest after snapshot sorting: 10/10
- Visual gate: PASS
  - `ModelViewerSmoke`: PASS
  - `VisualGoldenValidation`: PASS

**Artifacts:**

- Logs: terminal build/test output; Spark plan, code review, and follow-up confirmation messages
- Screenshots: `build/win_x64_debug/Tests/VisualArtifacts/Debug/ModelViewer/R7_DX11_320x180.ppm`
- Golden: `Tests/Golden/ModelViewer/R7_DX11_320x180.ppm`
- Diffs: failure diff path configured as `build/win_x64_debug/Tests/VisualArtifacts/Debug/ModelViewer/R7_DX11_320x180.diff.ppm`

**Spark plan review result:**

- Verdict: PASS.
- Blockers resolved: N/A
- Non-blocking suggestions adopted: documented that R9a checks only registered main-chain passes, added the `SceneRenderer` pass-chain stats accessor to scope, and added `RenderGraphValidation` to explicit build validation.

**Spark code review result:**

- Verdict: PASS, confirmed again after sorting `RenderPassRegistry::GetPassStatuses()`.
- Blockers resolved: N/A
- Non-blocking suggestions adopted: `GetPassStatuses()` now returns a stable priority-sorted snapshot.
- Non-blocking suggestions deferred: bind pass frame resources before support evaluation when R9b makes `ShadowPass` genuinely supported.

**Notes / follow-ups:**

- R9a intentionally does not mark Shadow, Skybox, Bloom, ToneMapping, TAA, clustered lighting, or IBL as implemented.
- `ShadowPass` is now registered in the default pass chain, but it is not requested by default and remains unsupported until its real resources exist.
- Local CTest discovery did not include newly added non-fixture `RenderPassStatusValidation` cases until a fresh configure; direct executable coverage recorded them as 4/4.
- Old untracked framework/spec documents and `vulkan_pipeline_cache.bin` are intentionally excluded from the R9a commit.
- Next R9 substage must reread the render-first plan and R9 scope, create/confirm its implementation plan, pass Spark plan review, implement, pass validation including the visual gate, pass Spark code review, update this log, and commit before moving on.

---

### R9b: Shadow/PSSM Minimum Resource Path

**Date:** 2026-06-06
**Commit:** R9b stage commit created immediately after this log entry
**Spark plan review agent:** `019e9ceb-72f3-7e72-8e3a-d79dd73a8db0`
**Spark code review agent:** `019e9d1c-54cc-7072-9185-de319163a620`
**Spark final follow-up review agent:** `019e9d21-4e99-76f2-88b8-60d05747f2e1`

**Plan source:**

- Document: `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`
- Section: `15. R9 - Render Pass Completion`
- Stage plan: `Docs/superpowers/specs/2026-06-06-r9b-shadow-pssm-minimum-path-plan.md`
- Lines checked: roadmap lines 485-513 were reread before implementation.

**Prerequisite status:** PASS

- Previous R-SP: R9a Render Pass Chain Honesty
- Evidence: R9a committed as `2f2c077` with hash correction committed as `4647949`; R9b implementation plan passed Spark plan review before code changes.

**Approved scope:**

- Add a minimal depth-only shader and `PipelineCache` depth-only graphics pipeline.
- Make `ShadowPass` supported when required render infrastructure exists and a directional shadow light requests it.
- Generate practical minimum PSSM split depths and non-identity light view-projection matrices.
- Declare one transient depth texture per cascade through `RenderGraph` and export it to `ShaderResource` so the enabled shadow pass remains live until later shadow sampling is implemented.
- Resolve cascade depth textures through `RenderGraph` and `ResourceViewCache`, then render shadow-casting geometry with the depth-only pipeline.
- Add per-frame `SceneRenderer` preparation that disables `ShadowPass` by default and enables/configures it only for an eligible directional shadow-casting light.
- Add deterministic `ShadowPass` stats for setup and execution validation.
- Extend `PipelineCacheValidation` and `RenderPassValidation` for depth-only pipeline, PSSM resource declaration, execution, and disabled/no-light behavior.
- Keep ModelViewer visual output unchanged because R9b does not sample shadows in lighting.

**Out of scope:**

- Sampling shadow maps in lighting or material shaders.
- Clustered lighting, IBL, ToneMapping, Bloom, TAA, SSAO, SSR, or Skybox implementation.
- Point/spot shadows, texture arrays, atlas packing, stable texel snapping, cascade blending, or PCF.
- Changing the R7 ModelViewer golden image.
- ECS/Object migration, `SceneEntity` deletion, or multithreaded render proxy command queues.

**Files changed:**

- `Docs/superpowers/specs/2026-06-06-r9b-shadow-pssm-minimum-path-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `Render/Shaders/DepthOnly.hlsl`
- `Render/Include/Render/PipelineCache.h`
- `Render/Private/PipelineCache.cpp`
- `Render/Include/Render/Passes/ShadowPass.h`
- `Render/Private/Passes/ShadowPass.cpp`
- `Render/Include/Render/Renderer/SceneRenderer.h`
- `Render/Private/Renderer/SceneRenderer.cpp`
- `Tests/PipelineCacheValidation/main.cpp`
- `Tests/RenderPassValidation/main.cpp`

**Validation commands:**

```powershell
cmake --build build/win_x64_debug --config Debug --target PipelineCacheValidation RenderPassValidation RenderGraphValidation RenderSceneValidation ModelViewer VisualGoldenValidation ImageCompareValidation
build\win_x64_debug\Tests\Debug\PipelineCacheValidation.exe
build\win_x64_debug\Tests\Debug\RenderPassValidation.exe
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "PipelineCacheValidation|RenderPassValidation|RenderGraphValidation|RenderSceneValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "RenderGraphValidation|RenderHonestyValidation|RenderSceneValidation|RenderPassValidation|MaterialSystemValidation|ResourceInstantiationValidation|PipelineCacheValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
git diff --check
```

**Validation result:**

- Build: PASS
- Tests: PASS
  - `PipelineCacheValidation`: 15/15 by direct executable
  - `RenderPassValidation`: 16/16 by direct executable
  - Required R9b filtered CTest: 77/77
  - Broad render regression CTest: 186/186
  - `git diff --check`: PASS, CRLF warnings only
- Visual gate: PASS
  - `ModelViewerSmoke`: PASS
  - `VisualGoldenValidation`: PASS

**Artifacts:**

- Logs: terminal build/test output; Spark plan, code review, and final follow-up review messages
- Screenshots: `build/win_x64_debug/Tests/VisualArtifacts/Debug/ModelViewer/R7_DX11_320x180.ppm`
- Golden: `Tests/Golden/ModelViewer/R7_DX11_320x180.ppm`
- Diffs: failure diff path configured as `build/win_x64_debug/Tests/VisualArtifacts/Debug/ModelViewer/R7_DX11_320x180.diff.ppm`

**Spark plan review result:**

- Verdict: `PASS_WITH_NON_BLOCKING_SUGGESTIONS`
- Blockers resolved: N/A
- Non-blocking suggestions adopted: documented RenderGraph liveness strategy, backend-sensitive no-pixel-shader fallback contract, and validation command consistency.

**Spark code review result:**

- Verdict: `PASS_WITH_NON_BLOCKING_SUGGESTIONS`
- Blockers resolved: N/A
- Non-blocking suggestions adopted: added disabled/no-light `ShadowPass` regression coverage before final validation.
- Non-blocking suggestions deferred: consider a future line-ending policy cleanup for CRLF warnings; add stronger depth-only input layout documentation or assertions when mesh layout variability grows; add direct `SceneRenderer` no-light frame preparation tests once a lightweight renderer harness exists.

**Notes / follow-ups:**

- R9b makes `ShadowPass` a real RenderGraph-declared minimum resource path, but it does not yet feed shadow maps into lighting.
- The disabled/no-light regression calls `AddToGraph()` directly and verifies no cascade resources or draws are produced; `SceneRenderer` already excludes disabled passes through the R9a pass-chain gate.
- `ShadowPass` exports cascade depth textures to `ShaderResource` to keep the enabled pass live until the later shadow sampling stage consumes them directly.
- Old untracked framework/spec documents and `vulkan_pipeline_cache.bin` are intentionally excluded from the R9b commit.
- Next R9 substage must reread the render-first plan and R9 scope, create/confirm its implementation plan, pass Spark plan review, implement, pass validation including the visual gate, pass Spark code review, update this log, and commit before moving on.

---

### R9c: Clustered Lighting Minimum Buffer Path

**Date:** 2026-06-06
**Commit:** R9c stage commit created immediately after this log entry
**Spark plan review agent:** `019e9d39-7fdc-7e50-bc6b-6b39a6dabcbe`
**Spark code review agent:** `019e9d45-af0b-7d01-9009-f24e97d3dd31`

**Plan source:**

- Document: `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`
- Section: `15. R9 - Render Pass Completion`
- Stage plan: `Docs/superpowers/specs/2026-06-06-r9c-clustered-lighting-minimum-buffer-path-plan.md`
- Lines checked: roadmap lines 485-513 were reread before implementation.

**Prerequisite status:** PASS

- Previous R-SP: R9b Shadow/PSSM Minimum Resource Path
- Evidence: R9b committed as `67c605b`; R9c implementation plan passed Spark plan review before code changes.

**Approved scope:**

- Make `ClusteredLighting` expose honest success/failure through bool-returning lifecycle and per-frame methods.
- Add explicit initialized/frame-begun state and `GetLastError()`.
- Validate cluster dimensions, near/far planes, max lights per cluster, and allocation-size limits before resource creation.
- Create required cluster AABB, cluster data, light index, and cluster constants buffers only after validation succeeds.
- Fix `Reconfigure()` so it preserves the device pointer and does not destroy existing initialized state on invalid config.
- Build deterministic per-frame cluster AABBs, assign point/spot lights, handle empty light sets, and expose deterministic stats.
- Replace silent `RHIBuffer::Upload()` usage with explicit Map/memcpy/Unmap upload paths so map failure and partial upload failure are visible.
- Add `ClusteredLightingValidation` coverage for invalid inputs, buffer descriptors, reconfigure, frame/update ordering, assignment stats, empty lights, uploads, and partial upload failure.
- Keep shader/PipelineCache/SceneRenderer integration out of this sub-stage.

**Out of scope:**

- Binding clustered buffers into descriptor layouts.
- Changing `DefaultLit.hlsl`, `PBRLit.hlsl`, material shaders, or ModelViewer lighting.
- Adding a new render pass to the default `SceneRenderer` pass chain.
- GPU/compute cluster building, shadow sampling, IBL, ToneMapping, Bloom, or TAA.
- ECS/Object migration or RenderProxy changes.

**Files changed:**

- `Docs/superpowers/specs/2026-06-06-r9c-clustered-lighting-minimum-buffer-path-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `Render/Include/Render/Lighting/ClusteredLighting.h`
- `Render/Private/Lighting/ClusteredLighting.cpp`
- `Tests/CMakeLists.txt`
- `Tests/ClusteredLightingValidation/main.cpp`

**Validation commands:**

```powershell
cmake --build build/win_x64_debug --config Debug --target ClusteredLightingValidation RenderGraphValidation RenderSceneValidation RenderPassValidation ModelViewer VisualGoldenValidation ImageCompareValidation
build\win_x64_debug\Tests\Debug\ClusteredLightingValidation.exe
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "ClusteredLightingValidation|RenderGraphValidation|RenderSceneValidation|RenderPassValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "ClusteredLightingValidation|RenderGraphValidation|RenderHonestyValidation|RenderSceneValidation|RenderPassValidation|MaterialSystemValidation|ResourceInstantiationValidation|PipelineCacheValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
git diff --check
```

**Validation result:**

- Build: PASS
- Tests: PASS
  - `ClusteredLightingValidation`: 8/8 by direct executable
  - Required R9c filtered CTest: 70/70
  - Broad render regression CTest: 194/194
  - `git diff --check`: PASS
- Visual gate: PASS
  - `ModelViewerSmoke`: PASS
  - `VisualGoldenValidation`: PASS

**Artifacts:**

- Logs: terminal build/test output; Spark plan and code review messages
- Screenshots: `build/win_x64_debug/Tests/VisualArtifacts/Debug/ModelViewer/R7_DX11_320x180.ppm`
- Golden: `Tests/Golden/ModelViewer/R7_DX11_320x180.ppm`
- Diffs: failure diff path configured as `build/win_x64_debug/Tests/VisualArtifacts/Debug/ModelViewer/R7_DX11_320x180.diff.ppm`

**Spark plan review result:**

- Verdict: `PASS_WITH_NON_BLOCKING_SUGGESTIONS`
- Blockers resolved: N/A
- Non-blocking suggestions adopted: added empty-light assignment regression, explicit reconfigure device-preservation regression, and partial upload failure regression.

**Spark code review result:**

- Verdict: `PASS_WITH_NON_BLOCKING_SUGGESTIONS`
- Blockers resolved: N/A
- Non-blocking suggestions adopted: ran a compatibility grep for the new bool API and confirmed there are no production `ClusteredLighting` call sites to update.
- Non-blocking suggestions deferred: add richer partial-upload state tracking if upper layers begin consuming clustered buffers; add device-replacement semantics if `ClusteredLighting` gains multi-device ownership.

**Notes / follow-ups:**

- R9c creates a verified CPU/GPU buffer path for clustered lighting but does not claim shader-visible clustered lighting output.
- `DefaultLit.hlsl` still uses `ViewConstants.LightDirection`; clustered shader binding remains a later R9 sub-stage.
- Old untracked framework/spec documents and `vulkan_pipeline_cache.bin` are intentionally excluded from the R9c commit.
- Next R9 substage must reread the render-first plan and R9 scope, create/confirm its implementation plan, pass Spark plan review, implement, pass validation including the visual gate, pass Spark code review, update this log, and commit before moving on.

---

### R-SP: `R9d - ToneMapping Fullscreen Minimum Path`

**Date:** 2026-06-06
**Commit:** `336e55d`
**Spark plan review agent:** Mendel (`019e9d4c-0ce8-72f1-8749-5221ed699028`)
**Spark code review agents:** Huygens (`019e9d61-8bf8-7502-a19b-9f8c5df944f8`), Hume (`019e9d6a-ee75-7d62-880d-e2404e209f02`)

**Plan source:**

- Document: `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`
- Section: `15. R9 - Render Pass Completion`
- Lines checked: parent R9 section around lines 485-497; R9d plan document lines 7-104

**Prerequisite status:** PASS

- Previous R-SP: R9c - Clustered Lighting Resource Bridge
- Evidence: R9c committed as `a7d9274`; R9c broad render regression and visual gate passed before R9d planning.

**Approved scope:**

- Add a post-process fullscreen foundation in `PipelineCache`: ToneMapping shader compilation, post-process descriptor set layout, post-process pipeline layout, and a no-input fullscreen ToneMapping graphics pipeline.
- Make `ToneMappingPass` executable only after valid `PipelineCache` and `ResourceViewCache` resources are injected; keep missing-resource paths visibly unsupported or skipped.
- Add RenderGraph read/write declarations and a fullscreen triangle draw path for ToneMapping.
- Add `PostProcessStack` execute stats, honest no-effect/no-work reporting, and multi-pass transient ping-pong texture chaining.
- Keep `PostProcessStack` and ToneMapping out of `SceneRenderer`'s default frame path in R9d so ModelViewer/golden output remains stable.
- Extend `PipelineCache` manifest v2 to track ToneMapping shader and pipeline hashes, adopting Spark's code-review suggestion.

**Out of scope:**

- Bloom, TAA, IBL, skybox/BRDF LUT/environment prefiltering, or post-process stack integration into `SceneRenderer`.
- Replacing the final backbuffer path or changing default ModelViewer visual output.
- ECS/Object refactoring or RenderProxy changes.

**Files changed:**

- `Docs/superpowers/specs/2026-06-06-r9d-tonemapping-fullscreen-minimum-path-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `Render/Include/Render/PipelineCache.h`
- `Render/Private/PipelineCache.cpp`
- `Render/Shaders/PostProcess/ToneMapping.hlsl`
- `Render/Include/Render/PostProcess/ToneMapping.h`
- `Render/Private/PostProcess/ToneMapping.cpp`
- `Render/Include/Render/PostProcess/PostProcessStack.h`
- `Render/Private/PostProcess/PostProcessStack.cpp`
- `Tests/PipelineCacheValidation/main.cpp`
- `Tests/RenderHonestyValidation/main.cpp`
- `Tests/RenderPassValidation/main.cpp`

**Validation commands:**

```powershell
cmake --build build/win_x64_debug --config Debug --target PipelineCacheValidation RenderHonestyValidation RenderPassValidation RenderGraphValidation RenderSceneValidation ModelViewer VisualGoldenValidation ImageCompareValidation
build\win_x64_debug\Tests\Debug\PipelineCacheValidation.exe
build\win_x64_debug\Tests\Debug\RenderHonestyValidation.exe
build\win_x64_debug\Tests\Debug\RenderPassValidation.exe
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "PipelineCacheValidation|RenderHonestyValidation|RenderPassValidation|RenderGraphValidation|RenderSceneValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "ClusteredLightingValidation|RenderGraphValidation|RenderHonestyValidation|RenderSceneValidation|RenderPassValidation|MaterialSystemValidation|ResourceInstantiationValidation|PipelineCacheValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
git diff --check
```

**Validation result:**

- Build: PASS
- Tests: PASS
  - `PipelineCacheValidation`: 18/18 by direct executable
  - `RenderHonestyValidation`: 16/16 by direct executable
  - `RenderPassValidation`: 21/21 by direct executable
  - Required R9d filtered CTest: 99/99
  - Broad render regression CTest: 201/201
  - `git diff --check`: PASS, with only Git CRLF warnings
- Visual gate: PASS
  - `ModelViewerSmoke`: PASS
  - `VisualGoldenValidation`: PASS

**Artifacts:**

- Logs: terminal build/test output; Spark plan review and two Spark code-review messages
- Plan: `Docs/superpowers/specs/2026-06-06-r9d-tonemapping-fullscreen-minimum-path-plan.md`
- Screenshots/golden: unchanged default visual gate path from prior ModelViewer smoke/golden validation

**Spark plan review result:**

- Verdict: `PASS_WITH_NON_BLOCKING_SUGGESTIONS`
- Blockers resolved: N/A
- Non-blocking suggestions adopted: explicitly kept R9d out of `SceneRenderer` default output, added no-effect visibility, descriptor integrity, multi-pass ping-pong, and unchanged-default-output criteria.

**Spark code review result:**

- Verdict: `PASS_WITH_NON_BLOCKING_SUGGESTIONS`
- Blockers resolved: N/A
- Non-blocking suggestions adopted: added ToneMapping shader/pipeline hashes to `PipelineCache` manifest v2 and added `ManifestInvalidatesWhenToneMappingPipelineHashChanges`.
- Incremental re-review verdict: `PASS_WITH_NON_BLOCKING_SUGGESTIONS`
- Non-blocking suggestions deferred: make the manifest test helper match field names by exact line prefix, add a shader-hash-specific manifest invalidation test, and add a destroyed pipeline/layout skip test if later lifecycle work makes that path more relevant.

**Notes / follow-ups:**

- R9d turns ToneMapping into a real RenderGraph-declared fullscreen draw path when resources are injected; it does not claim the full post-process stack is integrated into runtime output.
- Bloom/TAA and IBL remain the next R9 feature candidates and must still follow the same per-stage plan, Spark plan review, implementation, validation, Spark code review, phase-log, and commit protocol.
- Old untracked framework/spec documents and `vulkan_pipeline_cache.bin` are intentionally excluded from the R9d commit.

---

### R-SP: `R9e - Bloom Fullscreen Minimum Path`

**Date:** 2026-06-06
**Commit:** `65d0af9`
**Spark plan review agent:** Wegener (`019e9d75-4f0f-7580-b251-266b34f58fc8`)
**Spark code review agents:** Dewey (`019e9d8a-640c-73c3-b5b1-2b1c2c16deac`), Noether (`019e9d94-362a-7e91-b9d3-9f068ff1a468`)

**Plan source:**

- Document: `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`
- Section: `15. R9 - Render Pass Completion`
- Lines checked: parent R9 section around lines 485-497; R9e plan document lines 1-121

**Prerequisite status:** PASS

- Previous R-SP: R9d - ToneMapping Fullscreen Minimum Path
- Evidence: R9d committed as `336e55d` with follow-up log correction `2f4c9b7`; R9d validation and visual gate passed before R9e planning.

**Approved scope:**

- Turn Bloom from an unsupported/stub graph pass into a minimum fullscreen post-process path when resources are injected.
- Compile `PostProcess/Bloom.hlsl` VS/PS in `PipelineCache`, create the sixth graphics pipeline, and expose `GetBloomPipeline()`.
- Upgrade the pipeline manifest to v3 and include Bloom VS, PS, and pipeline hashes in strict manifest validation, matching, writing, and invalidation tests.
- Use the existing post-process descriptor convention: constants at `b0`, input texture at `t1`, sampler at `s2`.
- Keep Bloom honest when resources are missing: unsupported before `SetResources()`, visible skips during execution, and no silent fallback.
- Add Bloom graph read/write declarations, descriptor binding, render-pass setup, fullscreen `Draw(3)`, and Bloom-before-ToneMapping stack order coverage.
- Adopt Spark code-review suggestions by correcting the `BloomPass` header comment to describe the minimum fullscreen path and removing the inactive mip-count API.

**Out of scope:**

- Full mip-chain downsample/upsample Bloom, compute Bloom, lens dirt, temporal stabilization, or backend-specific quality tuning.
- Integrating Bloom or `PostProcessStack` into `SceneRenderer`'s default frame output.
- TAA, IBL, skybox/BRDF LUT/environment prefiltering, ECS/Object refactoring, or RenderProxy changes.

**Files changed:**

- `Docs/superpowers/specs/2026-06-06-r9e-bloom-fullscreen-minimum-path-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `Render/Include/Render/PipelineCache.h`
- `Render/Private/PipelineCache.cpp`
- `Render/Shaders/PostProcess/Bloom.hlsl`
- `Render/Include/Render/PostProcess/Bloom.h`
- `Render/Private/PostProcess/Bloom.cpp`
- `Tests/PipelineCacheValidation/main.cpp`
- `Tests/RenderHonestyValidation/main.cpp`
- `Tests/RenderPassValidation/main.cpp`

**Validation commands:**

```powershell
cmake --build build/win_x64_debug --config Debug --target PipelineCacheValidation RenderHonestyValidation RenderPassValidation
build\win_x64_debug\Tests\Debug\PipelineCacheValidation.exe
build\win_x64_debug\Tests\Debug\RenderHonestyValidation.exe
build\win_x64_debug\Tests\Debug\RenderPassValidation.exe
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "PipelineCacheValidation|RenderHonestyValidation|RenderPassValidation|RenderGraphValidation|RenderSceneValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
cmake --build build/win_x64_debug --config Debug --target PipelineCacheValidation RenderHonestyValidation RenderPassValidation RenderGraphValidation RenderSceneValidation ModelViewer VisualGoldenValidation ImageCompareValidation ClusteredLightingValidation MaterialSystemValidation ResourceInstantiationValidation
build\win_x64_debug\Tests\Debug\ClusteredLightingValidation.exe --gtest_filter=ClusteredLightingValidationFixture.ReconfigurePreservesDeviceAndRebuildsBuffers
build\win_x64_debug\Tests\Debug\ClusteredLightingValidation.exe
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "ClusteredLightingValidation|RenderGraphValidation|RenderHonestyValidation|RenderSceneValidation|RenderPassValidation|MaterialSystemValidation|ResourceInstantiationValidation|PipelineCacheValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
git diff --check
rg "SetMipCount|GetMipCount|m_mipCount" .
```

**Validation result:**

- Build: PASS
- Tests: PASS
  - `PipelineCacheValidation`: 21/21 by direct executable
  - `RenderHonestyValidation`: 17/17 by direct executable
  - `RenderPassValidation`: 25/25 by direct executable
  - Required R9e filtered CTest: 107/107
  - Broad render/resource regression CTest: 209/209 on final rerun
  - One earlier broad run reported `ClusteredLightingValidationFixture.ReconfigurePreservesDeviceAndRebuildsBuffers`; the failing test then passed standalone, the full `ClusteredLightingValidation` executable passed 8/8, and the final broad CTest passed 209/209.
  - `git diff --check`: PASS, with only Git CRLF warnings
  - `rg "SetMipCount|GetMipCount|m_mipCount" .`: no matches after removing the inactive Bloom mip-count API
- Visual gate: PASS
  - `ModelViewerSmoke`: PASS
  - `VisualGoldenValidation`: PASS

**Artifacts:**

- Logs: terminal build/test output; Spark plan review, Spark code review, and Spark incremental code-review messages
- Plan: `Docs/superpowers/specs/2026-06-06-r9e-bloom-fullscreen-minimum-path-plan.md`
- Screenshots/golden: unchanged default visual gate path from prior ModelViewer smoke/golden validation

**Spark plan review result:**

- Verdict: `PASS_WITH_NON_BLOCKING_SUGGESTIONS`
- Blockers resolved: N/A
- Non-blocking suggestions adopted: rewrote the existing Bloom shader as a minimum fullscreen path, used the `b0/t1/s2` post-process descriptor convention, upgraded manifest coverage to v3 with Bloom hashes, treated Bloom as the sixth pipeline, added Bloom-to-ToneMapping ordering coverage, and kept mip-chain semantics out of R9e.

**Spark code review result:**

- Verdict: `PASS_WITH_NON_BLOCKING_SUGGESTIONS`
- Blockers resolved: N/A
- Non-blocking suggestions adopted: updated `BloomPass` documentation to say minimum fullscreen approximation and removed the inactive `m_mipCount` API.
- Incremental re-review verdict: `PASS_WITH_NON_BLOCKING_SUGGESTIONS`
- Non-blocking suggestions deferred: centralize runtime resource injection for Bloom/ToneMapping when the post-process stack is integrated into the runtime frame path.

**Notes / follow-ups:**

- R9e makes Bloom a real RenderGraph-declared fullscreen draw path when resources are injected; it still does not claim a full production mip-chain Bloom.
- `PostProcessStack` still relies on callers to inject resources into Bloom/ToneMapping before graph submission; this is acceptable for R9e and should be addressed before runtime integration.
- TAA and IBL remain R9 candidates and must follow the same per-stage plan, Spark plan review, implementation, validation including visual gate, Spark code review, phase-log, and commit protocol.
- Old untracked framework/spec documents and `vulkan_pipeline_cache.bin` are intentionally excluded from the R9e commit.

---

### R-SP: `R9f - IBL-Approximate Ambient Minimum Path`

**Date:** 2026-06-06
**Commit:** `216153f`
**Spark plan review agent:** Popper (`019e9db5-178f-7633-ac79-4f8b4d129e10`)
**Spark code review agent:** Kant (`019e9dbc-25c0-7c12-97ad-aff6d552a083`)

**Plan source:**

- Document: `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`
- Section: `15. R9 - Render Pass Completion`
- Lines checked: parent R9 section around lines 485-497; R9e phase-log notes around lines 1873 and 1981; R9f plan document lines 1-111

**Prerequisite status:** PASS

- Previous R-SP: R9e Bloom fullscreen minimum path
- Evidence: R9e committed as `65d0af9` with follow-up log correction `6817d18`; R9e validation and visual gate passed before R9f planning.

**Approved scope:**

- Add explicit IBL-approximate ambient settings to `ViewData` with defaults matching the old `DefaultLit` hard-coded ambient constants.
- Append diffuse/specular IBL-approximate ambient values to `PipelineCache::ViewConstants` without reordering existing fields.
- Upload default, custom, and disabled IBL-approximate ambient values through `PipelineCache::UpdateViewConstants()`.
- Replace `DefaultLit.hlsl` hidden ambient magic constants with view constants while preserving default ModelViewer output.
- Keep `Lighting.hlsli` comments honest: IBL-approximate only, no cubemap/irradiance/prefilter/BRDF LUT sampling.
- Add C++ layout/upload tests and shader-source guardrails.

**Out of scope:**

- Full cubemap IBL, irradiance, prefiltered environment, or BRDF LUT descriptor bindings/sampling.
- GPU IBL convolution passes.
- Making `SkyboxPass` draw or connecting `SkyboxComponent` to `ViewData`.
- TAA or additional post-process integration.
- Changing ModelViewer default visual output.

**Files changed:**

- `Docs/superpowers/specs/2026-06-06-r9f-ibl-ambient-minimum-path-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `Render/Include/Render/Renderer/ViewData.h`
- `Render/Include/Render/PipelineCache.h`
- `Render/Private/PipelineCache.cpp`
- `Render/Shaders/DefaultLit.hlsl`
- `Render/Shaders/Include/Lighting.hlsli`
- `Tests/PipelineCacheValidation/main.cpp`

**Validation commands:**

```powershell
cmake --build build/win_x64_debug --config Debug --target PipelineCacheValidation RenderPassValidation RenderSceneValidation ModelViewer VisualGoldenValidation ImageCompareValidation
build\win_x64_debug\Tests\Debug\PipelineCacheValidation.exe
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "PipelineCacheValidation|RenderPassValidation|RenderSceneValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "ClusteredLightingValidation|RenderGraphValidation|RenderHonestyValidation|RenderSceneValidation|RenderPassValidation|MaterialSystemValidation|ResourceInstantiationValidation|PipelineCacheValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
cmake --build build/win_x64_debug --config Debug --target PipelineCacheValidation
build\win_x64_debug\Tests\Debug\PipelineCacheValidation.exe
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "PipelineCacheValidation|RenderPassValidation|RenderSceneValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
git diff --check
```

**Validation result:**

- Build: PASS
- Tests: PASS
  - `PipelineCacheValidation`: 25/25 by direct executable after final suggestion adoption
  - Required R9f filtered CTest: 67/67 after final suggestion adoption
  - Broad render/resource regression CTest: 213/213 before final non-blocking doc/test suggestion adoption
  - `git diff --check`: PASS, with only Git CRLF warnings
- Visual gate: PASS
  - `ModelViewerSmoke`: PASS
  - `VisualGoldenValidation`: PASS

**Artifacts:**

- Logs: terminal build/test output; Spark plan review, Spark code review, and Spark incremental/final code-review messages
- Plan: `Docs/superpowers/specs/2026-06-06-r9f-ibl-ambient-minimum-path-plan.md`
- Screenshots/golden: unchanged default visual gate path from ModelViewer smoke/golden validation

**Spark plan review result:**

- Verdict: `PASS_WITH_NON_BLOCKING_SUGGESTIONS`
- Blockers resolved: N/A
- Non-blocking suggestions adopted: narrowed the stage name and wording to IBL-approximate ambient, explicitly excluded full cubemap IBL sampling, documented the default formula, aligned `Lighting.hlsli` comments, and added C++ cbuffer offset/layout tests.

**Spark code review result:**

- Verdict: `PASS_WITH_NON_BLOCKING_SUGGESTIONS`
- Blockers resolved: N/A
- Non-blocking suggestions adopted: changed `ViewData::iblAmbientEnabled` from `bool` to `uint8`, added `Core/Types.h`, documented `Lighting.hlsli` independence from `DefaultLit` view constants, fixed the plan document current-state wording, and added a non-zero `uint8` enable-value upload test.
- Final incremental re-review verdict: `PASS`

**Notes / follow-ups:**

- R9f is an honest minimum path: it parameterizes ambient lighting through view constants but does not claim complete texture IBL.
- Full IBL texture sampling, Skybox draw integration, and TAA remain future R9 or later candidates and must follow the same phase protocol.
- Old untracked framework/spec documents and `vulkan_pipeline_cache.bin` are intentionally excluded from the R9f commit.

---

### R9g: SceneRenderer PostProcess Runtime Integration

**Date:** 2026-06-06
**Commit:** `0645fe6`
**Spark plan review agent:** Popper (`019e9db5-178f-7633-ac79-4f8b4d129e10`)
**Spark code review agent:** Kant (`019e9dbc-25c0-7c12-97ad-aff6d552a083`)

**Plan source:**

- Document: `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`
- Section: R9 shading/post-process path (`ToneMapping`, `Bloom/TAA`, IBL, declared pass chain, ModelViewer visual gate)
- Stage plan: `Docs/superpowers/specs/2026-06-06-r9g-scene-renderer-postprocess-runtime-integration-plan.md`
- Lines checked: R9 section and prior R9e/R9f phase-log notes before implementation

**Prerequisite status:** PASS

- Previous R-SP: R9f IBL-Approx Ambient Minimum Path
- Evidence: R9f implementation and log-correction commits are present; R9f narrow and broad render/resource validations passed.

**Approved scope:**

- Add a runtime `PostProcessStack` owned by `SceneRenderer`.
- Register supported runtime post-process effects (`BloomPass`, `ToneMappingPass`) with real `PipelineCache` and `ResourceViewCache` resources.
- Evaluate requested/supported/enabled effects before building the graph and expose runtime post-process stats.
- When at least one requested, supported, enabled effect exists, render scene passes into a transient scene-color staging target and execute the post-process stack into the imported backbuffer.
- Preserve backbuffer import/export to `Present`.
- Resolve graph-owned color targets inside `OpaquePass`, `TransparentPass`, and `SkyboxPass` execution so declared RenderGraph writes match actual render target views.
- Keep the default visual baseline pass-through: bloom is requested with intensity `0.0`, tone mapping is requested with `ToneMappingOperator::None`.
- Add R9g tests for effect evaluation, unsupported resource accounting, and graph RTV resolution.

**Out of scope:**

- Full post-process settings UI or editor integration.
- TAA, FXAA, SSR, SSAO, motion blur, DOF, color grading, vignette, chromatic aberration, film grain, or volumetric lighting runtime implementation.
- Full cubemap IBL or skybox draw integration.
- Deleting legacy render paths.
- Rebaseline of ModelViewer golden images.

**Files changed:**

- `Docs/superpowers/specs/2026-06-06-r9g-scene-renderer-postprocess-runtime-integration-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `Render/Include/Render/PostProcess/PostProcessStack.h`
- `Render/Private/PostProcess/PostProcessStack.cpp`
- `Render/Include/Render/Renderer/SceneRenderer.h`
- `Render/Private/Renderer/SceneRenderer.cpp`
- `Render/Private/Passes/OpaquePass.cpp`
- `Render/Private/Passes/TransparentPass.cpp`
- `Render/Private/Passes/SkyboxPass.cpp`
- `Render/Shaders/PostProcess/Bloom.hlsl`
- `Render/Shaders/PostProcess/ToneMapping.hlsl`
- `Tests/RenderPassValidation/main.cpp`

**Validation commands:**

```powershell
cmake --build build/win_x64_debug --config Debug --target RenderPassValidation RenderGraphValidation PipelineCacheValidation RenderSceneValidation ModelViewer VisualGoldenValidation ImageCompareValidation
build\win_x64_debug\Tests\Debug\RenderPassValidation.exe
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "RenderPassValidation|RenderGraphValidation|PipelineCacheValidation|RenderSceneValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "ClusteredLightingValidation|RenderGraphValidation|RenderHonestyValidation|RenderSceneValidation|RenderPassValidation|MaterialSystemValidation|ResourceInstantiationValidation|PipelineCacheValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
git diff --check
cmake --build build/win_x64_debug --config Debug --target RenderPassValidation ModelViewer
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "RenderPassValidation|ModelViewerSmoke|VisualGoldenValidation"
```

**Validation result:**

- Build: PASS
- Tests: PASS
  - `RenderPassValidation`: 28/28 by direct executable
  - Required R9g filtered CTest: 96/96
  - Broad render/resource regression CTest: 215/215
  - Final quick regression after include-order cleanup: 23/23
  - `git diff --check`: PASS, with only Git CRLF warnings
- Visual gate: PASS
  - `ModelViewerSmoke`: PASS
  - `VisualGoldenValidation`: PASS

**Artifacts:**

- Logs: terminal build/test output; Spark plan review and Spark code review messages
- Plan: `Docs/superpowers/specs/2026-06-06-r9g-scene-renderer-postprocess-runtime-integration-plan.md`
- Screenshots/golden: unchanged default visual gate path from ModelViewer smoke/golden validation

**Spark plan review result:**

- Verdict: `PASS`
- Blockers resolved: clarified scene-color staging vs stack transient intermediates, default enabled-effect semantics, runtime stats, Present export preservation, and fallback/direct behavior.

**Spark code review result:**

- Verdict: `PASS_WITH_NON_BLOCKING_SUGGESTIONS`
- Blockers resolved: N/A
- Commit readiness: yes
- Non-blocking suggestions retained for a later test-hardening pass: SceneRenderer-level staging/direct branch regression tests, Transparent/Skybox graph RTV fallback tests, and additional EvaluateEffects semantics tests.

**Notes / follow-ups:**

- R9g connects the already implemented Bloom/ToneMapping passes into the runtime SceneRenderer graph without changing the visual golden baseline.
- `SceneRenderer` now has observable post-process stats, including scene-color staging, direct-to-backbuffer state, and stack requested/enabled/unsupported counters.
- `OpaquePass`, `TransparentPass`, and `SkyboxPass` now use RenderGraph-resolved RTVs during execution when graph handles are active; this prevents scene-color staging from remaining black.
- Old untracked framework/spec documents and `vulkan_pipeline_cache.bin` are intentionally excluded from the R9g commit.

---

### R10a: Particle Rendering Honesty Hardening

**Date:** 2026-06-06
**Commit:** `14436d4`
**Spark plan review agent:** Popper (`019e9db5-178f-7633-ac79-4f8b4d129e10`)
**Spark code review agent:** Kant (`019e9dbc-25c0-7c12-97ad-aff6d552a083`)

**Plan source:**

- Document: `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`
- Section: R10 - Particle Rendering Honesty and Minimal Visual Path
- Stage plan: `Docs/superpowers/specs/2026-06-06-r10a-particle-rendering-honesty-plan.md`
- Lines checked: execution rules, R10 scope/done criteria, and final visual-gate requirements before implementation

**Prerequisite status:** PASS

- Previous R-SP: R9g SceneRenderer PostProcess Runtime Integration
- Evidence: R9g implementation and log-correction commits are present; R9g narrow, broad, ModelViewer smoke, and visual golden validations passed.

**Approved scope:**

- Select the R10 explicit unsupported path rather than CPU fallback visible path.
- Make `ParticlePass::GetStatus()` honest by overriding `IsSupported()` and `GetUnsupportedReason()`.
- Keep `ParticlePass::IsEnabled()` false unless the pass is requested, supported, and has renderable batches.
- Make `ParticlePass::Setup()` copy current `ViewData` color/depth handles and declare no graph resources when unsupported.
- Change `ParticleRenderer::DrawParticles()` and `DrawParticlesIndirect()` to return `bool`, with `true` only when a draw command is actually submitted.
- Return `false` for unsupported renderer, missing instance/system, zero alive particles, missing simulator, missing render buffers, missing pipeline, or missing indirect draw buffer.
- Extend `RenderHonestyValidation` with particle status, draw-return, and unsupported RenderGraph setup tests.

**Out of scope:**

- CPU particle fallback simulation connection.
- Particle graphics pipeline creation.
- Shader descriptor binding for particle buffers, alive index buffers, textures, depth, or constants.
- Particle sample scene or visual golden rebaseline.
- Integrating `ParticlePass` into `SceneRenderer`'s production pass chain.
- Full particle authoring, editor panels, or advanced simulation.

**Files changed:**

- `Docs/superpowers/specs/2026-06-06-r10a-particle-rendering-honesty-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `Particle/Include/Particle/Rendering/ParticleRenderer.h`
- `Particle/Private/Rendering/ParticleRenderer.cpp`
- `Particle/Include/Particle/Rendering/ParticlePass.h`
- `Particle/Private/Rendering/ParticlePass.cpp`
- `Tests/RenderHonestyValidation/main.cpp`

**Validation commands:**

```powershell
cmake --build build/win_x64_debug --config Debug --target RenderHonestyValidation RenderPassValidation RenderSceneValidation ModelViewer VisualGoldenValidation ImageCompareValidation
build\win_x64_debug\Tests\Debug\RenderHonestyValidation.exe
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "RenderHonestyValidation|RenderPassValidation|RenderSceneValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "ClusteredLightingValidation|RenderGraphValidation|RenderHonestyValidation|RenderSceneValidation|RenderPassValidation|MaterialSystemValidation|ResourceInstantiationValidation|PipelineCacheValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
cmake --build build/win_x64_debug --config Debug --target RenderHonestyValidation
build\win_x64_debug\Tests\Debug\RenderHonestyValidation.exe
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "RenderHonestyValidation|RenderPassValidation|RenderSceneValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
git diff --check
```

**Validation result:**

- Build: PASS
- Tests: PASS
  - `RenderHonestyValidation`: 19/19 by direct executable after final Spark suggestion adoption
  - Required R10a filtered CTest: 63/63 after final Spark suggestion adoption
  - Broad render/resource regression CTest: 217/217 before final non-blocking suggestion adoption
  - `git diff --check`: PASS, with only Git CRLF warnings
- Visual gate: PASS
  - `ModelViewerSmoke`: PASS
  - `VisualGoldenValidation`: PASS

**Artifacts:**

- Logs: terminal build/test output; Spark plan review, Spark code review, and Spark incremental/final code-review messages
- Plan: `Docs/superpowers/specs/2026-06-06-r10a-particle-rendering-honesty-plan.md`
- Screenshots/golden: unchanged default visual gate path from ModelViewer smoke/golden validation

**Spark plan review result:**

- Verdict: `PASS_WITH_NON_BLOCKING`
- Blockers resolved: N/A
- Non-blocking suggestions adopted: status assertion for `ParticlePass::GetStatus().supported == false`, unsupported setup no-resource test, and draw-return false tests for disconnected renderer paths.

**Spark code review result:**

- Verdict: `PASS_WITH_NON_BLOCKING`
- Blockers resolved: N/A
- Non-blocking suggestions adopted: added `DrawParticlesIndirect()` zero-alive-count false return for symmetry with direct draw.
- Final incremental re-review verdict: `PASS`

**Notes / follow-ups:**

- R10a completes the explicit unsupported branch of R10; it does not claim a visible particle rendering path.
- A future CPU fallback visible path must be its own gated phase because it needs simulator connection, particle graphics pipelines, descriptor bindings, and sample visual coverage.
- `ParticlePass` now avoids false pass-chain support reporting and avoids declaring RenderGraph resources when disconnected.
- Old untracked framework/spec documents and `vulkan_pipeline_cache.bin` are intentionally excluded from the R10a commit.

---

### R-SP: `R12 - Final ModelViewer Validation`

**Date:** 2026-06-06
**Commit:** this R12 stage commit
**Spark plan review agent:** Sagan
**Spark code review agent:** Sagan

**Plan source:**

- Document: `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`
- Section: `17. R11 - GPU-Driven Optional Advanced`; `18. R12 - Final ModelViewer Validation`
- Lines checked: R11/R12 source block around lines 542-585

**Prerequisite status:** PASS

- Previous R-SP: `R10a - Particle Rendering Honesty Hardening`
- Evidence: R10a implementation and log correction commits completed; current stage re-read the render-first roadmap before starting.

**Approved scope:**

- Defer R11 because it is P2/L optional GPU-driven advanced work and not required for final ModelViewer acceptance.
- Build and validate Release `ModelViewer`.
- Run Release CTest and focused Release render validation.
- Run manual Release ModelViewer smoke against the fixed R7 glTF fixture.
- Record backend, GPU/driver, resolution, fixture, commands, logs, screenshots, and render path.
- Fix R12 validation blockers discovered during acceptance if they directly affect the final gate.

**Out of scope:**

- Starting R11 GPU-driven culling, compaction, indirect draw, or bindless work.
- Full DX12/Vulkan screenshot acceptance; current ModelViewer screenshot readback remains DX11-only.
- New rendering features beyond targeted R12 gate fixes.

**Files changed:**

- `Docs/superpowers/specs/2026-06-06-r12-final-modelviewer-validation-plan.md`
- `Docs/superpowers/specs/2026-06-06-r12-final-modelviewer-validation-report.md`
- `Docs/superpowers/specs/phase-log.md`
- `RHI_DX11/Private/DX11Resources.cpp`
- `RHI_DX11/Private/DX11Resources.h`
- `Tests/DX11Validation/main.cpp`
- `Tests/RenderPassValidation/main.cpp`

**Validation commands:**

```powershell
Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,AdapterCompatibility
cmake --build build\win_x64_debug --config Release
cmake --build build\win_x64_debug --config Release --target DX11Validation ModelViewer VisualGoldenValidation ImageCompareValidation
cmake --build build\win_x64_debug --config Debug --target DX11Validation
build\win_x64_debug\Tests\Release\DX11Validation.exe --gtest_filter=DX11Validation.UploadCopySourceBufferMaps
build\win_x64_debug\Tests\Debug\DX11Validation.exe --gtest_filter=DX11Validation.UploadCopySourceBufferMaps
build\win_x64_debug\Tests\Release\DX11Validation.exe
ctest --test-dir build\win_x64_debug -C Release --output-on-failure -O build\win_x64_debug\R12Artifacts\Logs\release-full-ctest.log
ctest --test-dir build\win_x64_debug -C Release --output-on-failure -O build\win_x64_debug\R12Artifacts\Logs\release-focused-render-ctest.log -R "DX11Validation|ClusteredLightingValidation|RenderGraphValidation|RenderHonestyValidation|RenderSceneValidation|RenderPassValidation|MaterialSystemValidation|ResourceInstantiationValidation|PipelineCacheValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
build\win_x64_debug\Samples\ModelViewer\Release\ModelViewer.exe --smoke --model Tests\Fixtures\ModelViewer\R7Triangle.gltf --backend dx11 --width 320 --height 180 --frames 8 --screenshot build\win_x64_debug\Tests\VisualArtifacts\Release\ModelViewer\R12_DX11_320x180.ppm --validation
build\win_x64_debug\Tests\Release\VisualGoldenValidation.exe --expected Tests\Golden\ModelViewer\R7_DX11_320x180.ppm --actual build\win_x64_debug\Tests\VisualArtifacts\Release\ModelViewer\R12_DX11_320x180.ppm --diff build\win_x64_debug\Tests\VisualArtifacts\Release\ModelViewer\R12_DX11_320x180.diff.ppm --tolerance 0.0 --max-different-pixels 0
git diff --check
```

**Validation result:**

- Build: PASS
- Tests: PASS
  - Release full CTest: 434/434
  - Focused Release render CTest: 238/238
  - Release `DX11Validation.exe`: 20/20
  - `DX11Validation.UploadCopySourceBufferMaps`: PASS in Debug and Release
  - `git diff --check`: PASS, with only Git CRLF warnings
- Visual gate: PASS
  - `ModelViewerSmoke`: PASS
  - `VisualGoldenValidation`: PASS
  - Manual ModelViewer smoke: PASS; final log has no `[error]` lines
  - Manual golden compare: MSE 0, PSNR 100, differing pixels 0

**Artifacts:**

- Report: `Docs/superpowers/specs/2026-06-06-r12-final-modelviewer-validation-report.md`
- Full Release CTest log: `build/win_x64_debug/R12Artifacts/Logs/release-full-ctest.log`
- Focused Release render CTest log: `build/win_x64_debug/R12Artifacts/Logs/release-focused-render-ctest.log`
- Manual ModelViewer log: `build/win_x64_debug/R12Artifacts/Logs/manual-modelviewer-dx11.log`
- Manual golden comparison log: `build/win_x64_debug/R12Artifacts/Logs/manual-golden-compare.log`
- Screenshot: `build/win_x64_debug/Tests/VisualArtifacts/Release/ModelViewer/R12_DX11_320x180.ppm`

**Spark plan review result:**

- Verdict: `PASS_WITH_NON_BLOCKING`
- Blockers resolved: N/A
- Non-blocking suggestions adopted: explicit R11 deferral rationale, DX11 visual-backend exception note, and saved validation log paths.

**Spark code review result:**

- First verdict: `BLOCKED` on procedural documentation gates only.
- Code blockers: none reported.
- Procedural blockers addressed in this entry and the R12 report.
- Final incremental re-review verdict: `PASS_WITH_NON_BLOCKING`.
- Remaining blockers: none.

**Notes / follow-ups:**

- R11 remains deferred as optional advanced GPU-driven work.
- DX11 is the final visual backend because current ModelViewer screenshot readback explicitly requires DX11.
- DX12/Vulkan remain covered by Release backend and cross-backend tests; screenshot acceptance for those backends remains a future RHI readback task.
- R12 validation exposed and fixed a DX11 `Upload + CopySrc` buffer creation issue that polluted ModelViewer logs with default material texture upload errors.
- `RenderPassValidationFixture.NoSupportedEffectsReportsNoWork` now verifies the no-effect post-process no-work path under the fixture logger lifecycle.

---

### RQ1-A/B/C/D/E: HDR Scene Color and ToneMapping Format Split

**Date:** 2026-06-07
**Commit:** this commit
**Spark plan review agent:** `019ea086-5b9a-7fc0-b402-944a98d76842`
**Spark code review agent:** `019ea0a2-cfb9-7dd2-b38a-b5e84eee981e`, `019ea0b1-6cef-77b1-b42f-e9870002c78b`

**Plan source:**

- Document: user-approved RQ1 implementation plan in this thread.
- Section: RQ1-A/B/C/D/E.
- Lines checked: local code evidence for `SceneRenderer`, `PipelineCache`, `PostProcessStack`, and `ToneMapping.hlsl`.

**Prerequisite status:** PASS

- Previous R-SP: R12 / Render Program v1 visual baseline.
- Evidence: Spark plan review reported no direction blocker after requiring PipelineCache format split before graph edits.

**Approved scope:**

- Add explicit scene/post-process/tone-mapping render target format policy.
- Route scene/material and Bloom pipelines to HDR scene/intermediate formats.
- Route ToneMapping final pipeline to the LDR backbuffer format.
- Create HDR scene color when supported post-process is active.
- Expose HDR fallback and selected formats in `SceneRenderPostProcessStats`.
- Add PostProcessStack stats for intermediate/final formats and ToneMapping boundary validity.
- Keep a single ToneMapping display conversion for the current UNORM backbuffer path.
- Refresh the DX11 visual golden after verified HDR/tonemap output changes.

**Out of scope:**

- IBL, ACES/AgX operator switch, auto exposure, sRGB swapchain rewrite, Vulkan golden, and dynamic pipeline recreation when post-process is disabled after initialization.

**Files changed:**

- `Render/Include/Render/PipelineCache.h`
- `Render/Private/PipelineCache.cpp`
- `Render/Include/Render/Renderer/SceneRenderer.h`
- `Render/Private/Renderer/SceneRenderer.cpp`
- `Render/Private/Passes/OpaquePass.cpp`
- `Render/Private/Passes/TransparentPass.cpp`
- `Render/Include/Render/PostProcess/PostProcessStack.h`
- `Render/Private/PostProcess/Bloom.cpp`
- `Render/Private/PostProcess/PostProcessStack.cpp`
- `Render/Private/PostProcess/ToneMapping.cpp`
- `Render/Shaders/PostProcess/ToneMapping.hlsl`
- `Tests/ClusteredLightingValidation/main.cpp`
- `Tests/PipelineCacheValidation/main.cpp`
- `Tests/RenderPassValidation/main.cpp`
- `Tests/Golden/ModelViewer/R7_DX11_320x180.ppm`

**Validation commands:**

```powershell
cmake --build build\win_x64_debug --config Debug --target PipelineCacheValidation RenderPassValidation
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "PipelineCacheValidation|RenderPassValidation"
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "RenderGraphValidation|RenderHonestyValidation|RenderSceneValidation|MaterialSystemValidation|ClusteredLightingValidation"
cmake --build build\win_x64_debug --config Debug --target ModelViewer VisualGoldenValidation ImageCompareValidation
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
cmake --build build\win_x64_debug --config Debug --target ClusteredLightingValidation
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "ClusteredLightingValidation"
git diff --check
```

**Validation result:**

- Build: PASS.
- Tests: PASS for direct and regression gates listed above.
- Visual gate: PASS after refreshing `R7_DX11_320x180.ppm` from the verified HDR/tonemap output.

**Artifacts:**

- Actual golden source: `build/win_x64_debug/Tests/VisualArtifacts/Debug/ModelViewer/R7_DX11_320x180.ppm`
- Temporary preview: `build/win_x64_debug/Tests/VisualArtifacts/Debug/ModelViewer/R7_DX11_320x180.bmp`

**Spark plan review result:**

- Verdict: PASS after requiring `SceneColorFormatPolicy` and `PipelineCache` split formats before `BuildRenderGraph`/`PostProcessStack` changes.
- Blockers resolved: single `m_renderTargetFormat` was split for scene, post-process intermediate, and ToneMapping output formats; manifest/config fields were expanded.

**Spark code review result:**

- Verdict: PASS.
- Blockers resolved: runtime output-format mismatch was fixed by selecting scene, Bloom, and ToneMapping pipelines from the active RenderGraph target format instead of only using initialization-time primary formats.

**Notes / follow-ups:**

- Current HDR support policy is renderer-level and conservative for `None`/`Auto`; a real per-format RHI capability query remains a future infrastructure item.
- Dynamic runtime pipeline format requests are supported through PipelineCache lazy creation; full per-format RHI capability queries remain future work.
- ToneMapping still uses the current `None` operator default in `SceneRenderer`; RQ1 fixes the HDR/display path, not the filmic operator choice.
- `ClusteredLightingValidationFixture.ReconfigurePreservesDeviceAndRebuildsBuffers` no longer relies on allocator pointer uniqueness and now validates rebuild by creation count and buffer sizes.

---

### R-SP: `RQ2a - RHI Cubemap Subresource and Upload Foundation`

**Date:** 2026-06-07
**Commit:** `970d5bd feat(render): support cubemap texture upload foundation`
**Spark plan review agent:** Russell / Heisenberg (`gpt-5.3-codex-spark`)
**Spark code review agent:** Heisenberg (`gpt-5.3-codex-spark`)

**Plan source:**

- Document: `Docs/superpowers/specs/2026-06-07-rq2a-rhi-cubemap-upload-foundation-plan.md`
- Section: full document, especially §1.1 Binding Indexing Contract and §1.2 Blocker Resolution Order
- Lines checked: current RQ2a plan before implementation

**Prerequisite status:** PASS

- Previous R-SP: `RQ1 - HDR Scene Color and ToneMapping Format Split`
- Evidence: RQ1 committed as `2a2c7bd feat(render): render scene color in HDR before tonemap`; RQ2a plan reviewed by Spark before implementation.

**Approved scope:**

- Define RHI texture physical-layer/subresource helpers and document `TextureCube.arraySize` as cube count.
- Align DX11/DX12/Vulkan/Metal/OpenGL cubemap creation, view, memory-requirement, and copy paths with physical-layer subresource decoding.
- Extend staged texture upload to multi-mip, array, and cubemap payloads with aligned rows and per-subresource copy descriptors.
- Extend GPU resource texture preparation to accept valid 2D arrays and cubemaps, and repack mipped cubemap CPU data into RHI flat layer-major/mip-minor order.
- Update validation tests for helper contracts, multi-subresource copy descriptors, cubemap/array upload, mipped cubemap repacking, and continued honest rejection of invalid layouts.

**Out of scope:**

- Shader-side cubemap IBL sampling.
- Material descriptor binding for irradiance, prefiltered environment, or BRDF LUT resources.
- GPU IBL convolution passes, skybox rendering, compressed texture upload, 3D texture upload, and visual golden changes.

**Files changed:**

- `Docs/superpowers/specs/2026-06-07-rq2a-rhi-cubemap-upload-foundation-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `RHI/Include/RHI/RHICommandContext.h`
- `RHI/Include/RHI/RHITexture.h`
- `RHI_DX11/Private/DX11Resources.cpp`
- `RHI_DX12/Private/DX12Device.cpp`
- `RHI_DX12/Private/DX12Resources.cpp`
- `RHI_Metal/Private/MetalCommandContext.mm`
- `RHI_Metal/Private/MetalDevice.mm`
- `RHI_Metal/Private/MetalResources.mm`
- `RHI_OpenGL/Private/OpenGLCommandContext.cpp`
- `RHI_OpenGL/Private/OpenGLResources.cpp`
- `RHI_Vulkan/Private/VulkanCommandContext.cpp`
- `RHI_Vulkan/Private/VulkanDevice.cpp`
- `RHI_Vulkan/Private/VulkanResources.cpp`
- `Render/Private/GPUResourceManager.cpp`
- `Render/Private/GPUUploadService.cpp`
- `Tests/GPUResourceManagerValidation/main.cpp`
- `Tests/GPUUploadServiceValidation/main.cpp`

**Validation commands:**

```powershell
cmake --build build\win_x64_debug --config Debug --target ModelViewer GPUUploadServiceValidation GPUResourceManagerValidation RenderPassValidation
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "GPUUploadServiceValidation|GPUResourceManagerValidation|RenderPassValidation|RenderGraphValidation|RenderHonestyValidation|RenderSceneValidation|MaterialSystemValidation|PipelineCacheValidation|ClusteredLightingValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
git diff --check
```

**Validation result:**

- Build: PASS.
- Tests: PASS, 189/189 selected tests passed.
- Visual gate: PASS, `ModelViewerSmoke` and `VisualGoldenValidation` passed.

**Artifacts:**

- Logs: terminal build, ctest, and `git diff --check` output in this thread.
- Screenshots: none.
- Diffs: current RQ2a working tree diff before commit.

**Spark plan review result:**

- Verdict: PASS after plan revision.
- Blockers resolved: explicit cube-count vs physical-layer contract, backend copy-path decode order, Metal cube-array type handling, and HDR loader mip-major/face-major repack strategy were added to the plan before implementation.

**Spark code review result:**

- Verdict: PASS.
- Blockers resolved: none. Non-blocking OpenGLTextureView physical-layer note was adopted and re-confirmed by Spark as PASS.

**Notes / follow-ups:**

- RQ2a only makes cubemap/array/mip resources uploadable and viewable; real IBL descriptor binding and shader sampling remain a later stage.
- Metal changes were made according to the shared RHI contract but were not compiled on this Windows build host.

---

### R-SP: `RQ2b - DefaultLit Texture IBL Binding`

**Date:** 2026-06-07
**Commit:** pending in this commit
**Spark plan review agent:** Carson (`gpt-5.3-codex-spark`)
**Spark code review agent:** Parfit (`gpt-5.3-codex-spark`)

**Plan source:**

- Document: `Docs/superpowers/specs/2026-06-07-rq2b-defaultlit-texture-ibl-binding-plan.md`
- Section: full document, especially §3 Scope, §6 Required Tests, and §10 Spark Review
- Lines checked: current RQ2b plan before implementation

**Prerequisite status:** PASS

- Previous R-SP: `RQ2a - RHI Cubemap Subresource and Upload Foundation`
- Evidence: RQ2a committed as `970d5bd feat(render): support cubemap texture upload foundation`; RQ2b plan reviewed by Spark before implementation.

**Approved scope:**

- Append texture IBL parameters to `ViewData` and `PipelineCache::ViewConstants` without moving existing cbuffer fields.
- Add DefaultLit texture IBL bindings for irradiance cubemap, prefiltered cubemap, and BRDF LUT at set 2 bindings 7, 8, and 9.
- Keep the R9f approximate ambient branch as explicit fallback when texture IBL is disabled.
- Extend PipelineCache reflection validation for the new sampled texture bindings.
- Extend MaterialSystem set 2 descriptors with descriptor-complete IBL fallback resources and live environment resource binding when all IBL textures are GPU-ready.
- Add SceneRenderer environment IBL discovery from `SkyboxComponent`, upload requests, readiness checks, fallback stats, and view constant enablement.
- Update focused validation tests for layout, cbuffer packing, shader guardrails, fallback descriptors, live IBL descriptors, and descriptor cache invalidation.

**Out of scope:**

- SkyboxPass draw implementation.
- Loading a new HDRI in ModelViewer.
- GPU-side IBL convolution.
- Tone mapping operator changes, bindless descriptors, material texture slot rewrites, and visual golden recapture.

**Files changed:**

- `Docs/superpowers/specs/2026-06-07-rq2b-defaultlit-texture-ibl-binding-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `Render/Include/Render/Material/MaterialSystem.h`
- `Render/Include/Render/PipelineCache.h`
- `Render/Include/Render/Renderer/SceneRenderer.h`
- `Render/Include/Render/Renderer/ViewData.h`
- `Render/Private/Material/MaterialSystem.cpp`
- `Render/Private/PipelineCache.cpp`
- `Render/Private/Renderer/SceneRenderer.cpp`
- `Render/Shaders/DefaultLit.hlsl`
- `Tests/MaterialSystemValidation/main.cpp`
- `Tests/PipelineCacheValidation/main.cpp`

**Validation commands:**

```powershell
cmake --build build\win_x64_debug --config Debug --target PipelineCacheValidation MaterialSystemValidation RenderSceneValidation RenderPassValidation GPUUploadServiceValidation GPUResourceManagerValidation ModelViewer
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "PipelineCacheValidation|MaterialSystemValidation|RenderSceneValidation|RenderPassValidation|GPUResourceManagerValidation|GPUUploadServiceValidation|RenderGraphValidation|RenderHonestyValidation|ClusteredLightingValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
git diff --check
```

**Validation result:**

- Build: PASS.
- Tests: PASS, 193/193 selected tests passed.
- Visual gate: PASS, `ModelViewerSmoke` and `VisualGoldenValidation` passed.
- Diff check: PASS, with CRLF warnings only.

**Artifacts:**

- Logs: terminal build, ctest, and `git diff --check` output in this thread.
- Screenshots: none.
- Diffs: current RQ2b working tree diff before commit.

**Spark plan review result:**

- Verdict: PASS.
- Non-blocking notes adopted: keep set 2 bindings 7, 8, and 9 collision-free, and include fallback/live state plus view generation in the descriptor cache key.

**Spark code review result:**

- Verdict: PASS.
- Blockers resolved: none.
- Non-blocking note: CRLF warnings only.

**Notes / follow-ups:**

- RQ2b enables real texture IBL consumption only when a scene provides GPU-ready `SkyboxComponent` irradiance, prefiltered environment, and BRDF LUT resources.
- Current ModelViewer does not yet load or author a SkyboxComponent IBL environment, so the visual gate can remain unchanged while the binding path is validated.
- Skybox rendering, sample HDRI wiring, and GPU IBL convolution remain later stages and must follow the same plan/review/commit protocol.

---

### R-SP: `RQ2c - ModelViewer Procedural IBL Wiring`

**Date:** 2026-06-07
**Commit:** pending in this commit
**Spark plan review agent:** Confucius (`gpt-5.3-codex-spark`)
**Spark code review agent:** Confucius (`gpt-5.3-codex-spark`)

**Plan source:**

- Document: `Docs/superpowers/specs/2026-06-07-rq2c-modelviewer-procedural-ibl-plan.md`
- Section: full document, especially §3 Scope, §6 Required Tests, and §10 Spark Review
- Lines checked: current RQ2c plan before implementation

**Prerequisite status:** PASS

- Previous R-SP: `RQ2b - DefaultLit Texture IBL Binding`
- Evidence: RQ2b committed as `cad6ef0 feat(render): bind texture IBL resources for default lit`; RQ2c plan reviewed by Spark before implementation.

**Approved scope:**

- Add ModelViewer CLI controls for `--no-ibl` and `--expect-ibl-ready`.
- Keep procedural IBL enabled by default for normal ModelViewer runs.
- Create small procedural irradiance cubemap, prefiltered mipped cubemap, and BRDF LUT resources in ModelViewer.
- Generate cubemap CPU data in mip-major / face-major source order for `GPUResourceManager` repacking.
- Attach the procedural IBL resources to a `SkyboxComponent` and upload all three textures immediately.
- Preserve the existing golden path by passing `--no-ibl` to `ModelViewerSmoke`.
- Add `ModelViewerIBLSmoke` as a readiness-only smoke test with no image compare.

**Out of scope:**

- SkyboxPass background drawing.
- External HDRI loading in ModelViewer.
- CPU or GPU environment convolution.
- Visual golden recapture.
- RenderSubsystem upload scheduling refactor.
- Descriptor layout, DefaultLit shader math, or RQ2b binding changes.

**Files changed:**

- `Docs/superpowers/specs/2026-06-07-rq2c-modelviewer-procedural-ibl-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `Samples/ModelViewer/main.cpp`
- `Tests/CMakeLists.txt`

**Validation commands:**

```powershell
cmake --build build\win_x64_debug --config Debug --target ModelViewer PipelineCacheValidation MaterialSystemValidation GPUUploadServiceValidation GPUResourceManagerValidation RenderPassValidation RenderSceneValidation
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "ModelViewerSmoke|ModelViewerIBLSmoke|VisualGoldenValidation"
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "PipelineCacheValidation|MaterialSystemValidation|GPUUploadServiceValidation|GPUResourceManagerValidation|RenderGraphValidation|RenderHonestyValidation|RenderSceneValidation|RenderPassValidation|ClusteredLightingValidation"
git diff --check
```

**Validation result:**

- Build: PASS.
- Visual tests: PASS, 3/3 selected tests passed (`ModelViewerSmoke`, `VisualGoldenValidation`, `ModelViewerIBLSmoke`).
- Regression tests: PASS, 187/187 selected tests passed.
- Diff check: PASS, with CRLF warnings only.

**Artifacts:**

- Logs: terminal build, ctest, and `git diff --check` output in this thread.
- Screenshots: none.
- Diffs: current RQ2c working tree diff before commit.

**Spark plan review result:**

- Verdict: PASS.
- Non-blocking notes adopted: keep CLI flags aligned with the existing parser, use mip-major / face-major cubemap source data, upload all three resources immediately, and keep `ModelViewerIBLSmoke` readiness-only.

**Spark code review result:**

- Verdict: PASS.
- Blockers resolved: none.
- Non-blocking note: `--expect-ibl-ready` is intentionally smoke-only and harmless in interactive mode.

**Notes / follow-ups:**

- ModelViewer now exercises the RQ2b texture IBL path by default without requiring a committed HDRI fixture.
- Existing zero-tolerance visual golden remains stable through `--no-ibl`.
- Skybox drawing, real HDRI sample wiring, and environment convolution remain later stages and must follow the same plan/review/commit protocol.

---

### R-SP: `RQ2d - Procedural Skybox Minimum Draw`

**Date:** 2026-06-07
**Commit:** pending in this commit
**Spark plan review agent:** Confucius (`gpt-5.3-codex-spark`)
**Spark code review agent:** Confucius (`gpt-5.3-codex-spark`)

**Plan source:**

- Document: `Docs/superpowers/specs/2026-06-07-rq2d-procedural-skybox-minimum-draw-plan.md`
- Section: full document, especially §3 Scope, §6 Required Tests, and §10 Spark Review
- Lines checked: current RQ2d plan before implementation

**Prerequisite status:** PASS

- Previous R-SP: `RQ2c - ModelViewer Procedural IBL Wiring`
- Evidence: RQ2c committed as `7a7fef9 feat(samples): wire procedural IBL into model viewer`; RQ2d plan reviewed by Spark before implementation.

**Approved scope:**

- Add `Render/Shaders/Skybox.hlsl` for a procedural fullscreen sky background.
- Extend PipelineCache with skybox shader compilation, a dedicated skybox descriptor layout, a skybox pipeline layout, and skybox pipeline accessors.
- Add skybox shader/pipeline hashes to PipelineCache manifest metadata and tests.
- Add depth-tested and no-depth skybox pipeline variants; only the primary depth-tested pipeline participates in manifest metadata.
- Make `SkyboxPass` allocate constants, retain descriptor sets, resolve graph color/depth views, bind the skybox pipeline, and draw a fullscreen triangle.
- Gate drawing through the first active enabled `SkyboxComponent` in scene traversal order.
- Support `SkyboxType::Procedural` and `SkyboxType::Color`; keep cubemap/equirectangular drawing out of scope and visibly non-drawing.
- Mark ModelViewer's default procedural IBL skybox as `SkyboxType::Procedural` while preserving the `--no-ibl` golden path.
- Update focused PipelineCache and RenderPass validation coverage.

**Out of scope:**

- Cubemap or equirectangular skybox texture sampling.
- External HDRI loading in ModelViewer.
- CPU or GPU environment convolution.
- Physically based sky, atmosphere, clouds, or sun disk quality work.
- Visual golden recapture.
- RenderProxy, ECS/Object refactors, or material descriptor layout changes.

**Files changed:**

- `Docs/superpowers/specs/2026-06-07-rq2d-procedural-skybox-minimum-draw-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `Render/Shaders/Skybox.hlsl`
- `Render/Include/Render/PipelineCache.h`
- `Render/Private/PipelineCache.cpp`
- `Render/Include/Render/Passes/SkyboxPass.h`
- `Render/Private/Passes/SkyboxPass.cpp`
- `Render/Include/Render/Renderer/SceneRenderer.h`
- `Render/Private/Renderer/SceneRenderer.cpp`
- `Samples/ModelViewer/main.cpp`
- `Tests/PipelineCacheValidation/main.cpp`
- `Tests/RenderPassValidation/main.cpp`

**Validation commands:**

```powershell
cmake --build build\win_x64_debug --config Debug --target PipelineCacheValidation RenderPassValidation RenderHonestyValidation ModelViewer
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "PipelineCacheValidation|RenderPassValidation|RenderHonestyValidation"
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "ModelViewerSmoke|VisualGoldenValidation|ModelViewerIBLSmoke"
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "RenderGraphValidation|RenderSceneValidation|MaterialSystemValidation|GPUUploadServiceValidation|GPUResourceManagerValidation|ClusteredLightingValidation"
git diff --check
```

**Validation result:**

- Build: PASS.
- Focused tests: PASS, 79/79 selected tests passed.
- Visual tests: PASS, 3/3 selected tests passed (`ModelViewerSmoke`, `VisualGoldenValidation`, `ModelViewerIBLSmoke`).
- Regression tests: PASS, 116/116 selected tests passed.
- Diff check: PASS, with CRLF warnings only.

**Artifacts:**

- Logs: terminal build, ctest, and `git diff --check` output in this thread.
- Screenshots: none.
- Diffs: current RQ2d working tree diff before commit.

**Spark plan review result:**

- First verdict: BLOCKED.
- Blockers adopted: added component draw gating so `ModelViewerSmoke --no-ibl` remains skybox-free; added PipelineCache manifest/hash/count requirements; added `SceneRenderer` background parameter bridge.
- Second verdict: PASS.

**Spark code review result:**

- Verdict: PASS.
- Blockers resolved: none.
- Non-blocking follow-ups: add future scene-level non-draw coverage for cubemap/equirectangular skybox types, and add a future direct ModelViewer state/log assertion for `--no-ibl` no-skybox creation.

**Notes / follow-ups:**

- RQ2d intentionally draws only procedural/solid backgrounds. Texture skybox drawing remains a later stage.
- The old zero-tolerance visual golden remained stable because the golden smoke path passes `--no-ibl` and creates no skybox component.
- No-depth skybox drawing uses an on-demand no-depth pipeline and does not overwrite the primary manifest skybox hash.

---

### RQ2e: Texture Skybox Cubemap Draw

**Date:** 2026-06-07
**Commit:** pending in this commit
**Spark plan review agent:** Zeno (`gpt-5.5`, xhigh)
**Spark code review agent:** Darwin (`gpt-5.5`, xhigh)

**Plan source:**

- Document: `Docs/superpowers/specs/2026-06-07-rq2e-texture-skybox-cubemap-draw-plan.md`
- Section: entire RQ2e plan
- Lines checked: plan review performed after blocker fixes

**Prerequisite status:** PASS

- Previous R-SP: RQ2d procedural skybox minimum draw, plus bugfix `a3b66b0`.
- Evidence: RQ2d visual/modelviewer gates were green before RQ2e; RQ2e plan review passed after blocker fixes.

**Approved scope:**

- Draw `SkyboxType::Cubemap` as a real texture skybox when the cubemap is GPU-ready.
- Expand skybox shader/layout to bind constants, cubemap SRV, and sampler.
- Keep procedural/color skybox paths working under the expanded layout.
- Keep `SkyboxType::Equirectangular` honestly unsupported.
- Add mandatory SceneRenderer cubemap bridge coverage for missing, not-ready upload request, ready handoff, and equirectangular unsupported.
- Fix skybox pipeline hash/manifest identity to include skybox shader inputs and the RQ2e layout contract.

**Out of scope:**

- Equirectangular panorama draw/conversion.
- HDRI loader wiring in ModelViewer.
- IBL convolution or DefaultLit lighting quality changes.
- ECS/Object/RenderProxy refactors.

**Files changed:**

- `Docs/superpowers/specs/2026-06-07-rq2e-texture-skybox-cubemap-draw-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `Render/CMakeLists.txt`
- `Render/Shaders/Skybox.hlsl`
- `Render/Include/Render/Passes/SkyboxPass.h`
- `Render/Private/Passes/SkyboxPass.cpp`
- `Render/Include/Render/Renderer/SceneRenderer.h`
- `Render/Private/Renderer/SceneRenderer.cpp`
- `Render/Private/Renderer/SceneSkyboxPassBridge.h`
- `Render/Private/Renderer/SceneSkyboxPassBridge.cpp`
- `Render/Private/PipelineCache.cpp`
- `Tests/CMakeLists.txt`
- `Tests/PipelineCacheValidation/main.cpp`
- `Tests/RenderPassValidation/main.cpp`
- `Tests/RenderSceneValidation/main.cpp`

**Validation commands:**

```powershell
cmake --build build\win_x64_debug --config Debug --target PipelineCacheValidation RenderPassValidation RenderSceneValidation
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "PipelineCacheValidation|RenderPassValidation|RenderSceneValidation"
cmake --build build\win_x64_debug --config Debug --target ModelViewer VisualGoldenValidation RenderHonestyValidation MaterialSystemValidation GPUUploadServiceValidation GPUResourceManagerValidation ClusteredLightingValidation RenderGraphValidation
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "ModelViewerSmoke|VisualGoldenValidation|ModelViewerIBLSmoke"
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "RenderGraphValidation|RenderHonestyValidation|MaterialSystemValidation|GPUUploadServiceValidation|GPUResourceManagerValidation|ClusteredLightingValidation"
```

**Validation result:**

- Build: PASS.
- Focused tests: PASS, 85/85 selected tests passed.
- Visual tests: PASS, 3/3 selected tests passed (`ModelViewerSmoke`, `VisualGoldenValidation`, `ModelViewerIBLSmoke`).
- Regression tests: PASS, 118/118 selected tests passed.
- Diff check: PASS, with CRLF warnings only.

**Artifacts:**

- Logs: terminal build and ctest output in this thread.
- Screenshots: none.
- Diffs: current RQ2e working tree diff before commit.

**Spark plan review result:**

- First verdict: BLOCKED.
- Blockers adopted: made skybox shader/layout manifest/hash coverage mandatory; made SceneRenderer cubemap bridge tests mandatory.
- Second verdict: PASS.

**Spark code review result:**

- Verdict: PASS.
- Blockers resolved: none.

**Notes / follow-ups:**

- RQ2e intentionally does not make ModelViewer use an external cubemap background yet; it only enables the engine path when a GPU-ready cubemap component is present.
- The existing `ModelViewerSmoke --no-ibl` golden remained stable.

---

### RQ2f: ModelViewer HDRI Environment Wiring

**Date:** 2026-06-07
**Commit:** pending in this commit
**Spark plan review agent:** Lovelace (`gpt-5.5`, xhigh)
**Spark code review agent:** Pascal (`gpt-5.5`, xhigh)

**Plan source:**

- Document: `Docs/superpowers/specs/2026-06-07-rq2f-modelviewer-hdri-environment-plan.md`
- Section: entire RQ2f plan
- Lines checked: plan review performed before implementation

**Prerequisite status:** PASS

- Previous R-SP: RQ2e texture skybox cubemap draw.
- Evidence: RQ2e committed as `840b14e feat(render): draw cubemap texture skybox`; RQ2f plan review passed before implementation.

**Approved scope:**

- Add `--hdri <path>` to ModelViewer for explicit HDR/EXR environment loading.
- Use `HDRTextureLoader::LoadIBL()` to generate environment cubemap, irradiance cubemap, prefiltered cubemap, and BRDF LUT resources.
- Upload HDRI resources immediately and bind them through a `SkyboxComponent`.
- Add `--expect-skybox-ready` and keep `--expect-ibl-ready` as smoke assertions.
- Add a tiny HDR fixture writer and `ModelViewerHDRISmoke` ctest.
- Preserve procedural IBL behavior when `--hdri` is absent and preserve the `--no-ibl` golden path.

**Out of scope:**

- GPU-side environment convolution.
- Direct equirectangular draw in `SkyboxPass`.
- Bundling a production HDRI asset.
- Making HDRI the default ModelViewer environment without `--hdri`.
- Tonemap, exposure automation, bloom, BRDF, ECS/Object, or RenderProxy changes.

**Files changed:**

- `Docs/superpowers/specs/2026-06-07-rq2f-modelviewer-hdri-environment-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `Samples/ModelViewer/main.cpp`
- `Tests/CMakeLists.txt`
- `Tests/ModelViewerHDRIFixtureWriter/main.cpp`

**Validation commands:**

```powershell
cmake --build build\win_x64_debug --config Debug --target ModelViewer ModelViewerHDRIFixtureWriter
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "ModelViewerHDRIFixture|ModelViewerHDRISmoke|ModelViewerIBLSmoke"
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "ModelViewerSmoke|VisualGoldenValidation"
cmake --build build\win_x64_debug --config Debug --target RenderSceneValidation RenderPassValidation MaterialSystemValidation GPUUploadServiceValidation GPUResourceManagerValidation PipelineCacheValidation
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "RenderSceneValidation|RenderPassValidation|MaterialSystemValidation|GPUUploadServiceValidation|GPUResourceManagerValidation|PipelineCacheValidation"
git diff --check
```

**Validation result:**

- Build: PASS.
- Focused tests: PASS, 3/3 selected tests passed (`ModelViewerHDRIFixture`, `ModelViewerHDRISmoke`, `ModelViewerIBLSmoke`).
- Visual stability: PASS, 2/2 selected tests passed (`ModelViewerSmoke`, `VisualGoldenValidation`).
- Regression tests: PASS, 149/149 selected tests passed.
- Diff check: PASS, with CRLF warnings only.

**Artifacts:**

- Logs: terminal build, ctest, and `git diff --check` output in this thread.
- Screenshots: none.
- Diffs: current RQ2f working tree diff before commit.

**Spark plan review result:**

- Verdict: PASS.
- Non-blocking suggestions adopted: documented `--expect-skybox-ready` through `SceneRenderer::GetPassChainStats().passStatuses`, named the fixture test `ModelViewerHDRIFixture`, and kept smoke HDRI generation settings small.

**Spark code review result:**

- Verdict: PASS.
- Blockers resolved: none.

**Notes / follow-ups:**

- RQ2f uses CPU-side `HDRTextureLoader` generation as a sample wiring path; GPU IBL convolution remains a later stage.
- The zero-tolerance golden path remains `ModelViewerSmoke --no-ibl` and is unchanged.

---

### RQ2g: HDRTextureLoader CPU IBL Sampling Controls

**Date:** 2026-06-07
**Commit:** pending in this commit
**Spark plan review agent:** Franklin (`gpt-5.5`, xhigh), Archimedes (`gpt-5.5`, xhigh)
**Spark code review agent:** Epicurus (`gpt-5.5`, xhigh)

**Plan source:**

- Document: `Docs/superpowers/specs/2026-06-07-rq2g-hdrtextureloader-cpu-ibl-sampling-plan.md`
- Section: entire RQ2g plan
- Lines checked: plan review performed before implementation and after scope supplement

**Prerequisite status:** PASS

- Previous R-SP: RQ2f ModelViewer HDRI environment wiring.
- Evidence: RQ2f committed as `5911b42 feat(samples): load HDRI environments in model viewer`; RQ2g plan review passed after adopting blocker fixes.

**Approved scope:**

- Make `HDRLoadOptions::convolutionSamples` drive diffuse irradiance generation instead of being ignored.
- Clamp zero/low sample counts for irradiance, prefiltered environment, and BRDF LUT generation.
- Harden prefiltered generation for one mip and zero-weight samples.
- Add CPU-only `HDRTextureLoaderValidation` coverage for finite output, zero-sample clamp, and non-uniform cubemap sample-count differences.
- Preserve RQ2f `ModelViewerHDRISmoke`; if it exposes blocking upload completion issues, fix upload infrastructure rather than weakening readiness assertions.

**Out of scope:**

- GPU IBL convolution or compute prefiltering.
- Renderer wiring, shader layout, SkyboxPass, or SceneRenderer changes.
- DefaultLit BRDF/math changes.
- Production HDRI assets, visual golden recapture, tonemap, exposure, bloom, or color grading changes.

**Files changed:**

- `Docs/superpowers/specs/2026-06-07-rq2g-hdrtextureloader-cpu-ibl-sampling-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `Resource/Private/Loader/HDRTextureLoader.cpp`
- `Render/Include/Render/GPUUploadService.h`
- `Render/Private/GPUUploadService.cpp`
- `Tests/CMakeLists.txt`
- `Tests/HDRTextureLoaderValidation/main.cpp`

**Validation commands:**

```powershell
cmake --build build\win_x64_debug --config Debug --target HDRTextureLoaderValidation ModelViewer GPUUploadServiceValidation GPUResourceManagerValidation
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "HDRTextureLoaderValidation|ModelViewerHDRISmoke|ModelViewerIBLSmoke|GPUUploadServiceValidation|GPUResourceManagerValidation"
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "ModelViewerSmoke|VisualGoldenValidation"
cmake --build build\win_x64_debug --config Debug --target MaterialSystemValidation RenderSceneValidation RenderPassValidation
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "MaterialSystemValidation|RenderSceneValidation|RenderPassValidation"
git diff --check
```

**Validation result:**

- Pre-fix proof: the new `HDRTextureLoaderValidation` failed on the old implementation for ignored irradiance sample counts and non-finite zero-sample prefilter output.
- Build: PASS.
- Focused/upload tests: PASS, 49/49 selected tests passed.
- Visual stability: PASS, 2/2 selected tests passed (`ModelViewerSmoke`, `VisualGoldenValidation`).
- Regression tests: PASS, 73/73 selected tests passed.
- Diff check: PASS, with CRLF warnings only.

**Artifacts:**

- Logs: terminal build, ctest, and `git diff --check` output in this thread.
- Screenshots: none.
- Diffs: current RQ2g working tree diff before commit.

**Spark plan review result:**

- First verdict: BLOCKED.
- Blockers adopted: added non-uniform cubemap output-difference test for irradiance sample controls, and added zero-sample clamp tests for irradiance, prefiltered map, and BRDF LUT.
- Second verdict: PASS.
- Scope supplement review: PASS; `ModelViewerHDRISmoke` exposed a blocking upload completion issue, so the upload infrastructure was allowed to be fixed without weakening smoke readiness assertions.

**Spark code review result:**

- Verdict: PASS.
- Blockers resolved: none.
- Non-blocking follow-ups: strengthen BRDF LUT half-data tests and add a direct fake-fence upload service fallback test.

**Notes / follow-ups:**

- RQ2g improves CPU IBL generation honesty and robustness only; GPU convolution remains a later stage.
- The upload service fix preserves `UploadImmediate()` blocking semantics for staged uploads after `WaitIdle()`.

---

## Entry Template

### R-SP: `<id and title>`

**Date:**
**Commit:**
**Spark plan review agent:**
**Spark code review agent:**

**Plan source:**

- Document:
- Section:
- Lines checked:

**Prerequisite status:** PASS / BLOCKED

- Previous R-SP:
- Evidence:

**Approved scope:**

-

**Out of scope:**

-

**Files changed:**

-

**Validation commands:**

```powershell

```

**Validation result:**

- Build:
- Tests:
- Visual gate: PASS / BLOCKED / N/A

**Artifacts:**

- Logs:
- Screenshots:
- Diffs:

**Spark plan review result:**

- Verdict:
- Blockers resolved:

**Spark code review result:**

- Verdict:
- Blockers resolved:

**Notes / follow-ups:**

-

---
