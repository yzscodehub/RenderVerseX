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

    $previousErrorActionPreference = $ErrorActionPreference
    try
    {
        # PowerShell can promote a native process' stderr to a terminating
        # NativeCommandError when the caller uses Stop. CTest owns the exit
        # status and JSON/JUnit artifacts, so preserve its complete output and
        # classify the result below instead of losing that evidence here.
        $ErrorActionPreference = "Continue"
        $output = & ctest @arguments 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally
    {
        $ErrorActionPreference = $previousErrorActionPreference
    }
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

function Get-BuildTruthTestLabels
{
    param([Parameter(Mandatory)][object]$Test)

    $labelProperty = @(
        $Test.properties |
            Where-Object { $_.name -eq "LABELS" } |
            Select-Object -First 1
    )
    if ($labelProperty.Count -eq 0)
    {
        return @()
    }
    return @($labelProperty[0].value)
}

function Assert-BuildTruthRequiredSelection
{
    param(
        [Parameter(Mandatory)][object]$Inventory,
        [Parameter(Mandatory)][string]$RequiredLabel,
        [int]$ExpectedTestCount = 0,
        [string]$Context = "required selection"
    )

    if ($RequiredLabel -notmatch "^(lane|suite)\.[A-Za-z0-9][A-Za-z0-9_.-]*$")
    {
        Throw-BuildTruthFailure -Category "RequiredLabel" -Message (
            "$Context has invalid required label '$RequiredLabel'")
    }

    Assert-BuildTruthInventoryLanes -Inventory $Inventory -Context $Context
    $tests = @($Inventory.tests)
    if ($ExpectedTestCount -lt 0)
    {
        Throw-BuildTruthFailure -Category "ExpectedCount" -Message (
            "$Context expected test count cannot be negative")
    }
    if ($ExpectedTestCount -gt 0 -and $tests.Count -ne $ExpectedTestCount)
    {
        Throw-BuildTruthFailure -Category "NotRun" -Message (
            "$Context selects $($tests.Count) tests, expected $ExpectedTestCount")
    }

    $missingLabel = @(
        $tests | Where-Object {
            $labels = @(Get-BuildTruthTestLabels -Test $_)
            $RequiredLabel -notin $labels
        }
    )
    if ($missingLabel.Count -ne 0)
    {
        $names = @($missingLabel | Select-Object -First 10 | ForEach-Object { $_.name })
        Throw-BuildTruthFailure -Category "RequiredLabel" -Message (
            "$Context contains tests without '$RequiredLabel': $($names -join ', ')")
    }
}

function Get-BuildTruthPathStringComparison
{
    if ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT)
    {
        return [StringComparison]::OrdinalIgnoreCase
    }
    return [StringComparison]::Ordinal
}

function Test-BuildTruthPathContained
{
    param(
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][string]$Candidate,
        [switch]$AllowRoot
    )

    $rootPath = [IO.Path]::GetFullPath($Root).TrimEnd(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar)
    $candidatePath = [IO.Path]::GetFullPath($Candidate)
    $comparison = Get-BuildTruthPathStringComparison
    if ($AllowRoot -and [string]::Equals($candidatePath, $rootPath, $comparison))
    {
        return $true
    }
    $prefix = $rootPath + [IO.Path]::DirectorySeparatorChar
    return $candidatePath.StartsWith($prefix, $comparison)
}

function Test-BuildTruthLinkOrReparsePoint
{
    param([Parameter(Mandatory)][object]$Item)

    if (($Item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
    {
        return $true
    }
    $linkType = $Item.PSObject.Properties["LinkType"]
    if ($null -ne $linkType -and -not [string]::IsNullOrWhiteSpace([string]$linkType.Value))
    {
        return $true
    }
    $target = $Item.PSObject.Properties["Target"]
    return $null -ne $target -and $null -ne $target.Value -and @($target.Value).Count -ne 0
}

function Assert-BuildTruthExistingPathAncestors
{
    param(
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][string]$Path
    )

    $rootPath = [IO.Path]::GetFullPath($Root).TrimEnd(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar)
    $candidatePath = [IO.Path]::GetFullPath($Path)
    if (-not (Test-BuildTruthPathContained -Root $rootPath -Candidate $candidatePath -AllowRoot))
    {
        Throw-BuildTruthFailure -Category "PathEscape" -Message (
            "Path '$Path' resolves outside '$rootPath'")
    }
    if (-not (Test-Path -LiteralPath $rootPath -PathType Container))
    {
        Throw-BuildTruthFailure -Category "MissingArtifact" -Message (
            "Containment root is missing or not a directory: $rootPath")
    }

    $relativePath = [IO.Path]::GetRelativePath($rootPath, $candidatePath)
    $segments = @($relativePath -split "[\\/]" | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    $currentPath = $rootPath
    $pathsToCheck = [System.Collections.Generic.List[string]]::new()
    $pathsToCheck.Add($currentPath)
    foreach ($segment in $segments)
    {
        $currentPath = Join-Path $currentPath $segment
        if (-not (Test-Path -LiteralPath $currentPath))
        {
            break
        }
        $pathsToCheck.Add($currentPath)
    }

    foreach ($existingPath in $pathsToCheck)
    {
        $item = Get-Item -LiteralPath $existingPath -Force
        if (Test-BuildTruthLinkOrReparsePoint -Item $item)
        {
            Throw-BuildTruthFailure -Category "PathEscape" -Message (
                "Path '$Path' has a link or reparse-point ancestor '$existingPath'")
        }
        $resolvedPath = (Resolve-Path -LiteralPath $existingPath).Path
        if (-not (Test-BuildTruthPathContained `
            -Root $rootPath `
            -Candidate $resolvedPath `
            -AllowRoot))
        {
            Throw-BuildTruthFailure -Category "PathEscape" -Message (
                "Path '$Path' resolves through an ancestor outside '$rootPath'")
        }
    }
}

function Resolve-BuildTruthContainedPath
{
    param(
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][string]$Path,
        [switch]$AllowMissing
    )

    $rootPath = [IO.Path]::GetFullPath($Root).TrimEnd(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar)
    $candidate = if ([IO.Path]::IsPathRooted($Path))
    {
        [IO.Path]::GetFullPath($Path)
    }
    else
    {
        [IO.Path]::GetFullPath((Join-Path $rootPath $Path))
    }
    if (-not (Test-BuildTruthPathContained -Root $rootPath -Candidate $candidate))
    {
        Throw-BuildTruthFailure -Category "PathEscape" -Message (
            "Path '$Path' resolves outside '$rootPath'")
    }

    # A cold configure may legitimately target a build root which does not
    # exist yet.  There are no ancestors to dereference in that case, so allow
    # the already-checked lexical containment only for the explicit missing
    # path call site.  Every artifact read/write re-enters this helper without
    # -AllowMissing after the root exists and therefore still performs the
    # reparse-point/ancestor checks below.
    if (-not (Test-Path -LiteralPath $rootPath -PathType Container))
    {
        if (Test-Path -LiteralPath $rootPath)
        {
            Throw-BuildTruthFailure -Category "MissingArtifact" -Message (
                "Containment root is not a directory: $rootPath")
        }
        if ($AllowMissing)
        {
            return $candidate
        }
        Throw-BuildTruthFailure -Category "MissingArtifact" -Message (
            "Containment root is missing: $rootPath")
    }

    Assert-BuildTruthExistingPathAncestors -Root $rootPath -Path $candidate

    if (Test-Path -LiteralPath $candidate)
    {
        $resolved = (Resolve-Path -LiteralPath $candidate).Path
        if (-not (Test-BuildTruthPathContained -Root $rootPath -Candidate $resolved))
        {
            Throw-BuildTruthFailure -Category "PathEscape" -Message (
                "Path '$Path' resolves through a link outside '$rootPath'")
        }
        $item = Get-Item -LiteralPath $candidate -Force
        if (Test-BuildTruthLinkOrReparsePoint -Item $item)
        {
            Throw-BuildTruthFailure -Category "PathEscape" -Message (
                "Path '$Path' is a reparse point")
        }
    }
    elseif (-not $AllowMissing)
    {
        Throw-BuildTruthFailure -Category "MissingArtifact" -Message (
            "Required path is missing: $candidate")
    }

    return $candidate
}

function Get-BuildTruthFileEvidence
{
    param(
        [Parameter(Mandatory)][string]$BuildRoot,
        [Parameter(Mandatory)][AllowEmptyCollection()][string[]]$Paths
    )

    $evidence = [System.Collections.Generic.List[object]]::new()
    foreach ($path in $Paths)
    {
        $absolutePath = Resolve-BuildTruthContainedPath `
            -Root $BuildRoot `
            -Path $path
        if (-not (Test-Path -LiteralPath $absolutePath -PathType Leaf))
        {
            Throw-BuildTruthFailure -Category "MissingArtifact" -Message (
                "Required evidence is not a file: $absolutePath")
        }
        $item = Get-Item -LiteralPath $absolutePath
        $hash = (Get-FileHash -LiteralPath $absolutePath -Algorithm SHA256).Hash.ToLowerInvariant()
        $evidence.Add([ordered]@{
            path = $absolutePath
            size = $item.Length
            lastWriteTimeUtc = $item.LastWriteTimeUtc.ToString("o")
            sha256 = $hash
        })
    }
    return @($evidence)
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

function Assert-BuildTruthJUnitOutcomeMatches
{
    param(
        [Parameter(Mandatory)][object]$Expected,
        [Parameter(Mandatory)][object]$Actual,
        [string]$Context = "JUnit receipt evidence"
    )

    foreach ($propertyName in @(
        "testCaseCount", "failureCount", "errorCount", "skippedCount",
        "notRunCount", "disabledSuiteCount"))
    {
        $expectedValue = Get-BuildTruthObjectPropertyValue `
            -Value $Expected `
            -Name $propertyName
        $actualValue = Get-BuildTruthObjectPropertyValue `
            -Value $Actual `
            -Name $propertyName
        if ($null -eq $expectedValue -or $null -eq $actualValue -or
            [int]$expectedValue -ne [int]$actualValue)
        {
            Throw-BuildTruthFailure -Category "ReceiptTampered" -Message (
                "$Context changed '$propertyName' after execution")
        }
    }
}

function Assert-BuildTruthNativeEvidenceIdentity
{
    param(
        [Parameter(Mandatory)][object]$Expected,
        [Parameter(Mandatory)][object]$Actual,
        [string]$Context = "native receipt evidence"
    )

    foreach ($propertyName in @("requestedBackend", "actualBackend"))
    {
        $expectedValue = Get-BuildTruthObjectPropertyValue `
            -Value $Expected `
            -Name $propertyName
        $actualValue = Get-BuildTruthObjectPropertyValue `
            -Value $Actual `
            -Name $propertyName
        if ([string]$expectedValue -ne [string]$actualValue)
        {
            Throw-BuildTruthFailure -Category "ReceiptTampered" -Message (
                "$Context changed '$propertyName' after execution")
        }
    }

    $expectedFallback = Get-BuildTruthObjectPropertyValue -Value $Expected -Name "fallback"
    $actualFallback = Get-BuildTruthObjectPropertyValue -Value $Actual -Name "fallback"
    foreach ($propertyName in @(
        "backendFallback", "softwareAdapter", "softwareAdapterAllowed"))
    {
        $expectedValue = Get-BuildTruthObjectPropertyValue `
            -Value $expectedFallback `
            -Name $propertyName
        $actualValue = Get-BuildTruthObjectPropertyValue `
            -Value $actualFallback `
            -Name $propertyName
        if ([bool]$expectedValue -ne [bool]$actualValue)
        {
            Throw-BuildTruthFailure -Category "ReceiptTampered" -Message (
                "$Context changed fallback '$propertyName' after execution")
        }
    }
}

function Enter-BuildTruthRunLock
{
    param([Parameter(Mandatory)][string]$BuildPath)

    $normalizedPath = [IO.Path]::GetFullPath($BuildPath).TrimEnd(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar)
    $lockIdentity = if ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT)
    {
        $normalizedPath.ToUpperInvariant()
    }
    else
    {
        $normalizedPath
    }
    $bytes = [Text.Encoding]::UTF8.GetBytes($lockIdentity)
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try
    {
        $digest = ([BitConverter]::ToString($sha256.ComputeHash($bytes))).Replace("-", "").ToLowerInvariant()
    }
    finally
    {
        $sha256.Dispose()
    }
    $name = "RVX.BuildTruth.RunLock.$digest"
    $createdNew = $false
    try
    {
        $mutex = [Threading.Mutex]::new($false, $name, [ref]$createdNew)
    }
    catch
    {
        Throw-BuildTruthFailure -Category "RunLock" -Message (
            "Unable to create run lock '$name': $($_.Exception.Message)")
    }

    $acquired = $false
    try
    {
        try
        {
            $acquired = $mutex.WaitOne(0)
        }
        catch [Threading.AbandonedMutexException]
        {
            # The OS transfers ownership to this process. The stale owner is
            # gone, so continue while retaining the normal release discipline.
            $acquired = $true
        }
        if (-not $acquired)
        {
            Throw-BuildTruthFailure -Category "RunLock" -Message (
                "Another Build Truth or Required Suite run owns build directory '$normalizedPath'")
        }
        return [pscustomobject]@{
            name = $name
            buildPath = $normalizedPath
            mutex = $mutex
            acquired = $true
        }
    }
    catch
    {
        $mutex.Dispose()
        throw
    }
}

function Exit-BuildTruthRunLock
{
    param([AllowNull()][object]$Lock)

    if ($null -eq $Lock)
    {
        return
    }
    try
    {
        if ($Lock.acquired)
        {
            $Lock.mutex.ReleaseMutex()
        }
    }
    finally
    {
        $Lock.mutex.Dispose()
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
    $previousErrorActionPreference = $ErrorActionPreference
    try
    {
        # A failing CTest writes a summary to stderr. Keep that as ordinary
        # command evidence so JUnit remains the semantic source of failure
        # classification even when the runner itself uses Stop.
        $ErrorActionPreference = "Continue"
        $ctestOutput = & ctest @arguments 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally
    {
        $ErrorActionPreference = $previousErrorActionPreference
    }
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

function Get-BuildTruthReceiptFileEvidence
{
    param(
        [Parameter(Mandatory)][string]$RepositoryRoot,
        [Parameter(Mandatory)][string]$Path
    )

    $absolutePath = Resolve-BuildTruthContainedPath `
        -Root $RepositoryRoot `
        -Path $Path
    if (-not (Test-Path -LiteralPath $absolutePath -PathType Leaf))
    {
        Throw-BuildTruthFailure -Category "MissingArtifact" -Message (
            "Receipt evidence is not a file: $absolutePath")
    }

    $repositoryPath = [IO.Path]::GetFullPath($RepositoryRoot).TrimEnd(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar)
    $relativePath = [IO.Path]::GetRelativePath($repositoryPath, $absolutePath).Replace(
        [IO.Path]::DirectorySeparatorChar,
        "/")
    if ([string]::IsNullOrWhiteSpace($relativePath) -or
        [IO.Path]::IsPathRooted($relativePath) -or
        $relativePath -match "(^|/)\.\.(/|$)")
    {
        Throw-BuildTruthFailure -Category "ReceiptPath" -Message (
            "Receipt evidence path is not repository-relative: $Path")
    }

    $item = Get-Item -LiteralPath $absolutePath -Force
    return [ordered]@{
        path = $relativePath
        sha256 = (Get-FileHash -LiteralPath $absolutePath -Algorithm SHA256).Hash.ToLowerInvariant()
        size = $item.Length
    }
}

function Get-BuildTruthObjectPropertyValue
{
    param(
        [Parameter(Mandatory)][object]$Value,
        [Parameter(Mandatory)][string]$Name
    )

    if ($Value -is [Collections.IDictionary])
    {
        if ($Value.Contains($Name))
        {
            return $Value[$Name]
        }
        return $null
    }
    $property = $Value.PSObject.Properties[$Name]
    if ($null -eq $property)
    {
        return $null
    }
    return $property.Value
}

function Assert-BuildTruthReceiptFileEvidence
{
    param(
        [Parameter(Mandatory)][string]$RepositoryRoot,
        [Parameter(Mandatory)][object]$Evidence,
        [string]$Context = "receipt evidence"
    )

    $pathValue = Get-BuildTruthObjectPropertyValue -Value $Evidence -Name "path"
    $hashValue = Get-BuildTruthObjectPropertyValue -Value $Evidence -Name "sha256"
    if ($null -eq $pathValue -or $null -eq $hashValue)
    {
        Throw-BuildTruthFailure -Category "ReceiptSchema" -Message (
            "$Context is missing path or sha256")
    }
    $path = [string]$pathValue
    $expectedHash = [string]$hashValue
    if ([IO.Path]::IsPathRooted($path) -or $path -match "(^|[\\/])\.\.([\\/]|$)")
    {
        Throw-BuildTruthFailure -Category "ReceiptPath" -Message (
            "$Context has an unsafe repository-relative path '$path'")
    }
    if ($expectedHash -notmatch "^[0-9a-f]{64}$")
    {
        Throw-BuildTruthFailure -Category "ReceiptSchema" -Message (
            "$Context has an invalid SHA-256 digest")
    }

    $actual = Get-BuildTruthReceiptFileEvidence `
        -RepositoryRoot $RepositoryRoot `
        -Path $path
    if ($actual.sha256 -ne $expectedHash)
    {
        Throw-BuildTruthFailure -Category "ReceiptTampered" -Message (
            "$Context SHA-256 does not match '$path'")
    }
}

function Get-BuildTruthRunnerDigest
{
    param(
        [Parameter(Mandatory)][string]$RepositoryRoot,
        [Parameter(Mandatory)][AllowEmptyCollection()][string[]]$Paths
    )

    if ($Paths.Count -eq 0)
    {
        Throw-BuildTruthFailure -Category "ReceiptSchema" -Message (
            "Runner digest requires at least one script path")
    }

    $files = [System.Collections.Generic.List[object]]::new()
    foreach ($path in ($Paths | Sort-Object -Unique))
    {
        $files.Add((Get-BuildTruthReceiptFileEvidence `
            -RepositoryRoot $RepositoryRoot `
            -Path $path))
    }
    $serialized = @(
        $files | ForEach-Object { "$($_.path)`0$($_.sha256)" }
    ) -join "`n"
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

    return [ordered]@{
        algorithm = "sha256:path-content-v1"
        digest = $digest
        files = @($files)
    }
}

function Write-BuildTruthImmutableReceipt
{
    param(
        [Parameter(Mandatory)][string]$RepositoryRoot,
        [Parameter(Mandatory)][ValidateSet("BuildTruth", "RequiredSuite")][string]$ReceiptKind,
        [Parameter(Mandatory)][string]$RunId,
        [Parameter(Mandatory)][object]$Value,
        [Parameter(Mandatory)][string]$Status
    )

    if ($Status -ne "passed")
    {
        Throw-BuildTruthFailure -Category "ReceiptStatus" -Message (
            "A $ReceiptKind receipt cannot be written for status '$Status'")
    }
    if ($RunId -notmatch "^[0-9a-f]{32}$")
    {
        Throw-BuildTruthFailure -Category "ReceiptSchema" -Message (
            "Receipt runId must be a 32-character lowercase hexadecimal value")
    }

    $repositoryPath = [IO.Path]::GetFullPath($RepositoryRoot).TrimEnd(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar)
    $receiptDirectory = [IO.Path]::GetFullPath((Join-Path $repositoryPath ".agent-harness/receipts"))
    $relativeReceiptPath = ".agent-harness/receipts/$ReceiptKind.$RunId.json"
    $receiptPath = [IO.Path]::GetFullPath((Join-Path $repositoryPath $relativeReceiptPath))
    if (-not (Test-BuildTruthPathContained `
        -Root $repositoryPath `
        -Candidate $receiptPath))
    {
        Throw-BuildTruthFailure -Category "ReceiptPath" -Message (
            "Receipt destination escapes the repository: $receiptPath")
    }

    $expectedSchema = if ($ReceiptKind -eq "BuildTruth")
    {
        "RVX.AgentHarness.BuildTruthReceipt"
    }
    else
    {
        "RVX.AgentHarness.RequiredSuiteReceipt"
    }
    $receiptSchema = Get-BuildTruthObjectPropertyValue -Value $Value -Name "schema"
    $receiptSchemaVersion = Get-BuildTruthObjectPropertyValue -Value $Value -Name "schemaVersion"
    $receiptRunId = Get-BuildTruthObjectPropertyValue -Value $Value -Name "runId"
    $receiptIdentityPath = Get-BuildTruthObjectPropertyValue `
        -Value $Value `
        -Name "immutableReceiptPath"
    if ($null -eq $receiptSchema -or
        $receiptSchema -ne $expectedSchema -or
        $null -eq $receiptSchemaVersion -or
        $receiptSchemaVersion -ne 1 -or
        $null -eq $receiptRunId -or
        $receiptRunId -ne $RunId -or
        $null -eq $receiptIdentityPath -or
        $receiptIdentityPath -ne $relativeReceiptPath)
    {
        Throw-BuildTruthFailure -Category "ReceiptSchema" -Message (
            "$ReceiptKind receipt does not match its immutable schema identity")
    }

    # Validate all existing ancestors before creating anything beneath the
    # repository. Repeat after creation to reject a raced-in link/reparse
    # point before opening a temporary or final receipt file.
    Assert-BuildTruthExistingPathAncestors -Root $repositoryPath -Path $receiptDirectory
    New-Item -ItemType Directory -Force -Path $receiptDirectory | Out-Null
    Assert-BuildTruthExistingPathAncestors -Root $repositoryPath -Path $receiptDirectory
    if (Test-Path -LiteralPath $receiptPath)
    {
        Throw-BuildTruthFailure -Category "ReceiptCollision" -Message (
            "Immutable receipt already exists: $relativeReceiptPath")
    }

    $temporaryPath = Join-Path $receiptDirectory (".$ReceiptKind.$RunId." + [guid]::NewGuid().ToString("N") + ".tmp")
    try
    {
        $serialized = $Value | ConvertTo-Json -Depth 32
        $bytes = [Text.UTF8Encoding]::new($false).GetBytes($serialized + [Environment]::NewLine)
        $stream = [IO.File]::Open(
            $temporaryPath,
            [IO.FileMode]::CreateNew,
            [IO.FileAccess]::Write,
            [IO.FileShare]::None)
        try
        {
            $stream.Write($bytes, 0, $bytes.Length)
            $stream.Flush($true)
        }
        finally
        {
            $stream.Dispose()
        }

        try
        {
            [IO.File]::Move($temporaryPath, $receiptPath)
        }
        catch
        {
            if (Test-Path -LiteralPath $receiptPath)
            {
                Throw-BuildTruthFailure -Category "ReceiptCollision" -Message (
                    "Immutable receipt already exists: $relativeReceiptPath")
            }
            Throw-BuildTruthFailure -Category "ReceiptWrite" -Message (
                "Unable to publish immutable receipt '$relativeReceiptPath': $($_.Exception.Message)")
        }
    }
    finally
    {
        if (Test-Path -LiteralPath $temporaryPath)
        {
            Remove-Item -LiteralPath $temporaryPath -Force
        }
    }

    $evidence = Get-BuildTruthReceiptFileEvidence `
        -RepositoryRoot $repositoryPath `
        -Path $relativeReceiptPath
    return [ordered]@{
        runId = $RunId
        immutableReceiptPath = $relativeReceiptPath
        sha256 = $evidence.sha256
    }
}

function Write-BuildTruthReceiptPointer
{
    param(
        [Parameter(Mandatory)][string]$RepositoryRoot,
        [Parameter(Mandatory)][ValidateSet("BuildTruth", "RequiredSuite")][string]$ReceiptKind,
        [Parameter(Mandatory)][string]$PointerFileName,
        [Parameter(Mandatory)][string]$RunId,
        [Parameter(Mandatory)][string]$ImmutableReceiptPath,
        [Parameter(Mandatory)][string]$ImmutableReceiptSha256
    )

    if ($PointerFileName -notmatch "^[A-Za-z0-9][A-Za-z0-9_.-]*\.json$")
    {
        Throw-BuildTruthFailure -Category "ReceiptPath" -Message (
            "Receipt pointer file name is unsafe: $PointerFileName")
    }
    if ($RunId -notmatch "^[0-9a-f]{32}$" -or
        $ImmutableReceiptSha256 -notmatch "^[0-9a-f]{64}$")
    {
        Throw-BuildTruthFailure -Category "ReceiptSchema" -Message (
            "Receipt pointer has an invalid immutable receipt identity")
    }

    $receiptEvidence = Get-BuildTruthReceiptFileEvidence `
        -RepositoryRoot $RepositoryRoot `
        -Path $ImmutableReceiptPath
    if ($receiptEvidence.sha256 -ne $ImmutableReceiptSha256)
    {
        Throw-BuildTruthFailure -Category "ReceiptTampered" -Message (
            "Immutable receipt changed before its pointer could be written")
    }

    $repositoryPath = [IO.Path]::GetFullPath($RepositoryRoot).TrimEnd(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar)
    $receiptDirectory = [IO.Path]::GetFullPath((Join-Path $repositoryPath ".agent-harness/receipts"))
    $pointerPath = [IO.Path]::GetFullPath((Join-Path $receiptDirectory $PointerFileName))
    if (-not (Test-BuildTruthPathContained `
        -Root $receiptDirectory `
        -Candidate $pointerPath))
    {
        Throw-BuildTruthFailure -Category "ReceiptPath" -Message (
            "Receipt pointer escapes the receipt directory: $PointerFileName")
    }

    Assert-BuildTruthExistingPathAncestors -Root $repositoryPath -Path $receiptDirectory
    New-Item -ItemType Directory -Force -Path $receiptDirectory | Out-Null
    Assert-BuildTruthExistingPathAncestors -Root $repositoryPath -Path $receiptDirectory
    $pointer = [ordered]@{
        schema = "RVX.AgentHarness.ImmutableReceiptPointer"
        schemaVersion = 1
        receiptKind = $ReceiptKind
        updatedAtUtc = (Get-Date).ToUniversalTime().ToString("o")
        runId = $RunId
        immutableReceiptPath = $ImmutableReceiptPath
        immutableReceiptSha256 = $ImmutableReceiptSha256
    }
    $temporaryPath = Join-Path $receiptDirectory (".$PointerFileName." + [guid]::NewGuid().ToString("N") + ".tmp")
    try
    {
        $serialized = $pointer | ConvertTo-Json -Depth 8
        $bytes = [Text.UTF8Encoding]::new($false).GetBytes($serialized + [Environment]::NewLine)
        $sha256 = [Security.Cryptography.SHA256]::Create()
        try
        {
            $pointerSha256 = ([BitConverter]::ToString(
                $sha256.ComputeHash($bytes))).Replace("-", "").ToLowerInvariant()
        }
        finally
        {
            $sha256.Dispose()
        }
        $stream = [IO.File]::Open(
            $temporaryPath,
            [IO.FileMode]::CreateNew,
            [IO.FileAccess]::Write,
            [IO.FileShare]::None)
        try
        {
            $stream.Write($bytes, 0, $bytes.Length)
            $stream.Flush($true)
        }
        finally
        {
            $stream.Dispose()
        }

        # The overwrite overload maps to a same-volume replace operation, so
        # readers observe either the previous complete pointer or the new
        # complete pointer, never the temporary file.
        [IO.File]::Move($temporaryPath, $pointerPath, $true)
    }
    catch
    {
        Throw-BuildTruthFailure -Category "ReceiptWrite" -Message (
            "Unable to publish receipt pointer '$PointerFileName': $($_.Exception.Message)")
    }
    finally
    {
        if (Test-Path -LiteralPath $temporaryPath)
        {
            Remove-Item -LiteralPath $temporaryPath -Force
        }
    }

    # Nothing after the atomic replacement can fail: a failed publication must
    # never leave a pointer that advances to a receipt the runner reports as
    # failed. The returned digest is over exactly the published byte sequence.
    return [ordered]@{
        path = ".agent-harness/receipts/" + $PointerFileName
        sha256 = $pointerSha256
        size = $bytes.Length
    }
}

Export-ModuleMember -Function @(
    "Assert-BuildTruthExistingPathAncestors",
    "Assert-BuildTruthInventoryLanes",
    "Assert-BuildTruthJUnitOutcome",
    "Assert-BuildTruthJUnitOutcomeMatches",
    "Assert-BuildTruthNativeEvidenceIdentity",
    "Assert-BuildTruthRequiredSelection",
    "Enter-BuildTruthRunLock",
    "Exit-BuildTruthRunLock",
    "Get-BuildTruthCTestInventory",
    "Get-BuildTruthFailureCategory",
    "Get-BuildTruthFileEvidence",
    "Get-BuildTruthJUnitOutcome",
    "Get-BuildTruthNativeEvidence",
    "Get-BuildTruthPathFingerprint",
    "Get-BuildTruthPathStringComparison",
    "Get-BuildTruthReceiptFileEvidence",
    "Get-BuildTruthRunnerDigest",
    "Get-BuildTruthTestLabels",
    "Get-BuildTruthTestLanes",
    "Get-BuildTruthWorkingSourceFingerprint",
    "Invoke-BuildTruthCTestLane",
    "New-BuildTruthFailure",
    "Resolve-BuildTruthContainedPath",
    "Assert-BuildTruthReceiptFileEvidence",
    "Test-BuildTruthPathContained",
    "Throw-BuildTruthFailure",
    "Write-BuildTruthImmutableReceipt",
    "Write-BuildTruthReceiptPointer",
    "Write-BuildTruthJson"
)
