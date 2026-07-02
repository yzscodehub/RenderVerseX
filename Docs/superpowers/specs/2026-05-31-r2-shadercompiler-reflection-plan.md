# R2 ShaderCompiler and Reflection Implementation Plan

**Date:** 2026-05-31
**Source roadmap:** `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`
**Source section:** `7. R2 - ShaderCompiler and Reflection`
**Sub-stage:** R2 - ShaderCompiler and Reflection
**Prerequisite:** R1 RHI Core Contract through sub-stage R1b, committed as `d82cd1b`

---

## 1. Stage Goal

R2 makes shader compilation results reliable enough for R3 pipeline layout and R5 material work:

- Asynchronous compile requests must own stable copies of source, entry point, source path, target profile, and defines.
- Successful compile results must carry source dependency metadata when the compiler can observe it.
- Include dependency changes must be detectable by `ShaderSourceInfo` / cache validation.
- Compile results must carry reflection metadata for backend paths that can provide it.
- OpenGL reload/load paths must pass generated GLSL source to the RHI, not SPIR-V bytecode.
- Invalid compile/load inputs must return visible failures, not placeholder success.

This stage is still infrastructure. It does not build PSOs, material pipelines, visual rendering, or RenderProxy.
Visual gate: N/A for R2 because this stage does not produce frame output; visual validation starts at R7 per the render-first roadmap.

Plan review gate:

- Spark plan review should judge whether this document correctly identifies the current gaps, keeps the work inside R2 scope, defines implementable steps, and names sufficient tests.
- The gaps listed in section 2 are the intended implementation targets for R2. They are not expected to be fixed before plan review.
- A plan-review blocker should be a document/scope/ordering/testability problem, not the mere existence of the current code gaps that R2 is designed to close.

---

## 2. Current Code Observations

From the current codebase:

- `ShaderCompileService::CompileAsync()` stores `ShaderCompileOptions` by value, but `ShaderCompileOptions` contains raw `const char*` pointers. Async callers such as `ShaderManager::LoadFromSourceAsync()` pass pointers into temporary/local strings.
- `TrackingIncludeHandler` exists and can record `ShaderSourceInfo`, but `DXCCompiler.cpp` still uses the default DXC include handler, so compile results do not expose include dependencies.
- Non-DXC/unsupported compile paths should return empty `sourceInfo` rather than fake dependency metadata.
- `ShaderCompileResult` has no `ShaderSourceInfo`, so cache/hot reload cannot reuse dependency metadata produced by the compiler.
- `ShaderManager` writes only the main source file into cache entries, losing include dependencies even if a compile path observed them.
- DX12, Vulkan, and DX11 compile paths generally return bytecode without reflection in `ShaderCompileResult`; `ShaderManager` has a fallback reflection pass, but direct compiler callers cannot depend on metadata.
- `ShaderHotReloader::ReloadShader()` always passes `result.bytecode` to `CreateShader()`. For OpenGL, `OpenGLDevice::CreateShader()` expects GLSL source text in `RHIShaderDesc::bytecode`, while `ShaderCompileResult::bytecode` stores SPIR-V.
- Tests do not currently include a `ShaderCompilerValidation` target for async lifetime, include invalidation, reflection metadata, or OpenGL GLSL output behavior.

---

## 3. R2 Scope

### R2.1 - Async compile option ownership

Files:

- `ShaderCompiler/Include/ShaderCompiler/ShaderCompileService.h`
- `ShaderCompiler/Private/ShaderCompileService.cpp`
- `Tests/ShaderCompilerValidation/main.cpp`

Tasks:

- Add owned storage to queued compile requests for:
  - `sourceCode`
  - `entryPoint`
  - `sourcePath`
  - `targetProfile`
  - `defines`
- Ensure moved queued requests refresh internal pointers before worker execution.
- Keep the public `ShaderCompileOptions` API compatible.
- Add an optional compiler-injection constructor for `ShaderCompileService` so tests can deterministically verify async lifetime without depending on DXC timing.

Acceptance:

- A queued async compile still observes the original source/strings after the caller mutates or destroys them.
- Existing synchronous and asynchronous call sites continue to compile.

### R2.2 - Source dependency metadata

Files:

- `ShaderCompiler/Include/ShaderCompiler/ShaderCompiler.h`
- `ShaderCompiler/Private/DXCCompiler.cpp`
- `ShaderCompiler/Private/TrackingIncludeHandler.cpp`
- `ShaderCompiler/Private/ShaderManager.cpp`
- `ShaderCompiler/Private/ShaderCacheManager.cpp` if needed only for validation compatibility
- `Tests/ShaderCompilerValidation/main.cpp`

Tasks:

- Add `ShaderSourceInfo sourceInfo` to `ShaderCompileResult`.
- Use `TrackingIncludeHandler` in DXC DX12 and DXC SPIR-V compile paths when a source path is present.
- Record main file hash and include file hashes into `sourceInfo`.
- Compute `sourceInfo.combinedHash` after successful compile.
- Save `compileResult.sourceInfo` into `ShaderCacheEntry` instead of reconstructing main-file-only metadata in `ShaderManager`.
- Keep unsupported/non-DXC platforms honest: no fake dependency data if include tracking is unavailable.
- Tests should assert this honesty through helper-level `ShaderSourceInfo` coverage and by not requiring non-DXC platforms to report dependencies they cannot observe.

Acceptance:

- A shader that includes a file returns a `sourceInfo` containing the main file and include file.
- Mutating the include file makes `sourceInfo.HasChanged()` return `true`.
- Cache entries can preserve include dependency metadata.

### R2.3 - Reflection metadata in compile results

Files:

- `ShaderCompiler/Private/DXCCompiler.cpp`
- `ShaderCompiler/Private/ShaderReflection.cpp` if fixes are needed
- `ShaderCompiler/Private/ShaderLayout.cpp` if ordering/validation fixes are needed
- `Tests/ShaderCompilerValidation/main.cpp`

Tasks:

- Populate `ShaderCompileResult::reflection` on successful DX11, DX12, Vulkan, and OpenGL compile paths where supported.
- Keep OpenGL reflection from SPIRV-Cross translation.
- Preserve explicit compile failure if reflection extraction cannot support a backend only when the compiled backend requires reflection for downstream layout generation. Otherwise return success with empty reflection and log the unsupported state.
- Ensure `BuildAutoPipelineLayout()` can consume reflection metadata without duplicate/invalid descriptor layout entries for the tested cases.

Acceptance:

- Direct compiler users can inspect resource binding metadata after successful DX12/Vulkan/OpenGL compilation.
- A simple reflected shader can build an auto pipeline layout.

### R2.4 - OpenGL GLSL load/reload boundary

Files:

- `ShaderCompiler/Private/ShaderHotReloader.cpp`
- `ShaderCompiler/Private/ShaderManager.cpp` if an async load edge is missing
- `Tests/ShaderCompilerValidation/main.cpp`

Tasks:

- Ensure OpenGL shader creation uses `ShaderCompileResult::glslSource`.
- Fail visibly if an OpenGL compile/load/reload result succeeds but has no GLSL source.
- Add regression coverage that the OpenGL compile result contains GLSL source and that helper/load paths prefer it over SPIR-V where observable.

Acceptance:

- OpenGL hot reload no longer passes SPIR-V bytes into `OpenGLDevice::CreateShader()`.
- OpenGL missing-GLSL conditions return clear errors.

### R2.5 - Compile/load boundary honesty

Files:

- `ShaderCompiler/Private/ShaderCompileService.cpp`
- `ShaderCompiler/Private/ShaderManager.cpp`
- `Tests/ShaderCompilerValidation/main.cpp`
- `Tests/CMakeLists.txt`

Tasks:

- Keep missing source/entry/compiler errors visible through `ShaderCompileResult::success == false` and non-empty `errorMessage`.
- Add `ShaderCompilerValidation` to CTest.
- Cover:
  - invalid compile options fail;
  - async lifetime case;
  - include invalidation case;
  - reflection metadata and auto pipeline layout case;
  - OpenGL compile returns GLSL source when the local compiler supports it.

Acceptance:

- `ShaderCompilerValidation` builds and passes.
- Existing material/render honesty tests still pass.

---

## 4. Out of Scope

- Pipeline/PSO creation, pipeline cache serialization redesign, or render pass integration.
- Material binding behavior beyond consuming existing shader compile results.
- Full shader hot-reload file watcher redesign.
- Linux shader compiler replacement beyond honest unsupported behavior.
- Metal runtime validation on Windows.
- Visual rendering or ModelViewer validation.

---

## 5. Implementation Order

1. Add the `ShaderCompilerValidation` target skeleton and test utilities.
2. Add owned compile-request storage and compiler injection for deterministic async tests.
3. Add `ShaderCompileResult::sourceInfo` and wire DXC include tracking into DX12/SPIR-V compile paths.
4. Populate compile-result reflection for DX11/DX12/Vulkan and preserve OpenGL reflection.
5. Update `ShaderManager` cache save/load to preserve `sourceInfo` and keep OpenGL GLSL selection explicit.
6. Fix `ShaderHotReloader` OpenGL reload source selection and error handling.
7. Add/finish tests for invalid inputs, async lifetime, include invalidation, reflection/layout metadata, and OpenGL GLSL output.
8. Build and run the R2 validation set.
9. Run Spark code review.
10. Update `phase-log.md`.
11. Commit R2.

---

## 6. Validation Commands

Expected commands after implementation:

```powershell
cmake --build build/win_x64_debug --config Debug --target ShaderCompilerValidation RenderHonestyValidation MaterialSystemValidation
build\win_x64_debug\Tests\Debug\ShaderCompilerValidation.exe
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "ShaderCompilerValidation|RenderHonestyValidation|MaterialSystemValidation"
```

Optional if time/risk justifies:

```powershell
cmake --build build/win_x64_debug --config Debug --target DX12Validation VulkanValidation
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "DX12Validation|VulkanValidation"
```

---

## 7. Done Criteria

- Spark plan review result: PASS.
- Async compile requests own stable option data.
- Compile results expose source dependency metadata where supported.
- Include file mutation invalidates recorded `ShaderSourceInfo`.
- Compile results expose reflection metadata for required backend paths.
- OpenGL load/reload paths use GLSL source, not SPIR-V bytecode.
- Required validation targets pass.
- Spark code review result: PASS.
- `phase-log.md` records R2 evidence.
- R2 commit is created before starting R3.
