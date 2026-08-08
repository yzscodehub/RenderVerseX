# RenderVerseSamples

`RenderVerseSamples` is the product-facing sample host for RenderVerseX. One
executable owns the common Engine/World/frame/capture/report lifecycle, while
each sample id maps to an independent scene controller.

Samples create scene meaning and set backend-neutral engine policy. They do not
record RHI commands, construct backend objects, schedule private RenderGraph
passes, or maintain separate Direct and GPU-driven scene paths.

## Commands

```text
RenderVerseSamples --list
RenderVerseSamples --sample model-viewer --model DamagedHelmet.glb
RenderVerseSamples --sample pbr-materials
RenderVerseSamples --sample asset-gallery
RenderVerseSamples --sample lighting-shadows
RenderVerseSamples --sample render-pipeline
RenderVerseSamples --sample gpu-driven
RenderVerseSamples --sample gpu-driven --render-path direct
```

Graphics API selection is a host/automation concern, not a scene concern:

```text
RenderVerseSamples --sample pbr-materials --backend dx12
RenderVerseSamples --sample pbr-materials --backend vulkan
```

No sample controller branches on DX12, Vulkan, Metal, DX11, or OpenGL.

## Current samples

| Id | Purpose | Asset behavior |
|---|---|---|
| `model-rendering` | Minimal production model-to-scene rendering | Compatible catalog override |
| `model-viewer` | User model loading, bounds framing, orbit inspection, optional HDR/EXR environment | Catalog asset or `--model` |
| `pbr-materials` | 5x5x5 metallic/roughness/base-color parameter cube under texture IBL | PBR model plus required environment |
| `asset-gallery` | Static multi-model catalog gallery through the production resource path | Three hermetic defaults; `--asset`/`--model` focuses one item |
| `lighting-shadows` | Directional light and engine-owned cascaded shadow sampling | Fixed caster/receiver fixture |
| `render-pipeline` | Engine-owned shadow, scene, sky, bloom, and tone-mapping Pass chain | Fixed pipeline fixture |
| `gpu-driven` | Direct/GPU-driven parity over identical scene semantics | Select with `--render-path` |

`asset-gallery` intentionally does not offer in-process model switching yet.
Per-run selection remains deterministic until scene destruction and Resource
unloading are qualified together; the report marks `RuntimeAssetSwitching` as
unsupported rather than hiding the limitation.

`pbr-materials` uses a true 5x5x5 validation layout: X varies metallic, Y
varies perceptual roughness, and Z selects five fixed base-color families. The
default factor cube and the optional `pbr-material-texture-cube` asset preserve
identical scene semantics while exercising factor-driven and texture-driven
metallic/roughness input in separate runs. Automation compares their captures
instead of consuming a material-parameter axis for implementation provenance.

## Asset contract

- `--asset <id>` resolves one catalog model.
- `--model <path>` loads exactly that user path and records user-supplied
  provenance; it never falls back to another model.
- `--environment <id>` resolves one catalog environment when the selected
  sample accepts environments.
- `--environment-file <path.hdr|path.exr>` loads exactly that environment.
- `--catalog <catalog.json>` and `--asset-root <directory>` select a strict
  catalog/root pair.
- Smoke runs accept only redistributable catalog assets with complete license
  and source metadata.

Catalog paths are normalized and required to stay under the selected asset
root. Missing entries, traversal, duplicate ids, incomplete provenance, and
missing files fail closed.

## Readiness and evidence

Finite automation can use bounded warm-up:

```text
RenderVerseSamples --sample pbr-materials --backend dx12 --smoke \
  --frames 1 --wait-ready --ready-timeout-ms 30000 \
  --ready-max-frames 64 --screenshot actual.ppm --report report.json
```

`--frames` is the minimum frame count when `--wait-ready` is present. The host
stops only after the sample-specific readiness contract is met, or fails after
the timeout/frame bound. A requested screenshot is captured on the next frame
after readiness so it represents the qualified state.

Reports distinguish requested policy from realized execution. GPU-driven
success requires a recorded GPU culling graph pass and an indirect submission;
Direct success requires a Direct execution report and no indirect submission.
DX12/Vulkan parity gates render the same `gpu-driven` scene twice and require
zero differing pixels. Metal registers the same gates when built on Apple.

## Legacy targets

`Samples/Basic` contains low-level contract and diagnostic executables. Many
targets share an implementation and are not product-level visual examples.

`Samples/Showcase` contains legacy validation/report harnesses retained during
incremental migration. Target names there must not be treated as evidence that
the corresponding feature has a complete visual sample. New product-facing
work belongs in an independent controller under `Scenes/` and is registered in
`RegisterSamples.cpp`.
