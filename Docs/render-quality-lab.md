# RenderQualityLab qualification contract

RenderQualityLab is the composition-level GFX24 qualification scenario. It is
not a replacement for the focused model, material, shadow, transparency,
particle, water, or ray-tracing samples. A passing result proves only that an
already-supported combination has produced a complete, same-frame capture and
has met its explicitly selected visual and performance policy.

The scenario must use a fixed public camera bookmark, fixed simulation delta,
fixed deterministic seed, Cooked-only assets, and a public recipe/profile. It
must not add RenderGraph passes or create RHI resources itself. The renderer's
completion-owned capture feature exports the following contract; the scripts in
this document never render, cook, import, or update a golden baseline.

## Semantic attachment bundle

`check_render_quality_bundle.py` consumes three JSON documents and payloads:

```powershell
py Scripts/check_render_quality_bundle.py `
  --bundle <current>/bundle.json `
  --baseline <approved>/bundle.json `
  --policy <policy>/visual-policy.json `
  --sample-manifest Samples/RenderVerseSamples/Manifests/renderverse-samples.json `
  --temporal-bundle <current>/frame-061.bundle.json `
  --expected-source-commit <git-commit> `
  --expected-run-id <capture-run-id> `
  --expected-preset <preset> `
  --expected-receipt-sha256 <render-execution-receipt-sha256> `
  --heatmap-dir <build>/RenderQualityLab/heatmaps `
  --report <build>/RenderQualityLab/visual-result.json
```

The first `--bundle` and every repeated `--temporal-bundle` form an ordered,
completion-owned temporal sequence. At least two bundles are mandatory. The
policy requires a warm-up of 60 or more completed frames before its first
bundle. Every temporal bundle must have the exact requested source commit, run
id, preset, and qualification identity except for its declared `captureTick`.
The tick must be present in that sample's public, strictly increasing
`captureTicks` list and must itself advance strictly in the temporal sequence.
Its receipt and bundle id must be unique, and its logical frame,
submitted-frame, and completed-frame ordinals must all advance strictly. A
stale, duplicate, undeclared, or out-of-order value is rejected.

The container is UTF-8 JSON named `RVX.RenderQualityAttachmentBundle`, schema
version `1`. It has exactly these root fields:

```text
schema, schemaVersion, bundleId, sourceCommit, qualificationIdentity,
execution, attachments
```

`qualificationIdentity` is exact and contains:

```text
sampleId, pipeline, quality, backend, view, captureTick,
visualContractVersion
```

`execution` binds one same-frame submission and completion:

```text
runId, preset, renderExecutionReceiptSha256, completionOwned=true,
frameSequence, submittedFrame, completedFrame, sceneRevision,
surfaceGeneration, configurationRevision, viewId, viewSceneRuntimeId,
viewCameraEntityPacked, viewSubViewIndex, viewRole, graphIdentity,
graphRecordingGeneration, graphRecordEpoch
```

`frameSequence` is the renderer's logical frame identity. `submittedFrame` and
`completedFrame` are the exact monotonic submission/completion ordinals for the
submission which owns that logical frame; completion may not precede
submission. They are deliberately not inferred from wall-clock time or from a
later unrelated present. For a deterministic Sample capture, `frameSequence`
must be at least the declared simulation `captureTick`; this proves the output
cannot precede the scenario state it claims to qualify without assuming that
GPU submission ordinals numerically equal simulation ticks.

`attachments` contains exactly one entry for each semantic output below. V1
uses a compact raw, little-endian float32 payload referenced by safe,
bundle-relative `dataPath`; no absolute, drive, UNC, backslash, or `..` paths
are accepted.

| Semantic | Required format | Color domain |
| --- | --- | --- |
| `BeautyLDR` | `RGBA32_FLOAT` | `LDR` |
| `PreToneHDR` | `RGBA32_FLOAT` | `HDR` |
| `LinearDepth` | `R32_FLOAT` | `LinearDepth` |
| `Normal` | `RGBA32_FLOAT` | `WorldNormal` |
| `Velocity` | `RG32_FLOAT` | `Motion` |
| `AmbientOcclusion` | `R32_FLOAT` | `LinearMask` |
| `ReactiveMask` | `R32_FLOAT` | `LinearMask` |

An attachment contains:

```text
semantic, dataPath, format, colorDomain, width, height, channels,
scalarType=float32le, rowPitchBytes, payloadBytes, sha256, provenance
```

`rowPitchBytes` is the tight `width * channels * 4` canonical row pitch;
backend-native copy alignment and orientation are removed by the completed
readback mapper. `payloadBytes` must equal `rowPitchBytes * height`; its
SHA-256 must match the raw file and every decoded float must be finite.
V1 caps an attachment at 8192×8192 pixels and 256 MiB, and caps the whole
seven-surface bundle at 512 MiB before any raw file is opened.

`provenance` repeats the exact bundle boundary:

```text
sourceCommit, runId, preset, renderExecutionReceiptSha256,
frameSequence, submittedFrame, completedFrame, sceneRevision,
surfaceGeneration, configurationRevision, viewId,
viewSceneRuntimeId, viewCameraEntityPacked, viewSubViewIndex, viewRole,
graphIdentity, graphRecordingGeneration, graphRecordEpoch,
graphResourceIdentity, graphResourceGeneration, producerIdentity,
resourceName, graphPasses
```

The receipt, execution fields, and graph/view identifiers are checked before
pixel comparison. A missing attachment, partial bundle, bad hash, truncation,
NaN/Inf, malformed graph provenance, or stale source/run/preset/receipt
identity is a failed result, never a partial pass.

`RVX.RenderQualityVisualPolicy` v1 has a matching
`qualificationIdentity`, one per-semantic ROI list, and temporal settings:

```text
temporal.warmupCompletedFrames >= 60
temporal.minimumSamples >= 2
ROI: id, normalizedRegionId, x, y, width, height, temporalMode (static|dynamic),
     dataRange, maxMae, maxAbs, minPsnr, minSsim, maxTemporalMae
```

`normalizedRegionId` refers to the public scenario's
`roiMetricPolicy.regionsOfInterest` in `renderverse-samples.json`; the checker
does not read renderer-private scene or RenderGraph state. The conversion is
machine-defined for every semantic extent `W × H` using the normalized
half-open rectangle `(nx, ny, nw, nh)`:

```text
x      = floor(nx * W)
y      = floor(ny * H)
right  = ceil((nx + nw) * W)
bottom = ceil((ny + nh) * H)
width  = right - x
height = bottom - y
```

The integer ROI embedded in the visual policy must match this outward mapping
exactly. This keeps an ROI consistent across LDR, HDR, depth, normal, motion,
and mask surfaces without relying on a private renderer implementation.

The same selected sample must declare a non-empty, strictly increasing
`captureTicks` array. Temporal validation may advance from the current policy
tick (for example `120`) to a later declared tick (for example `360`), while
all identity fields other than `captureTick` remain exact.

The checker compares every ROI and emits MAE, max absolute error, PSNR, and
global ROI SSIM. `dataRange` is explicit per ROI so HDR, depth, velocity, and
unit-range mask PSNR are never compared with an assumed range. It measures
temporal flicker as mean absolute difference over
each declared static or dynamic ROI between consecutive completion-owned
bundles. It writes one diagnostic PPM heatmap per semantic attachment. These
heatmaps are non-golden, generated build artifacts and must not be committed.

## Performance report

`check_render_quality_performance.py` is a separate gate. Do not mix visual
capture readback latency with renderer performance timing.

```powershell
py Scripts/check_render_quality_performance.py `
  --report <current>/performance.json `
  --baseline <approved>/performance.json `
  --policy <policy>/performance-policy.json `
  --expected-source-commit <commit> `
  --expected-run-id <run-id> `
  --expected-preset <preset> `
  --out <build>/RenderQualityLab/performance-result.json
```

Both report files use `RVX.RenderQualityPerformanceReport` v2. Its matching
`RVX.RenderQualityPerformancePolicy` is also v2, so reports or policies with
the legacy whole-frame field are rejected rather than interpreted loosely:

```text
schema, schemaVersion, sourceCommit, runId, preset,
qualificationIdentity, hardwareIdentity,
telemetryCoverage, baselineIdentity, completedWarmupFrames, samples
```

`hardwareIdentity` is intentionally exact:

```text
backend, adapterName, driverVersion, osBuild, capabilityHash
```

`capabilityHash` is produced from the renderer's normalized public RHI
capability snapshot. It distinguishes API/feature-level changes without
inventing backend-specific vendor or shader-model fields that are not exposed
on every production backend.

Every sample is completion-owned and has a unique 64-character execution
receipt digest plus monotonically increasing `frameSequence`, `submittedFrame`,
and `completedFrame` after the warm-up. Completion must not precede the exact
submission ordinal. It records CPU phase durations for
`extraction`, `visibility`, `graphCompile`, and `submission`. `submission` is
CPU-side submission work only; it does not measure Present. GPU evidence has
two raw completion-owned timestamp spans, their conservative qualification
maximum, and a non-empty ordered per-pass GPU duration array:

```text
graphicsTerminalCommandSpanMs
  = GPU timestamp span from the Graphics prelude through the terminal
    Graphics command of the qualified submission.
  = a raw diagnostic component, not by itself the GFX24 policy threshold.

renderGraphPassEnvelopeMs
  = GPU envelope from the first through last RenderGraph pass timestamp.
  = a raw diagnostic component; a general RenderGraph DAG can have a branch
    outside the terminal critical path, so it is not required to fit inside
    graphicsTerminalCommandSpanMs.

graphicsQualificationSpanMs
  = max(graphicsTerminalCommandSpanMs, renderGraphPassEnvelopeMs).
  = the only GPU span constrained by the 16.67 ms GFX24 policy threshold.
  = conservative for these two completion-owned Graphics spans, but not a
    CPU submit, Present, or multi-queue busy-time sum.
```

All three values must be finite and non-negative. A nonzero timestamp pair may
quantize to exactly `0 ms`; Render-owned evidence still validates that pair
before it reaches the report writer. The derived qualification span must equal
the maximum of the two raw spans within the report's declared numeric tolerance.

`gpu.passes` is an ordered array of exactly these records:

```text
{ executionIndex, name, durationMs }
```

`executionIndex` starts at zero and is contiguous. It, rather than `name`, is
the stable pass identity: RenderGraph can legally record several descriptive
passes with the same name. The policy and p95 summary retain the same ordered
identity using `{ executionIndex, name, p95Ms }`. Legacy `passMs` name-keyed
objects are rejected rather than collapsing or silently overwriting pass data.

The performance report never claims a whole-frame GPU duration. CPU submit and
Present remain independent concerns and must not be inferred from its raw or
derived GPU timestamp spans. The bounded telemetry contract is explicit and
machine-readable:

```text
telemetryCoverage:
  schemaVersion=1
  trackedUploadCoverageMask=7
  trackedEstimatedFootprintCoverageMask=7

per sample:
  trackedUploadBytes, trackedUploadCoverageMask=7
  trackedEstimatedFootprint:
    schemaVersion=1, coverageMask=7,
    transientPoolPhysicalEstimatedBytes,
    importedHistoryNamedEstimatedBytes,
    resourceRegistryResidentEstimatedBytes
  resourceCounts: schemaVersion=1, counts
```

`trackedUploadBytes` covers completion-qualified mutation streams, particle
frame uploads, and water frame uploads only. `trackedEstimatedFootprint`
contains estimates for the transient-pool physical allocation, named imported
history resources, and registry-resident resources only. These values are not
driver-wide or allocator-wide GPU counters. The report, approved baseline
(which is another report), and policy must declare exactly the same coverage;
missing, partial, or changed coverage is rejected. The policy requires at
least 60 completed warm-up frames and at least 300 subsequent samples.

The result contains nearest-rank p95 values for the conservative Graphics
qualification span, both raw components, each CPU phase, and every GPU pass.
Only `graphicsQualificationSpanP95Ms` is constrained by the 16.67 ms GPU
policy budget; the raw spans are reported for diagnosis, not thresholding.
It checks maximum tracked-upload and named estimated-footprint budgets. It rejects
post-warm-up resource-count or named estimated-footprint growth. The policy
supplies exact hardware identity, baseline id, the same telemetry coverage,
those bounded budgets, and a permitted percentage regression against the
baseline p95 data. Any hardware, baseline, or qualification identity mismatch
yields `status: "notComparable"` and exit code `2`; it can never pass or
silently establish a new baseline.

The current native performance producer is deliberately limited to the fixed
DX12 qualification GPU. Vulkan and Metal timestamp recording are not yet
implemented for this report path, so those backends fail closed rather than
publishing synthetic or partially timed performance evidence. Their raster
visual qualification remains a separate required path.

## Integration requirements

The renderer-side GFX24 capture owner must add semantic graph copies to
bundle-owned readback buffers and associate them with the same submission
completion as the `RenderExecutionReceipt`. `PollCompletion` may map completed
readback buffers; it must not call `WaitIdle`. It must fail the whole bundle if
one semantic attachment or its graph provenance is missing.

CMake/CTest integration belongs to the composition owner. It should register a
producer fixture that runs the deterministic RenderQualityLab host, then
visual, temporal, and performance validators as consumers of the same
completion-owned evidence. The producer owns `RESOURCE_LOCK rvx_gpu`; offline
validators only read produced files and do not take the GPU lock. The current
hermetic contract test is:

```powershell
py Scripts/test_render_quality_qualification.py
```

It creates temporary synthetic inputs to exercise parser and failure behavior.
It is not native rendering evidence and does not establish a golden image or
performance baseline.
