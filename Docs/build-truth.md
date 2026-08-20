# Build Truth v3 Gate

Build Truth is the authoritative local and CI validation entry point for
architecture work. A run performs the module self-test, configures once, builds
`RVXValidationAll` once, captures the complete CTest `json-v1` inventory, and
then executes each required lane with an independent JUnit artifact.

Every discovered test must have exactly one primary `lane.*` label. A required
lane fails on zero selected tests, a missing or malformed artifact, a disabled,
skipped, or not-run case, a JUnit/inventory count mismatch, or any test failure.
The runner also verifies that the tracked and non-ignored untracked source
snapshot does not change while validation is running.

The Runtime presets set `RVX_BUILD_EDITOR=OFF`. Editor remains a deferred
product and is not a Runtime correctness dependency.

Windows Debug from an empty build directory:

```powershell
$env:RVX_NATIVE_ALLOW_SOFTWARE_ADAPTER = "1" # Optional local/CI opt-in.
Scripts\run_build_truth.ps1 `
    -ConfigurePreset win_x64_debug `
    -BuildPreset win_x64_debug `
    -LaneTestPresets "win_x64_debug_architecture,win_x64_debug_unit,win_x64_debug_integration,win_x64_debug_native" `
    -BuildDir build\win_x64_debug `
    -Configuration Debug `
    -BaseRef master `
    -RequiredNativeBackend DX12 `
    -Fresh
```

Linux and macOS use the matching four lane presets and build directories. The
same script runs under PowerShell 7 (`pwsh`) on every CI host. Vulkan is the
required Linux native backend and Metal is the required macOS native backend.
Linux runs under Xvfb with an explicitly selected Vulkan ICD and installed font
fixtures required by the portable UI tests.

The platform-primary Native preset selects only the production backend and the
fixed lifecycle smoke: DX12 on Windows, Vulkan on Linux, and Metal on macOS.
Selecting the Native lane requires `-RequiredNativeBackend`; omitting it is a
fail-closed configuration error rather than a way to bypass native evidence.
The lifecycle test writes raw evidence for the requested and actual backend,
adapter identity, render-thread identity, present, resize, and shutdown. Build
Truth wraps that evidence with the source commit, preset, fallback verdict, and
software-adapter policy. Missing evidence, backend fallback, or an unapproved
software adapter fails the gate.

Successful execution writes these files below the selected build directory:

- `BuildTruth/BuildTruth.json`
- `BuildTruth/CTestInventory.json`
- `BuildTruth/CTestInventory.<lane>.json`
- `BuildTruth/JUnit.<lane>.xml`
- `BuildTruth/NativeRenderLifecycle.raw.json` when Native evidence is requested
- `BuildTruth/NativeEvidence.json` when Native evidence is requested

`Scripts/run_architecture_baseline.ps1` remains a thin, label-driven entry point
for running only the Architecture lane from an existing build. It does not own
a separate hard-coded test-name baseline.

ThreadSanitizer remains a separate focused Linux preset:

```bash
cmake --preset linux_x64_tsan
cmake --build --preset linux_x64_tsan --target RenderConcurrencyTSAN
ctest --preset linux_x64_tsan --output-on-failure
```

Exit code `0` means every requested lane and evidence check passed. Build Truth
does not replace Nightly qualification, broad compatibility-backend runtime
coverage, visual parity, soak testing, or the later Shipping product gate.

## Required PR and Scheduled Validation

The required PR graph deliberately executes Architecture once on Ubuntu. The
Windows, Linux, and macOS Debug jobs then run only Unit, Integration, and the
platform-primary Native lane. Each platform also configures a separate PR-smoke
tree that registers exactly the fixed `SceneLifecycle` Direct capture, image
validator, and JSON report validator at 320x180, Low quality, and eight frames.

Linux TSAN and the three-platform Shipping matrix are independent required
jobs. `PR Verdict` depends on every required job and fails when any result is
missing, cancelled, skipped, or unsuccessful. Shipping software adapters are
permitted only through the explicit `RVX_NATIVE_ALLOW_SOFTWARE_ADAPTER` opt-in;
the actual adapter and opt-in verdict remain part of the evidence.

Nightly runs full three-platform validation, Windows qualification/parity,
dedicated DX11/OpenGL compatibility smokes, a hash-pinned external real-asset
library, and the complete TSAN Integration lane. The scheduled repository must
provide `RVX_EXTERNAL_ASSET_URL` and `RVX_EXTERNAL_ASSET_SHA256` secrets; a
missing secret, bad hash, or incomplete archive fails the Nightly run.

Weekly uses a hermetic `R7Triangle` ModelViewer fixture and runs the
`suite.weekly-soak` contract for DX12, Vulkan, and Metal. That contract requires
120 warm-up plus 9,880 observation frames and at least 300,000 ms.
`Scripts/check_ci_workflows.py` and its self-tests keep the workflow, preset,
and runtime soak contracts synchronized and fail closed on missing jobs.
