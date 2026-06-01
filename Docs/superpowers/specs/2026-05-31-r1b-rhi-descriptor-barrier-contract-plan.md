# R1b RHI Descriptor/Barrier Base Contract Implementation Plan

**Date:** 2026-05-31
**Source roadmap:** `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`
**Source section:** `6. R1 - RHI Core Contract`
**Sub-stage:** R1b - Descriptor and Barrier Base Contract
**Prerequisite:** R1a committed as `4a441d1`

---

## 1. Stage Goal

R1b completes the base RHI contract work that remains after R1a:

- Descriptor set layout, pipeline layout, descriptor set creation, and descriptor update failures must be explicit and testable.
- Barrier commands must reject invalid/no-work barriers without crashing or silently reporting success.
- Split-barrier semantics must match `RHICapabilities::supportsSplitBarrier`.
- Higher render layers must be able to branch on descriptor/barrier capability state without relying on warn-only pseudo-success.

This is still a base-contract stage. It does not implement bindless parity, advanced descriptor indexing, RenderGraph aliasing, or visual rendering features.

---

## 2. Current Code Observations

From the current codebase:

- `RHIDescriptorSet::Update()` returns `void`, so invalid descriptor updates cannot be asserted by callers or tests.
- `RHIDescriptorSetLayout` does not expose its binding entries through the base interface, so common descriptor validation cannot be shared.
- Backend factories can construct descriptor objects even when `layout == nullptr`, duplicate bindings exist, resources do not match the declared binding type, or pipeline layouts contain invalid set layouts.
- DX12 descriptor update paths ignore unknown binding indices instead of reporting a failed update.
- Vulkan descriptor update paths log warnings for mismatches but still provide no return value.
- DX11 descriptor sets store bindings without validating them against the layout.
- OpenGL rejects null descriptor-set layout at device creation, but validation is backend-local and incomplete.
- Barrier calls are `void`; null resources and same-state barriers depend on backend behavior. DX12 in particular needs null guards before dereferencing backend resources.
- Split-barrier comments currently imply fallback behavior, while some backends simply no-op. R1b must make this honest.

---

## 3. R1b Scope

### R1b.1 - Public descriptor contract

Files:

- `RHI/Include/RHI/RHIDescriptor.h`
- backend descriptor layout/set headers and implementations
- validation fakes in `Tests/*Validation/main.cpp`

Tasks:

- Add base layout inspection to `RHIDescriptorSetLayout`, such as:
  - `virtual std::span<const RHIBindingLayoutEntry> GetEntries() const = 0;`
  - optional helper `FindEntry(uint32 binding) const` if useful.
- Change `RHIDescriptorSet::Update()` from `void` to `bool`.
- Document the descriptor base contract:
  - duplicate binding indices are invalid;
  - `count == 0` is invalid;
  - dynamic bindings must use dynamic buffer binding types;
  - non-dynamic bindings must not be marked dynamic;
  - each submitted descriptor binding must exist in the layout;
  - each submitted binding must provide exactly the resource kind required by the layout type;
  - combined texture-sampler bindings require both texture view and sampler;
  - null resources in submitted bindings are invalid.

Acceptance:

- Invalid descriptor layout/set/update cases can be asserted without parsing logs.
- Existing render code continues to compile.

### R1b.2 - Shared descriptor validation helpers

Files:

- `RHI/Include/RHI/RHIDescriptor.h` or a new small public header under `RHI/Include/RHI/`
- backend device/pipeline files:
  - `RHI_DX12/Private/DX12Device.cpp`
  - `RHI_DX12/Private/DX12Pipeline.cpp`
  - `RHI_Vulkan/Private/VulkanDevice.cpp`
  - `RHI_Vulkan/Private/VulkanPipeline.cpp`
  - `RHI_DX11/Private/DX11Device.cpp`
  - `RHI_DX11/Private/DX11Pipeline.cpp`
  - `RHI_OpenGL/Private/OpenGLDevice.cpp`
  - `RHI_OpenGL/Private/OpenGLDescriptor.cpp`
  - `RHI_Metal/Private/MetalDevice.mm`
  - Metal descriptor/pipeline implementation files if needed

Tasks:

- Add small validation helpers for:
  - descriptor set layout descriptions;
  - pipeline layout descriptions;
  - descriptor set descriptions;
  - descriptor update binding lists.
- Keep helpers backend-agnostic and allocation-light.
- Backend factory functions must return `nullptr` on invalid create descriptions.
- `RHIDescriptorSet::Update()` implementations must return `false` and leave the previous valid bindings intact when validation fails.
- Initial descriptor-set creation with invalid initial bindings must return `nullptr` at the factory layer instead of creating a partially invalid object.

Acceptance:

- DX12, Vulkan, DX11, OpenGL, and Metal expose the same create/update failure semantics.
- Validation failure is logged once at the boundary where the invalid input is rejected.

### R1b.3 - Descriptor capability matrix

Files:

- `RHI/Include/RHI/RHICapabilities.h`
- backend device initialization files
- `Tests/*Validation/main.cpp`

Tasks:

- Add explicit descriptor/barrier base capability bits, likely:
  - `supportsDescriptorSets`
  - `supportsDynamicDescriptorOffsets`
  - `maxDescriptorSets`
  - `supportsExplicitResourceBarriers`
  - `emulatesResourceBarriers`
- Fill honest values:
  - DX12: descriptor sets true, dynamic offsets true, explicit barriers true, emulated false, split barriers true.
  - Vulkan: descriptor sets true, dynamic offsets true, explicit barriers true, emulated false, split barriers false until event-based split barriers are implemented.
  - DX11: descriptor sets true through emulation/remapping, dynamic offsets conditional/limited, explicit barriers false, emulated barriers true, split barriers false.
  - OpenGL: descriptor sets true through emulation/binding, dynamic offsets false unless already implemented, explicit memory barriers true, emulated barriers false, split barriers false.
  - Metal: descriptor sets true if the current backend implements them, dynamic offsets according to current implementation, explicit encoder memory barriers true, emulated barriers false, split barriers false.

Acceptance:

- Capability defaults remain false/zero in fake devices.
- Backend validation tests assert the descriptor/barrier capability values that are supported by the current implementation.

### R1b.4 - Barrier no-work and invalid-input contract

Files:

- `RHI/Include/RHI/RHICommandContext.h`
- `RHI_DX12/Private/DX12CommandContext.cpp`
- `RHI_Vulkan/Private/VulkanCommandContext.cpp`
- `RHI_DX11/Private/DX11CommandContext.cpp`
- `RHI_OpenGL/Private/OpenGLCommandContext.cpp`
- `RHI_Metal/Private/MetalCommandContext.mm`
- barrier-related validation tests

Tasks:

- Document that null barrier resources and same-state transitions are no-work and must not emit backend API calls.
- Add null resource guards before backend casts/dereferences.
- Add same-state guards.
- `Barriers()` must tolerate empty spans and must not emit work or fail.
- Split barrier behavior:
  - if `supportsSplitBarrier=true`, `BeginBarrier()` and `EndBarrier()` emit real split barriers;
  - if false, `BeginBarrier()` logs unsupported/no-work and `EndBarrier()` either emits a full barrier when explicit barriers exist, or no-ops with visible unsupported/emulated semantics.
- Do not change RenderGraph aliasing behavior in this stage.

Acceptance:

- Null/same-state barrier tests pass without crashes.
- Split barrier tests follow the capability matrix.

### R1b.5 - Tests

Required tests:

- `RenderHonestyValidation`
  - default descriptor/barrier capabilities are false/zero;
  - invalid descriptor layouts/sets/updates fail through test fakes or helper-level tests.
- `DX12Validation`
  - descriptor capabilities;
  - invalid descriptor layout duplicate binding returns null;
  - descriptor set with null layout returns null;
  - invalid update returns false and previous valid state remains usable where observable;
  - null/same-state barriers and split barriers do not crash.
- `VulkanValidation`
  - same descriptor and barrier contract checks where hardware backend is available.
- `DX11Validation`
  - descriptor emulation capabilities;
  - invalid descriptors fail;
  - barriers are explicit-unsupported/emulated without crashing.
- `CrossBackendValidation`
  - common descriptor create failure semantics;
  - common barrier no-work semantics;
  - capability-gated split-barrier expectations.

Optional if compile target is available:

- OpenGL-specific validation for descriptor and barrier unsupported paths.
- Metal compile-only conformance through normal build target if available on platform.

---

## 4. Out of Scope

- Bindless descriptor arrays, update-after-bind, descriptor indexing, descriptor heap residency tuning.
- RenderGraph aliasing barriers or transient resource aliasing implementation.
- Shader reflection-driven descriptor layout generation.
- Material system binding model changes.
- GPU-driven descriptor work.
- ModelViewer visual validation.

---

## 5. Implementation Order

1. Update the public descriptor/barrier contract comments and method signatures.
2. Update test fakes to compile with the new descriptor update signature.
3. Add descriptor validation helpers.
4. Wire validation into DX12, Vulkan, DX11, OpenGL, and Metal descriptor factories/updates.
5. Add descriptor/barrier capability fields and backend values.
6. Add barrier null/same-state guards and split-barrier honesty per backend.
7. Add/adjust validation tests.
8. Build and run the R1b validation set.
9. Run Spark code review.
10. Update `phase-log.md`.
11. Commit R1b.

---

## 6. Validation Commands

Expected commands after implementation:

```powershell
cmake --build build/win_x64_debug --config Debug --target RenderHonestyValidation
cmake --build build/win_x64_debug --config Debug --target DX12Validation
cmake --build build/win_x64_debug --config Debug --target VulkanValidation
cmake --build build/win_x64_debug --config Debug --target DX11Validation
cmake --build build/win_x64_debug --config Debug --target CrossBackendValidation
build\win_x64_debug\Tests\Debug\RenderHonestyValidation.exe
build\win_x64_debug\Tests\Debug\DX12Validation.exe
build\win_x64_debug\Tests\Debug\VulkanValidation.exe
build\win_x64_debug\Tests\Debug\DX11Validation.exe
build\win_x64_debug\Tests\Debug\CrossBackendValidation.exe
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "RenderHonestyValidation|DX12Validation|VulkanValidation|DX11Validation|CrossBackendValidation"
```

---

## 7. Done Criteria

- Spark plan review result: PASS.
- Code implementation stays inside R1b scope.
- All descriptor create/update invalid inputs have explicit false/null outcomes.
- Barrier null/no-op/split behavior is capability-consistent.
- Required validation targets pass.
- Spark code review result: PASS.
- `phase-log.md` records R1b evidence.
- R1b commit is created before starting R2.
