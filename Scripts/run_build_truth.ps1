param(
    [string]$ConfigurePreset = "win_x64_debug",
    [string]$BuildPreset = "win_x64_debug",
    [string]$TestPreset = "win_x64_debug_unit_lint",
    [string]$BuildDir = "build\win_x64_debug",
    [string]$Configuration = "Debug",
    [string]$BaseRef = "master",
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
$commands = [Collections.Generic.List[object]]::new()
$status = "failed"
$failure = ""
$testCount = 0
$sourceCommit = ""

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
        schemaVersion = 1
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
        status = $status
        failure = $failure
        commands = $commands
    }
    $report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $reportPath -Encoding utf8
    Pop-Location
}

Write-Host "Build truth passed. Report: $reportPath"
