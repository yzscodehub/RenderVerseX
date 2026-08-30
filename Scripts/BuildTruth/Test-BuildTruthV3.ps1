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
    & git -c core.excludesFile=/dev/null -C $fingerprintRoot add -- tracked.txt
    if ($LASTEXITCODE -ne 0)
    {
        throw "Unable to stage fingerprint self-test input"
    }
    Set-Content -LiteralPath (Join-Path $fingerprintRoot "untracked.txt") -Value "first" -Encoding utf8
    $unicodeFileName = [string]::new([char[]]@(0x672A, 0x8DDF, 0x8E2A)) + ".txt"
    Set-Content -LiteralPath (Join-Path $fingerprintRoot $unicodeFileName) -Value "unicode path" -Encoding utf8
    $orderedFingerprint = Get-BuildTruthPathFingerprint `
        -Root $fingerprintRoot `
        -RelativePaths @("tracked.txt", $unicodeFileName, "untracked.txt", "tracked.txt")
    $reorderedFingerprint = Get-BuildTruthPathFingerprint `
        -Root $fingerprintRoot `
        -RelativePaths @("untracked.txt", "tracked.txt", $unicodeFileName)
    Assert-Equal `
        -Actual $orderedFingerprint.fileCount `
        -Expected 3 `
        -Message "path fingerprint removes exact duplicates"
    Assert-Equal `
        -Actual $orderedFingerprint.digest `
        -Expected $reorderedFingerprint.digest `
        -Message "path fingerprint uses order-independent ordinal serialization"
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

    New-Item -ItemType Directory -Path (Join-Path $fingerprintRoot ".agent-harness\receipts") | Out-Null
    Set-Content -LiteralPath (Join-Path $fingerprintRoot "feature_list.json") -Value "ledger-one" -Encoding utf8
    Set-Content -LiteralPath (Join-Path $fingerprintRoot "progress.md") -Value "progress-one" -Encoding utf8
    Set-Content -LiteralPath (Join-Path $fingerprintRoot "session-handoff.md") -Value "handoff-one" -Encoding utf8
    Set-Content -LiteralPath (Join-Path $fingerprintRoot ".agent-harness\receipts\BuildTruth.test.json") -Value "receipt-one" -Encoding utf8
    $controlPlaneFingerprint = Get-BuildTruthWorkingSourceFingerprint `
        -Root $fingerprintRoot `
        -GitSafeDirectory $fingerprintSafeDirectory
    Assert-Equal -Actual $controlPlaneFingerprint.digest -Expected $secondFingerprint.digest `
        -Message "mutable control-plane files do not alter product fingerprint"
    Set-Content -LiteralPath (Join-Path $fingerprintRoot "feature_list.json") -Value "ledger-two" -Encoding utf8
    Set-Content -LiteralPath (Join-Path $fingerprintRoot ".agent-harness\receipts\BuildTruth.test.json") -Value "receipt-two" -Encoding utf8
    $updatedControlPlaneFingerprint = Get-BuildTruthWorkingSourceFingerprint `
        -Root $fingerprintRoot `
        -GitSafeDirectory $fingerprintSafeDirectory
    Assert-Equal -Actual $updatedControlPlaneFingerprint.digest -Expected $secondFingerprint.digest `
        -Message "control-plane updates do not alter product fingerprint"

    Set-Content -LiteralPath (Join-Path $fingerprintRoot "init.ps1") -Value "product contract" -Encoding utf8
    $contractFingerprint = Get-BuildTruthWorkingSourceFingerprint `
        -Root $fingerprintRoot `
        -GitSafeDirectory $fingerprintSafeDirectory
    if ($contractFingerprint.digest -eq $secondFingerprint.digest)
    {
        throw "Product fingerprint ignored an init.ps1 contract change."
    }

    $userExcludeFile = Join-Path $fingerprintRoot "user-global-ignore.txt"
    Set-Content -LiteralPath $userExcludeFile -Value "hidden-by-user.txt" -Encoding utf8
    & git -C $fingerprintRoot config core.excludesFile $userExcludeFile
    if ($LASTEXITCODE -ne 0)
    {
        throw "Unable to configure the temporary global-excludes self-test"
    }
    $beforeExcludedProduct = Get-BuildTruthWorkingSourceFingerprint `
        -Root $fingerprintRoot `
        -GitSafeDirectory $fingerprintSafeDirectory
    Set-Content -LiteralPath (Join-Path $fingerprintRoot "hidden-by-user.txt") -Value "must remain visible" -Encoding utf8
    $afterExcludedProduct = Get-BuildTruthWorkingSourceFingerprint `
        -Root $fingerprintRoot `
        -GitSafeDirectory $fingerprintSafeDirectory
    if ($beforeExcludedProduct.digest -eq $afterExcludedProduct.digest)
    {
        throw "Product fingerprint was affected by core.excludesFile."
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

    $receiptRepository = Join-Path $temporaryRoot "receipt-repository"
    New-Item -ItemType Directory -Path $receiptRepository | Out-Null
    $receiptInputPath = Join-Path $receiptRepository "evidence.txt"
    Set-Content -LiteralPath $receiptInputPath -Value "before" -Encoding utf8
    $receiptEvidence = Get-BuildTruthReceiptFileEvidence `
        -RepositoryRoot $receiptRepository `
        -Path $receiptInputPath
    Assert-BuildTruthReceiptFileEvidence `
        -RepositoryRoot $receiptRepository `
        -Evidence $receiptEvidence `
        -Context "self-test receipt evidence"
    Set-Content -LiteralPath $receiptInputPath -Value "after" -Encoding utf8
    Assert-BuildTruthFailure -Category "ReceiptTampered" -Action {
        Assert-BuildTruthReceiptFileEvidence `
            -RepositoryRoot $receiptRepository `
            -Evidence $receiptEvidence `
            -Context "self-test tampering"
    }
    Assert-BuildTruthFailure -Category "PathEscape" -Action {
        Get-BuildTruthReceiptFileEvidence `
            -RepositoryRoot $receiptRepository `
            -Path (Join-Path $temporaryRoot "outside.txt") | Out-Null
    }

    $missingBuildRoot = Join-Path $temporaryRoot "missing-build-root"
    $missingBuildPath = Join-Path $missingBuildRoot "cold-build"
    $resolvedMissingBuildPath = Resolve-BuildTruthContainedPath `
        -Root $missingBuildRoot `
        -Path $missingBuildPath `
        -AllowMissing
    Assert-Equal `
        -Actual $resolvedMissingBuildPath `
        -Expected ([IO.Path]::GetFullPath($missingBuildPath)) `
        -Message "missing build root lexical containment"
    Assert-BuildTruthFailure -Category "MissingArtifact" -Action {
        Resolve-BuildTruthContainedPath `
            -Root $missingBuildRoot `
            -Path $missingBuildPath | Out-Null
    }

    $caseRoot = Join-Path $temporaryRoot "case-root"
    New-Item -ItemType Directory -Path $caseRoot | Out-Null
    $caseCandidate = Join-Path $caseRoot.ToUpperInvariant() "inside.txt"
    if ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT)
    {
        Resolve-BuildTruthContainedPath `
            -Root $caseRoot `
            -Path $caseCandidate `
            -AllowMissing | Out-Null
    }
    else
    {
        Assert-BuildTruthFailure -Category "PathEscape" -Action {
            Resolve-BuildTruthContainedPath `
                -Root $caseRoot `
                -Path $caseCandidate `
                -AllowMissing | Out-Null
        }
    }

    $linkRoot = Join-Path $temporaryRoot "link-root"
    $outsideRoot = Join-Path $temporaryRoot "outside-root"
    New-Item -ItemType Directory -Path $linkRoot, $outsideRoot | Out-Null
    $linkPath = Join-Path $linkRoot "linked"
    $linkCreated = $false
    try
    {
        New-Item -ItemType SymbolicLink -Path $linkPath -Target $outsideRoot -ErrorAction Stop | Out-Null
        $linkCreated = $true
    }
    catch
    {
        Write-Host "Symbolic-link ancestor self-test skipped: $($_.Exception.Message)"
    }
    if ($linkCreated)
    {
        Assert-BuildTruthFailure -Category "PathEscape" -Action {
            Resolve-BuildTruthContainedPath `
                -Root $linkRoot `
                -Path (Join-Path $linkPath "escaped.txt") `
                -AllowMissing | Out-Null
        }
    }

    $lockPath = Join-Path $temporaryRoot "locked-build"
    $firstLock = Enter-BuildTruthRunLock -BuildPath $lockPath
    if ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT)
    {
        $caseVariantLock = Enter-BuildTruthRunLock -BuildPath $lockPath.ToUpperInvariant()
        try
        {
            Assert-Equal -Actual $caseVariantLock.name -Expected $firstLock.name `
                -Message "Windows run lock path casing"
        }
        finally
        {
            Exit-BuildTruthRunLock -Lock $caseVariantLock
        }
    }
    $lockJob = $null
    try
    {
        $lockJob = Start-Job -ScriptBlock {
            param([string]$Name)
            $mutex = [Threading.Mutex]::OpenExisting($Name)
            try
            {
                if ($mutex.WaitOne(0))
                {
                    $mutex.ReleaseMutex()
                    return "acquired"
                }
                return "blocked"
            }
            finally
            {
                $mutex.Dispose()
            }
        } -ArgumentList $firstLock.name
        Wait-Job -Job $lockJob -Timeout 15 | Out-Null
        $lockResult = Receive-Job -Job $lockJob -ErrorAction Stop
        Assert-Equal -Actual $lockResult -Expected "blocked" -Message "cross-process run lock contention"
    }
    finally
    {
        if ($null -ne $lockJob)
        {
            Remove-Job -Job $lockJob -Force -ErrorAction SilentlyContinue
        }
        Exit-BuildTruthRunLock -Lock $firstLock
    }
    $secondLock = Enter-BuildTruthRunLock -BuildPath $lockPath
    Exit-BuildTruthRunLock -Lock $secondLock

    $receiptRunId = "0123456789abcdef0123456789abcdef"
    $receiptValue = [ordered]@{
        schema = "RVX.AgentHarness.BuildTruthReceipt"
        schemaVersion = 2
        runId = $receiptRunId
        immutableReceiptPath = ".agent-harness/receipts/BuildTruth.$receiptRunId.json"
        generatedAtUtc = (Get-Date).ToUniversalTime().ToString("o")
        request = [ordered]@{ softwareAdapterAllowed = $false }
        source = [ordered]@{}
        report = [ordered]@{}
        junit = [ordered]@{ lanes = @() }
        lanes = @()
        nativeEvidence = $null
    }
    Assert-Equal `
        -Actual $receiptValue.request.softwareAdapterAllowed `
        -Expected $false `
        -Message "Build Truth receipt software adapter request"
    $buildTruthRunnerSource = Get-Content `
        -LiteralPath (Join-Path $PSScriptRoot "..\run_build_truth.ps1") `
        -Raw
    if ($buildTruthRunnerSource -notmatch 'softwareAdapterAllowed\s*=\s*\$SoftwareAdapterAllowed')
    {
        throw "Build Truth receipt publisher does not bind softwareAdapterAllowed."
    }
    if ($buildTruthRunnerSource -notmatch 'ValidationProcessEnvironment\.psm1' -or
        $buildTruthRunnerSource -match 'SetEnvironmentVariable\("PATH"')
    {
        throw "Build Truth does not use the clean validation process environment launcher."
    }
    if ($buildTruthRunnerSource -notmatch '\$LaneTestPresets\s+-join\s+","')
    {
        throw "Build Truth no longer serializes LaneTestPresets for PowerShell -File binding."
    }
    if ($buildTruthRunnerSource -notmatch 'schemaVersion\s*=\s*3')
    {
        throw "Build Truth publisher does not emit receipt schema version 3."
    }
    if ($buildTruthRunnerSource -notmatch '\[switch\]\$AllowSoftwareAdapter' -or
        $buildTruthRunnerSource -notmatch 'RVX_NATIVE_ALLOW_SOFTWARE_ADAPTER\s*=\s*"1"' -or
        $buildTruthRunnerSource -notmatch 'Remove-Item Env:RVX_NATIVE_ALLOW_SOFTWARE_ADAPTER')
    {
        throw "Build Truth native software-adapter policy is not invocation-bound."
    }
    $receiptValue.schemaVersion = 1
    Assert-BuildTruthFailure -Category "ReceiptSchema" -Action {
        Write-BuildTruthImmutableReceipt `
            -RepositoryRoot $receiptRepository `
            -ReceiptKind "BuildTruth" `
            -RunId $receiptRunId `
            -Value $receiptValue `
            -Status "passed" | Out-Null
    }
    $v4ReceiptRunId = "22222222222222222222222222222222"
    $receiptValue.schemaVersion = 4
    $receiptValue.runId = $v4ReceiptRunId
    $receiptValue.immutableReceiptPath =
        ".agent-harness/receipts/BuildTruth.$v4ReceiptRunId.json"
    Assert-BuildTruthFailure -Category "ReceiptSchema" -Action {
        Write-BuildTruthImmutableReceipt `
            -RepositoryRoot $receiptRepository `
            -ReceiptKind "BuildTruth" `
            -RunId $v4ReceiptRunId `
            -Value $receiptValue `
            -Status "passed" | Out-Null
    }
    $receiptValue.schemaVersion = 2
    $receiptValue.runId = $receiptRunId
    $receiptValue.immutableReceiptPath =
        ".agent-harness/receipts/BuildTruth.$receiptRunId.json"
    $receipt = Write-BuildTruthImmutableReceipt `
        -RepositoryRoot $receiptRepository `
        -ReceiptKind "BuildTruth" `
        -RunId $receiptRunId `
        -Value $receiptValue `
        -Status "passed"
    Assert-Equal `
        -Actual $receipt.immutableReceiptPath `
        -Expected ".agent-harness/receipts/BuildTruth.$receiptRunId.json" `
        -Message "immutable receipt path"
    if ($receipt.sha256 -notmatch "^[0-9a-f]{64}$")
    {
        throw "Immutable receipt SHA-256 is incomplete."
    }
    $pointer = Write-BuildTruthReceiptPointer `
        -RepositoryRoot $receiptRepository `
        -ReceiptKind "BuildTruth" `
        -PointerFileName "BuildTruth.latest.json" `
        -RunId $receipt.runId `
        -ImmutableReceiptPath $receipt.immutableReceiptPath `
        -ImmutableReceiptSha256 $receipt.sha256
    if ($pointer.path -ne ".agent-harness/receipts/BuildTruth.latest.json")
    {
        throw "Build Truth receipt pointer path is incomplete."
    }

    $secondReceiptRunId = "11111111111111111111111111111111"
    $receiptValue.runId = $secondReceiptRunId
    $receiptValue.immutableReceiptPath = ".agent-harness/receipts/BuildTruth.$secondReceiptRunId.json"
    $receiptValue.schemaVersion = 3
    $secondReceipt = Write-BuildTruthImmutableReceipt `
        -RepositoryRoot $receiptRepository `
        -ReceiptKind "BuildTruth" `
        -RunId $secondReceiptRunId `
        -Value $receiptValue `
        -Status "passed"
    Write-BuildTruthReceiptPointer `
        -RepositoryRoot $receiptRepository `
        -ReceiptKind "BuildTruth" `
        -PointerFileName "BuildTruth.latest.json" `
        -RunId $secondReceipt.runId `
        -ImmutableReceiptPath $secondReceipt.immutableReceiptPath `
        -ImmutableReceiptSha256 $secondReceipt.sha256 | Out-Null
    $pointerContent = Get-Content -LiteralPath (Join-Path $receiptRepository $pointer.path) -Raw | ConvertFrom-Json
    Assert-Equal -Actual $pointerContent.runId -Expected $secondReceiptRunId -Message "atomic receipt pointer replacement"
    Assert-BuildTruthFailure -Category "ReceiptTampered" -Action {
        Write-BuildTruthReceiptPointer `
            -RepositoryRoot $receiptRepository `
            -ReceiptKind "BuildTruth" `
            -PointerFileName "BuildTruth.latest.json" `
            -RunId $secondReceipt.runId `
            -ImmutableReceiptPath $secondReceipt.immutableReceiptPath `
            -ImmutableReceiptSha256 ("0" * 64) | Out-Null
    }
    $pointerAfterFailure = Get-Content -LiteralPath (Join-Path $receiptRepository $pointer.path) -Raw | ConvertFrom-Json
    Assert-Equal -Actual $pointerAfterFailure.runId -Expected $secondReceiptRunId -Message "failed pointer publication does not advance"
    Assert-BuildTruthFailure -Category "ReceiptCollision" -Action {
        Write-BuildTruthImmutableReceipt `
            -RepositoryRoot $receiptRepository `
            -ReceiptKind "BuildTruth" `
            -RunId $secondReceiptRunId `
            -Value $receiptValue `
            -Status "passed" | Out-Null
    }

    $runnerDigestRepository = Join-Path $temporaryRoot "runner-digest-repository"
    foreach ($relativePath in @(
        "Scripts/run_build_truth.ps1",
        "Scripts/run_required_suite.ps1",
        "Scripts/BuildTruth/BuildTruthV3.psm1",
        "Scripts/BuildTruth/ValidationProcessEnvironment.psm1"))
    {
        $path = Join-Path $runnerDigestRepository $relativePath
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
        Set-Content -LiteralPath $path -Value $relativePath -Encoding utf8
    }
    foreach ($receiptKind in @("BuildTruth", "RequiredSuite"))
    {
        $v2Paths = @(Get-BuildTruthReceiptRunnerPaths `
            -ReceiptKind $receiptKind -SchemaVersion 2)
        $v3Paths = @(Get-BuildTruthReceiptRunnerPaths `
            -ReceiptKind $receiptKind -SchemaVersion 3)
        Assert-Equal -Actual $v2Paths.Count -Expected 2 `
            -Message "$receiptKind v2 runner path count"
        Assert-Equal -Actual $v3Paths.Count -Expected 3 `
            -Message "$receiptKind v3 runner path count"
        if ($v2Paths -contains "Scripts/BuildTruth/ValidationProcessEnvironment.psm1" -or
            $v3Paths -notcontains "Scripts/BuildTruth/ValidationProcessEnvironment.psm1")
        {
            throw "$receiptKind runner paths do not distinguish receipt schema versions."
        }

        $v2Digest = Get-BuildTruthRunnerDigest `
            -RepositoryRoot $runnerDigestRepository -Paths $v2Paths
        Assert-BuildTruthRunnerDigest -RunnerDigest $v2Digest -ExpectedPaths $v2Paths
        $v3Digest = Get-BuildTruthRunnerDigest `
            -RepositoryRoot $runnerDigestRepository -Paths $v3Paths
        Assert-BuildTruthRunnerDigest -RunnerDigest $v3Digest -ExpectedPaths $v3Paths
        Assert-BuildTruthFailure -Category "ReceiptSchema" -Action {
            Assert-BuildTruthRunnerDigest -RunnerDigest $v3Digest -ExpectedPaths $v2Paths
        }
        Assert-BuildTruthFailure -Category "ReceiptSchema" -Action {
            Assert-BuildTruthRunnerDigest -RunnerDigest $v2Digest -ExpectedPaths $v3Paths
        }
        $tamperedDigest = [ordered]@{
            algorithm = $v2Digest.algorithm
            digest = "0" * 64
            files = @($v2Digest.files)
        }
        Assert-BuildTruthFailure -Category "ReceiptTampered" -Action {
            Assert-BuildTruthRunnerDigest -RunnerDigest $tamperedDigest -ExpectedPaths $v2Paths
        }
    }
    Assert-BuildTruthFailure -Category "ReceiptSchema" -Action {
        Get-BuildTruthReceiptRunnerPaths -ReceiptKind BuildTruth -SchemaVersion 4 | Out-Null
    }

    $failedRunId = "abcdefabcdefabcdefabcdefabcdefab"
    $failedReceiptValue = [ordered]@{
        schema = "RVX.AgentHarness.BuildTruthReceipt"
        schemaVersion = 3
        runId = $failedRunId
        immutableReceiptPath = ".agent-harness/receipts/BuildTruth.$failedRunId.json"
    }
    Assert-BuildTruthFailure -Category "ReceiptStatus" -Action {
        Write-BuildTruthImmutableReceipt `
            -RepositoryRoot $receiptRepository `
            -ReceiptKind "BuildTruth" `
            -RunId $failedRunId `
            -Value $failedReceiptValue `
            -Status "failed" | Out-Null
    }
    if (Test-Path -LiteralPath (Join-Path $receiptRepository $failedReceiptValue.immutableReceiptPath))
    {
        throw "A failed run wrote an immutable receipt."
    }

    Write-Host "Build Truth v2/v3 receipt compatibility self-test passed."
}
finally
{
    if (Test-Path -LiteralPath $temporaryRoot)
    {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
}
