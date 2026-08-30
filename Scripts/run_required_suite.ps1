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

# Some Windows launchers expose a pwsh shim that actually starts Windows
# PowerShell. Re-enter through a real PowerShell 7 binary before computing
# fingerprints or publishing immutable validation receipts.
if ($PSVersionTable.PSVersion.Major -lt 7)
{
    $forward = @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $PSCommandPath,
        "-ConfigurePreset", $ConfigurePreset,
        "-BuildPreset", $BuildPreset,
        "-TestPreset", $TestPreset,
        "-BuildDir", $BuildDir,
        "-Configuration", $Configuration,
        "-BuildTarget", ($BuildTarget -join ","),
        "-RequiredLabel", $RequiredLabel,
        "-ExpectedTestCount", [string]$ExpectedTestCount,
        "-BaseRef", $BaseRef
    )
    if ($RequiredEvidence.Count -gt 0)
    {
        $forward += @("-RequiredEvidence", ($RequiredEvidence -join ";"))
    }
    if ($Fresh) { $forward += "-Fresh" }
    if ($SkipConfigure) { $forward += "-SkipConfigure" }

    $candidates = @(Get-Command pwsh -All -ErrorAction SilentlyContinue |
        ForEach-Object Source | Select-Object -Unique)
    foreach ($candidate in $candidates)
    {
        $major = & $candidate -NoProfile -Command (
            "[Console]::Write(`$PSVersionTable.PSVersion.Major)") 2>$null
        if ($LASTEXITCODE -eq 0 -and [string]$major -match "^\s*(\d+)\s*$" -and
            [int]$Matches[1] -ge 7)
        {
            Write-Warning "The first pwsh is PowerShell $($PSVersionTable.PSVersion); re-launching with '$candidate'."
            & $candidate @forward
            exit $LASTEXITCODE
        }
    }
    throw "PowerShell 7 or newer is required, but no usable pwsh 7 executable was found"
}

$BuildTarget = @($BuildTarget | ForEach-Object { $_ -split "," } |
    ForEach-Object { $_.Trim() } | Where-Object { $_ })

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

Import-Module (Join-Path $PSScriptRoot "BuildTruth\ValidationProcessEnvironment.psm1") -Force

# MSBuild rejects a Windows inherited environment block containing both PATH
# and Path. Re-enter exactly once with a clean child environment rather than
# mutating this host process's environment in place.
$canonicalizationArguments = @(
    "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $PSCommandPath,
    "-ConfigurePreset", $ConfigurePreset,
    "-BuildPreset", $BuildPreset,
    "-TestPreset", $TestPreset,
    "-BuildDir", $BuildDir,
    "-Configuration", $Configuration,
    "-BuildTarget", ($BuildTarget -join ","),
    "-RequiredLabel", $RequiredLabel,
    "-ExpectedTestCount", [string]$ExpectedTestCount,
    "-BaseRef", $BaseRef
)
if ($RequiredEvidence.Count -gt 0)
{
    $canonicalizationArguments += @("-RequiredEvidence", ($RequiredEvidence -join ";"))
}
if ($Fresh) { $canonicalizationArguments += "-Fresh" }
if ($SkipConfigure) { $canonicalizationArguments += "-SkipConfigure" }
$environmentCanonicalization = Invoke-ValidationProcessEnvironmentCanonicalization `
    -WorkingDirectory (Get-Location).Path `
    -InvocationArguments $canonicalizationArguments
if ($environmentCanonicalization.relaunched)
{
    exit $environmentCanonicalization.exitCode
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

function Get-RequiredSuiteRepositoryRelativeDirectory
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
    return Get-BuildTruthRepositoryRelativePath `
        -RepositoryRoot $RepositoryRoot `
        -Path $absolutePath
}

function Publish-RequiredSuiteImmutableReceipt
{
    param(
        [Parameter(Mandatory)][string]$RepositoryRoot,
        [Parameter(Mandatory)][string]$RunId,
        [Parameter(Mandatory)][string]$PointerFileName,
        [Parameter(Mandatory)][string]$ReportPath,
        [Parameter(Mandatory)][string]$BuildPath,
        [Parameter(Mandatory)][string]$BaseRef,
        [Parameter(Mandatory)][string]$ConfigurePreset,
        [Parameter(Mandatory)][string]$BuildPreset,
        [Parameter(Mandatory)][string]$TestPreset,
        [Parameter(Mandatory)][string]$Configuration,
        [Parameter(Mandatory)][string[]]$BuildTargets,
        [Parameter(Mandatory)][string]$RequiredLabel,
        [Parameter(Mandatory)][int]$ExpectedTestCount,
        [Parameter(Mandatory)][bool]$Fresh,
        [Parameter(Mandatory)][bool]$SkipConfigure,
        [Parameter(Mandatory)][string]$SourceCommit,
        [Parameter(Mandatory)][object]$SourceFingerprintAtStart,
        [Parameter(Mandatory)][object]$SourceFingerprintAtEnd,
        [Parameter(Mandatory)][object]$Inventory,
        [Parameter(Mandatory)][string]$JUnitPath,
        [Parameter(Mandatory)][object]$JUnitOutcome,
        [Parameter(Mandatory)][object]$JUnitEvidence,
        [Parameter(Mandatory)][AllowEmptyCollection()][object[]]$SemanticEvidence
    )

    $selectedTests = @($Inventory.tests | ForEach-Object { $_.name })
    if ($selectedTests.Count -eq 0)
    {
        Throw-BuildTruthFailure -Category "ZeroTests" -Message (
            "A passed required suite has no selected tests")
    }
    Assert-BuildTruthReceiptFileEvidence `
        -RepositoryRoot $RepositoryRoot `
        -Evidence $JUnitEvidence `
        -Context "Required Suite JUnit"
    $currentJUnitOutcome = Get-BuildTruthJUnitOutcome -Path $JUnitPath
    Assert-BuildTruthJUnitOutcome `
        -Outcome $currentJUnitOutcome `
        -ExpectedTestCount $selectedTests.Count `
        -Context "required suite receipt"
    Assert-BuildTruthJUnitOutcomeMatches `
        -Expected $JUnitOutcome `
        -Actual $currentJUnitOutcome `
        -Context "Required Suite JUnit"

    $semanticArtifacts = [System.Collections.Generic.List[object]]::new()
    foreach ($semanticArtifact in $SemanticEvidence)
    {
        $currentEvidence = Get-BuildTruthReceiptFileEvidence `
            -RepositoryRoot $RepositoryRoot `
            -Path $semanticArtifact.path
        if ($currentEvidence.sha256 -ne $semanticArtifact.sha256)
        {
            Throw-BuildTruthFailure -Category "ReceiptTampered" -Message (
                "Semantic artifact changed after validation: $($semanticArtifact.path)")
        }
        $semanticArtifacts.Add($currentEvidence)
    }

    $relativeBuildDirectory = Get-RequiredSuiteRepositoryRelativeDirectory `
        -RepositoryRoot $RepositoryRoot `
        -Path $BuildPath
    $runnerDigest = Get-BuildTruthRunnerDigest `
        -RepositoryRoot $RepositoryRoot `
        -Paths @(
            "Scripts/run_required_suite.ps1",
            "Scripts/BuildTruth/ValidationProcessEnvironment.psm1",
            "Scripts/BuildTruth/BuildTruthV3.psm1"
        )
    $receiptPath = ".agent-harness/receipts/RequiredSuite.$RunId.json"
    $receipt = [ordered]@{
        schema = "RVX.AgentHarness.RequiredSuiteReceipt"
        schemaVersion = 3
        runId = $RunId
        immutableReceiptPath = $receiptPath
        generatedAtUtc = (Get-Date).ToUniversalTime().ToString("o")
        request = [ordered]@{
            baseRef = $BaseRef
            configurePreset = $ConfigurePreset
            buildPreset = $BuildPreset
            testPreset = $TestPreset
            buildDir = $relativeBuildDirectory
            configuration = $Configuration
            buildTargets = @($BuildTargets)
            requiredLabel = $RequiredLabel
            expectedTestCount = if ($ExpectedTestCount -gt 0) { $ExpectedTestCount } else { $null }
            fresh = $Fresh
            skipConfigure = $SkipConfigure
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
            path = $JUnitEvidence.path
            sha256 = $JUnitEvidence.sha256
            outcome = $currentJUnitOutcome
        }
        requiredLabel = $RequiredLabel
        selectedTests = @($selectedTests)
        semanticArtifacts = @($semanticArtifacts)
    }
    $immutableReceipt = Write-BuildTruthImmutableReceipt `
        -RepositoryRoot $RepositoryRoot `
        -ReceiptKind "RequiredSuite" `
        -RunId $RunId `
        -Value $receipt `
        -Status "passed"
    $latestPointer = Write-BuildTruthReceiptPointer `
        -RepositoryRoot $RepositoryRoot `
        -ReceiptKind "RequiredSuite" `
        -PointerFileName $PointerFileName `
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
$gitSafeDirectory = $repoRoot.Replace(
    [IO.Path]::DirectorySeparatorChar,
    [IO.Path]::AltDirectorySeparatorChar)
$buildRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot "build"))
$buildPath = [IO.Path]::GetFullPath((Join-Path $repoRoot $BuildDir))
$null = Resolve-BuildTruthContainedPath `
    -Root $buildRoot `
    -Path $buildPath `
    -AllowMissing

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
$junitExecutionEvidence = $null
$semanticEvidence = @()
$startedAt = Get-Date
$receiptInfo = $null
$runLock = Enter-BuildTruthRunLock -BuildPath $buildPath
$locationPushed = $false
try
{
    Push-Location $repoRoot
    $locationPushed = $true
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
        -Name "validation-process-environment-self-test" `
        -FailureCategory "SelfTest" `
        -Action {
            & (Join-Path $PSScriptRoot "BuildTruth\Test-ValidationProcessEnvironment.ps1")
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
        $junitExecutionEvidence = Get-BuildTruthReceiptFileEvidence `
            -RepositoryRoot $repoRoot `
            -Path $junitPath
        $capturedJUnitOutcome = Get-BuildTruthJUnitOutcome -Path $junitPath
        Assert-BuildTruthJUnitOutcome `
            -Outcome $capturedJUnitOutcome `
            -ExpectedTestCount $selectedTests.Count `
            -Context "required suite captured JUnit"
        Assert-BuildTruthJUnitOutcomeMatches `
            -Expected $junitOutcome `
            -Actual $capturedJUnitOutcome `
            -Context "Required Suite captured JUnit"
        $junitOutcome = $capturedJUnitOutcome
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
    try
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

        if ($status -eq "passed")
        {
            try
            {
                $receiptInfo = Publish-RequiredSuiteImmutableReceipt `
                    -RepositoryRoot $repoRoot `
                    -RunId $runId `
                    -PointerFileName "RequiredSuite.$suiteToken.latest.json" `
                    -ReportPath $reportPath `
                    -BuildPath $buildPath `
                    -BaseRef $BaseRef `
                    -ConfigurePreset $ConfigurePreset `
                    -BuildPreset $BuildPreset `
                    -TestPreset $TestPreset `
                    -Configuration $Configuration `
                    -BuildTargets @($BuildTarget) `
                    -RequiredLabel $RequiredLabel `
                    -ExpectedTestCount $ExpectedTestCount `
                    -Fresh ([bool]$Fresh) `
                    -SkipConfigure ([bool]$SkipConfigure) `
                    -SourceCommit $sourceCommit `
                    -SourceFingerprintAtStart $sourceFingerprintAtStart `
                    -SourceFingerprintAtEnd $sourceFingerprintAtEnd `
                    -Inventory $inventory `
                    -JUnitPath $junitPath `
                    -JUnitOutcome $junitOutcome `
                    -JUnitEvidence $junitExecutionEvidence `
                    -SemanticEvidence @($semanticEvidence)
            }
            catch
            {
                $status = "failed"
                $failureCategory = "ReceiptPublication"
                $failure = "BT3.ReceiptPublication: $($_.Exception.Message)"
                $report.status = $status
                $report.failureCategory = $failureCategory
                $report.failure = [ordered]@{
                    category = $failureCategory
                    message = $failure
                }
                Write-BuildTruthJson -Value $report -Path $reportPath -Depth 16
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

Write-Host "Required suite passed. Report: $reportPath"
Write-Host "Immutable receipt: $($receiptInfo.immutableReceiptPath) (runId=$($receiptInfo.runId), sha256=$($receiptInfo.sha256))"
Write-Host "Latest pointer: $($receiptInfo.latestPointerPath) (sha256=$($receiptInfo.latestPointerSha256))"
