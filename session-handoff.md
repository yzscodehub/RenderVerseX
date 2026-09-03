<!-- harness-handoff: gfx-24 -->

# Session Handoff

The single active coordinator feature now covers GFX24 qualification closeout
and the user-requested GFX25 production PBR/showcase implementation.

Current branch `codex/runtime-architecture-validation` is at `6ad09dcb`.
Bundle/Series/Golden V2 binds actual render path, instancing mode, recipe,
quality, backend, profile hash, commit, run, and preset. Forward+ and Deferred
DX12 Goldens are explicitly user-approved and committed. The isolated clean
Release raster required suite passed 4/4 with run
`9881ff4eea0a42a3be1a5e4be6365bc7` and immutable receipt SHA-256
`109843e2638c7220d7a296fbcb599cfdd87b6a322c25f14265423463b9198ad5`.
Architecture diagnostics pass 65/65; RenderQuality self-tests pass 46/46,
cross-backend self-tests 4/4, and CI policy self-tests 61/61.

The user explicitly deferred the fixed RTX 4070 Ti A4 calibration and all
Windows/Linux/macOS plus DX12/Vulkan RT remote qualification until GFX25 is
implemented. Do not reuse the rejected A4 runs and do not call GFX24 or GFX25
fully qualified before those final gates run.

Next implementation order:

1. Generalize deterministic cooked visual qualification for `model-viewer`
   and `pbr-materials`.
2. Upgrade MaterialStaticDescriptor and CookedMaterialArtifact to strict V2,
   with specular, clearcoat, transmission, volume, IOR, and emissive-strength
   glTF semantics and no V1 reader.
3. Qualify small redistributable required assets; keep Porsche and large HDRI
   as optional developer/nightly evidence until their catalog provenance and
   complete material mapping pass.
4. Run the deferred A4 and cross-platform/RT evidence closure.

Preserve the existing user-owned changes in `AGENTS.md`, `CLAUDE.md`,
`Docs/README.md`, `Docs/engine-capabilities.json`, `ViewData.h`,
`GPUSceneRaster.hlsli`, and the deletion of `BACKEND_ANALYSIS.md`. Do not
edit or stage `.agent-harness/`. Do not push or modify master.
