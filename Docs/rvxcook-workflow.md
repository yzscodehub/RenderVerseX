# RVXCook Workflow

This document describes the current RenderVerseX asset cook workflow for engineers and CI jobs. It records the implemented command line contract, profile syntax, output layout, and validation gates for cooked runtime assets.

## Command

`RVXCook` cooks importable source assets from a source directory into runtime `.rva` artifacts and writes a deterministic `RVX_COOK_MANIFEST_V1` manifest.

```powershell
build\win_x64_debug\Tools\Debug\RVXCook.exe `
  --source Assets\Source `
  --output build\Cooked `
  --manifest build\Cooked\CookManifest.rvxmanifest `
  --profile Assets\CookProfile.rvxprofile `
  --rewrite-gltf-texture-uris `
  --fail-on-errors
```

Supported options:

- `--source <dir>`: source asset directory.
- `--output <dir>`: cooked output directory.
- `--manifest <path>`: manifest path; defaults to `<output>/CookManifest.rvxmanifest`.
- `--profile <path>`: versioned cook profile.
- `--texture-compression <auto|none|bc1|bc3|bc5|bc7>`: global texture compression mode.
- `--rewrite-gltf-texture-uris`: emit runtime `.gltf` copies with external image URIs redirected to cooked `.rva` textures.
- `--non-recursive`: cook only direct children of `--source`.
- `--fail-on-errors`: return non-zero when any manifest entry failed.

## Cook Profile

Profiles are line-oriented text files. Empty lines and lines starting with `#` or `;` are ignored.

```ini
RVX_COOK_PROFILE_V1
texture.compression=none
texture.compression[Textures/BaseColor_*.tga]=bc7
texture.compression[Textures/*_Normal.tga]=bc5
texture.compression[Textures/Foliage*.tga]=bc3
texture.compression[Textures/AO.tga]=bc1
mesh.generateTangents=true
mesh.optimizeMesh=false
mesh.generateLODs=true
mesh.lodCount=2
mesh.lodReductionFactor=0.5
```

Rules:

- The first non-comment line must be `RVX_COOK_PROFILE_V1`.
- `texture.compression` and `texture.compression.default` set the default texture mode.
- `texture.compression[pattern]` applies to normalized source-relative paths.
- Patterns are case-insensitive and use `/` separators; `*` and `?` wildcards are supported.
- Later matching rules override earlier matching rules.
- Supported modes are `auto`, `none`, `bc1`, `bc3`, `bc5`, and `bc7`.
- `mesh.generateTangents`, `mesh.optimizeMesh`, `mesh.generateLODs`, `mesh.lodCount`, and `mesh.lodReductionFactor` set global mesh cook options for glTF/GLB sources.
- `mesh.lodCount` must be between 1 and 16; `mesh.lodReductionFactor` must be greater than 0 and less than 1.

Current automatic texture selection keeps existing behavior: normal maps use BC5, alpha color textures use BC3, opaque RGBA color/data textures use BC1, and unsupported texture layouts fall back to uncompressed payloads with a manifest warning. BC7 is explicit opt-in through the CLI or profile.

## Outputs

For each importable source file, `RVXCook` writes a sibling cooked artifact under `--output` with the same relative path and a `.rva` extension. The manifest records source path, output path, asset type, success/failure, warnings, timestamps, and output size.

Texture artifacts use `RVX_TEXTURE_PREBAKE_V1` and include metadata such as `format`, `usage`, `srgb`, `requestedCompressionMode`, `compression`, and `dataSize`. Supported cooked compressed texture formats are BC1, BC3, BC5, and BC7.

Mesh and shader artifacts use their own `.rva` headers and are loaded by header dispatch rather than extension guessing. Mesh artifacts can include lower LOD payloads with compacted vertex attributes and remapped UInt32 index data when `mesh.generateLODs=true`.

## glTF Runtime Rewrite

When `--rewrite-gltf-texture-uris` is set, `RVXCook` performs the normal cook first, then writes runtime `.gltf` copies under `--output`.

Rewrite behavior:

- Only external `images[].uri` strings are mutated.
- `data:` URIs, URI-scheme paths, and already cooked `.rva` URIs are left unchanged.
- A URI is rewritten only when it resolves to a successfully cooked texture manifest entry.
- Runtime glTF copies are appended to the manifest as successful `Mesh` entries.
- The runtime glTF manifest entry includes a warning such as `runtimeGltfTextureUriRewrites=5`.

This is the recommended authored-material workflow: keep source glTF files pointing at authored images, select texture formats in the cook profile, and load the rewritten runtime glTF from the cooked output directory.

## Validation Gates

Current regression gates for this workflow:

- `RenderHonestyValidationFixture.RVXCookCliWritesManifestForTextureDirectory`
- `RenderHonestyValidationFixture.RVXCookCliAppliesTextureCompressionProfileRules`
- `RenderHonestyValidationFixture.RVXCookCliAppliesMeshProfileLODOptions`
- `RenderHonestyValidationFixture.RVXCookCliWritesBC7TextureArtifact`
- `RenderHonestyValidationFixture.RVXCookCliRewritesGltfTextureUrisToCookedArtifacts`
- `RenderHonestyValidationFixture.RVXCookCliRejectsInvalidMeshCookProfile`
- `RenderHonestyValidationFixture.ResourceManagerLoadsCookedMeshArtifactWithLOD`
- `GPUResourceManagerValidation.BC7TextureDataUploadsWithBlockLayout`
- `ModelViewerCookedBCMaterialFixture`
- `ModelViewerOpenGLCookedBCMaterialSmoke`
- `OpenGLCookedBCMaterialImageContentValidation`

The cooked BC ModelViewer fixture is intentionally generated from authored `.tga` image URIs and a cook profile. It then runs `RVXCook --rewrite-gltf-texture-uris`; the ModelViewer smoke test consumes the rewritten runtime glTF and renders BC7/BC3/BC5/BC1 material textures through OpenGL.

## Known Limits

- The current BC7 encoder is deterministic and legal, but not an offline-quality exhaustive encoder.
- `--rewrite-gltf-texture-uris` handles `.gltf` JSON files; binary `.glb` rewrite is not implemented.
- Cook cache/DDC and content-addressed incremental skip are future work.
- Mesh LOD generation currently uses deterministic triangle-subset compaction; it writes real lower LOD payloads, but it is not a production-quality geometric simplifier.
