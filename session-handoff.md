<!-- harness-handoff: gfx-24 -->

# Session Handoff

The single active coordinator feature now covers GFX24 qualification closeout
and the user-requested GFX25 production PBR/showcase implementation.

Historical qualification checkpoint: `6ad09dcb`; current product HEAD is `a38fbb8b`.
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

## Current implementation: GFX25-C / VAL-R / GFX-T0

The user approved these three slices under the existing gfx-24 coordinator.
GFX25-C lighting/magic fix is committed as 1b7668a6. The user explicitly
authorized only the Material V2 magic line in the protected capability file.
Architecture run 9aaa233ebfb34021b947013967b5de35 passed 65/65 before commits;
GFX25 candidate run 285f561f8c8848f18adbe2bb7367800e passed 16/16. These are
historical receipts, not current-HEAD release claims. Golden approval is pending.

VAL-R adds SampleSurfaceQualification to the actual SampleRunner. It rejects
unrequested extent/generation changes before plateau observation and permits
only the exact authorized resize. Finite lifetime windows disallow manual
resize. GFX25 candidates now use case/runs/RVX_VALIDATION_RUN_ID; existing run
directories fail rather than being deleted. Producer stdout/stderr are retained.

GFX-T0 CPU reference is committed as 95b2c865; GPU infrastructure is a38fbb8b. It adds graph-owned
RG16F motion/R8 reactive/R32F paired linear-depth outputs, fixed typed surface inputs, optional Translucency integration,
shader-provider/PipelineCache ownership and a no-history GPU variant. Only a
single fully contributing, non-refractive sorted surface with qualified history
selects transparent motion. Mixtures, WBOIT, refraction and invalid history
preserve opaque motion and raise reactive. Global view reset cannot be overridden
by per-pixel flags. The original opaque velocity texture is never written.
DX12/Vulkan graph-native readback cases, Feature and depth-domain tests passed
83/83; see build/win_x64_debug/t0-depth-focused.log. TAA invalidates history on
device/linear-depth switches and commits that domain only after presentation;
an abandoned candidate cannot change it. Metal remains pending. Actual mesh,
water and particle temporal producers are later GFX26/GFX27 work. Existing
RenderViewHistoryCoordinator/TAA presentation commits remain authoritative.
Final paired-depth Build Truth passed as run 05bddea8260242ac8542f58f942bedba
(Architecture 65, Unit 2106, Integration 765, DX12 Native 64). Its receipt is
bound to the pre-commit 95b2c865 worktree fingerprint, not final HEAD. The older
2efc478c4d2249f799c3721be063b2b3 result predates the depth fix. Preserve both.
Final log: build/win_x64_debug/t0-build-truth-03.log. Refresh post-commit
Architecture before claiming current harness verification.

Next verification commands (from repository root):

    cmake --build build/win_x64_debug --config Debug --target SampleCLIValidation RenderContractsValidation RenderViewHistoryValidation RenderVerseSamplesCooked --parallel 8
    ctest --test-dir build/win_x64_debug -C Debug -R "^(SampleCLIValidation|RenderContractsValidation|TransparentTemporalPolicy|RenderViewHistoryValidation|Gfx25CandidateRunIsolationSelfTest)(\\.|$)" --output-on-failure

Run the opt-in suite.gfx25-candidate with a fresh explicit run id after the
cooked executable and DX12 shader aggregate are built. Review full-size beauty
and seven attachments before requesting Golden approval. VAL-R is committed as
14cc30a1; native resize passed 120 warmup + 480 observation frames, one resize
and two plateaus (Tests/ValR/native-20260912-02 under the Debug build).
The GPU T0 implementation is committed. Golden approval, refreshed Shipping,
fixed-GPU performance and remote/Metal qualification remain pending. No push.
