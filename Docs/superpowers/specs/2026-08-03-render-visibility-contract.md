# Render Visibility and GPU-Culling Frame-Resource Contract

**Status:** Task 8 implementation contract
**Date:** 2026-08-03
**Scope:** Renderer core; DX12 Tier 1 reference implementation with portable
CPU/GPU visibility semantics

## 1. Purpose

Task 8 separates frame/view candidates from final visibility. The renderer,
not a Sample, owns candidate generation, visibility policy, compaction, and
submission. Direct remains the correctness reference; GPU-driven lanes consume
coarse candidates and decide final frustum/distance visibility on the GPU.

The required flow is:

```text
retained RenderScene
  -> frame/view RenderCandidateSet
  -> one canonical object visibility evaluation
  -> pass-aware packet projection
       -> Direct lane: CPU-visible packets
       -> GPU lane: coarse candidates, GPU cull, indirect commands
```

`RenderCandidateSet`, visibility results, and packet mappings are frame-owned
values. Persistent passes and Samples must not retain or reinterpret them.

## 2. Candidate and identity rules

- A scene-object candidate uses `pass == None` and has no source-packet index.
- A pass candidate is identified by `(pass, sourcePacketIndex)` and carries the
  stable object and packet relationship used by compaction.
- Duplicate `(pass, sourcePacketIndex)` values invalidate the complete set.
- Source ordinals are compatibility/debug values and are never identity keys.
- Direct and GPU consumers fail closed if the planned packet, visibility
  candidate, object, or immutable packet signature no longer agrees.
- Depth and Opaque own independent GPU-culling state. A malformed or failed
  lane cannot reset, reuse, or submit the other lane's data.

## 3. Canonical bounds and frustum rules

- Bounds are world-space AABBs derived from the retained render object.
- Matrices use column vectors and `viewProjection = projection * view`.
- Clip depth is `[0, 1]`. Frustum planes are `row3 +/- row0`,
  `row3 +/- row1`, `row2`, and `row3 - row2`.
- Backend presentation Y-flips are excluded from visibility math.
- AABB testing uses center/extent and projected plane radius. Equality is
  visible so a boundary-touching object cannot flicker.
- Degenerate planes, including an infinite/reverse-Z far plane, are ignored.
- Invalid or non-finite bounds fail open for rendering and increment the
  invalid-bounds diagnostic. GPU input sets `forceVisible` for the same case.
- Distance culling uses the AABB bounding radius and the same world-space
  camera position on CPU and GPU.

The shared C++/HLSL `GPUInstanceData` layout is a versioned ABI. Field offsets
and total size must be asserted in C++ and represented once in the shared HLSL
include.

## 4. CPU and GPU provider semantics

The CPU provider performs the canonical object frustum/distance evaluation once
per frame/view. Pass visibility is a projection from dense object masks; adding
pass candidates must not repeat bounds tests.

Direct lanes consume only CPU-visible pass packets. GPU lanes consume all
structurally drawable coarse pass candidates and perform final visibility in
compute. Consequently:

- GPU input count may exceed the final visible draw count;
- a CPU-culled reference object may still be submitted as a GPU candidate;
- stable source-packet mapping survives GPU compaction;
- production command recording does not read GPU visibility back to the CPU.

HZB occlusion is unavailable in Task 8. A request is diagnostic-only and must
not silently enable an incomplete occlusion or two-phase path.

## 5. Honest count and report semantics

Count-buffer indirect execution has no exact CPU-visible executed count without
a readback. Reports therefore distinguish:

- `submittedDrawUpperBound`: the maximum draw count recorded for submission;
- `executedDrawCountAvailable`: whether the exact count is known;
- `executedDrawCount`: valid only when the availability flag is true;
- CPU reference visible/culled counts: validation evidence, never a substitute
  for a GPU result.

GPU compute plus indirect-count normally reports an unavailable exact count.
CPU fallback and Direct recording may report exact counts. Diagnostics, Samples,
tests, and qualification artifacts must not relabel an upper bound or CPU
reference value as an actual GPU execution count.

## 6. In-flight frame-resource ownership

CPU-writable persistent upload data read by GPU culling is versioned by the
RenderContext in-flight frame slot. Each Depth/Opaque owner provides, per slot:

- an instance upload buffer;
- a culling-constants upload buffer;
- a descriptor set binding those slot-local inputs;
- slot-local RenderGraph access snapshots for those resources.

The renderer selects the current slot only after `RenderContext::BeginFrame`
has waited for that slot's prior completion. The CPU may then overwrite that
slot, while resources for other in-flight slots remain immutable until their
own completion wait. A fence wait before every upload is not an acceptable
steady-state implementation because it serializes frames.

Immutable inputs such as the identity instance-index stream may be shared.
GPU-only cull outputs may remain shared while all cull and draw uses execute on
the same ordered graphics queue; moving culling to async compute requires a new
ownership and synchronization contract.

Resize/reconfiguration retires every slot-local resource and descriptor using
the last submitted owner snapshot. Access state committed after graph execution
is written back to the selected slot, never copied from a different slot.

## 7. Acceptance gates

- CPU boundary, invalid-bounds, behind-camera, near/far, zero-object, and
  distance fixtures.
- Stable pass/source mapping and duplicate rejection.
- A fixture proving pass projection reuses canonical object visibility.
- Different in-flight slots expose different upload buffers/descriptors;
  wrapping reuses only the selected slot.
- GPU candidate input exceeds CPU reference visibility without readback.
- Honest unknown executed-count assertions for the GPU path.
- Independent Depth/Opaque owner failure isolation.
- DX12 Debug Layer, GBV, repeated-frame, resize/lifetime, Direct/GPU parity,
  and real multi-submesh asset gates.

Vulkan and Metal must implement the same visibility and reporting semantics.
Their backend submission strategies remain Tasks 12 and 13.
