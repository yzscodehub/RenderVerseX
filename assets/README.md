# RenderVerseX Asset Layout

The `assets` tree contains source assets used by samples and optional validation.
Runtime code must not depend on machine-local absolute paths.

## Layout

```text
assets/
  models/<asset-id>/
    scene.gltf or <asset-id>.glb
    textures/...
    license.txt
  textures/<asset-id>/...
  environments/<asset-id>/...
```

Keep every third-party asset in its own directory with its original license,
source URL, author, and attribution requirements. Do not merge texture folders
from unrelated downloads.

Large developer assets are intentionally not mandatory test inputs. Configure
an external asset library explicitly when they are needed:

```powershell
cmake -S . -B build/win_x64_debug `
  -DRVX_ENABLE_EXTERNAL_ASSET_TESTS=ON `
  -DRVX_EXTERNAL_TEST_ASSET_ROOT=E:/WorkSpace/RenderVerseX/assets
```

This registers `external-assets` CTest cases without hard-coding the local
path. The external suite is development evidence only. Closing a production
qualification gate requires a small, redistributable, attribution-complete
asset checked into the repository with deterministic capture expectations.

Generated imports, compressed textures, mesh optimization output, thumbnails,
and shader caches belong under the build or derived-data directory, never next
to the immutable source asset.

## Optional Model Viewer catalog model

`free-1975-porsche-911-930-turbo` is the cataloged real-model validation asset.
It exercises 75 meshes, 14 materials, 27 source textures, hierarchy bounds,
camera framing, and the complete CPUReady -> GPUUploadPending -> RenderReady
path. The source model remains in the ignored developer asset library because
of its size; its adjacent `license.txt` records the CC-BY-4.0 attribution.

Run the Product Model Viewer interactively without an absolute runtime path:

```powershell
$sample = ".\build\win_x64_debug\Samples\RenderVerseSamples\Debug\RenderVerseSamples.exe"
& $sample --sample model-viewer `
  --asset free-1975-porsche-911-930-turbo `
  --catalog ".\assets\catalog.json" `
  --asset-root ".\assets" `
  --backend dx12 --quality low `
  --width 1280 --height 720 `
  --diagnostics --validation
```

For a finite automated readiness validation, add the bounded frame contract:

```powershell
& $sample --sample model-viewer `
  --asset free-1975-porsche-911-930-turbo `
  --catalog ".\assets\catalog.json" `
  --asset-root ".\assets" `
  --backend dx12 --smoke --frames 1 --wait-ready `
  --ready-timeout-ms 180000 --ready-max-frames 1024 `
  --quality low --width 1280 --height 720 `
  --diagnostics --validation
```

`--wait-ready` intentionally requires a finite `--frames` value; omit both
options for the interactive viewer.

Use `--backend vulkan` for the equivalent Vulkan path. With
`RVX_ENABLE_EXTERNAL_ASSET_TESTS=ON`, CTest registers screenshot, image-content,
catalog provenance, full model readiness, and draw-execution validation for
both DX12 and Vulkan. The bundled `r7-triangle` remains the deterministic
Model Viewer default for ordinary CI and clean source checkouts.

## Optional environment catalog

`catalog.json` records only environment files whose exact source page, author,
license, and local file are all known. `cowboy-town-hall-8k` and
`valley-of-desolation-4k` are Poly Haven CC0 assets. The catalog intentionally
omits files with incomplete provenance; an unlisted file must be selected only
as an explicit `--environment-file` and is never accepted by hermetic smoke
validation.

The bundled `pbr-reference-environment` remains the deterministic CI default.
To inspect a catalog environment manually from PowerShell, keep the generated
PBR grid as the explicit model and select the environment by catalog ID:

```powershell
$sample = ".\build\win_x64_debug\Samples\RenderVerseSamples\Debug\RenderVerseSamples.exe"
& $sample --sample pbr-materials `
  --model ".\Tests\Fixtures\Samples\PBRMaterialGrid.gltf" `
  --environment valley-of-desolation-4k `
  --catalog ".\assets\catalog.json" `
  --asset-root ".\assets" `
  --backend vulkan --frames 8 --wait-ready `
  --quality low --width 1280 --height 720
```

With both `RVX_ENABLE_EXTERNAL_ASSET_TESTS=ON` and
`RVX_ENABLE_EXTERNAL_ENVIRONMENT_TESTS=ON`, the test configuration registers
the same path as an opt-in screenshot, report-provenance, and PBR/IBL readiness
gate for each enabled primary backend. The large EXR files are not copied into
the sample executable and are not required by ordinary CI. Enabling the
environment gate with an incomplete catalog root fails configuration instead
of silently choosing another environment. Catalog schema or provenance errors
still fail before engine startup when the strict catalog loader resolves the
selected ID.
