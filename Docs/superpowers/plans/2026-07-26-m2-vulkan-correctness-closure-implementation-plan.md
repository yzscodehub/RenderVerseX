# M2 Vulkan Correctness Closure Implementation Plan

**Status:** Proposed; best-practice review incorporated
**Parent:** `2026-07-26-m2-tier1-rhi-proof-master-plan.md`
**Depends on:** Frozen shared conformance, pipeline, descriptor, and state
contracts

## Goal

Prove that the shared base RHI contract is genuinely portable by translating
it to Vulkan without changing shared meaning or relying on DX12 assumptions.

Required evidence runs on:

- Windows Vulkan;
- Linux Vulkan;
- an approved real adapter or documented software ICD according to the support
  policy, with the report identifying which was used.

## Translation Decisions

### Shader and pipeline interface

- Use SPIR-V reflection to populate canonical location/format identity;
  semantic text is diagnostic metadata rather than the portable key.
- Validate vertex attributes and bindings before `vkCreateGraphicsPipelines`.
- Map render target/depth/sample contracts to dynamic rendering or render-pass
  compatibility consistently with the existing backend choice.
- Include normalized shader interface and attachment identity in pipeline cache
  keys.

### Descriptors

- Base-tier descriptor sets are fully populated.
- Do not require descriptor indexing, partially bound arrays, or
  update-after-bind for required cases.
- Enable optional descriptor-indexing behavior only when capabilities and
  feature chains prove support.
- Descriptor replacements must not update an in-flight set illegally.

### Scoped dependencies and lifetime snapshots

Map the shared dependency plus lifetime snapshot through synchronization2 to:

- image layout;
- source/destination access masks;
- source/destination pipeline stage masks;
- queue-family ownership when physical queues differ.

Equal layouts may still require a memory dependency. `VK_IMAGE_LAYOUT_UNDEFINED`
is selected only when contents may legally be discarded; it is not a
substitute for an unknown or forgotten realized layout.

An imported snapshot without sufficient source access/domain meaning must fail
validation rather than default every case to `ALL_COMMANDS`. Buffer range and
texture subresource tracking preserve the shared snapshot. Conservative broad
stages remain a diagnosed fallback for a genuinely unclassified compatibility
path, not the normal RenderGraph output.

### Presentation

- Require exact requested Vulkan realization.
- Cover acquire, render, present, out-of-date/resize, and idle.
- Do not count a headless/offscreen-only run as surface proof.

## Implementation Steps

### VK1: Add translation regressions

- [ ] Reflected vertex locations/formats match the normalized interface.
- [ ] Missing vertex attribute fails before native pipeline creation.
- [ ] Complete descriptor sets allocate and update every required element.
- [ ] Required base cases do not request optional partially-bound features.
- [ ] Scoped dependency and lifetime snapshot map to expected
  layout/access/stage.
- [ ] Same-layout compute-write to graphics-read emits a memory dependency.
- [ ] Discard uses `UNDEFINED` only when prior contents are invalid.
- [ ] Queue ownership transfer appears only when source/target families differ.

### VK2: Implement pipeline and descriptor translation

- [ ] Carry the shared shader interface through Vulkan shader modules.
- [ ] Integrate common pipeline preflight.
- [ ] Validate attachment/sample compatibility.
- [ ] Map shared descriptor stability to safe allocation/update behavior.
- [ ] Reject capability/feature-chain disagreement.

### VK3: Implement synchronization2 dependency translation

- [ ] Consume access/content-bearing external and transient leases.
- [ ] Generate precise synchronization2 image/buffer dependencies.
- [ ] Preserve same-layout memory hazards instead of returning early.
- [ ] Preserve subresource/range state.
- [ ] Translate M1 physical domains to queue-family ownership only when native
  ownership actually differs.
- [ ] Publish final access/content/domain snapshots.
- [ ] Add validation diagnostics containing resource and pass names.

### VK4: Run shared offscreen conformance

- [ ] Resource/view
- [ ] Descriptor
- [ ] Graphics/compute pipeline
- [ ] UAV/SRV and copy/readback
- [ ] Indirect argument
- [ ] Queue/fence
- [ ] Query when supported
- [ ] Deferred destruction

### VK5: Run surface and ModelViewer integration

- [ ] Present deterministic frames.
- [ ] Process resize/out-of-date.
- [ ] Prove idle does not reacquire/re-present.
- [ ] Run bounded R7 integration with validation.
- [ ] Run enabled and disabled GPU-driven controls where capability permits.
- [ ] Capture report and screenshot/diff.

## Primary File Scope

- `RHI_Vulkan/Private/VulkanPipeline.*`
- `RHI_Vulkan/Private/VulkanResources.*`
- `RHI_Vulkan/Private/VulkanCommandContext.*`
- `RHI_Vulkan/Private/VulkanDescriptorAllocator.*`
- Vulkan device/swapchain files selected by implementation
- `ShaderCompiler/Private/ShaderReflection.cpp`
- shared conformance tests
- `Tests/VulkanValidation/main.cpp`
- `Tests/CrossBackendValidation/main.cpp`
- platform CI scripts/workflows

## Validation

On Windows and Linux, build with `/m:1` or the platform-equivalent bounded
parallelism, then run:

```text
RHIConformanceValidation --backend vulkan --validation --surface --report <path>
VulkanValidation
CrossBackendValidation
bounded ModelViewer Vulkan smoke
```

The report must include validation-layer messages and realized device identity.

## Stop Conditions

Stop and return to shared-contract review if Vulkan requires:

- a DX12-native flag in public RHI;
- silently broadening every barrier to `ALL_COMMANDS`;
- treating equal layouts as proof that no memory dependency is required;
- using `UNDEFINED` because the engine forgot the real prior layout;
- mandatory descriptor indexing for the base tier;
- global `vkDeviceWaitIdle` as the normal lifetime solution;
- reporting a different realized backend.

## Exit Gate

- every required shared case passes on Windows and Linux Vulkan evidence;
- unexpected validation-layer warning and error counts are zero;
- capabilities equal enabled native features;
- surface and bounded integration cases pass;
- no change weakens the already-green DX12 contract.
