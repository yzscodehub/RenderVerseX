# Build Truth Gate

The build-truth gate is the authoritative precondition for architecture work.
It configures the selected preset, builds the project and complete validation
inventory, rejects unavailable CTest executables, runs the focused architecture
baseline and unit/lint preset, checks branch and working-tree whitespace, and
writes machine-readable evidence below the build directory.

The authoritative Runtime presets set `RVX_BUILD_EDITOR=OFF`. Editor remains an
optional separate build product and is not a Runtime correctness dependency.

Windows Debug from an empty build directory:

```powershell
Scripts\run_build_truth.ps1 `
    -ConfigurePreset win_x64_debug `
    -BuildPreset win_x64_debug `
    -TestPreset win_x64_debug_unit_lint `
    -BuildDir build\win_x64_debug `
    -Configuration Debug `
    -BaseRef master `
    -NativeTestRegex "^NativeRenderLifecycleValidation\.RequiredBackendPresentsResizesAndStops$" `
    -RequiredNativeBackend DX12 `
    -Fresh
```

Linux and macOS use the matching presets and build directories. The same script
runs under PowerShell 7 (`pwsh`) on all CI hosts, with Vulkan and Metal as their
required native backends. Linux runs under Xvfb with Mesa Vulkan available.

M1 extends the gate with one platform-primary native lifecycle smoke. The test
must write owned JSON evidence for adapter identity, render-thread identity,
present, resize, and shutdown. A skipped required backend, missing artifact, or
backend mismatch fails the gate. `Architecture.M1ArchitectureCut` is part of
the existing architecture baseline and is not executed a second time.

ThreadSanitizer remains a separate Linux preset because it instruments a small
Runtime Core target rather than the full Runtime build:

```bash
cmake --preset linux_x64_tsan
cmake --build --preset linux_x64_tsan --target RenderConcurrencyTSAN
ctest --preset linux_x64_tsan --output-on-failure
```

Editor compatibility is compiled once with
`win_x64_debug_editor_compile`; Editor tests are not part of Runtime Build
Truth.

Successful execution produces:

- `BuildTruth/BuildTruth.json`
- `BuildTruth/ArchitectureBaseline.json`
- `BuildTruth/ArchitectureBaseline.junit.xml`
- `BuildTruth/CTestInventory.txt`
- `BuildTruth/NativeRenderLifecycle.json` when a required native backend is
  supplied

Exit code `0` means every configured command and evidence check passed. Missing
Python, roots, manifests, targets, test executables, baseline patterns, Git base
references, or whitespace cleanliness produce a non-zero exit.

The gate proves build, validation, and one thin native lifecycle path. It does
not replace deterministic fake-RHI lifetime suites, TSAN, or broader backend
feature and visual validation.
