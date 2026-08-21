param(
    [Parameter(Mandatory)][string]$ConfigurePreset,
    [Parameter(Mandatory)][string]$BuildPreset,
    [Parameter(Mandatory)][string]$TestPreset,
    [Parameter(Mandatory)][string]$BuildDir,
    [Parameter(Mandatory)][string]$Configuration,
    [Parameter(Mandatory)][string[]]$BuildTarget,
    [Parameter(Mandatory)][string]$RequiredLabel,
    [int]$ExpectedTestCount = 0,
    [string[]]$RequiredEvidence = @(),
    [string]$BaseRef = "master",
    [switch]$Fresh,
    [switch]$SkipConfigure
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# Some Windows process launchers preserve both PATH and Path. MSBuild copies
# environment variables into a case-insensitive dictionary and fails before
# invoking CL when both spellings are present. Re-publish one canonical entry.
if ($env:OS -eq "Windows_NT")
{
    $canonicalPath = $env:Path
    Remove-Item Env:PATH -ErrorAction SilentlyContinue
    $env:Path = $canonicalPath
}

Import-Module (Join-Path $PSScriptRoot "BuildTruth\BuildTruthV3.psm1") -Force

function Invoke-RequiredSuiteNativeCommand
{
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()]
        [System.Collections.Generic.List[object]]$Commands,
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][string]$FailureCategory,
        [Parameter(Mandatory)][scriptblock]$Action
    )

    $started = Get-Date
    $exitCode = 0
    try
    {
        $output = & $Action
        $exitCode = $LASTEXITCODE
        $output | ForEach-Object { Write-Host $_ }
        if ($exitCode -ne 0)
        {
            Throw-BuildTruthFailure -Category $FailureCategory -Message (
                "$Name failed with exit code $exitCode")
        }
    }
    catch
    {
        if ($_.Exception.Message -match "^BT3\.")
        {
            throw
        }
        Throw-BuildTruthFailure -Category $FailureCategory -Message (
            "$Name threw: $($_.Exception.Message)")
    }
    finally
    {
        $Commands.Add([ordered]@{
            name = $Name
            exitCode = $exitCode
            durationMs = [int]((Get-Date) - $started).TotalMilliseconds
        })
    }
}

function Get-RequiredSuiteToken
{
    param([Parameter(Mandatory)][string]$Label)

    $token = $Label -replace "[^A-Za-z0-9_.-]", "-"
    return $token.Trim("-", ".", "_")
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$gitSafeDirectory = $repoRoot.Replace(
    [IO.Path]::DirectorySeparatorChar,
    [IO.Path]::AltDirectorySeparatorChar)
$buildRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot "build"))
$buildPath = [IO.Path]::GetFullPath((Join-Path $repoRoot $BuildDir))
$buildRootPrefix = $buildRoot.TrimEnd(
    [IO.Path]::DirectorySeparatorChar,
    [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
if (-not $buildPath.StartsWith($buildRootPrefix, [StringComparison]::OrdinalIgnoreCase))
{
    throw "BuildDir must resolve below '$buildRoot': $buildPath"
}

$suiteToken = Get-RequiredSuiteToken -Label $RequiredLabel
if ([string]::IsNullOrWhiteSpace($suiteToken))
{
    throw "RequiredLabel does not produce a valid report token"
}
$reportDir = Join-Path $buildPath "BuildTruth"
$reportPath = Join-Path $reportDir "RequiredSuite.$suiteToken.json"
$inventoryPath = Join-Path $reportDir "CTestInventory.$suiteToken.json"
$junitPath = Join-Path $reportDir "JUnit.$suiteToken.xml"
$runId = [guid]::NewGuid().ToString("N")
$commands = [System.Collections.Generic.List[object]]::new()
$status = "failed"
$failure = ""
$failureCategory = ""
$sourceCommit = ""
$sourceFingerprintAtStart = $null
$sourceFingerprintAtEnd = $null
$inventory = $null
$junitOutcome = $null
$semanticEvidence = @()
$startedAt = Get-Date

Push-Location $repoRoot
try
{
    Invoke-RequiredSuiteNativeCommand `
        -Commands $commands `
        -Name "verify-base-ref" `
        -FailureCategory "BaseRef" `
        -Action {
            git -c "safe.directory=$gitSafeDirectory" -C $repoRoot `
                rev-parse --verify "$BaseRef^{commit}"
        }

    $sourceCommitOutput = & git -c "safe.directory=$gitSafeDirectory" `
        -C $repoRoot rev-parse HEAD
    if ($LASTEXITCODE -ne 0)
    {
        Throw-BuildTruthFailure -Category "SourceCommit" -Message (
            "Unable to resolve the source commit")
    }
    $sourceCommit = ($sourceCommitOutput -join [Environment]::NewLine).Trim()
    $sourceFingerprintAtStart = Get-BuildTruthWorkingSourceFingerprint `
        -Root $repoRoot `
        -GitSafeDirectory $gitSafeDirectory

    Invoke-RequiredSuiteNativeCommand `
        -Commands $commands `
        -Name "build-truth-v3-self-test" `
        -FailureCategory "SelfTest" `
        -Action {
            & (Join-Path $PSScriptRoot "BuildTruth\Test-BuildTruthV3.ps1")
        }
    Invoke-RequiredSuiteNativeCommand `
        -Commands $commands `
        -Name "required-suite-self-test" `
        -FailureCategory "SelfTest" `
        -Action {
            & (Join-Path $PSScriptRoot "BuildTruth\Test-RequiredSuiteV1.ps1")
        }

    if ($Fresh -and (Test-Path -LiteralPath $buildPath))
    {
        Remove-Item -LiteralPath $buildPath -Recurse -Force
    }

    if (-not $SkipConfigure)
    {
        Invoke-RequiredSuiteNativeCommand `
            -Commands $commands `
            -Name "configure" `
            -FailureCategory "Configure" `
            -Action { cmake --preset $ConfigurePreset }
    }
    elseif (-not (Test-Path -LiteralPath (Join-Path $buildPath "CMakeCache.txt") -PathType Leaf))
    {
        Throw-BuildTruthFailure -Category "Configuration" -Message (
            "SkipConfigure requires an existing configured build tree: $buildPath")
    }

    $buildArguments = @("--build", "--preset", $BuildPreset, "--target")
    $buildArguments += @($BuildTarget)
    Invoke-RequiredSuiteNativeCommand `
        -Commands $commands `
        -Name "build-required-targets" `
        -FailureCategory "Build" `
        -Action { cmake @buildArguments }

    New-Item -ItemType Directory -Force -Path $reportDir | Out-Null
    $inventory = Get-BuildTruthCTestInventory `
        -BuildPath $buildPath `
        -Configuration $Configuration `
        -TestPreset $TestPreset
    Assert-BuildTruthRequiredSelection `
        -Inventory $inventory `
        -RequiredLabel $RequiredLabel `
        -ExpectedTestCount $ExpectedTestCount `
        -Context "required preset '$TestPreset'"
    Write-BuildTruthJson -Value $inventory -Path $inventoryPath
    $selectedTests = @($inventory.tests)

    $normalizedEvidencePaths = [System.Collections.Generic.List[string]]::new()
    foreach ($evidenceArgument in $RequiredEvidence)
    {
        foreach ($evidencePath in ($evidenceArgument -split ";"))
        {
            $trimmedEvidencePath = $evidencePath.Trim()
            if ($trimmedEvidencePath)
            {
                $normalizedEvidencePaths.Add($trimmedEvidencePath)
            }
        }
    }

    $resolvedEvidencePaths = [System.Collections.Generic.List[string]]::new()
    foreach ($evidencePath in $normalizedEvidencePaths)
    {
        $absoluteEvidencePath = Resolve-BuildTruthContainedPath `
            -Root $buildPath `
            -Path $evidencePath `
            -AllowMissing
        if (Test-Path -LiteralPath $absoluteEvidencePath)
        {
            if (-not (Test-Path -LiteralPath $absoluteEvidencePath -PathType Leaf))
            {
                Throw-BuildTruthFailure -Category "EvidencePath" -Message (
                    "Required evidence path is not a file: $absoluteEvidencePath")
            }
            Remove-Item -LiteralPath $absoluteEvidencePath -Force
        }
        $resolvedEvidencePaths.Add($absoluteEvidencePath)
    }

    $previousRunId = $env:RVX_VALIDATION_RUN_ID
    $previousCommit = $env:RVX_VALIDATION_SOURCE_COMMIT
    $previousPreset = $env:RVX_VALIDATION_PRESET
    $env:RVX_VALIDATION_RUN_ID = $runId
    $env:RVX_VALIDATION_SOURCE_COMMIT = $sourceCommit
    $env:RVX_VALIDATION_PRESET = $TestPreset
    try
    {
        $laneRun = Invoke-BuildTruthCTestLane `
            -TestPreset $TestPreset `
            -JUnitPath $junitPath `
            -ExpectedTestCount $selectedTests.Count `
            -Lane $RequiredLabel
        $junitOutcome = $laneRun.outcome
    }
    finally
    {
        if ($null -eq $previousRunId)
        {
            Remove-Item Env:RVX_VALIDATION_RUN_ID -ErrorAction SilentlyContinue
        }
        else
        {
            $env:RVX_VALIDATION_RUN_ID = $previousRunId
        }
        if ($null -eq $previousCommit)
        {
            Remove-Item Env:RVX_VALIDATION_SOURCE_COMMIT -ErrorAction SilentlyContinue
        }
        else
        {
            $env:RVX_VALIDATION_SOURCE_COMMIT = $previousCommit
        }
        if ($null -eq $previousPreset)
        {
            Remove-Item Env:RVX_VALIDATION_PRESET -ErrorAction SilentlyContinue
        }
        else
        {
            $env:RVX_VALIDATION_PRESET = $previousPreset
        }
    }

    $semanticEvidence = Get-BuildTruthFileEvidence `
        -BuildRoot $buildPath `
        -Paths @($resolvedEvidencePaths)

    $sourceCommitAtEnd = (& git -c "safe.directory=$gitSafeDirectory" `
        -C $repoRoot rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or $sourceCommitAtEnd -ne $sourceCommit)
    {
        Throw-BuildTruthFailure -Category "SourceChanged" -Message (
            "HEAD changed while the required suite was running")
    }
    $sourceFingerprintAtEnd = Get-BuildTruthWorkingSourceFingerprint `
        -Root $repoRoot `
        -GitSafeDirectory $gitSafeDirectory
    if ($sourceFingerprintAtEnd.digest -ne $sourceFingerprintAtStart.digest -or
        $sourceFingerprintAtEnd.fileCount -ne $sourceFingerprintAtStart.fileCount)
    {
        Throw-BuildTruthFailure -Category "SourceChanged" -Message (
            "Working-source content changed while the required suite was running")
    }

    Invoke-RequiredSuiteNativeCommand `
        -Commands $commands `
        -Name "working-diff-hygiene" `
        -FailureCategory "DiffHygiene" `
        -Action { git -c "safe.directory=$gitSafeDirectory" -C $repoRoot diff --check }

    $status = "passed"
}
catch
{
    $failure = $_.Exception.Message
    $failureCategory = Get-BuildTruthFailureCategory -Message $failure
    throw
}
finally
{
    New-Item -ItemType Directory -Force -Path $reportDir | Out-Null
    $junitEvidence = $null
    if (Test-Path -LiteralPath $junitPath -PathType Leaf)
    {
        $junitEvidence = [ordered]@{
            path = $junitPath
            sha256 = (Get-FileHash -LiteralPath $junitPath -Algorithm SHA256).Hash.ToLowerInvariant()
            outcome = $junitOutcome
        }
    }
    $report = [ordered]@{
        schema = "RVX.RequiredSuite.Report"
        schemaVersion = 1
        runId = $runId
        sourceCommit = $sourceCommit
        baseRef = $BaseRef
        sourceFingerprintBefore = $sourceFingerprintAtStart
        sourceFingerprintAfter = $sourceFingerprintAtEnd
        configurePreset = $ConfigurePreset
        buildPreset = $BuildPreset
        testPreset = $TestPreset
        buildDir = $buildPath
        configuration = $Configuration
        buildTargets = @($BuildTarget)
        requiredLabel = $RequiredLabel
        expectedTestCount = if ($ExpectedTestCount -gt 0) { $ExpectedTestCount } else { $null }
        selectedTests = if ($null -eq $inventory) { @() } else { @($inventory.tests | ForEach-Object { $_.name }) }
        inventoryPath = if ($null -eq $inventory) { $null } else { $inventoryPath }
        junitPath = if ($null -eq $junitEvidence) { $null } else { $junitEvidence.path }
        junitSha256 = if ($null -eq $junitEvidence) { $null } else { $junitEvidence.sha256 }
        junit = $junitEvidence
        semanticArtifacts = @($semanticEvidence)
        commands = @($commands)
        startedAtUtc = $startedAt.ToUniversalTime().ToString("o")
        durationMs = [int]((Get-Date) - $startedAt).TotalMilliseconds
        status = $status
        failureCategory = if ([string]::IsNullOrWhiteSpace($failureCategory)) { $null } else { $failureCategory }
        failure = if ([string]::IsNullOrWhiteSpace($failure)) { $null } else { [ordered]@{
            category = $failureCategory
            message = $failure
        }}
    }
    Write-BuildTruthJson -Value $report -Path $reportPath -Depth 16
    Pop-Location
}
