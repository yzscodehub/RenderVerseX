param(
    [string]$ConfigurePreset = "win_x64_debug",
    [string]$BuildPreset = "win_x64_debug",
    [string]$TestPreset = "win_x64_debug_unit_lint",
    [string]$BuildDir = "build\win_x64_debug",
    [string]$Configuration = "Debug",
    [string]$BaseRef = "master",
    [string]$NativeTestRegex = "",
    [string]$RequiredNativeBackend = "",
    [switch]$Fresh
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$gitSafeDirectory = $repoRoot.Replace([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
$buildPath = [IO.Path]::GetFullPath((Join-Path $repoRoot $BuildDir))
$buildRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot "build"))
$buildRootPrefix = $buildRoot.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
$reportDir = Join-Path $buildPath "BuildTruth"
$reportPath = Join-Path $reportDir "BuildTruth.json"
$baselineReport = Join-Path $reportDir "ArchitectureBaseline.json"
$nativeReport = Join-Path $reportDir "NativeRenderLifecycle.json"
$commands = [Collections.Generic.List[object]]::new()
$status = "failed"
$failure = ""
$testCount = 0
$sourceCommit = ""
$nativeEvidence = $null

function Invoke-Checked([string]$Name, [scriptblock]$Command) {
    $started = Get-Date
    try {
        & $Command
        $exitCode = $LASTEXITCODE
    }
    catch {
        $exitCode = 1
        $commands.Add([ordered]@{
            name = $Name
            exitCode = $exitCode
            durationMs = [int]((Get-Date) - $started).TotalMilliseconds
        })
        throw
    }
    $commands.Add([ordered]@{
        name = $Name
        exitCode = $exitCode
        durationMs = [int]((Get-Date) - $started).TotalMilliseconds
    })
    if ($exitCode -ne 0) {
        throw "$Name failed with exit code $exitCode"
    }
}

if (-not $buildPath.StartsWith($buildRootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "BuildDir must resolve below '$buildRoot': $buildPath"
}

Push-Location $repoRoot
try {
    Invoke-Checked "verify-base-ref" {
        git -c "safe.directory=$gitSafeDirectory" -C $repoRoot rev-parse --verify "$BaseRef^{commit}" | Out-Null
    }
    $sourceCommitLines = Invoke-Checked "source-commit" {
        git -c "safe.directory=$gitSafeDirectory" -C $repoRoot rev-parse HEAD
    }
    $sourceCommit = ($sourceCommitLines -join "`n").Trim()

    if ($Fresh -and (Test-Path -LiteralPath $buildPath)) {
        Remove-Item -LiteralPath $buildPath -Recurse -Force
    }

    Invoke-Checked "configure" {
        cmake --preset $ConfigurePreset
    }
    $editorSetting = Select-String `
        -LiteralPath (Join-Path $buildPath "CMakeCache.txt") `
        -Pattern "^RVX_BUILD_EDITOR:BOOL=OFF$"
    if (-not $editorSetting) {
        throw "M0 build truth requires RVX_BUILD_EDITOR=OFF"
    }
    Invoke-Checked "build" {
        cmake --build --preset $BuildPreset
    }
    Invoke-Checked "validation-inventory-target" {
        cmake --build $buildPath --config $Configuration --target RVXValidationInventory
    }

    New-Item -ItemType Directory -Force -Path $reportDir | Out-Null
    $inventoryLog = Join-Path $reportDir "CTestInventory.txt"
    $inventoryStarted = Get-Date
    $inventoryLines = & ctest --test-dir $buildPath -C $Configuration -N 2>&1
    $inventoryExit = $LASTEXITCODE
    $inventoryLines | Set-Content -LiteralPath $inventoryLog -Encoding utf8
    $commands.Add([ordered]@{
        name = "ctest-inventory"
        exitCode = $inventoryExit
        durationMs = [int]((Get-Date) - $inventoryStarted).TotalMilliseconds
    })
    if ($inventoryExit -ne 0) {
        throw "CTest inventory command failed with exit code $inventoryExit"
    }
    $inventoryText = $inventoryLines -join "`n"
    if ($inventoryText -match "NOT_BUILT|Could not find executable") {
        throw "CTest inventory contains unavailable executables; see $inventoryLog"
    }
    $totalMatch = [regex]::Match($inventoryText, "Total Tests:\s+(\d+)")
    $testCount = if ($totalMatch.Success) { [int]$totalMatch.Groups[1].Value } else { 0 }
    if ($testCount -eq 0) {
        throw "CTest inventory reported zero tests"
    }

    Invoke-Checked "architecture-baseline" {
        & pwsh -NoProfile -File (Join-Path $PSScriptRoot "run_architecture_baseline.ps1") `
            -BuildDir $BuildDir `
            -Configuration $Configuration `
            -ReportPath $baselineReport
    }

    if (-not [string]::IsNullOrWhiteSpace($TestPreset)) {
        Invoke-Checked "unit-lint" {
            ctest --preset $TestPreset
        }
    }

    $hasNativeRegex = -not [string]::IsNullOrWhiteSpace($NativeTestRegex)
    $hasRequiredNativeBackend =
        -not [string]::IsNullOrWhiteSpace($RequiredNativeBackend)
    if ($hasNativeRegex -ne $hasRequiredNativeBackend) {
        throw "NativeTestRegex and RequiredNativeBackend must be supplied together"
    }
    if ($hasNativeRegex) {
        if (Test-Path -LiteralPath $nativeReport) {
            Remove-Item -LiteralPath $nativeReport -Force
        }
        $previousNativeReport = $env:RVX_NATIVE_LIFECYCLE_REPORT
        $env:RVX_NATIVE_LIFECYCLE_REPORT = $nativeReport
        try {
            Invoke-Checked "native-render-lifecycle" {
                ctest --test-dir $buildPath -C $Configuration `
                    -R $NativeTestRegex --output-on-failure
            }
        }
        finally {
            if ($null -eq $previousNativeReport) {
                Remove-Item Env:RVX_NATIVE_LIFECYCLE_REPORT `
                    -ErrorAction SilentlyContinue
            }
            else {
                $env:RVX_NATIVE_LIFECYCLE_REPORT = $previousNativeReport
            }
        }
        if (-not (Test-Path -LiteralPath $nativeReport)) {
            throw "Required native lifecycle evidence is missing: $nativeReport"
        }
        try {
            $nativeEvidence =
                Get-Content -LiteralPath $nativeReport -Raw |
                    ConvertFrom-Json
        }
        catch {
            throw "Required native lifecycle evidence is invalid JSON: $($_.Exception.Message)"
        }
        if ($nativeEvidence.schema -ne "RVX.M1.NativeRenderLifecycle" -or
            $nativeEvidence.schemaVersion -ne 1 -or
            $nativeEvidence.status -ne "passed") {
            throw "Required native lifecycle evidence has an invalid schema or verdict"
        }
        if ($nativeEvidence.requiredBackend -ne $RequiredNativeBackend) {
            throw "Required native lifecycle backend mismatch: expected '$RequiredNativeBackend', got '$($nativeEvidence.requiredBackend)'"
        }
        if ([string]::IsNullOrWhiteSpace($nativeEvidence.adapterName) -or
            $nativeEvidence.mainThreadIdentityHash -eq 0 -or
            $nativeEvidence.renderThreadIdentityHash -eq 0 -or
            $nativeEvidence.mainThreadIdentityHash -eq
                $nativeEvidence.renderThreadIdentityHash -or
            $nativeEvidence.surfaceGeneration -lt 2 -or
            $nativeEvidence.lastPresentedFrameSequence -lt 2 -or
            $nativeEvidence.resizeAcceptedCount -lt 1 -or
            $nativeEvidence.shutdownCode -ne 1) {
            throw "Required native lifecycle evidence is incomplete"
        }
    }

    Invoke-Checked "branch-diff-hygiene" {
        git -c "safe.directory=$gitSafeDirectory" -C $repoRoot diff --check "$BaseRef...HEAD"
    }
    Invoke-Checked "working-diff-hygiene" {
        git -c "safe.directory=$gitSafeDirectory" -C $repoRoot diff --check
    }
    Invoke-Checked "staged-diff-hygiene" {
        git -c "safe.directory=$gitSafeDirectory" -C $repoRoot diff --cached --check
    }
    $status = "passed"
}
catch {
    $failure = $_.Exception.Message
    throw
}
finally {
    New-Item -ItemType Directory -Force -Path $reportDir | Out-Null
    $report = [ordered]@{
        schema = "RVX.BuildTruth.Report"
        schemaVersion = 2
        sourceCommit = $sourceCommit
        baseRef = $BaseRef
        configurePreset = $ConfigurePreset
        buildPreset = $BuildPreset
        testPreset = $TestPreset
        buildDir = $buildPath
        configuration = $Configuration
        fresh = [bool]$Fresh
        ctestInventoryCount = $testCount
        architectureBaselineReport = $baselineReport
        m1ArchitectureGate = "Architecture.M1ArchitectureCut"
        nativeLifecycle = if ($null -eq $nativeEvidence) {
            $null
        } else {
            [ordered]@{
                testRegex = $NativeTestRegex
                requiredBackend = $RequiredNativeBackend
                reportPath = $nativeReport
                evidence = $nativeEvidence
            }
        }
        status = $status
        failure = $failure
        commands = $commands
    }
    $report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $reportPath -Encoding utf8
    Pop-Location
}

Write-Host "Build truth passed. Report: $reportPath"
