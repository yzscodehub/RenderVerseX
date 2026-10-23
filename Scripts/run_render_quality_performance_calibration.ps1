#requires -Version 7.0

param(
    [ValidateRange(5, 5)][int]$RunCount = 5,
    [string]$ConfigurePreset = "win_x64_gfx24_performance_calibration_release",
    [string]$BuildPreset = "win_x64_gfx24_performance_calibration_release",
    [string]$TestPreset = "win_x64_gfx24_performance_calibration_release_run",
    [string]$BuildDir = "build/gfx24_perf_calibration",
    [string]$OutputRoot = "build/gfx24_perf_calibration/PerformanceCalibration",
    [string]$PythonExecutable = "",
    [switch]$SkipConfigure,
    [switch]$SelfTestOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$buildRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot "build"))
$buildPath = [IO.Path]::GetFullPath((Join-Path $repoRoot $BuildDir))
$outputPath = [IO.Path]::GetFullPath((Join-Path $repoRoot $OutputRoot))
Import-Module (Join-Path $PSScriptRoot "BuildTruth/BuildTruthV3.psm1") -Force
$null = Resolve-BuildTruthContainedPath -Root $buildRoot -Path $buildPath -AllowMissing
$null = Resolve-BuildTruthContainedPath -Root $buildRoot -Path $outputPath -AllowMissing

$configuredForPreflight = $false
$cachePath = Join-Path $buildPath "CMakeCache.txt"
if (-not (Test-Path -LiteralPath $cachePath -PathType Leaf))
{
    if ($SkipConfigure)
    {
        throw "SkipConfigure requires an existing calibration CMakeCache.txt"
    }
    cmake -S $repoRoot --preset $ConfigurePreset
    if ($LASTEXITCODE -ne 0)
    {
        throw "Calibration configure preset failed"
    }
    $configuredForPreflight = $true
}
if ([string]::IsNullOrWhiteSpace($PythonExecutable))
{
    $pythonMatch = Select-String -LiteralPath $cachePath -Pattern (
        "^_Python3_EXECUTABLE:INTERNAL=(.+)$") | Select-Object -First 1
    if ($null -eq $pythonMatch)
    {
        throw "CMake cache has no Python3 interpreter; pass -PythonExecutable"
    }
    $PythonExecutable = $pythonMatch.Matches[0].Groups[1].Value
}
& $PythonExecutable --version
if ($LASTEXITCODE -ne 0)
{
    throw "Python interpreter probe failed: $PythonExecutable"
}

$contractSelfTest = Join-Path $PSScriptRoot "test_render_quality_qualification.py"
& $PythonExecutable $contractSelfTest -v
if ($LASTEXITCODE -ne 0)
{
    throw "GFX24 qualification contract self-test failed"
}
if ($SelfTestOnly)
{
    Write-Host "GFX24 performance calibration preflight passed."
    exit 0
}

$sessionId = [guid]::NewGuid().ToString("N")
$sessionPath = Join-Path $outputPath $sessionId
$runsPath = Join-Path $sessionPath "runs"
$candidatePath = Join-Path $sessionPath "candidate"
New-Item -ItemType Directory -Path $runsPath | Out-Null

$suiteLabel = "suite.gfx24-performance-calibration"
$evidencePath = Join-Path $buildPath (
    "Tests/Gfx24PerformanceCalibration/Release/" +
    "DX12DeferredHigh/performance.json")
$pointerPath = Join-Path $repoRoot (
    ".agent-harness/receipts/" +
    "RequiredSuite.suite.gfx24-performance-calibration.latest.json")
$requiredSuiteReport = Join-Path $buildPath (
    "BuildTruth/RequiredSuite.suite.gfx24-performance-calibration.json")
$runner = Join-Path $PSScriptRoot "run_required_suite.ps1"
$sourceCommit = $null
$sourceFingerprintDigest = $null
$runDirectories = [System.Collections.Generic.List[string]]::new()

for ($ordinal = 1; $ordinal -le $RunCount; ++$ordinal)
{
    Write-Host "Starting GFX24 performance calibration run $ordinal/$RunCount"
    $arguments = @{
        ConfigurePreset = $ConfigurePreset
        BuildPreset = $BuildPreset
        TestPreset = $TestPreset
        BuildDir = $BuildDir
        Configuration = "Release"
        BuildTarget = @("RVXValidationSuiteGfx24PerformanceCalibration")
        RequiredLabel = $suiteLabel
        ExpectedTestCount = 1
        RequiredEvidence = @($evidencePath)
    }
    if ($SkipConfigure -or $configuredForPreflight -or $ordinal -gt 1)
    {
        $arguments.SkipConfigure = $true
    }
    & $runner @arguments
    if ($LASTEXITCODE -ne 0)
    {
        throw "RequiredSuite failed for calibration run $ordinal"
    }
    if (-not (Test-Path -LiteralPath $evidencePath -PathType Leaf))
    {
        throw "Calibration report is missing after run $ordinal"
    }
    if (-not (Test-Path -LiteralPath $pointerPath -PathType Leaf))
    {
        throw "RequiredSuite pointer is missing after run $ordinal"
    }

    $report = Get-Content -LiteralPath $evidencePath -Raw | ConvertFrom-Json
    $pointer = Get-Content -LiteralPath $pointerPath -Raw | ConvertFrom-Json
    $receiptPath = Resolve-BuildTruthContainedPath -Root $repoRoot -Path ([string]$pointer.immutableReceiptPath)
    $receipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
    if ([string]$report.runId -ne [string]$pointer.runId -or
        [string]$receipt.runId -ne [string]$report.runId)
    {
        throw "Run id mismatch in calibration run $ordinal"
    }
    if ([string]$report.sourceCommit -ne [string]$receipt.source.commit -or
        [string]$report.preset -ne $TestPreset)
    {
        throw "Source commit or preset mismatch in calibration run $ordinal"
    }
    if ($null -eq $sourceCommit)
    {
        $sourceCommit = [string]$report.sourceCommit
    }
    elseif ($sourceCommit -ne [string]$report.sourceCommit)
    {
        throw "Source commit changed between calibration runs"
    }
    $receiptHash = (Get-FileHash -LiteralPath $receiptPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($receiptHash -ne ([string]$pointer.immutableReceiptSha256).ToLowerInvariant())
    {
        throw "RequiredSuite receipt hash mismatch in run $ordinal"
    }
    $reportHash = (Get-FileHash -LiteralPath $evidencePath -Algorithm SHA256).Hash.ToLowerInvariant()
    $boundArtifact = @($receipt.semanticArtifacts) | Where-Object {
        ([string]$_.sha256).ToLowerInvariant() -eq $reportHash
    }
    if ($boundArtifact.Count -ne 1)
    {
        throw "RequiredSuite receipt does not uniquely bind run $ordinal report"
    }

    $fingerprintBefore = $receipt.source.fingerprintBefore
    $fingerprintAfter = $receipt.source.fingerprintAfter
    if ([string]$fingerprintBefore.digest -ne
            [string]$fingerprintAfter.digest -or
        [int]$fingerprintBefore.fileCount -ne
            [int]$fingerprintAfter.fileCount)
    {
        throw "Source fingerprint changed during calibration run $ordinal"
    }
    if ($null -eq $sourceFingerprintDigest)
    {
        $sourceFingerprintDigest = [string]$fingerprintBefore.digest
    }
    elseif ($sourceFingerprintDigest -ne [string]$fingerprintBefore.digest)
    {
        throw "Source fingerprint changed between calibration runs"
    }

    $runPath = Join-Path $runsPath ("{0:D2}" -f $ordinal)
    New-Item -ItemType Directory -Path $runPath | Out-Null
    Copy-Item -LiteralPath $evidencePath -Destination (
        Join-Path $runPath "performance.json")
    Copy-Item -LiteralPath $receiptPath -Destination (
        Join-Path $runPath "required-suite-receipt.json")
    Copy-Item -LiteralPath $requiredSuiteReport -Destination (
        Join-Path $runPath "required-suite-report.json")
    $runDirectories.Add($runPath)
}

$tool = Join-Path $PSScriptRoot "render_quality_performance_baseline.py"
$prepareArguments = @(
    $tool,
    "prepare",
    "--output-dir", $candidatePath,
    "--baseline-id", "gfx24-dx12-rtx4070ti-v1",
    "--expected-source-commit", $sourceCommit,
    "--expected-source-fingerprint", $sourceFingerprintDigest,
    "--expected-preset", $TestPreset,
    "--graphics-budget-ms", "16.67",
    "--render-graph-budget-ms", "15.0",
    "--max-regression-percent", "10.0"
)
foreach ($runDirectory in $runDirectories)
{
    $prepareArguments += @("--run-dir", $runDirectory)
}
& $PythonExecutable @prepareArguments
if ($LASTEXITCODE -ne 0)
{
    throw "Performance baseline candidate preparation failed"
}

$recordPath = Join-Path $candidatePath "baseline-record.json"
Write-Host "GFX24 performance calibration completed."
Write-Host "Candidate record: $recordPath"
Write-Warning (
    "The candidate is not approved and cannot satisfy the PR performance " +
    "gate. Review it, then run the explicit approve command.")
