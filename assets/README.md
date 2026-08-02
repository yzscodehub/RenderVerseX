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
