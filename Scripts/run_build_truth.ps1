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

function Get-BuildTruthFailureCategory
{
    param([string]$Message)

    if ($Message -match "^BT3\.([A-Za-z0-9]+):")
    {
        return $Matches[1]
    }
    return "Unexpected"
}

function Test-BuildTruthEnabledEnvironmentFlag
{
    param([AllowNull()][string]$Value)

    return -not [string]::IsNullOrWhiteSpace($Value) -and
        $Value -match "^(?i:1|true)$"
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$gitSafeDirectory = $repoRoot.Replace([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
$buildPath = [IO.Path]::GetFullPath((Join-Path $repoRoot $BuildDir))
$buildRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot "build"))
$buildRootPrefix = $buildRoot.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
$reportDir = Join-Path $buildPath "BuildTruth"
$reportPath = Join-Path $reportDir "BuildTruth.json"
$inventoryPath = Join-Path $reportDir "CTestInventory.json"
$nativeLifecyclePath = Join-Path $reportDir "NativeRenderLifecycle.raw.json"
$nativeEvidencePath = Join-Path $reportDir "NativeEvidence.json"
$commands = [System.Collections.Generic.List[object]]::new()
$laneReports = [System.Collections.Generic.List[object]]::new()
$status = "failed"
$failure = ""
$failureCategory = ""
$sourceCommit = ""
$sourceCommitAtStart = ""
$sourceFingerprintAtStart = $null
$sourceFingerprintAtEnd = $null
$nativeEvidence = $null
$fullInventory = $null
$laneTestPresets = Get-BuildTruthLanePresetList -Values $LaneTestPresets -ConfigurePreset $ConfigurePreset

if (-not $buildPath.StartsWith($buildRootPrefix, [StringComparison]::OrdinalIgnoreCase))
{
    throw "BuildDir must resolve below '$buildRoot': $buildPath"
}

Push-Location $repoRoot
try
{
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

        $isNativeLane = $lane -eq "native"
        if ($isNativeLane -and [string]::IsNullOrWhiteSpace($RequiredNativeBackend))
        {
            Throw-BuildTruthFailure -Category "NativeEvidenceMissing" -Message (
                "Native lane '$testPreset' requires RequiredNativeBackend so lifecycle evidence cannot be skipped")
        }
        $previousNativeLifecyclePath = $env:RVX_NATIVE_LIFECYCLE_REPORT
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
    Pop-Location
}

Write-Host "Build Truth v3 passed. Report: $reportPath"
