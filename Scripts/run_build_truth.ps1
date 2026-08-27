param(
    [string]$ConfigurePreset = "win_x64_debug",
    [string]$BuildPreset = "win_x64_debug",
    [string[]]$LaneTestPresets = @(),
    [string]$BuildDir = "build\win_x64_debug",
    [string]$Configuration = "Debug",
    [string]$BaseRef = "master",
    [string]$RequiredNativeBackend = "",
    [switch]$Fresh
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ($env:OS -eq "Windows_NT")
{
    $canonicalPath = $env:Path
    [Environment]::SetEnvironmentVariable("PATH", $null, "Process")
    [Environment]::SetEnvironmentVariable("Path", $null, "Process")
    [Environment]::SetEnvironmentVariable("PATH", $canonicalPath, "Process")
}

Import-Module (Join-Path $PSScriptRoot "BuildTruth\BuildTruthV3.psm1") -Force

function Add-BuildTruthCommand
{
    param(
        [System.Collections.Generic.List[object]]$Commands,
        [Parameter(Mandatory)][string]$Name,
        [string]$FailureCategory = "Command",
        [Parameter(Mandatory)][scriptblock]$Action
    )

    $started = Get-Date
    try
    {
        $result = & $Action
        $exitCode = $LASTEXITCODE
        if ($exitCode -ne 0)
        {
            Throw-BuildTruthFailure -Category $FailureCategory -Message "$Name failed with exit code $exitCode"
        }
        return $result
    }
    catch
    {
        Throw-BuildTruthFailure -Category $FailureCategory -Message (
            "$Name threw: $($_.Exception.Message)")
    }
    finally
    {
        $Commands.Add([ordered]@{
            name = $Name
            exitCode = $LASTEXITCODE
            durationMs = [int]((Get-Date) - $started).TotalMilliseconds
        })
    }
}

function Get-BuildTruthLanePresetList
{
    param([string[]]$Values, [string]$ConfigurePreset)

    $presets = [System.Collections.Generic.List[string]]::new()
    foreach ($value in $Values)
    {
        foreach ($item in ($value -split ","))
        {
            $trimmed = $item.Trim()
            if (-not [string]::IsNullOrWhiteSpace($trimmed))
            {
                $presets.Add($trimmed)
            }
        }
    }
    if ($presets.Count -eq 0)
    {
        foreach ($lane in @("architecture", "unit", "integration", "native"))
        {
            $presets.Add("${ConfigurePreset}_${lane}")
        }
    }
    return @($presets)
}

function Test-BuildTruthEnabledEnvironmentFlag
{
    param([AllowNull()][string]$Value)

    return -not [string]::IsNullOrWhiteSpace($Value) -and
        $Value -match "^(?i:1|true)$"
}

function Get-BuildTruthRepositoryRelativeDirectory
{
    param(
        [Parameter(Mandatory)][string]$RepositoryRoot,
        [Parameter(Mandatory)][string]$Path
    )

    $absolutePath = Resolve-BuildTruthContainedPath `
        -Root $RepositoryRoot `
        -Path $Path
    if (-not (Test-Path -LiteralPath $absolutePath -PathType Container))
    {
        Throw-BuildTruthFailure -Category "ReceiptPath" -Message (
            "Directory '$Path' is not an existing repository-contained directory")
    }
    $repositoryPath = [IO.Path]::GetFullPath($RepositoryRoot).TrimEnd(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar)
    return [IO.Path]::GetRelativePath($repositoryPath, $absolutePath).Replace(
        [IO.Path]::DirectorySeparatorChar,
        "/")
}

function Publish-BuildTruthImmutableReceipt
{
    param(
        [Parameter(Mandatory)][string]$RepositoryRoot,
        [Parameter(Mandatory)][string]$RunId,
        [Parameter(Mandatory)][string]$ReportPath,
        [Parameter(Mandatory)][string]$BuildPath,
        [Parameter(Mandatory)][string]$BaseRef,
        [Parameter(Mandatory)][string]$ConfigurePreset,
        [Parameter(Mandatory)][string]$BuildPreset,
        [Parameter(Mandatory)][string[]]$LaneTestPresets,
        [Parameter(Mandatory)][string]$Configuration,
        [Parameter(Mandatory)][AllowEmptyString()][string]$RequiredNativeBackend,
        [Parameter(Mandatory)][bool]$Fresh,
        [Parameter(Mandatory)][string]$SourceCommit,
        [Parameter(Mandatory)][object]$SourceFingerprintAtStart,
        [Parameter(Mandatory)][object]$SourceFingerprintAtEnd,
        [Parameter(Mandatory)][AllowEmptyCollection()][object[]]$LaneReports,
        [Parameter(Mandatory)][hashtable]$LaneRuntimeEvidence,
        [AllowNull()][object]$NativeEvidence,
        [AllowNull()][object]$NativeRuntimeEvidence,
        [Parameter(Mandatory)][bool]$SoftwareAdapterAllowed,
        [Parameter(Mandatory)][string]$NativeLifecyclePath,
        [Parameter(Mandatory)][string]$NativeEvidencePath
    )

    $junitLanes = [System.Collections.Generic.List[object]]::new()
    $lanes = [System.Collections.Generic.List[object]]::new()
    foreach ($laneReport in $LaneReports)
    {
        if (-not $LaneRuntimeEvidence.ContainsKey($laneReport.lane))
        {
            Throw-BuildTruthFailure -Category "ReceiptSchema" -Message (
                "A passed Build Truth lane has no captured runtime evidence: $($laneReport.lane)")
        }
        $runtimeEvidence = $LaneRuntimeEvidence[$laneReport.lane]
        Assert-BuildTruthReceiptFileEvidence `
            -RepositoryRoot $RepositoryRoot `
            -Evidence $runtimeEvidence.junit `
            -Context "Build Truth lane '$($laneReport.lane)' JUnit"
        Assert-BuildTruthReceiptFileEvidence `
            -RepositoryRoot $RepositoryRoot `
            -Evidence $runtimeEvidence.inventory `
            -Context "Build Truth lane '$($laneReport.lane)' inventory"
        $currentJUnitOutcome = Get-BuildTruthJUnitOutcome -Path $laneReport.junitPath
        Assert-BuildTruthJUnitOutcome `
            -Outcome $currentJUnitOutcome `
            -ExpectedTestCount $laneReport.selectedTestCount `
            -Context "Build Truth lane '$($laneReport.lane)' receipt"
        Assert-BuildTruthJUnitOutcomeMatches `
            -Expected $runtimeEvidence.outcome `
            -Actual $currentJUnitOutcome `
            -Context "Build Truth lane '$($laneReport.lane)' JUnit"
        $junitLanes.Add([ordered]@{
            lane = $laneReport.lane
            testPreset = $laneReport.testPreset
            selectedTestCount = $laneReport.selectedTestCount
            path = $runtimeEvidence.junit.path
            sha256 = $runtimeEvidence.junit.sha256
            outcome = $runtimeEvidence.outcome
        })
        $lanes.Add([ordered]@{
            lane = $laneReport.lane
            testPreset = $laneReport.testPreset
            selectedTestCount = $laneReport.selectedTestCount
            inventory = $runtimeEvidence.inventory
            durationMs = $laneReport.durationMs
        })
    }
    if ($junitLanes.Count -eq 0)
    {
        Throw-BuildTruthFailure -Category "ReceiptSchema" -Message (
            "A passed Build Truth run has no lane JUnit evidence")
    }

    $nativeReceiptEvidence = $null
    if ($null -ne $NativeEvidence)
    {
        if ($null -eq $NativeRuntimeEvidence)
        {
            Throw-BuildTruthFailure -Category "ReceiptSchema" -Message (
                "A passed Build Truth native lane has no captured runtime evidence")
        }
        Assert-BuildTruthReceiptFileEvidence `
            -RepositoryRoot $RepositoryRoot `
            -Evidence $NativeRuntimeEvidence.raw `
            -Context "Build Truth native raw evidence"
        Assert-BuildTruthReceiptFileEvidence `
            -RepositoryRoot $RepositoryRoot `
            -Evidence $NativeRuntimeEvidence.normalized `
            -Context "Build Truth native normalized evidence"
        $currentNativeEvidence = Get-BuildTruthNativeEvidence `
            -Path $NativeLifecyclePath `
            -SourceCommit $SourceCommit `
            -TestPreset $NativeRuntimeEvidence.testPreset `
            -RequestedBackend $RequiredNativeBackend `
            -SoftwareAdapterAllowed $SoftwareAdapterAllowed
        Assert-BuildTruthNativeEvidenceIdentity `
            -Expected $NativeRuntimeEvidence.identity `
            -Actual $currentNativeEvidence `
            -Context "Build Truth native evidence"
        $nativeReceiptEvidence = [ordered]@{
            raw = $NativeRuntimeEvidence.raw
            normalized = $NativeRuntimeEvidence.normalized
            requestedBackend = $RequiredNativeBackend
            actualBackend = $NativeEvidence.actualBackend
            fallback = $NativeEvidence.fallback
        }
    }
    elseif (-not [string]::IsNullOrWhiteSpace($RequiredNativeBackend))
    {
        Throw-BuildTruthFailure -Category "NativeEvidenceMissing" -Message (
            "A passed Build Truth run requested native evidence but did not produce it")
    }

    $relativeBuildDirectory = Get-BuildTruthRepositoryRelativeDirectory `
        -RepositoryRoot $RepositoryRoot `
        -Path $BuildPath
    $runnerDigest = Get-BuildTruthRunnerDigest `
        -RepositoryRoot $RepositoryRoot `
        -Paths @(
            "Scripts/run_build_truth.ps1",
            "Scripts/BuildTruth/BuildTruthV3.psm1"
        )
    $receiptPath = ".agent-harness/receipts/BuildTruth.$RunId.json"
    $receipt = [ordered]@{
        schema = "RVX.AgentHarness.BuildTruthReceipt"
        schemaVersion = 1
        runId = $RunId
        immutableReceiptPath = $receiptPath
        generatedAtUtc = (Get-Date).ToUniversalTime().ToString("o")
        request = [ordered]@{
            baseRef = $BaseRef
            configurePreset = $ConfigurePreset
            buildPreset = $BuildPreset
            laneTestPresets = @($LaneTestPresets)
            buildDir = $relativeBuildDirectory
            configuration = $Configuration
            requiredNativeBackend = $RequiredNativeBackend
            softwareAdapterAllowed = $SoftwareAdapterAllowed
            fresh = $Fresh
            runnerDigest = $runnerDigest
        }
        source = [ordered]@{
            commit = $SourceCommit
            fingerprintBefore = $SourceFingerprintAtStart
            fingerprintAfter = $SourceFingerprintAtEnd
        }
        report = Get-BuildTruthReceiptFileEvidence `
            -RepositoryRoot $RepositoryRoot `
            -Path $ReportPath
        junit = [ordered]@{
            lanes = @($junitLanes)
        }
        lanes = @($lanes)
        nativeEvidence = $nativeReceiptEvidence
    }
    $immutableReceipt = Write-BuildTruthImmutableReceipt `
        -RepositoryRoot $RepositoryRoot `
        -ReceiptKind "BuildTruth" `
        -RunId $RunId `
        -Value $receipt `
        -Status "passed"
    $latestPointer = Write-BuildTruthReceiptPointer `
        -RepositoryRoot $RepositoryRoot `
        -ReceiptKind "BuildTruth" `
        -PointerFileName "BuildTruth.latest.json" `
        -RunId $immutableReceipt.runId `
        -ImmutableReceiptPath $immutableReceipt.immutableReceiptPath `
        -ImmutableReceiptSha256 $immutableReceipt.sha256
    return [ordered]@{
        runId = $immutableReceipt.runId
        immutableReceiptPath = $immutableReceipt.immutableReceiptPath
        sha256 = $immutableReceipt.sha256
        latestPointerPath = $latestPointer.path
        latestPointerSha256 = $latestPointer.sha256
    }
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$gitSafeDirectory = $repoRoot.Replace([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
$buildPath = [IO.Path]::GetFullPath((Join-Path $repoRoot $BuildDir))
$buildRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot "build"))
$reportDir = Join-Path $buildPath "BuildTruth"
$reportPath = Join-Path $reportDir "BuildTruth.json"
$inventoryPath = Join-Path $reportDir "CTestInventory.json"
$nativeLifecyclePath = Join-Path $reportDir "NativeRenderLifecycle.raw.json"
$nativeEvidencePath = Join-Path $reportDir "NativeEvidence.json"
$commands = [System.Collections.Generic.List[object]]::new()
$laneReports = [System.Collections.Generic.List[object]]::new()
$laneRuntimeEvidence = @{}
$status = "failed"
$failure = ""
$failureCategory = ""
$sourceCommit = ""
$sourceCommitAtStart = ""
$sourceFingerprintAtStart = $null
$sourceFingerprintAtEnd = $null
$nativeEvidence = $null
$nativeRuntimeEvidence = $null
$fullInventory = $null
$runId = [guid]::NewGuid().ToString("N")
$receiptInfo = $null
$runLock = $null
$laneTestPresets = Get-BuildTruthLanePresetList -Values $LaneTestPresets -ConfigurePreset $ConfigurePreset

$null = Resolve-BuildTruthContainedPath `
    -Root $buildRoot `
    -Path $buildPath `
    -AllowMissing

$runLock = Enter-BuildTruthRunLock -BuildPath $buildPath
$locationPushed = $false
try
{
    Push-Location $repoRoot
    $locationPushed = $true
    Add-BuildTruthCommand -Commands $commands -Name "verify-base-ref" -FailureCategory "BaseRef" -Action {
        git -c "safe.directory=$gitSafeDirectory" -C $repoRoot rev-parse --verify "$BaseRef^{commit}" | Out-Null
    } | Out-Null
    $sourceCommitLines = Add-BuildTruthCommand -Commands $commands -Name "source-commit" -FailureCategory "SourceCommit" -Action {
        git -c "safe.directory=$gitSafeDirectory" -C $repoRoot rev-parse HEAD
    }
    $sourceCommit = ($sourceCommitLines -join [Environment]::NewLine).Trim()
    $sourceCommitAtStart = $sourceCommit
    Add-BuildTruthCommand -Commands $commands -Name "build-truth-v3-self-test" -FailureCategory "SelfTest" -Action {
        & (Join-Path $PSScriptRoot "BuildTruth\Test-BuildTruthV3.ps1")
        if ($LASTEXITCODE -ne 0)
        {
            Throw "module self-test exited with code $LASTEXITCODE"
        }
    } | Out-Null
    $sourceFingerprintAtStart = Get-BuildTruthWorkingSourceFingerprint `
        -Root $repoRoot `
        -GitSafeDirectory $gitSafeDirectory

    if ($Fresh -and (Test-Path -LiteralPath $buildPath))
    {
        Remove-Item -LiteralPath $buildPath -Recurse -Force
    }

    Add-BuildTruthCommand -Commands $commands -Name "configure" -FailureCategory "Configure" -Action {
        cmake --preset $ConfigurePreset
    } | Out-Null
    $cachePath = Join-Path $buildPath "CMakeCache.txt"
    $requiredValidationSettings = @(
        "RVX_BUILD_DEVELOPER:BOOL=ON",
        "RVX_BUILD_PROGRAMS:BOOL=ON",
        "RVX_BUILD_SHIPPING_PRODUCTS:BOOL=OFF",
        "RVX_BUILD_EDITOR:BOOL=OFF",
        "RVX_BUILD_SAMPLES:BOOL=ON",
        "RVX_BUILD_TESTS:BOOL=ON"
    )
    foreach ($requiredSetting in $requiredValidationSettings)
    {
        $setting = Select-String `
            -LiteralPath $cachePath `
            -Pattern "^$([regex]::Escape($requiredSetting))$"
        if (-not $setting)
        {
            Throw-BuildTruthFailure -Category "Configuration" -Message (
                "Build Truth validation profile requires $requiredSetting")
        }
    }
    Add-BuildTruthCommand -Commands $commands -Name "build-validation-all" -FailureCategory "Build" -Action {
        cmake --build --preset $BuildPreset --target RVXValidationAll
    } | Out-Null

    New-Item -ItemType Directory -Force -Path $reportDir | Out-Null
    $fullInventory = Get-BuildTruthCTestInventory -BuildPath $buildPath -Configuration $Configuration
    Assert-BuildTruthInventoryLanes -Inventory $fullInventory -Context "full inventory"
    Write-BuildTruthJson -Value $fullInventory -Path $inventoryPath

    $encounteredLanes = [System.Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($testPreset in $laneTestPresets)
    {
        $laneInventory = Get-BuildTruthCTestInventory `
            -BuildPath $buildPath `
            -Configuration $Configuration `
            -TestPreset $testPreset
        $selectedTests = @($laneInventory.tests)
        if ($selectedTests.Count -eq 0)
        {
            Throw-BuildTruthFailure -Category "ZeroTests" -Message (
                "Required lane preset '$testPreset' selects zero tests")
        }
        Assert-BuildTruthInventoryLanes -Inventory $laneInventory -Context "lane preset '$testPreset'"
        $lanes = @(
            $selectedTests |
                ForEach-Object { Get-BuildTruthTestLanes -Test $_ } |
                Select-Object -Unique
        )
        if ($lanes.Count -ne 1)
        {
            Throw-BuildTruthFailure -Category "LaneClassification" -Message (
                "Lane preset '$testPreset' selects multiple lanes: $($lanes -join ', ')")
        }
        $lane = $lanes[0].Substring("lane.".Length)
        if (-not $encounteredLanes.Add($lane))
        {
            Throw-BuildTruthFailure -Category "DuplicateLane" -Message (
                "Lane '$lane' is selected more than once")
        }

        $laneInventoryPath = Join-Path $reportDir "CTestInventory.$lane.json"
        $junitPath = Join-Path $reportDir "JUnit.$lane.xml"
        Write-BuildTruthJson -Value $laneInventory -Path $laneInventoryPath
        $laneInventoryEvidence = Get-BuildTruthReceiptFileEvidence `
            -RepositoryRoot $repoRoot `
            -Path $laneInventoryPath

        $isNativeLane = $lane -eq "native"
        if ($isNativeLane -and [string]::IsNullOrWhiteSpace($RequiredNativeBackend))
        {
            Throw-BuildTruthFailure -Category "NativeEvidenceMissing" -Message (
                "Native lane '$testPreset' requires RequiredNativeBackend so lifecycle evidence cannot be skipped")
        }
        $previousNativeLifecyclePath = $env:RVX_NATIVE_LIFECYCLE_REPORT
        $previousValidationSourceCommit =
            $env:RVX_VALIDATION_SOURCE_COMMIT
        $previousValidationPreset = $env:RVX_VALIDATION_PRESET
        $env:RVX_VALIDATION_SOURCE_COMMIT = $sourceCommit
        $env:RVX_VALIDATION_PRESET = $testPreset
        if ($isNativeLane -and -not [string]::IsNullOrWhiteSpace($RequiredNativeBackend))
        {
            $nativeLifecycleTests = @(
                $selectedTests | Where-Object {
                    $_.name -eq "NativeRenderLifecycleValidation.RequiredBackendPresentsResizesAndStops"
                }
            )
            if ($nativeLifecycleTests.Count -ne 1)
            {
                Throw-BuildTruthFailure -Category "NativeEvidenceMissing" -Message (
                    "Native lane '$testPreset' must discover exactly one required lifecycle test")
            }
            if (Test-Path -LiteralPath $nativeLifecyclePath)
            {
                Remove-Item -LiteralPath $nativeLifecyclePath -Force
            }
            $env:RVX_NATIVE_LIFECYCLE_REPORT = $nativeLifecyclePath
        }
        try
        {
            $laneRun = Invoke-BuildTruthCTestLane `
                -TestPreset $testPreset `
                -JUnitPath $junitPath `
                -ExpectedTestCount $selectedTests.Count `
                -Lane $lane
        }
        finally
        {
            if ($null -eq $previousNativeLifecyclePath)
            {
                Remove-Item Env:RVX_NATIVE_LIFECYCLE_REPORT -ErrorAction SilentlyContinue
            }
            else
            {
                $env:RVX_NATIVE_LIFECYCLE_REPORT = $previousNativeLifecyclePath
            }
            if ($null -eq $previousValidationSourceCommit)
            {
                Remove-Item Env:RVX_VALIDATION_SOURCE_COMMIT -ErrorAction SilentlyContinue
            }
            else
            {
                $env:RVX_VALIDATION_SOURCE_COMMIT = $previousValidationSourceCommit
            }
            if ($null -eq $previousValidationPreset)
            {
                Remove-Item Env:RVX_VALIDATION_PRESET -ErrorAction SilentlyContinue
            }
            else
            {
                $env:RVX_VALIDATION_PRESET = $previousValidationPreset
            }
        }

        $laneJUnitEvidence = Get-BuildTruthReceiptFileEvidence `
            -RepositoryRoot $repoRoot `
            -Path $junitPath
        $capturedJUnitOutcome = Get-BuildTruthJUnitOutcome -Path $junitPath
        Assert-BuildTruthJUnitOutcome `
            -Outcome $capturedJUnitOutcome `
            -ExpectedTestCount $selectedTests.Count `
            -Context "Build Truth lane '$lane' captured evidence"
        Assert-BuildTruthJUnitOutcomeMatches `
            -Expected $laneRun.outcome `
            -Actual $capturedJUnitOutcome `
            -Context "Build Truth lane '$lane' captured JUnit"
        $laneRuntimeEvidence[$lane] = [ordered]@{
            inventory = $laneInventoryEvidence
            junit = $laneJUnitEvidence
            outcome = $capturedJUnitOutcome
        }

        $laneReports.Add([ordered]@{
            lane = $lane
            testPreset = $testPreset
            selectedTestCount = $selectedTests.Count
            inventoryPath = $laneInventoryPath
            junitPath = $junitPath
            outcome = $laneRun.outcome
            durationMs = $laneRun.durationMs
        })

        if ($isNativeLane -and -not [string]::IsNullOrWhiteSpace($RequiredNativeBackend))
        {
            $nativeEvidence = Get-BuildTruthNativeEvidence `
                -Path $nativeLifecyclePath `
                -SourceCommit $sourceCommit `
                -TestPreset $testPreset `
                -RequestedBackend $RequiredNativeBackend `
                -SoftwareAdapterAllowed (Test-BuildTruthEnabledEnvironmentFlag `
                    -Value $env:RVX_NATIVE_ALLOW_SOFTWARE_ADAPTER)
            Write-BuildTruthJson -Value $nativeEvidence -Path $nativeEvidencePath
            $nativeRuntimeEvidence = [ordered]@{
                raw = Get-BuildTruthReceiptFileEvidence `
                    -RepositoryRoot $repoRoot `
                    -Path $nativeLifecyclePath
                normalized = Get-BuildTruthReceiptFileEvidence `
                    -RepositoryRoot $repoRoot `
                    -Path $nativeEvidencePath
                identity = $nativeEvidence
                testPreset = $testPreset
            }
        }
    }

    if (-not [string]::IsNullOrWhiteSpace($RequiredNativeBackend) -and $null -eq $nativeEvidence)
    {
        Throw-BuildTruthFailure -Category "NativeEvidenceMissing" -Message (
            "RequiredNativeBackend was supplied but no native lane was executed")
    }

    $sourceCommitAtEnd = (& git -c "safe.directory=$gitSafeDirectory" -C $repoRoot rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or $sourceCommitAtEnd -ne $sourceCommitAtStart)
    {
        Throw-BuildTruthFailure -Category "SourceChanged" -Message (
            "HEAD changed while Build Truth was running")
    }
    $sourceFingerprintAtEnd = Get-BuildTruthWorkingSourceFingerprint `
        -Root $repoRoot `
        -GitSafeDirectory $gitSafeDirectory
    if ($sourceFingerprintAtEnd.digest -ne $sourceFingerprintAtStart.digest -or
        $sourceFingerprintAtEnd.fileCount -ne $sourceFingerprintAtStart.fileCount)
    {
        Throw-BuildTruthFailure -Category "SourceChanged" -Message (
            "Tracked or non-ignored untracked working-source content changed while Build Truth was running")
    }
    Add-BuildTruthCommand -Commands $commands -Name "branch-diff-hygiene" -FailureCategory "DiffHygiene" -Action {
        git -c "safe.directory=$gitSafeDirectory" -C $repoRoot diff --check "$BaseRef...HEAD"
    } | Out-Null
    Add-BuildTruthCommand -Commands $commands -Name "working-diff-hygiene" -FailureCategory "DiffHygiene" -Action {
        git -c "safe.directory=$gitSafeDirectory" -C $repoRoot diff --check
    } | Out-Null
    Add-BuildTruthCommand -Commands $commands -Name "staged-diff-hygiene" -FailureCategory "DiffHygiene" -Action {
        git -c "safe.directory=$gitSafeDirectory" -C $repoRoot diff --cached --check
    } | Out-Null
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
    try
    {
        New-Item -ItemType Directory -Force -Path $reportDir | Out-Null
        $report = [ordered]@{
            schema = "RVX.BuildTruth.Report"
            schemaVersion = 3
            sourceCommit = $sourceCommit
            baseRef = $BaseRef
            configurePreset = $ConfigurePreset
            buildPreset = $BuildPreset
            laneTestPresets = $laneTestPresets
            buildDir = $buildPath
            configuration = $Configuration
            fresh = [bool]$Fresh
            sourceFingerprint = if ($null -eq $sourceFingerprintAtStart) { $null } else { [ordered]@{
                start = $sourceFingerprintAtStart
                end = $sourceFingerprintAtEnd
            }}
            inventory = if ($null -eq $fullInventory) { $null } else { [ordered]@{
                path = $inventoryPath
                testCount = @($fullInventory.tests).Count
            }}
            lanes = $laneReports
            nativeEvidence = if ($null -eq $nativeEvidence) { $null } else { [ordered]@{
                rawPath = $nativeLifecyclePath
                path = $nativeEvidencePath
                requestedBackend = $RequiredNativeBackend
                actualBackend = $nativeEvidence.actualBackend
                fallback = $nativeEvidence.fallback
            }}
            status = $status
            failure = if ([string]::IsNullOrWhiteSpace($failure)) { $null } else { [ordered]@{
                category = $failureCategory
                message = $failure
            }}
            commands = $commands
        }
        Write-BuildTruthJson -Value $report -Path $reportPath

        if ($status -eq "passed")
        {
            try
            {
                $receiptInfo = Publish-BuildTruthImmutableReceipt `
                    -RepositoryRoot $repoRoot `
                    -RunId $runId `
                    -ReportPath $reportPath `
                    -BuildPath $buildPath `
                    -BaseRef $BaseRef `
                    -ConfigurePreset $ConfigurePreset `
                    -BuildPreset $BuildPreset `
                    -LaneTestPresets $laneTestPresets `
                    -Configuration $Configuration `
                    -RequiredNativeBackend $RequiredNativeBackend `
                    -Fresh ([bool]$Fresh) `
                    -SourceCommit $sourceCommit `
                    -SourceFingerprintAtStart $sourceFingerprintAtStart `
                    -SourceFingerprintAtEnd $sourceFingerprintAtEnd `
                    -LaneReports $laneReports.ToArray() `
                    -LaneRuntimeEvidence $laneRuntimeEvidence `
                    -NativeEvidence $nativeEvidence `
                    -NativeRuntimeEvidence $nativeRuntimeEvidence `
                    -SoftwareAdapterAllowed (Test-BuildTruthEnabledEnvironmentFlag `
                        -Value $env:RVX_NATIVE_ALLOW_SOFTWARE_ADAPTER) `
                    -NativeLifecyclePath $nativeLifecyclePath `
                    -NativeEvidencePath $nativeEvidencePath
            }
            catch
            {
                $status = "failed"
                $failureCategory = "ReceiptPublication"
                $failure = "BT3.ReceiptPublication: $($_.Exception.Message)"
                $report.status = $status
                $report.failure = [ordered]@{
                    category = $failureCategory
                    message = $failure
                }
                Write-BuildTruthJson -Value $report -Path $reportPath
                Throw-BuildTruthFailure -Category $failureCategory -Message $failure
            }
        }
    }
    finally
    {
        try
        {
            if ($locationPushed)
            {
                Pop-Location
            }
        }
        finally
        {
            Exit-BuildTruthRunLock -Lock $runLock
        }
    }
}

Write-Host "Build Truth v3 passed. Report: $reportPath"
Write-Host "Immutable receipt: $($receiptInfo.immutableReceiptPath) (runId=$($receiptInfo.runId), sha256=$($receiptInfo.sha256))"
Write-Host "Latest pointer: $($receiptInfo.latestPointerPath) (sha256=$($receiptInfo.latestPointerSha256))"
