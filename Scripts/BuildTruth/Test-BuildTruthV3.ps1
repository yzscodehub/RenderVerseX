Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

Import-Module (Join-Path $PSScriptRoot "BuildTruthV3.psm1") -Force

function Assert-Equal
{
    param([object]$Actual, [object]$Expected, [string]$Message)

    if ($Actual -ne $Expected)
    {
        throw "$Message. Expected '$Expected', got '$Actual'."
    }
}

function Assert-BuildTruthFailure
{
    param([string]$Category, [scriptblock]$Action)

    try
    {
        & $Action
    }
    catch
    {
        if ($_.Exception.Message -notmatch "^BT3\.${Category}:")
        {
            throw "Expected BT3.${Category} failure, got '$($_.Exception.Message)'"
        }
        return
    }
    throw "Expected BT3.${Category} failure, but the action completed."
}

$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) (
    "RenderVerseX-BuildTruthV3-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $temporaryRoot | Out-Null

try
{
    $validInventory = [pscustomobject]@{
        tests = @(
            [pscustomobject]@{
                name = "Unit.One"
                properties = @(
                    [pscustomobject]@{
                        name = "LABELS"
                        value = @("lane.unit", "subsystem.self-test")
                    }
                )
            }
        )
    }
    Assert-BuildTruthInventoryLanes -Inventory $validInventory -Context "self-test"
    Assert-BuildTruthFailure -Category "LaneClassification" -Action {
        $invalidInventory = [pscustomobject]@{
            tests = @(
                [pscustomobject]@{
                    name = "Unclassified"
                    properties = @()
                }
            )
        }
        Assert-BuildTruthInventoryLanes -Inventory $invalidInventory -Context "self-test"
    }
    Assert-BuildTruthFailure -Category "ZeroTests" -Action {
        Assert-BuildTruthInventoryLanes -Inventory ([pscustomobject]@{ tests = @() }) -Context "self-test"
    }
    Assert-BuildTruthFailure -Category "LaneClassification" -Action {
        $multiLaneInventory = [pscustomobject]@{
            tests = @(
                [pscustomobject]@{
                    name = "MultiLane"
                    properties = @(
                        [pscustomobject]@{
                            name = "LABELS"
                            value = @("lane.unit", "lane.integration")
                        }
                    )
                }
            )
        }
        Assert-BuildTruthInventoryLanes -Inventory $multiLaneInventory -Context "self-test"
    }

    $fingerprintRoot = Join-Path $temporaryRoot "fingerprint-source"
    New-Item -ItemType Directory -Path $fingerprintRoot | Out-Null
    & git -C $fingerprintRoot init -q
    if ($LASTEXITCODE -ne 0)
    {
        throw "Unable to create temporary repository for fingerprint self-test"
    }
    Set-Content -LiteralPath (Join-Path $fingerprintRoot "tracked.txt") -Value "tracked" -Encoding utf8
    & git -C $fingerprintRoot add -- tracked.txt
    if ($LASTEXITCODE -ne 0)
    {
        throw "Unable to stage fingerprint self-test input"
    }
    Set-Content -LiteralPath (Join-Path $fingerprintRoot "untracked.txt") -Value "first" -Encoding utf8
    $unicodeFileName = [string]::new([char[]]@(0x672A, 0x8DDF, 0x8E2A)) + ".txt"
    Set-Content -LiteralPath (Join-Path $fingerprintRoot $unicodeFileName) -Value "unicode path" -Encoding utf8
    $fingerprintSafeDirectory = $fingerprintRoot.Replace(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar)
    $firstFingerprint = Get-BuildTruthWorkingSourceFingerprint `
        -Root $fingerprintRoot `
        -GitSafeDirectory $fingerprintSafeDirectory
    Set-Content -LiteralPath (Join-Path $fingerprintRoot "untracked.txt") -Value "changed" -Encoding utf8
    $secondFingerprint = Get-BuildTruthWorkingSourceFingerprint `
        -Root $fingerprintRoot `
        -GitSafeDirectory $fingerprintSafeDirectory
    if ($firstFingerprint.digest -eq $secondFingerprint.digest)
    {
        throw "Working-source fingerprint did not change after untracked content changed."
    }

    $validJUnit = Join-Path $temporaryRoot "valid.junit.xml"
    @"
<?xml version="1.0" encoding="UTF-8"?>
<testsuites tests="2">
  <testsuite name="unit" tests="2" failures="0" errors="0" skipped="0">
    <testcase name="one"/>
    <testcase name="two"/>
  </testsuite>
</testsuites>
"@ | Set-Content -LiteralPath $validJUnit -Encoding utf8
    $validOutcome = Get-BuildTruthJUnitOutcome -Path $validJUnit
    Assert-Equal -Actual $validOutcome.testCaseCount -Expected 2 -Message "valid JUnit count"
    Assert-BuildTruthJUnitOutcome -Outcome $validOutcome -ExpectedTestCount 2 -Context "self-test"
    Assert-BuildTruthFailure -Category "NotRun" -Action {
        Assert-BuildTruthJUnitOutcome -Outcome $validOutcome -ExpectedTestCount 3 -Context "self-test"
    }

    $failedJUnit = Join-Path $temporaryRoot "failed.junit.xml"
    @"
<?xml version="1.0" encoding="UTF-8"?>
<testsuites tests="1">
  <testsuite name="unit" tests="1" failures="1">
    <testcase name="failed"><failure message="expected failure"/></testcase>
  </testsuite>
</testsuites>
"@ | Set-Content -LiteralPath $failedJUnit -Encoding utf8
    Assert-BuildTruthFailure -Category "TestFailure" -Action {
        $outcome = Get-BuildTruthJUnitOutcome -Path $failedJUnit
        Assert-BuildTruthJUnitOutcome -Outcome $outcome -ExpectedTestCount 1 -Context "self-test"
    }

    $disabledJUnit = Join-Path $temporaryRoot "disabled.junit.xml"
    @"
<?xml version="1.0" encoding="UTF-8"?>
<testsuites tests="1">
  <testsuite name="unit" tests="1" disabled="1">
    <testcase name="disabled"/>
  </testsuite>
</testsuites>
"@ | Set-Content -LiteralPath $disabledJUnit -Encoding utf8
    Assert-BuildTruthFailure -Category "Disabled" -Action {
        $outcome = Get-BuildTruthJUnitOutcome -Path $disabledJUnit
        Assert-BuildTruthJUnitOutcome -Outcome $outcome -ExpectedTestCount 1 -Context "self-test"
    }

    $skippedJUnit = Join-Path $temporaryRoot "skipped.junit.xml"
    @"
<?xml version="1.0" encoding="UTF-8"?>
<testsuites tests="1">
  <testsuite name="unit" tests="1" skipped="1">
    <testcase name="skipped"><skipped message="not available"/></testcase>
  </testsuite>
</testsuites>
"@ | Set-Content -LiteralPath $skippedJUnit -Encoding utf8
    Assert-BuildTruthFailure -Category "Skipped" -Action {
        $outcome = Get-BuildTruthJUnitOutcome -Path $skippedJUnit
        Assert-BuildTruthJUnitOutcome -Outcome $outcome -ExpectedTestCount 1 -Context "self-test"
    }
    $notRunJUnit = Join-Path $temporaryRoot "not-run.junit.xml"
    @"
<?xml version="1.0" encoding="UTF-8"?>
<testsuites tests="1">
  <testsuite name="unit" tests="1">
    <testcase name="not-run" status="notrun"/>
  </testsuite>
</testsuites>
"@ | Set-Content -LiteralPath $notRunJUnit -Encoding utf8
    Assert-BuildTruthFailure -Category "NotRun" -Action {
        $outcome = Get-BuildTruthJUnitOutcome -Path $notRunJUnit
        Assert-BuildTruthJUnitOutcome -Outcome $outcome -ExpectedTestCount 1 -Context "self-test"
    }
    Assert-BuildTruthFailure -Category "MissingArtifact" -Action {
        Get-BuildTruthJUnitOutcome -Path (Join-Path $temporaryRoot "missing.junit.xml") | Out-Null
    }

    $nativePath = Join-Path $temporaryRoot "native.json"
    [ordered]@{
        schema = "RVX.M1.NativeRenderLifecycle"
        schemaVersion = 2
        status = "passed"
        requestedBackend = "DX12"
        actualBackend = "DX12"
        backendFallback = $false
        softwareAdapter = $false
        softwareAdapterAllowed = $false
        adapterName = "NVIDIA Test Adapter"
        driverVersion = "1"
        mainThreadIdentityHash = 11
        renderThreadIdentityHash = 12
        surfaceGeneration = 2
        lastPresentedFrameSequence = 2
        resizeAcceptedCount = 1
        shutdownCode = 1
    } | ConvertTo-Json | Set-Content -LiteralPath $nativePath -Encoding utf8
    $nativeEvidence = Get-BuildTruthNativeEvidence `
        -Path $nativePath `
        -SourceCommit "0123456789abcdef" `
        -TestPreset "win_x64_debug_native" `
        -RequestedBackend "DX12" `
        -SoftwareAdapterAllowed $false
    Assert-Equal -Actual $nativeEvidence.actualBackend -Expected "DX12" -Message "native backend"
    Assert-Equal -Actual $nativeEvidence.fallback.backendFallback -Expected $false -Message "native fallback"

    ($nativeEvidence.lifecycle | Add-Member -NotePropertyName backendFallback -NotePropertyValue $true -Force)
    $nativeEvidence.lifecycle | ConvertTo-Json | Set-Content -LiteralPath $nativePath -Encoding utf8
    Assert-BuildTruthFailure -Category "Fallback" -Action {
        Get-BuildTruthNativeEvidence `
            -Path $nativePath `
            -SourceCommit "0123456789abcdef" `
            -TestPreset "win_x64_debug_native" `
            -RequestedBackend "DX12" `
            -SoftwareAdapterAllowed $false | Out-Null
    }

    $nativeRaw = Get-Content -LiteralPath $nativePath -Raw | ConvertFrom-Json
    $nativeRaw.backendFallback = $false
    $nativeRaw.actualBackend = "Vulkan"
    $nativeRaw | ConvertTo-Json | Set-Content -LiteralPath $nativePath -Encoding utf8
    Assert-BuildTruthFailure -Category "NativeBackendMismatch" -Action {
        Get-BuildTruthNativeEvidence `
            -Path $nativePath `
            -SourceCommit "0123456789abcdef" `
            -TestPreset "win_x64_debug_native" `
            -RequestedBackend "DX12" `
            -SoftwareAdapterAllowed $false | Out-Null
    }

    $nativeRaw.actualBackend = "DX12"
    $nativeRaw.softwareAdapter = $true
    $nativeRaw.softwareAdapterAllowed = $false
    $nativeRaw | ConvertTo-Json | Set-Content -LiteralPath $nativePath -Encoding utf8
    Assert-BuildTruthFailure -Category "Fallback" -Action {
        Get-BuildTruthNativeEvidence `
            -Path $nativePath `
            -SourceCommit "0123456789abcdef" `
            -TestPreset "win_x64_debug_native" `
            -RequestedBackend "DX12" `
            -SoftwareAdapterAllowed $false | Out-Null
    }

    $nativeRaw.softwareAdapter = $false
    $nativeRaw.softwareAdapterAllowed = $true
    $nativeRaw | ConvertTo-Json | Set-Content -LiteralPath $nativePath -Encoding utf8
    Assert-BuildTruthFailure -Category "NativeEvidenceMismatch" -Action {
        Get-BuildTruthNativeEvidence `
            -Path $nativePath `
            -SourceCommit "0123456789abcdef" `
            -TestPreset "win_x64_debug_native" `
            -RequestedBackend "DX12" `
            -SoftwareAdapterAllowed $false | Out-Null
    }

    Write-Host "Build Truth v3 module self-test passed."
}
finally
{
    if (Test-Path -LiteralPath $temporaryRoot)
    {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
}
