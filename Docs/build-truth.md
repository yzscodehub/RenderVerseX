# Build Truth Gate

The M0 build-truth gate is the authoritative precondition for architecture work.
It configures the selected preset, builds the project and complete validation
inventory, rejects unavailable CTest executables, runs the focused architecture
baseline and unit/lint preset, checks branch and working-tree whitespace, and
writes machine-readable evidence below the build directory.

The authoritative M0 presets set `RVX_BUILD_EDITOR=OFF`. Editor remains an
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
    -Fresh
```

Linux and macOS use the matching presets and build directories. The same script
runs under PowerShell 7 (`pwsh`) on all CI hosts.

Successful execution produces:

- `BuildTruth/BuildTruth.json`
- `BuildTruth/ArchitectureBaseline.json`
- `BuildTruth/ArchitectureBaseline.junit.xml`
- `BuildTruth/CTestInventory.txt`

Exit code `0` means every configured command and evidence check passed. Missing
Python, roots, manifests, targets, test executables, baseline patterns, Git base
references, or whitespace cleanliness produce a non-zero exit.

The gate proves build and validation truth. It does not by itself certify GPU
behavior or Tier 1 backend production readiness.
