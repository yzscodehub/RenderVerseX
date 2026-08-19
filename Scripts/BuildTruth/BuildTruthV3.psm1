Set-StrictMode -Version Latest

function New-BuildTruthFailure
{
    param(
        [Parameter(Mandatory)]
        [string]$Category,
        [Parameter(Mandatory)]
        [string]$Message
    )

    return [System.InvalidOperationException]::new("BT3.${Category}: $Message")
}

function Throw-BuildTruthFailure
{
    param(
        [Parameter(Mandatory)]
        [string]$Category,
        [Parameter(Mandatory)]
        [string]$Message
    )

    throw (New-BuildTruthFailure -Category $Category -Message $Message)
}

function Write-BuildTruthJson
{
    param(
        [Parameter(Mandatory)]
        [object]$Value,
        [Parameter(Mandatory)]
        [string]$Path,
        [int]$Depth = 12
    )

    $directory = Split-Path -Parent $Path
    if (-not [string]::IsNullOrWhiteSpace($directory))
    {
        New-Item -ItemType Directory -Force -Path $directory | Out-Null
    }
    $Value | ConvertTo-Json -Depth $Depth | Set-Content -LiteralPath $Path -Encoding utf8
}

function Get-BuildTruthPathFingerprint
{
    param(
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][string[]]$RelativePaths
    )

    $records = [System.Collections.Generic.List[string]]::new()
    foreach ($relativePath in ($RelativePaths | Sort-Object -Unique))
    {
        if ([string]::IsNullOrWhiteSpace($relativePath) -or
            [IO.Path]::IsPathRooted($relativePath) -or
            $relativePath -match "(^|[\\/])\.\.([\\/]|$)")
        {
            Throw-BuildTruthFailure -Category "SourceFingerprint" -Message (
                "Git returned an unsafe worktree path '$relativePath'")
        }
        $absolutePath = Join-Path $Root $relativePath
        if (Test-Path -LiteralPath $absolutePath -PathType Container)
        {
            Throw-BuildTruthFailure -Category "SourceFingerprint" -Message (
                "Git worktree path '$relativePath' is a directory, not a file")
        }

        $state = "missing"
        $contentHash = ""
        if (Test-Path -LiteralPath $absolutePath -PathType Leaf)
        {
            $state = "file"
            $contentHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $absolutePath).Hash.ToLowerInvariant()
        }
        $records.Add("$relativePath`0$state`0$contentHash")
    }

    $serialized = ($records -join "`n")
    $bytes = [Text.Encoding]::UTF8.GetBytes($serialized)
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try
    {
        $digest = ([BitConverter]::ToString($sha256.ComputeHash($bytes))).Replace("-", "").ToLowerInvariant()
    }
    finally
    {
        $sha256.Dispose()
    }
    return [pscustomobject]@{
        algorithm = "sha256:path-state-content-v1"
        fileCount = $records.Count
        digest = $digest
    }
}

function Get-BuildTruthWorkingSourceFingerprint
{
    param(
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][string]$GitSafeDirectory
    )

    # Disable Git's C-style quoting so non-ASCII repository paths are passed
    # to the filesystem as their actual Unicode names rather than quoted
    # escape sequences that are not valid Windows paths.
    $paths = @(& git -c "safe.directory=$GitSafeDirectory" -c core.quotePath=false -C $Root ls-files --cached --others --exclude-standard)
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0)
    {
        Throw-BuildTruthFailure -Category "SourceFingerprint" -Message (
            "Unable to enumerate tracked and non-ignored untracked files (exit code $exitCode)")
    }
    return Get-BuildTruthPathFingerprint -Root $Root -RelativePaths $paths
}

function Get-BuildTruthCTestInventory
{
    param(
        [Parameter(Mandatory)]
        [string]$BuildPath,
        [Parameter(Mandatory)]
        [string]$Configuration,
        [string]$TestPreset = ""
    )

    $arguments = @()
    $inventoryContext = $BuildPath
    if ([string]::IsNullOrWhiteSpace($TestPreset))
    {
        $arguments += "--test-dir", $BuildPath, "-C", $Configuration
    }
    else
    {
        $arguments += "--preset", $TestPreset
        $inventoryContext = $TestPreset
    }
    $arguments += "--show-only=json-v1"

    $output = & ctest @arguments 2>&1
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0)
    {
        Throw-BuildTruthFailure -Category "InventoryCommand" -Message (
            "CTest inventory query failed for '$inventoryContext' with exit code $exitCode")
    }

    try
    {
        $inventory = (($output -join [Environment]::NewLine) | ConvertFrom-Json)
    }
    catch
    {
        Throw-BuildTruthFailure -Category "InventoryJson" -Message (
            "CTest did not emit valid json-v1 inventory for '$inventoryContext': " +
            $_.Exception.Message)
    }

    if ($null -eq $inventory.tests)
    {
        Throw-BuildTruthFailure -Category "InventorySchema" -Message (
            "CTest json-v1 inventory for '$inventoryContext' has no tests array")
    }
    return $inventory
}

function Get-BuildTruthTestLanes
{
    param([Parameter(Mandatory)][object]$Test)

    $labelProperty = @($Test.properties | Where-Object { $_.name -eq "LABELS" } | Select-Object -First 1)
    if ($labelProperty.Count -eq 0)
    {
        return @()
    }

    return @($labelProperty[0].value | Where-Object { $_ -match "^lane\." })
}

function Assert-BuildTruthInventoryLanes
{
    param(
        [Parameter(Mandatory)][object]$Inventory,
        [string]$Context = "full inventory"
    )

    $tests = @($Inventory.tests)
    if ($tests.Count -eq 0)
    {
        Throw-BuildTruthFailure -Category "ZeroTests" -Message "$Context contains zero tests"
    }

    $invalidTests = [System.Collections.Generic.List[string]]::new()
    foreach ($test in $tests)
    {
        $lanes = @(Get-BuildTruthTestLanes -Test $test)
        if ($lanes.Count -ne 1)
        {
            $invalidTests.Add("$($test.name) [$($lanes -join ',')]")
        }
    }
    if ($invalidTests.Count -ne 0)
    {
        Throw-BuildTruthFailure -Category "LaneClassification" -Message (
            "$Context contains tests without exactly one lane: " +
            (($invalidTests | Select-Object -First 10) -join "; "))
    }
}

function Get-BuildTruthJUnitOutcome
{
    param(
        [Parameter(Mandatory)]
        [string]$Path
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf))
    {
        Throw-BuildTruthFailure -Category "MissingArtifact" -Message "JUnit artifact is missing: $Path"
    }
    try
    {
        [xml]$junit = Get-Content -LiteralPath $Path -Raw
    }
    catch
    {
        Throw-BuildTruthFailure -Category "JUnitInvalid" -Message (
            "JUnit artifact is invalid XML: $Path ($($_.Exception.Message))")
    }

    $testCases = @($junit.SelectNodes("//*[local-name()='testcase']"))
    $failures = @($junit.SelectNodes("//*[local-name()='testcase']/*[local-name()='failure']"))
    $errors = @($junit.SelectNodes("//*[local-name()='testcase']/*[local-name()='error']"))
    $skipped = @($junit.SelectNodes("//*[local-name()='testcase']/*[local-name()='skipped']"))
    $notRun = @(
        $testCases | Where-Object {
            $_.GetAttribute("status") -match "^(notrun|not-run|disabled|skipped)$"
        }
    )
    $disabledSuites = @(
        $junit.SelectNodes("//*[@disabled]") | Where-Object {
            $value = $_.GetAttribute("disabled")
            $value -match "^[1-9][0-9]*$"
        }
    )

    return [ordered]@{
        testCaseCount = $testCases.Count
        failureCount = $failures.Count
        errorCount = $errors.Count
        skippedCount = $skipped.Count
        notRunCount = $notRun.Count
        disabledSuiteCount = $disabledSuites.Count
    }
}

function Assert-BuildTruthJUnitOutcome
{
    param(
        [Parameter(Mandatory)][object]$Outcome,
        [Parameter(Mandatory)][int]$ExpectedTestCount,
        [Parameter(Mandatory)][string]$Context
    )

    if ($Outcome.testCaseCount -eq 0)
    {
        Throw-BuildTruthFailure -Category "ZeroTests" -Message "$Context JUnit contains zero test cases"
    }
    if ($Outcome.testCaseCount -ne $ExpectedTestCount)
    {
        Throw-BuildTruthFailure -Category "NotRun" -Message (
            "$Context JUnit contains $($Outcome.testCaseCount) test cases, expected $ExpectedTestCount")
    }
    if ($Outcome.disabledSuiteCount -ne 0)
    {
        Throw-BuildTruthFailure -Category "Disabled" -Message (
            "$Context JUnit reports $($Outcome.disabledSuiteCount) disabled suite(s)")
    }
    if ($Outcome.skippedCount -ne 0)
    {
        Throw-BuildTruthFailure -Category "Skipped" -Message (
            "$Context JUnit reports $($Outcome.skippedCount) skipped test(s)")
    }
    if ($Outcome.notRunCount -ne 0)
    {
        Throw-BuildTruthFailure -Category "NotRun" -Message (
            "$Context JUnit reports $($Outcome.notRunCount) not-run test(s)")
    }
    if ($Outcome.failureCount -ne 0 -or $Outcome.errorCount -ne 0)
    {
        Throw-BuildTruthFailure -Category "TestFailure" -Message (
            "$Context JUnit reports $($Outcome.failureCount) failure(s) and $($Outcome.errorCount) error(s)")
    }
}

function Invoke-BuildTruthCTestLane
{
    param(
        [Parameter(Mandatory)][string]$TestPreset,
        [Parameter(Mandatory)][string]$JUnitPath,
        [Parameter(Mandatory)][int]$ExpectedTestCount,
        [Parameter(Mandatory)][string]$Lane
    )

    if (Test-Path -LiteralPath $JUnitPath)
    {
        Remove-Item -LiteralPath $JUnitPath -Force
    }

    $arguments = @(
        "--preset", $TestPreset,
        "--output-on-failure",
        "--output-junit", $JUnitPath
    )
    $started = Get-Date
    $ctestOutput = & ctest @arguments 2>&1
    $exitCode = $LASTEXITCODE
    $durationMs = [int]((Get-Date) - $started).TotalMilliseconds
    $ctestOutput | ForEach-Object { Write-Host $_ }

    $outcome = Get-BuildTruthJUnitOutcome -Path $JUnitPath
    Assert-BuildTruthJUnitOutcome -Outcome $outcome -ExpectedTestCount $ExpectedTestCount -Context "lane '$Lane'"
    if ($exitCode -ne 0)
    {
        Throw-BuildTruthFailure -Category "TestCommand" -Message (
            "CTest lane '$Lane' failed with exit code $exitCode")
    }

    return [pscustomobject]@{
        exitCode = $exitCode
        durationMs = $durationMs
        junitPath = $JUnitPath
        outcome = $outcome
    }
}

function Get-BuildTruthNativeEvidence
{
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$SourceCommit,
        [Parameter(Mandatory)][string]$TestPreset,
        [Parameter(Mandatory)][string]$RequestedBackend,
        [Parameter(Mandatory)][bool]$SoftwareAdapterAllowed
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf))
    {
        Throw-BuildTruthFailure -Category "MissingArtifact" -Message "Native lifecycle evidence is missing: $Path"
    }
    try
    {
        $native = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    }
    catch
    {
        Throw-BuildTruthFailure -Category "NativeEvidenceInvalid" -Message (
            "Native lifecycle evidence is invalid JSON: $($_.Exception.Message)")
    }

    $requiredProperties = @(
        "schema", "schemaVersion", "status", "requestedBackend", "actualBackend",
        "backendFallback", "softwareAdapter", "softwareAdapterAllowed", "adapterName",
        "mainThreadIdentityHash", "renderThreadIdentityHash", "surfaceGeneration",
        "lastPresentedFrameSequence", "resizeAcceptedCount", "shutdownCode"
    )
    foreach ($propertyName in $requiredProperties)
    {
        if ($null -eq $native.PSObject.Properties[$propertyName])
        {
            Throw-BuildTruthFailure -Category "NativeEvidenceIncomplete" -Message (
                "Native lifecycle evidence is missing '$propertyName'")
        }
    }
    if ($native.schema -ne "RVX.M1.NativeRenderLifecycle" -or
        $native.schemaVersion -ne 2 -or $native.status -ne "passed")
    {
        Throw-BuildTruthFailure -Category "NativeEvidenceInvalid" -Message (
            "Native lifecycle evidence has an unsupported schema or verdict")
    }
    if ($native.requestedBackend -ne $RequestedBackend)
    {
        Throw-BuildTruthFailure -Category "NativeBackendMismatch" -Message (
            "Requested backend '$RequestedBackend' does not match evidence backend '$($native.requestedBackend)'")
    }
    if ([string]::IsNullOrWhiteSpace([string]$native.adapterName) -or
        $native.mainThreadIdentityHash -eq 0 -or
        $native.renderThreadIdentityHash -eq 0 -or
        $native.mainThreadIdentityHash -eq $native.renderThreadIdentityHash -or
        $native.surfaceGeneration -lt 2 -or
        $native.lastPresentedFrameSequence -lt 2 -or
        $native.resizeAcceptedCount -lt 1 -or
        $native.shutdownCode -ne 1)
    {
        Throw-BuildTruthFailure -Category "NativeEvidenceIncomplete" -Message (
            "Native lifecycle evidence does not contain complete lifecycle observations")
    }

    $actualBackend = [string]$native.actualBackend
    $backendFallback = [bool]$native.backendFallback
    $softwareAdapter = [bool]$native.softwareAdapter
    $softwareAdapterAllowedInEvidence = [bool]$native.softwareAdapterAllowed
    if ($backendFallback)
    {
        Throw-BuildTruthFailure -Category "Fallback" -Message (
            "Native lifecycle evidence records a backend fallback to '$actualBackend'")
    }
    if (-not [string]::Equals(
        $actualBackend, $RequestedBackend, [StringComparison]::OrdinalIgnoreCase))
    {
        Throw-BuildTruthFailure -Category "NativeBackendMismatch" -Message (
            "Native lifecycle actual backend '$actualBackend' does not match '$RequestedBackend'")
    }
    if ($softwareAdapter -and -not $SoftwareAdapterAllowed)
    {
        Throw-BuildTruthFailure -Category "Fallback" -Message (
            "Native lifecycle selected a software adapter without an explicit opt-in")
    }
    if ($softwareAdapterAllowedInEvidence -ne $SoftwareAdapterAllowed)
    {
        Throw-BuildTruthFailure -Category "NativeEvidenceMismatch" -Message (
            "Native lifecycle software-adapter opt-in does not match the Build Truth invocation")
    }

    return [ordered]@{
        schema = "RVX.BuildTruth.NativeEvidence"
        schemaVersion = 3
        sourceCommit = $SourceCommit
        preset = $TestPreset
        requestedBackend = $RequestedBackend
        actualBackend = $actualBackend
        fallback = [ordered]@{
            backendFallback = $backendFallback
            softwareAdapter = $softwareAdapter
            softwareAdapterAllowed = $softwareAdapterAllowedInEvidence
        }
        lifecycle = $native
    }
}

Export-ModuleMember -Function @(
    "Assert-BuildTruthInventoryLanes",
    "Assert-BuildTruthJUnitOutcome",
    "Get-BuildTruthCTestInventory",
    "Get-BuildTruthJUnitOutcome",
    "Get-BuildTruthNativeEvidence",
    "Get-BuildTruthPathFingerprint",
    "Get-BuildTruthTestLanes",
    "Get-BuildTruthWorkingSourceFingerprint",
    "Invoke-BuildTruthCTestLane",
    "New-BuildTruthFailure",
    "Throw-BuildTruthFailure",
    "Write-BuildTruthJson"
)
