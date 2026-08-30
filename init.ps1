[CmdletBinding()]
param(
    [ValidateSet("Inspect", "Verify", "Close")]
    [string]$Mode = "Inspect",
    [string]$Preset = "",
    [string]$TestPreset = "",
    [string]$BuildDir = "",
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",
    [string]$BaseRef = "master",
    [switch]$AllowConfigure,
    [ValidateSet("Text", "Json")]
    [string]$OutputFormat = "Text"
)

# Some Windows launchers expose a pwsh shim that actually starts Windows
# PowerShell. Re-enter through a real PowerShell 7 binary before using Test-Json
# schema validation or the cross-platform Build Truth modules.
if ($PSVersionTable.PSVersion.Major -lt 7)
{
    $forward = @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $PSCommandPath,
        "-Mode", $Mode, "-Configuration", $Configuration,
        "-BaseRef", $BaseRef, "-OutputFormat", $OutputFormat
    )
    foreach ($pair in @(
        @("Preset", $Preset),
        @("TestPreset", $TestPreset),
        @("BuildDir", $BuildDir)
    ))
    {
        if (-not [string]::IsNullOrWhiteSpace([string]$pair[1]))
        {
            $forward += @("-$($pair[0])", [string]$pair[1])
        }
    }
    if ($AllowConfigure) { $forward += "-AllowConfigure" }

    $candidates = @(Get-Command pwsh -All -ErrorAction SilentlyContinue |
        ForEach-Object Source | Select-Object -Unique)
    foreach ($candidate in $candidates)
    {
        $major = & $candidate -NoProfile -Command (
            "[Console]::Write(`$PSVersionTable.PSVersion.Major)") 2>$null
        if ($LASTEXITCODE -eq 0 -and [string]$major -match "^\s*(\d+)\s*$" -and
            [int]$Matches[1] -ge 7)
        {
            if ($OutputFormat -eq "Text")
            {
                Write-Warning "The first pwsh is PowerShell $($PSVersionTable.PSVersion); re-launching with '$candidate'."
            }
            & $candidate @forward
            exit $LASTEXITCODE
        }
    }
    throw "PowerShell 7 or newer is required, but no usable pwsh 7 executable was found"
}

Import-Module (Join-Path $PSScriptRoot "Scripts\BuildTruth\ValidationProcessEnvironment.psm1") -Force

# Keep the harness bootstrap itself out of an inherited Windows environment
# block that contains both PATH and Path. The shared launcher preserves this
# entry point's optional arguments and exits with the child result.
$canonicalizationArguments = @(
    "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $PSCommandPath,
    "-Mode", $Mode,
    "-Configuration", $Configuration,
    "-BaseRef", $BaseRef,
    "-OutputFormat", $OutputFormat
)
foreach ($pair in @(
    @("Preset", $Preset),
    @("TestPreset", $TestPreset),
    @("BuildDir", $BuildDir)
))
{
    if (-not [string]::IsNullOrWhiteSpace([string]$pair[1]))
    {
        $canonicalizationArguments += @("-$($pair[0])", [string]$pair[1])
    }
}
if ($AllowConfigure) { $canonicalizationArguments += "-AllowConfigure" }
$environmentCanonicalization = Invoke-ValidationProcessEnvironmentCanonicalization `
    -WorkingDirectory (Get-Location).Path `
    -InvocationArguments $canonicalizationArguments
if ($environmentCanonicalization.relaunched)
{
    exit $environmentCanonicalization.exitCode
}

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
if (Test-Path variable:PSNativeCommandUseErrorActionPreference)
{
    $PSNativeCommandUseErrorActionPreference = $false
}

function Invoke-NativeChecked
{
    param(
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][string]$FilePath,
        [Parameter(Mandatory)][AllowEmptyCollection()][string[]]$Arguments
    )

    $output = @(& $FilePath @Arguments 2>&1)
    if ($LASTEXITCODE -ne 0)
    {
        $detail = ($output | Select-Object -Last 20) -join [Environment]::NewLine
        throw "$Name failed with exit code $LASTEXITCODE`n$detail"
    }
    return $output
}

function Get-DefaultPreset
{
    if ([Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
        [Runtime.InteropServices.OSPlatform]::Windows)) { return "win_x64_debug" }
    if ([Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
        [Runtime.InteropServices.OSPlatform]::OSX)) { return "mac_arm64_debug" }
    return "linux_x64_debug"
}

function Get-PathStringComparison
{
    if ([Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
        [Runtime.InteropServices.OSPlatform]::Windows))
    {
        return [StringComparison]::OrdinalIgnoreCase
    }
    return [StringComparison]::Ordinal
}

function Get-PathStringComparer
{
    if ([Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
        [Runtime.InteropServices.OSPlatform]::Windows))
    {
        return [StringComparer]::OrdinalIgnoreCase
    }
    return [StringComparer]::Ordinal
}

function Get-RequiredJsonProperty
{
    param(
        [Parameter(Mandatory)][object]$Object,
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][string]$Context
    )

    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property)
    {
        throw "$Context is missing required property '$Name'"
    }
    return $property.Value
}

function Get-OptionalJsonProperty
{
    param(
        [Parameter(Mandatory)][object]$Object,
        [Parameter(Mandatory)][string]$Name
    )

    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

function Get-RequiredJsonArrayProperty
{
    param(
        [Parameter(Mandatory)][object]$Object,
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][string]$Context
    )

    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property)
    {
        throw "$Context is missing required array property '$Name'"
    }
    $value = $property.Value
    if ($value -isnot [System.Array])
    {
        throw "$Context property '$Name' must be a JSON array"
    }
    return @($value)
}

function ConvertTo-CanonicalUtcTimestamp
{
    param(
        [Parameter(Mandatory)][object]$Value,
        [Parameter(Mandatory)][string]$Context
    )

    try
    {
        if ($Value -is [DateTimeOffset])
        {
            return $Value.ToUniversalTime().ToString("o")
        }
        if ($Value -is [DateTime])
        {
            return $Value.ToUniversalTime().ToString("o")
        }
        return [DateTimeOffset]::Parse(
            [string]$Value,
            [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::RoundtripKind).ToUniversalTime().ToString("o")
    }
    catch
    {
        throw "$Context is not a valid ISO-8601 timestamp"
    }
}

function Get-RepositoryRelativePath
{
    param(
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Context
    )

    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar)
    $pathFull = [IO.Path]::GetFullPath($Path)
    if ($pathFull.Equals($rootFull, (Get-PathStringComparison)))
    {
        return "."
    }
    $prefix = $rootFull + [IO.Path]::DirectorySeparatorChar
    if (-not $pathFull.StartsWith($prefix, (Get-PathStringComparison)))
    {
        throw "$Context must remain below the repository root"
    }
    return $pathFull.Substring($rootFull.Length).TrimStart(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar).Replace("\", "/")
}

function Get-NormalizedRelativePath
{
    param(
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Context
    )

    if ([string]::IsNullOrWhiteSpace($Path) -or [IO.Path]::IsPathRooted($Path) -or
        $Path -match "(^|[\\/])\.\.([\\/]|$)")
    {
        throw "$Context must be a non-empty repository-relative path"
    }
    $rootFull = [IO.Path]::GetFullPath($Root)
    $full = [IO.Path]::GetFullPath((Join-Path $rootFull $Path))
    $prefix = $rootFull.TrimEnd([IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if (-not $full.StartsWith($prefix, (Get-PathStringComparison)))
    {
        throw "$Context must remain below the repository root"
    }
    return Get-RepositoryRelativePath -Root $rootFull -Path $full -Context $Context
}

function Get-ContainedAbsolutePath
{
    param(
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Context
    )

    $rootFull = [IO.Path]::GetFullPath($Root)
    $full = [IO.Path]::GetFullPath($Path)
    $prefix = $rootFull.TrimEnd([IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if (-not $full.StartsWith($prefix, (Get-PathStringComparison)))
    {
        throw "$Context must remain below '$rootFull'"
    }
    return $full
}

function Assert-CanonicalValidationReceiptPath
{
    param(
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][string]$RelativePath,
        [Parameter(Mandatory)][string]$RunId,
        [Parameter(Mandatory)][ValidateSet("Architecture", "BuildTruth", "RequiredSuite")][string]$ReceiptKind,
        [Parameter(Mandatory)][string]$Context
    )

    if ($RunId -notmatch "^[a-f0-9]{32}$")
    {
        throw "$Context has an invalid runId"
    }
    $normalized = Get-NormalizedRelativePath $Root $RelativePath $Context
    $prefix = ".agent-harness/receipts/"
    if (-not $normalized.StartsWith($prefix, [StringComparison]::Ordinal))
    {
        throw "$Context must be stored below '$prefix'"
    }
    $expected = "$prefix$ReceiptKind.$RunId.json"
    if ($normalized -cne $expected)
    {
        throw "$Context must name '$expected'"
    }
    return $normalized
}

function Assert-CanonicalArchitectureReceiptPath
{
    param(
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][string]$RelativePath,
        [Parameter(Mandatory)][string]$RunId,
        [Parameter(Mandatory)][string]$Context
    )

    return Assert-CanonicalValidationReceiptPath -Root $Root -RelativePath $RelativePath `
        -RunId $RunId -ReceiptKind Architecture -Context $Context
}

function Assert-Fingerprint
{
    param(
        [Parameter(Mandatory)][object]$Fingerprint,
        [Parameter(Mandatory)][string]$Context
    )

    $algorithm = [string](Get-RequiredJsonProperty $Fingerprint "algorithm" $Context)
    $fileCount = [int](Get-RequiredJsonProperty $Fingerprint "fileCount" $Context)
    $digest = [string](Get-RequiredJsonProperty $Fingerprint "digest" $Context)
    if ($algorithm -ne "sha256:path-state-content-v1" -or $fileCount -lt 0 -or
        $digest -notmatch "^[a-f0-9]{64}$")
    {
        throw "$Context is not a valid BuildTruth path fingerprint"
    }
    return [pscustomobject]@{
        algorithm = $algorithm
        fileCount = $fileCount
        digest = $digest
    }
}

function Assert-FingerprintEqual
{
    param(
        [Parameter(Mandatory)][object]$Expected,
        [Parameter(Mandatory)][object]$Actual,
        [Parameter(Mandatory)][string]$Context
    )

    if ($Expected.algorithm -ne $Actual.algorithm -or
        $Expected.fileCount -ne $Actual.fileCount -or
        $Expected.digest -ne $Actual.digest)
    {
        throw "$Context fingerprints do not match"
    }
}

function Get-ProductSourceFingerprint
{
    param(
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][string]$GitSafeDirectory
    )

    # Build Truth and the harness share one product-source definition so
    # publishing receipts or updating the mutable ledger cannot invalidate the
    # verification those records describe.
    return Get-BuildTruthWorkingSourceFingerprint `
        -Root $Root `
        -GitSafeDirectory $GitSafeDirectory
}

function Get-CanonicalReceiptVerificationCommand
{
    param(
        [Parameter(Mandatory)][object]$Expectation,
        [Parameter(Mandatory)][string]$Context
    )

    $kind = [string](Get-RequiredJsonProperty $Expectation "kind" $Context)
    $buildDir = ([string](Get-RequiredJsonProperty $Expectation "buildDir" $Context)).Replace("/", "\")
    if ($kind -eq "Architecture")
    {
        $preset = [string](Get-RequiredJsonProperty $Expectation "preset" $Context)
        $testPreset = [string](Get-RequiredJsonProperty $Expectation "testPreset" $Context)
        $command = "pwsh -NoProfile -ExecutionPolicy Bypass -File .\init.ps1 -Mode Verify -Preset $preset"
        if ($testPreset -cne "${preset}_architecture")
        {
            $command += " -TestPreset $testPreset"
        }
        $command += " -BuildDir $buildDir -Configuration $([string](Get-RequiredJsonProperty $Expectation 'configuration' $Context))"
        $baseRef = [string](Get-RequiredJsonProperty $Expectation "baseRef" $Context)
        if ($baseRef -cne "master")
        {
            $command += " -BaseRef $baseRef"
        }
        if ([bool](Get-RequiredJsonProperty $Expectation "allowConfigure" $Context))
        {
            $command += " -AllowConfigure"
        }
        return $command
    }
    if ($kind -eq "BuildTruth")
    {
        $softwareAdapterAllowed = [bool](Get-RequiredJsonProperty `
            $Expectation "softwareAdapterAllowed" $Context)
        $lanePresets = @(Get-RequiredJsonArrayProperty $Expectation "laneTestPresets" $Context)
        $laneArgument = '"' + ($lanePresets -join ",") + '"'
        $command = "pwsh -NoProfile -ExecutionPolicy Bypass -File .\Scripts\run_build_truth.ps1" +
            " -ConfigurePreset $([string](Get-RequiredJsonProperty $Expectation 'configurePreset' $Context))" +
            " -BuildPreset $([string](Get-RequiredJsonProperty $Expectation 'buildPreset' $Context))" +
            " -LaneTestPresets $laneArgument -BuildDir $buildDir" +
            " -Configuration $([string](Get-RequiredJsonProperty $Expectation 'configuration' $Context))" +
            " -BaseRef $([string](Get-RequiredJsonProperty $Expectation 'baseRef' $Context))"
        $nativeBackend = [string](Get-RequiredJsonProperty $Expectation "requiredNativeBackend" $Context)
        if (-not [string]::IsNullOrWhiteSpace($nativeBackend))
        {
            $command += " -RequiredNativeBackend $nativeBackend"
        }
        if ($softwareAdapterAllowed)
        {
            $command += " -AllowSoftwareAdapter"
        }
        if ([bool](Get-RequiredJsonProperty $Expectation "fresh" $Context))
        {
            $command += " -Fresh"
        }
        return $command
    }
    if ($kind -eq "RequiredSuite")
    {
        $fresh = [bool](Get-RequiredJsonProperty $Expectation "fresh" $Context)
        $skipConfigure = [bool](Get-RequiredJsonProperty $Expectation "skipConfigure" $Context)
        if ($fresh -and $skipConfigure)
        {
            throw "$Context cannot combine fresh=true with skipConfigure=true"
        }
        $buildTargets = @(Get-RequiredJsonArrayProperty $Expectation "buildTargets" $Context)
        $targetArgument = $buildTargets -join ","
        $command = "pwsh -NoProfile -ExecutionPolicy Bypass -File .\Scripts\run_required_suite.ps1" +
            " -ConfigurePreset $([string](Get-RequiredJsonProperty $Expectation 'configurePreset' $Context))" +
            " -BuildPreset $([string](Get-RequiredJsonProperty $Expectation 'buildPreset' $Context))" +
            " -TestPreset $([string](Get-RequiredJsonProperty $Expectation 'testPreset' $Context))" +
            " -BuildDir $buildDir -Configuration $([string](Get-RequiredJsonProperty $Expectation 'configuration' $Context))" +
            " -BuildTarget $targetArgument" +
            " -RequiredLabel $([string](Get-RequiredJsonProperty $Expectation 'requiredLabel' $Context))"
        $expectedTestCount = Get-RequiredJsonProperty $Expectation "expectedTestCount" $Context
        if ($null -ne $expectedTestCount)
        {
            $command += " -ExpectedTestCount $([int]$expectedTestCount)"
        }
        $semanticArtifacts = @(Get-RequiredJsonArrayProperty $Expectation "requiredSemanticArtifacts" $Context)
        if ($semanticArtifacts.Count -ne 0)
        {
            $buildDirPrefix = ([string](Get-RequiredJsonProperty $Expectation "buildDir" $Context)).TrimEnd("/") + "/"
            $evidenceArguments = [System.Collections.Generic.List[string]]::new()
            foreach ($artifact in $semanticArtifacts)
            {
                $artifactPath = [string]$artifact
                if (-not $artifactPath.StartsWith($buildDirPrefix, [StringComparison]::Ordinal))
                {
                    throw "$Context semantic artifact '$artifactPath' is outside its buildDir"
                }
                $evidenceArguments.Add($artifactPath.Substring($buildDirPrefix.Length))
            }
            $command += " -RequiredEvidence '$($evidenceArguments -join ';')'"
        }
        $command += " -BaseRef $([string](Get-RequiredJsonProperty $Expectation 'baseRef' $Context))"
        if ($fresh) { $command += " -Fresh" }
        if ($skipConfigure) { $command += " -SkipConfigure" }
        return $command
    }
    throw "$Context has an unsupported receipt kind '$kind'"
}

function Read-HarnessState
{
    param([string]$StatePath, [string]$SchemaPath)

    $json = Get-Content -LiteralPath $StatePath -Raw
    if (-not ($json | Test-Json -SchemaFile $SchemaPath))
    {
        throw "feature_list.json does not satisfy feature_list.schema.json"
    }
    $state = $json | ConvertFrom-Json
    $known = @{}
    $active = @()
    $blocked = @()
    $passing = @()

    foreach ($feature in @($state.features))
    {
        $id = [string]$feature.id
        if ($known.ContainsKey($id)) { throw "Duplicate feature id: $id" }

        $verification = @{}
        foreach ($gate in @($feature.verification))
        {
            $gateId = [string]$gate.id
            if ($verification.ContainsKey($gateId))
            {
                throw "Feature '$id' has duplicate verification id '$gateId'"
            }
            $expectedArtifact = if ($null -eq $gate.expectedArtifact) {
                ""
            } else {
                [string]$gate.expectedArtifact
            }
            $expectedReceipt = Get-OptionalJsonProperty $gate "expectedReceipt"
            $expectedReceiptKind = if ($null -eq $expectedReceipt) {
                ""
            } else {
                [string](Get-RequiredJsonProperty $expectedReceipt "kind" `
                    "Feature '$id' verification '$gateId' expectedReceipt")
            }
            $runnerReceiptKind = ""
            if ([string]$gate.command -match "(^|[\\/])run_build_truth\.ps1(\s|$)")
            {
                $runnerReceiptKind = "BuildTruth"
            }
            elseif ([string]$gate.command -match "(^|[\\/])run_required_suite\.ps1(\s|$)")
            {
                $runnerReceiptKind = "RequiredSuite"
            }
            elseif ([string]$gate.command -match "(^|[\\/])init\.ps1\s+-Mode\s+Verify(\s|$)")
            {
                $runnerReceiptKind = "Architecture"
            }
            if (-not [string]::IsNullOrWhiteSpace($runnerReceiptKind))
            {
                $canonicalArtifact = ".agent-harness/receipts/$runnerReceiptKind.{runId}.json"
                if ($expectedArtifact -cne $canonicalArtifact -or
                    $expectedReceiptKind -cne $runnerReceiptKind)
                {
                    throw "Feature '$id' verification '$gateId' must bind '$canonicalArtifact' and a matching expectedReceipt"
                }
                $canonicalCommand = Get-CanonicalReceiptVerificationCommand `
                    -Expectation $expectedReceipt `
                    -Context "Feature '$id' verification '$gateId' expectedReceipt"
                if ([string]$gate.command -cne $canonicalCommand)
                {
                    throw "Feature '$id' verification '$gateId' command does not match its expectedReceipt"
                }
            }
            elseif ($null -ne $expectedReceipt -or $expectedArtifact.Contains("{runId}"))
            {
                throw "Feature '$id' verification '$gateId' declares a receipt without a recognized immutable runner"
            }
            $verification[$gateId] = $gate
        }
        $evidenceIds = @{}
        foreach ($record in @($feature.evidence))
        {
            $gateId = [string]$record.verificationId
            if ($evidenceIds.ContainsKey($gateId))
            {
                throw "Feature '$id' has duplicate evidence for verification '$gateId'"
            }
            if (-not $verification.ContainsKey($gateId))
            {
                throw "Feature '$id' evidence refers to unknown verification '$gateId'"
            }
            if ($record.command -cne $verification[$gateId].command)
            {
                throw "Feature '$id' evidence '$gateId' does not match its declared command"
            }
            if ($record.result -eq "passed" -and
                ($null -eq $record.exitCode -or [int]$record.exitCode -ne 0))
            {
                throw "Feature '$id' passed evidence '$gateId' requires exitCode 0"
            }
            $evidenceIds[$gateId] = $record
        }

        switch ([string]$feature.status)
        {
            "active" { $active += $feature }
            "blocked" { $blocked += $feature }
            "passing" { $passing += $feature }
        }
        $known[$id] = $feature
    }

    foreach ($feature in @($state.features))
    {
        foreach ($dependency in @($feature.dependencies))
        {
            $dependencyId = [string]$dependency
            if ($dependencyId -eq [string]$feature.id)
            {
                throw "Feature '$($feature.id)' may not depend on itself"
            }
            if (-not $known.ContainsKey($dependencyId))
            {
                throw "Feature '$($feature.id)' has unknown dependency '$dependencyId'"
            }
            if ($feature.status -in @("active", "passing") -and
                $known[$dependencyId].status -ne "passing")
            {
                throw "Feature '$($feature.id)' depends on non-passing '$dependencyId'"
            }
        }
    }

    $marks = @{}
    function Visit-FeatureDependency
    {
        param([Parameter(Mandatory)][string]$FeatureId)

        $mark = if ($marks.ContainsKey($FeatureId)) { [int]$marks[$FeatureId] } else { 0 }
        if ($mark -eq 1) { throw "Feature dependency cycle includes '$FeatureId'" }
        if ($mark -eq 2) { return }
        $marks[$FeatureId] = 1
        foreach ($dependency in @($known[$FeatureId].dependencies))
        {
            Visit-FeatureDependency -FeatureId ([string]$dependency)
        }
        $marks[$FeatureId] = 2
    }
    foreach ($id in @($known.Keys)) { Visit-FeatureDependency -FeatureId ([string]$id) }

    $wip = @($active) + @($blocked)
    if ($wip.Count -gt 1)
    {
        throw "Only one active or blocked feature may exist per worktree"
    }
    $activeId = if ($null -eq $state.activeFeature) { "" } else { [string]$state.activeFeature }
    if ($active.Count -eq 1 -and $activeId -ne [string]$active[0].id)
    {
        throw "activeFeature must name '$($active[0].id)'"
    }
    if ($active.Count -eq 0 -and -not [string]::IsNullOrWhiteSpace($activeId))
    {
        throw "activeFeature is set but no feature has status active"
    }

    return [pscustomobject]@{
        Known = $known
        Active = @($active)
        Blocked = @($blocked)
        Passing = @($passing)
        ActiveId = if ([string]::IsNullOrWhiteSpace($activeId)) { $null } else { $activeId }
        Wip = @($wip)
    }
}

function Read-HandoffTarget
{
    param([string]$Path, [hashtable]$Known)

    $match = [regex]::Match(
        (Get-Content -LiteralPath $Path -Raw),
        "<!--\s*harness-handoff:\s*([A-Za-z0-9._-]+)\s*-->",
        [Text.RegularExpressions.RegexOptions]::IgnoreCase)
    if (-not $match.Success)
    {
        throw "session-handoff.md needs '<!-- harness-handoff: none|feature-id -->'"
    }
    $target = $match.Groups[1].Value
    if ($target -eq "none") { return $null }
    if (-not $Known.ContainsKey($target))
    {
        throw "session-handoff.md refers to unknown feature '$target'"
    }
    if ($Known[$target].status -notin @("active", "blocked"))
    {
        throw "session-handoff.md may refer only to active or blocked work"
    }
    return $target
}

function Assert-PresetExists
{
    param([string]$Preset, [string]$TestPreset)

    $configureLines = Invoke-NativeChecked "CMake preset discovery" "cmake" @("--list-presets=configure")
    if (($configureLines -join "`n") -notmatch ('"' + [regex]::Escape($Preset) + '"'))
    {
        throw "Configure preset '$Preset' is not declared"
    }
    $testLines = Invoke-NativeChecked "CTest test-preset discovery" "ctest" @("--list-presets")
    if (($testLines -join "`n") -notmatch ('"' + [regex]::Escape($TestPreset) + '"'))
    {
        throw "CTest test preset '$TestPreset' is not declared"
    }
}

function Assert-RequiredSuiteReport
{
    param(
        [Parameter(Mandatory)][string]$ReportPath,
        [Parameter(Mandatory)][string]$ResolvedBuildDir,
        [Parameter(Mandatory)][string]$Preset,
        [Parameter(Mandatory)][string]$TestPreset,
        [Parameter(Mandatory)][string]$Configuration,
        [Parameter(Mandatory)][string]$BaseRef,
        [Parameter(Mandatory)][string]$CurrentCommit
    )

    if (-not (Test-Path -LiteralPath $ReportPath -PathType Leaf))
    {
        throw "Architecture required-suite report is missing: $ReportPath"
    }
    try { $report = Get-Content -LiteralPath $ReportPath -Raw | ConvertFrom-Json }
    catch { throw "Architecture required-suite report is malformed: $($_.Exception.Message)" }

    $context = "Architecture required-suite report"
    if ([string](Get-RequiredJsonProperty $report "schema" $context) -ne "RVX.RequiredSuite.Report" -or
        [int](Get-RequiredJsonProperty $report "schemaVersion" $context) -ne 1)
    {
        throw "$context has an unsupported schema"
    }
    $runId = [string](Get-RequiredJsonProperty $report "runId" $context)
    if ($runId -notmatch "^[a-f0-9]{32}$") { throw "$context has an invalid runId" }
    if ([string](Get-RequiredJsonProperty $report "status" $context) -ne "passed")
    {
        throw "$context did not pass"
    }
    if ([string](Get-RequiredJsonProperty $report "sourceCommit" $context) -cne $CurrentCommit)
    {
        throw "$context sourceCommit does not match HEAD"
    }
    if ([string](Get-RequiredJsonProperty $report "baseRef" $context) -cne $BaseRef -or
        [string](Get-RequiredJsonProperty $report "configurePreset" $context) -cne $Preset -or
        [string](Get-RequiredJsonProperty $report "buildPreset" $context) -cne $Preset -or
        [string](Get-RequiredJsonProperty $report "testPreset" $context) -cne $TestPreset -or
        [string](Get-RequiredJsonProperty $report "configuration" $context) -cne $Configuration -or
        [string](Get-RequiredJsonProperty $report "requiredLabel" $context) -cne "lane.architecture")
    {
        throw "$context request context does not match this harness invocation"
    }
    $reportBuildDir = [IO.Path]::GetFullPath([string](Get-RequiredJsonProperty $report "buildDir" $context))
    if ($reportBuildDir -cne [IO.Path]::GetFullPath($ResolvedBuildDir))
    {
        throw "$context buildDir does not match this harness invocation"
    }

    $targets = @((Get-RequiredJsonArrayProperty $report "buildTargets" $context) | ForEach-Object { [string]$_ })
    if ($targets -notcontains "RVXValidationArchitecture")
    {
        throw "$context did not build RVXValidationArchitecture"
    }
    $selectedTests = @((Get-RequiredJsonArrayProperty $report "selectedTests" $context))
    if ($selectedTests.Count -le 0)
    {
        throw "$context selected zero tests"
    }

    $sourceBefore = Assert-Fingerprint `
        (Get-RequiredJsonProperty $report "sourceFingerprintBefore" $context) "$context sourceFingerprintBefore"
    $sourceAfter = Assert-Fingerprint `
        (Get-RequiredJsonProperty $report "sourceFingerprintAfter" $context) "$context sourceFingerprintAfter"
    Assert-FingerprintEqual $sourceBefore $sourceAfter "$context source"

    $junitPath = Get-ContainedAbsolutePath $ResolvedBuildDir `
        ([string](Get-RequiredJsonProperty $report "junitPath" $context)) "$context junitPath"
    if (-not (Test-Path -LiteralPath $junitPath -PathType Leaf))
    {
        throw "$context JUnit artifact is missing"
    }
    $junitHash = (Get-FileHash -LiteralPath $junitPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ([string](Get-RequiredJsonProperty $report "junitSha256" $context) -cne $junitHash)
    {
        throw "$context JUnit hash does not match its artifact"
    }
    $reportedJUnit = Get-RequiredJsonProperty $report "junit" $context
    if ($null -eq $reportedJUnit -or
        [IO.Path]::GetFullPath([string](Get-RequiredJsonProperty $reportedJUnit "path" "$context junit")) -cne $junitPath -or
        [string](Get-RequiredJsonProperty $reportedJUnit "sha256" "$context junit") -cne $junitHash)
    {
        throw "$context embedded JUnit evidence does not match its artifact"
    }
    $actualOutcome = Get-BuildTruthJUnitOutcome -Path $junitPath
    $reportedOutcome = Get-RequiredJsonProperty $reportedJUnit "outcome" "$context junit"
    foreach ($field in @("testCaseCount", "failureCount", "errorCount", "skippedCount", "notRunCount", "disabledSuiteCount"))
    {
        if ([int](Get-RequiredJsonProperty $reportedOutcome $field "$context JUnit outcome") -ne [int]$actualOutcome.$field)
        {
            throw "$context JUnit outcome '$field' does not match its artifact"
        }
    }
    if ([int]$actualOutcome.testCaseCount -ne $selectedTests.Count -or
        [int]$actualOutcome.failureCount -ne 0 -or [int]$actualOutcome.errorCount -ne 0 -or
        [int]$actualOutcome.skippedCount -ne 0 -or [int]$actualOutcome.notRunCount -ne 0 -or
        [int]$actualOutcome.disabledSuiteCount -ne 0)
    {
        throw "$context JUnit outcome is not a complete passing result"
    }

    return [pscustomobject]@{
        report = $report
        reportPath = $ReportPath
        reportSha256 = (Get-FileHash -LiteralPath $ReportPath -Algorithm SHA256).Hash.ToLowerInvariant()
        runId = $runId
        sourceFingerprint = $sourceAfter
        junitPath = $junitPath
        junitSha256 = $junitHash
        junitOutcome = $actualOutcome
        selectedTestCount = $selectedTests.Count
    }
}

function Write-HarnessImmutableJsonFile
{
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][object]$Value,
        [Parameter(Mandatory)][int]$Depth,
        [Parameter(Mandatory)][string]$Context
    )

    $directory = Split-Path -Parent $Path
    $temporaryPath = Join-Path $directory (
        "." + [IO.Path]::GetFileName($Path) + "." + [guid]::NewGuid().ToString("N") + ".tmp")
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes(
        (($Value | ConvertTo-Json -Depth $Depth) + [Environment]::NewLine))
    $hasher = [Security.Cryptography.SHA256]::Create()
    try
    {
        $sha256 = ([BitConverter]::ToString(
            $hasher.ComputeHash($bytes))).Replace("-", "").ToLowerInvariant()
    }
    finally
    {
        $hasher.Dispose()
    }

    try
    {
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
            [IO.File]::Move($temporaryPath, $Path)
        }
        catch
        {
            if (Test-Path -LiteralPath $Path)
            {
                throw "$Context already exists and is immutable: $Path"
            }
            throw
        }
    }
    finally
    {
        if (Test-Path -LiteralPath $temporaryPath)
        {
            Remove-Item -LiteralPath $temporaryPath -Force
        }
    }

    return [pscustomobject]@{ sha256 = $sha256; size = $bytes.Length }
}

function Write-HarnessAtomicJsonFile
{
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][object]$Value,
        [Parameter(Mandatory)][int]$Depth,
        [Parameter(Mandatory)][string]$Context
    )

    $directory = Split-Path -Parent $Path
    $fileName = [IO.Path]::GetFileName($Path)
    $temporaryPath = Join-Path $directory (
        ".$fileName." + [guid]::NewGuid().ToString("N") + ".tmp")
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes(
        (($Value | ConvertTo-Json -Depth $Depth) + [Environment]::NewLine))
    try
    {
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

        $overwriteMove = @([IO.File].GetMethods() | Where-Object {
            $_.Name -eq "Move" -and
            $_.GetParameters().Count -eq 3 -and
            $_.GetParameters()[2].ParameterType -eq [bool]
        } | Select-Object -First 1)
        if ($overwriteMove.Count -ne 0)
        {
            [IO.File]::Move($temporaryPath, $Path, $true)
        }
        elseif (Test-Path -LiteralPath $Path)
        {
            $backupPath = Join-Path $directory (
                ".$fileName." + [guid]::NewGuid().ToString("N") + ".bak")
            [IO.File]::Replace($temporaryPath, $Path, $backupPath)
            Remove-Item -LiteralPath $backupPath -Force -ErrorAction SilentlyContinue
        }
        else
        {
            try
            {
                [IO.File]::Move($temporaryPath, $Path)
            }
            catch [IO.IOException]
            {
                if (-not (Test-Path -LiteralPath $Path))
                {
                    throw
                }
                $backupPath = Join-Path $directory (
                    ".$fileName." + [guid]::NewGuid().ToString("N") + ".bak")
                [IO.File]::Replace($temporaryPath, $Path, $backupPath)
                Remove-Item -LiteralPath $backupPath -Force -ErrorAction SilentlyContinue
            }
        }
    }
    catch
    {
        throw "Unable to publish $Context atomically: $($_.Exception.Message)"
    }
    finally
    {
        if (Test-Path -LiteralPath $temporaryPath)
        {
            Remove-Item -LiteralPath $temporaryPath -Force
        }
    }
}

function New-ArchitectureReceipt
{
    param(
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][string]$ResolvedBuildDir,
        [Parameter(Mandatory)][object]$ReportInfo,
        [Parameter(Mandatory)][object]$ProductFingerprint,
        [Parameter(Mandatory)][string]$Preset,
        [Parameter(Mandatory)][string]$TestPreset,
        [Parameter(Mandatory)][string]$Configuration,
        [Parameter(Mandatory)][string]$BaseRef,
        [Parameter(Mandatory)][string]$CurrentCommit,
        [Parameter(Mandatory)][bool]$AllowConfigure
    )

    # Receipts are durable repository evidence. BuildTruth remains the source
    # for runner output and the short-lived Verify lock only.
    $runnerFingerprint = Assert-Fingerprint `
        $ReportInfo.sourceFingerprint "Architecture runner source fingerprint"
    $validatedProductFingerprint = Assert-Fingerprint `
        $ProductFingerprint "Architecture product source fingerprint"
    Assert-FingerprintEqual $runnerFingerprint $validatedProductFingerprint `
        "Architecture runner and product source"
    $receiptDir = Join-Path $Root ".agent-harness\receipts"
    $receiptPath = Join-Path $receiptDir "Architecture.$($ReportInfo.runId).json"
    $latestPath = Join-Path $receiptDir "Architecture.latest.json"
    if (Test-Path -LiteralPath $receiptPath -PathType Leaf)
    {
        throw "Architecture receipt already exists and is immutable: $receiptPath"
    }
    [void](Resolve-BuildTruthContainedPath -Root $Root -Path $receiptPath -AllowMissing)
    New-Item -ItemType Directory -Force -Path $receiptDir | Out-Null
    [void](Resolve-BuildTruthContainedPath -Root $Root -Path $receiptDir)

    $receiptRelative = Assert-CanonicalArchitectureReceiptPath -Root $Root -RelativePath (Get-RepositoryRelativePath -Root $Root -Path $receiptPath -Context "New Architecture receipt") -RunId $ReportInfo.runId -Context "New Architecture receipt"
    $reportRelative = Get-RepositoryRelativePath -Root $Root -Path $ReportInfo.reportPath -Context "Architecture report path"
    $junitRelative = Get-RepositoryRelativePath -Root $Root -Path $ReportInfo.junitPath -Context "Architecture JUnit path"
    $receipt = [ordered]@{
        schema = "RVX.AgentHarness.ArchitectureReceipt"
        schemaVersion = 2
        generatedAtUtc = (Get-Date).ToUniversalTime().ToString("o")
        runId = $ReportInfo.runId
        immutableReceiptPath = $receiptRelative
        request = [ordered]@{
            preset = $Preset
            testPreset = $TestPreset
            configuration = $Configuration
            buildDir = Get-RepositoryRelativePath -Root $Root -Path $ResolvedBuildDir -Context "Architecture build directory"
            baseRef = $BaseRef
            buildTarget = "RVXValidationArchitecture"
            requiredLabel = "lane.architecture"
            allowConfigure = $AllowConfigure
        }
        source = [ordered]@{
            sourceCommit = $CurrentCommit
            productFingerprint = $validatedProductFingerprint
            runnerFingerprint = $runnerFingerprint
        }
        report = [ordered]@{
            path = $reportRelative
            sha256 = $ReportInfo.reportSha256
        }
        junit = [ordered]@{
            path = $junitRelative
            sha256 = $ReportInfo.junitSha256
            outcome = $ReportInfo.junitOutcome
        }
        selectedTestCount = $ReportInfo.selectedTestCount
    }
    $receiptWrite = Write-HarnessImmutableJsonFile -Path $receiptPath `
        -Value $receipt -Depth 16 -Context "Architecture receipt"
    $receiptSha256 = $receiptWrite.sha256

    # The mutable latest pointer only selects an immutable, hash-pinned receipt.
    $latest = [ordered]@{
        schema = "RVX.AgentHarness.ArchitectureReceiptPointer"
        schemaVersion = 1
        generatedAtUtc = (Get-Date).ToUniversalTime().ToString("o")
        runId = $ReportInfo.runId
        receiptPath = $receiptRelative
        receiptSha256 = $receiptSha256
    }
    Write-HarnessAtomicJsonFile -Path $latestPath -Value $latest -Depth 8 `
        -Context "Architecture latest receipt pointer"

    return [pscustomobject]@{
        receiptPath = $receiptRelative
        receiptSha256 = $receiptSha256
        runId = $ReportInfo.runId
    }
}

function Read-VerificationState
{
    param(
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][string]$ResolvedBuildDir,
        [Parameter(Mandatory)][string]$Preset,
        [Parameter(Mandatory)][string]$TestPreset,
        [Parameter(Mandatory)][string]$Configuration,
        [Parameter(Mandatory)][string]$BaseRef,
        [Parameter(Mandatory)][bool]$AllowConfigure,
        [Parameter(Mandatory)][string]$CurrentCommit,
        [Parameter(Mandatory)][object]$CurrentProductFingerprint
    )

    $receiptDirectory = Join-Path $Root ".agent-harness\receipts"
    $latestPath = Join-Path $receiptDirectory "Architecture.latest.json"
    if (-not (Test-Path -LiteralPath $latestPath -PathType Leaf))
    {
        return [pscustomobject]@{
            state = "missing"; reason = "No latest Architecture receipt exists"
            receiptPath = $null; receiptSha256 = $null; runId = $null
            reportPath = $null; reportSha256 = $null; sourceCommit = $null
            sourceDigest = $null; currentDigest = $CurrentProductFingerprint.digest
            selectedTestCount = 0; recordedAt = $null
        }
    }

    try
    {
        $safeLatestPath = Resolve-BuildTruthContainedPath -Root $Root -Path $latestPath
        $pointer = Get-Content -LiteralPath $safeLatestPath -Raw | ConvertFrom-Json
        $pointerContext = "Architecture latest receipt pointer"
        if ([string](Get-RequiredJsonProperty $pointer "schema" $pointerContext) -ne
                "RVX.AgentHarness.ArchitectureReceiptPointer" -or
            [int](Get-RequiredJsonProperty $pointer "schemaVersion" $pointerContext) -ne 1)
        {
            throw "$pointerContext has an unsupported schema"
        }
        $pointerRunId = [string](Get-RequiredJsonProperty $pointer "runId" $pointerContext)
        if ($pointerRunId -notmatch "^[a-f0-9]{32}$") { throw "$pointerContext has an invalid runId" }
        $receiptRelative = Assert-CanonicalArchitectureReceiptPath -Root $Root -RelativePath ([string](Get-RequiredJsonProperty $pointer "receiptPath" $pointerContext)) -RunId $pointerRunId -Context "$pointerContext receiptPath"
        $receiptPath = Join-Path $Root $receiptRelative
        [void](Get-ContainedAbsolutePath $receiptDirectory $receiptPath "$pointerContext receiptPath")
        $receiptPath = Resolve-BuildTruthContainedPath -Root $Root -Path $receiptPath
        if (-not (Test-Path -LiteralPath $receiptPath -PathType Leaf))
        {
            throw "$pointerContext refers to a missing immutable receipt"
        }
        $receiptBytes = [IO.File]::ReadAllBytes($receiptPath)
        $receiptHasher = [Security.Cryptography.SHA256]::Create()
        try
        {
            $receiptSha256 = ([BitConverter]::ToString(
                $receiptHasher.ComputeHash($receiptBytes))).Replace("-", "").ToLowerInvariant()
        }
        finally
        {
            $receiptHasher.Dispose()
        }
        if ([string](Get-RequiredJsonProperty $pointer "receiptSha256" $pointerContext) -cne $receiptSha256)
        {
            throw "$pointerContext receipt hash does not match its immutable receipt"
        }

        try
        {
            $receiptText = [Text.UTF8Encoding]::new($false, $true).GetString($receiptBytes)
            $receipt = $receiptText | ConvertFrom-Json
        }
        catch
        {
            throw "Architecture immutable receipt is not strict UTF-8 JSON: $($_.Exception.Message)"
        }
        $receiptContext = "Architecture immutable receipt"
        if ([string](Get-RequiredJsonProperty $receipt "schema" $receiptContext) -ne
                "RVX.AgentHarness.ArchitectureReceipt" -or
            [int](Get-RequiredJsonProperty $receipt "schemaVersion" $receiptContext) -ne 2 -or
            [string](Get-RequiredJsonProperty $receipt "runId" $receiptContext) -cne $pointerRunId -or
            (Assert-CanonicalArchitectureReceiptPath -Root $Root -RelativePath ([string](Get-RequiredJsonProperty $receipt "immutableReceiptPath" $receiptContext)) -RunId $pointerRunId -Context "$receiptContext immutableReceiptPath") -cne $receiptRelative)
        {
            throw "$receiptContext identity does not match the latest pointer"
        }

        $request = Get-RequiredJsonProperty $receipt "request" $receiptContext
        $receiptAllowConfigure = Get-RequiredJsonProperty `
            $request "allowConfigure" "$receiptContext request"
        if ([string](Get-RequiredJsonProperty $request "preset" "$receiptContext request") -cne $Preset -or
            [string](Get-RequiredJsonProperty $request "testPreset" "$receiptContext request") -cne $TestPreset -or
            [string](Get-RequiredJsonProperty $request "configuration" "$receiptContext request") -cne $Configuration -or
            [string](Get-RequiredJsonProperty $request "baseRef" "$receiptContext request") -cne $BaseRef -or
            [string](Get-RequiredJsonProperty $request "buildTarget" "$receiptContext request") -cne "RVXValidationArchitecture" -or
            [string](Get-RequiredJsonProperty $request "requiredLabel" "$receiptContext request") -cne "lane.architecture" -or
            $receiptAllowConfigure -isnot [bool] -or
            $receiptAllowConfigure -ne $AllowConfigure -or
            (Get-NormalizedRelativePath $Root ([string](Get-RequiredJsonProperty $request "buildDir" "$receiptContext request")) "$receiptContext request buildDir") -cne
                (Get-RepositoryRelativePath -Root $Root -Path $ResolvedBuildDir -Context "$receiptContext build directory"))
        {
            throw "$receiptContext request context does not match this harness invocation"
        }

        $receiptReport = Get-RequiredJsonProperty $receipt "report" $receiptContext
        $expectedReportPath = Get-RepositoryRelativePath -Root $Root -Path (Join-Path $ResolvedBuildDir "BuildTruth\RequiredSuite.lane.architecture.json") -Context "$receiptContext expected report"
        $reportPath = Get-NormalizedRelativePath $Root ([string](Get-RequiredJsonProperty $receiptReport "path" "$receiptContext report")) "$receiptContext report path"
        $reportSha256 = [string](Get-RequiredJsonProperty $receiptReport "sha256" "$receiptContext report")
        if ($reportPath -cne $expectedReportPath -or $reportSha256 -notmatch "^[a-f0-9]{64}$")
        {
            throw "$receiptContext report identity is invalid"
        }

        $source = Get-RequiredJsonProperty $receipt "source" $receiptContext
        if ([string](Get-RequiredJsonProperty $source "sourceCommit" "$receiptContext source") -cne $CurrentCommit)
        {
            throw "$receiptContext source commit does not match HEAD"
        }
        $receiptProductFingerprint = Assert-Fingerprint `
            (Get-RequiredJsonProperty $source "productFingerprint" "$receiptContext source") "$receiptContext product fingerprint"
        Assert-FingerprintEqual $receiptProductFingerprint $CurrentProductFingerprint "$receiptContext current product"
        $receiptRunnerFingerprint = Assert-Fingerprint `
            (Get-RequiredJsonProperty $source "runnerFingerprint" "$receiptContext source") "$receiptContext runner fingerprint"
        Assert-FingerprintEqual $receiptRunnerFingerprint $receiptProductFingerprint `
            "$receiptContext runner and product source"

        $receiptJUnit = Get-RequiredJsonProperty $receipt "junit" $receiptContext
        $expectedJUnitPath = Get-RepositoryRelativePath -Root $Root -Path (Join-Path $ResolvedBuildDir "BuildTruth\JUnit.lane.architecture.xml") -Context "$receiptContext expected JUnit"
        $junitPath = Get-NormalizedRelativePath $Root ([string](Get-RequiredJsonProperty $receiptJUnit "path" "$receiptContext JUnit")) "$receiptContext JUnit path"
        $junitSha256 = [string](Get-RequiredJsonProperty $receiptJUnit "sha256" "$receiptContext JUnit")
        $selectedTestCount = [int](Get-RequiredJsonProperty $receipt "selectedTestCount" $receiptContext)
        if ($junitPath -cne $expectedJUnitPath -or $junitSha256 -notmatch "^[a-f0-9]{64}$" -or $selectedTestCount -le 0)
        {
            throw "$receiptContext JUnit identity is invalid"
        }
        $receiptOutcome = Get-RequiredJsonProperty $receiptJUnit "outcome" "$receiptContext JUnit"
        if ([int](Get-RequiredJsonProperty $receiptOutcome "testCaseCount" "$receiptContext JUnit outcome") -ne $selectedTestCount)
        {
            throw "$receiptContext JUnit test count does not match selected tests"
        }
        foreach ($field in @("failureCount", "errorCount", "skippedCount", "notRunCount", "disabledSuiteCount"))
        {
            if ([int](Get-RequiredJsonProperty $receiptOutcome $field "$receiptContext JUnit outcome") -ne 0)
            {
                throw "$receiptContext JUnit outcome '$field' is not zero"
            }
        }

        return [pscustomobject]@{
            state = "verified"; reason = $null; receiptPath = $receiptRelative
            receiptSha256 = $receiptSha256; runId = $pointerRunId
            reportPath = $reportPath
            reportSha256 = $reportSha256; sourceCommit = $CurrentCommit
            sourceDigest = $receiptProductFingerprint.digest
            currentDigest = $CurrentProductFingerprint.digest
            selectedTestCount = $selectedTestCount
            recordedAt = ConvertTo-CanonicalUtcTimestamp `
                (Get-RequiredJsonProperty $receipt "generatedAtUtc" $receiptContext) `
                "$receiptContext generatedAtUtc"
        }
    }
    catch
    {
        $reason = $_.Exception.Message
        $state = if ($reason -match "request context|buildDir does not match") {
            "context_mismatch"
        } elseif ($reason -match "sourceCommit does not match|source commit does not match") {
            "stale_commit"
        } elseif ($reason -match "current product.*fingerprints do not match") {
            "stale_worktree"
        } elseif ($reason -match "report (hash|identity)") {
            "stale_report"
        } else {
            "failed"
        }
        return [pscustomobject]@{
            state = $state; reason = $reason
            receiptPath = $null; receiptSha256 = $null; runId = $null
            reportPath = $null; reportSha256 = $null; sourceCommit = $null
            sourceDigest = $null; currentDigest = $CurrentProductFingerprint.digest
            selectedTestCount = 0; recordedAt = (Get-Item -LiteralPath $latestPath).LastWriteTimeUtc.ToString("o")
        }
    }
}

function Assert-ArchitectureEvidenceReceipt
{
    param(
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][string]$ArtifactRelative,
        [Parameter(Mandatory)][object]$Receipt,
        [Parameter(Mandatory)][object]$Record,
        [Parameter(Mandatory)][object]$Gate
    )

    $receipt = $Receipt

    $context = "Evidence receipt '$ArtifactRelative'"
    if ([string](Get-RequiredJsonProperty $receipt "schema" $context) -ne
            "RVX.AgentHarness.ArchitectureReceipt" -or
        [int](Get-RequiredJsonProperty $receipt "schemaVersion" $context) -ne 2)
    {
        throw "$context has an unsupported schema"
    }
    $runId = [string](Get-RequiredJsonProperty $receipt "runId" $context)
    if ($runId -notmatch "^[a-f0-9]{32}$" -or
        $runId -cne [string](Get-OptionalJsonProperty $Record "runId"))
    {
        throw "$context runId does not match the evidence record"
    }
    $declaredPath = Assert-CanonicalArchitectureReceiptPath -Root $Root -RelativePath ([string](Get-RequiredJsonProperty $receipt "immutableReceiptPath" $context)) -RunId $runId -Context "$context immutableReceiptPath"
    if ($declaredPath -cne $ArtifactRelative)
    {
        throw "$context path does not match its immutableReceiptPath"
    }
    $generatedAtUtc = ConvertTo-CanonicalUtcTimestamp `
        (Get-RequiredJsonProperty $receipt "generatedAtUtc" $context) `
        "$context generatedAtUtc"
    $recordedAtUtc = ConvertTo-CanonicalUtcTimestamp `
        (Get-RequiredJsonProperty $Record "recordedAt" "$context evidence record") `
        "$context evidence recordedAt"
    if ($generatedAtUtc -cne $recordedAtUtc)
    {
        throw "$context generatedAtUtc does not match the evidence record"
    }

    $source = Get-RequiredJsonProperty $receipt "source" $context
    $sourceCommit = [string](Get-RequiredJsonProperty $source "sourceCommit" "$context source")
    $productFingerprint = Assert-Fingerprint `
        (Get-RequiredJsonProperty $source "productFingerprint" "$context source") `
        "$context product fingerprint"
    $runnerFingerprint = Assert-Fingerprint `
        (Get-RequiredJsonProperty $source "runnerFingerprint" "$context source") `
        "$context runner fingerprint"
    Assert-FingerprintEqual $runnerFingerprint $productFingerprint `
        "$context runner and product source"
    if ($sourceCommit -cne [string](Get-OptionalJsonProperty $Record "sourceCommit") -or
        $productFingerprint.digest -cne [string](Get-OptionalJsonProperty $Record "sourceDigest"))
    {
        throw "$context source identity does not match the evidence record"
    }

    $request = Get-RequiredJsonProperty $receipt "request" $context
    $expectation = Get-ReceiptExpectation -Gate $Gate -Kind Architecture -Context $context
    foreach ($field in @("preset", "testPreset", "configuration", "buildDir", "baseRef"))
    {
        if ([string]::IsNullOrWhiteSpace(
                [string](Get-RequiredJsonProperty $request $field "$context request")))
        {
            throw "$context request '$field' is empty"
        }
    }
    if ([string](Get-RequiredJsonProperty $request "buildTarget" "$context request") -cne
            "RVXValidationArchitecture" -or
        [string](Get-RequiredJsonProperty $request "requiredLabel" "$context request") -cne
            "lane.architecture")
    {
        throw "$context is not an Architecture verification receipt"
    }
    foreach ($field in @("preset", "testPreset", "configuration", "baseRef", "buildTarget", "requiredLabel", "allowConfigure"))
    {
        Assert-ReceiptExpectedScalar -Request $request -Expectation $expectation `
            -Field $field -Context $context
    }
    $buildDirRelative = Assert-ReceiptExpectedBuildDirectory -Root $Root -Request $request `
        -Expectation $expectation -Context $context

    $report = Get-RequiredJsonProperty $receipt "report" $context
    $validatedReport = Assert-ReceiptFileIdentity -Root $Root -Identity $report `
        -BuildDirRelative $buildDirRelative -Context "$context report"
    $expectedReportPath = "$buildDirRelative/BuildTruth/RequiredSuite.lane.architecture.json"
    if ($validatedReport.path -cne $expectedReportPath)
    {
        throw "$context report path does not match the Architecture suite contract"
    }

    $junit = Get-RequiredJsonProperty $receipt "junit" $context
    $validatedJUnit = Assert-ReceiptFileIdentity -Root $Root -Identity $junit `
        -BuildDirRelative $buildDirRelative -Context "$context JUnit"
    $expectedJUnitPath = "$buildDirRelative/BuildTruth/JUnit.lane.architecture.xml"
    if ($validatedJUnit.path -cne $expectedJUnitPath)
    {
        throw "$context JUnit path does not match the Architecture suite contract"
    }
    $selectedTestCount = [int](Get-RequiredJsonProperty $receipt "selectedTestCount" $context)
    $outcome = Get-RequiredJsonProperty $junit "outcome" "$context JUnit"
    if ($selectedTestCount -le 0 -or
        [int](Get-RequiredJsonProperty $outcome "testCaseCount" "$context JUnit outcome") -ne
            $selectedTestCount)
    {
        throw "$context has an invalid selected test count"
    }
    foreach ($field in @("failureCount", "errorCount", "skippedCount", "notRunCount", "disabledSuiteCount"))
    {
        if ([int](Get-RequiredJsonProperty $outcome $field "$context JUnit outcome") -ne 0)
        {
            throw "$context JUnit outcome '$field' is not zero"
        }
    }
}

function Assert-PassingReceiptJUnitOutcome
{
    param(
        [Parameter(Mandatory)][object]$Outcome,
        [Parameter(Mandatory)][int]$ExpectedTestCount,
        [Parameter(Mandatory)][string]$Context
    )

    if ($ExpectedTestCount -le 0 -or
        [int](Get-RequiredJsonProperty $Outcome "testCaseCount" $Context) -ne
            $ExpectedTestCount)
    {
        throw "$Context does not match the selected test count"
    }
    foreach ($field in @("failureCount", "errorCount", "skippedCount", "notRunCount", "disabledSuiteCount"))
    {
        if ([int](Get-RequiredJsonProperty $Outcome $field $Context) -ne 0)
        {
            throw "$Context '$field' is not zero"
        }
    }
}

function Get-ReceiptExpectation
{
    param(
        [Parameter(Mandatory)][object]$Gate,
        [Parameter(Mandatory)][ValidateSet("Architecture", "BuildTruth", "RequiredSuite")][string]$Kind,
        [Parameter(Mandatory)][string]$Context
    )

    $expectation = Get-OptionalJsonProperty $Gate "expectedReceipt"
    if ($null -eq $expectation -or
        [string](Get-RequiredJsonProperty $expectation "kind" "$Context expectation") -cne $Kind)
    {
        throw "$Context must declare an expectedReceipt of kind '$Kind'"
    }
    return $expectation
}

function Assert-ReceiptExpectedScalar
{
    param(
        [Parameter(Mandatory)][object]$Request,
        [Parameter(Mandatory)][object]$Expectation,
        [Parameter(Mandatory)][string]$Field,
        [Parameter(Mandatory)][string]$Context
    )

    $actual = Get-RequiredJsonProperty $Request $Field "$Context request"
    $expected = Get-RequiredJsonProperty $Expectation $Field "$Context expectation"
    if ($null -eq $expected)
    {
        if ($null -ne $actual)
        {
            throw "$Context request '$Field' does not match its gate expectation"
        }
        return
    }
    if ($expected -is [bool])
    {
        if ($actual -isnot [bool] -or $actual -ne $expected)
        {
            throw "$Context request '$Field' does not match its gate expectation"
        }
        return
    }
    if ($expected -is [byte] -or $expected -is [sbyte] -or
        $expected -is [int16] -or $expected -is [uint16] -or
        $expected -is [int32] -or $expected -is [uint32] -or
        $expected -is [int64] -or $expected -is [uint64])
    {
        $actualIsInteger = $actual -is [byte] -or $actual -is [sbyte] -or
            $actual -is [int16] -or $actual -is [uint16] -or
            $actual -is [int32] -or $actual -is [uint32] -or
            $actual -is [int64] -or $actual -is [uint64]
        if (-not $actualIsInteger -or [decimal]$actual -ne [decimal]$expected)
        {
            throw "$Context request '$Field' does not match its gate expectation"
        }
        return
    }
    if ($expected -isnot [string] -or $actual -isnot [string] -or
        $actual -cne $expected)
    {
        throw "$Context request '$Field' does not match its gate expectation"
    }
}

function Assert-ReceiptExpectedStringSequence
{
    param(
        [Parameter(Mandatory)][object[]]$Actual,
        [Parameter(Mandatory)][object[]]$Expected,
        [Parameter(Mandatory)][string]$Context
    )

    if ($Actual.Count -ne $Expected.Count)
    {
        throw "$Context count does not match its gate expectation"
    }
    for ($index = 0; $index -lt $Actual.Count; ++$index)
    {
        if ($Actual[$index] -isnot [string] -or
            $Expected[$index] -isnot [string] -or
            $Actual[$index] -cne $Expected[$index])
        {
            throw "$Context does not match its gate expectation"
        }
    }
}

function Assert-ReceiptExpectedBuildDirectory
{
    param(
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][object]$Request,
        [Parameter(Mandatory)][object]$Expectation,
        [Parameter(Mandatory)][string]$Context
    )

    $actual = Get-NormalizedRelativePath $Root `
        ([string](Get-RequiredJsonProperty $Request "buildDir" "$Context request")) `
        "$Context request buildDir"
    $expected = Get-NormalizedRelativePath $Root `
        ([string](Get-RequiredJsonProperty $Expectation "buildDir" "$Context expectation")) `
        "$Context expectation buildDir"
    if ($actual -cne $expected)
    {
        throw "$Context request 'buildDir' does not match its gate expectation"
    }
    return $actual
}

function Assert-ReceiptFileIdentity
{
    param(
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][object]$Identity,
        [Parameter(Mandatory)][string]$BuildDirRelative,
        [Parameter(Mandatory)][string]$Context
    )

    $path = Get-NormalizedRelativePath $Root `
        ([string](Get-RequiredJsonProperty $Identity "path" $Context)) "$Context path"
    $buildPrefix = $BuildDirRelative.TrimEnd("/") + "/"
    if (-not $path.StartsWith($buildPrefix, (Get-PathStringComparison)))
    {
        throw "$Context path must remain below '$BuildDirRelative'"
    }
    $sha256 = [string](Get-RequiredJsonProperty $Identity "sha256" $Context)
    if ($sha256 -notmatch "^[a-f0-9]{64}$")
    {
        throw "$Context hash is invalid"
    }
    return [pscustomobject]@{ path = $path; sha256 = $sha256 }
}

function Assert-ReceiptRunnerDigest
{
    param(
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][object]$RunnerDigest,
        [Parameter(Mandatory)][string[]]$ExpectedPaths,
        [Parameter(Mandatory)][string]$Context
    )

    try
    {
        Assert-BuildTruthRunnerDigest `
            -RunnerDigest $RunnerDigest `
            -ExpectedPaths $ExpectedPaths
    }
    catch
    {
        throw "$Context $($_.Exception.Message)"
    }
}

function Assert-RunEvidenceReceiptIdentity
{
    param(
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][string]$ArtifactRelative,
        [Parameter(Mandatory)][object]$Receipt,
        [Parameter(Mandatory)][object]$Record,
        [Parameter(Mandatory)][ValidateSet("BuildTruth", "RequiredSuite")][string]$ReceiptKind,
        [Parameter(Mandatory)][string]$ExpectedSchema,
        [Parameter(Mandatory)][string]$Context
    )

    $schemaVersion = [int](Get-RequiredJsonProperty $Receipt "schemaVersion" $Context)
    if ([string](Get-RequiredJsonProperty $Receipt "schema" $Context) -cne $ExpectedSchema -or
        $schemaVersion -notin @(2, 3))
    {
        throw "$Context has an unsupported schema"
    }
    $runId = [string](Get-RequiredJsonProperty $Receipt "runId" $Context)
    if ($runId -notmatch "^[a-f0-9]{32}$" -or
        $runId -cne [string](Get-OptionalJsonProperty $Record "runId"))
    {
        throw "$Context runId does not match the evidence record"
    }
    $declaredPath = Assert-CanonicalValidationReceiptPath -Root $Root `
        -RelativePath ([string](Get-RequiredJsonProperty $Receipt "immutableReceiptPath" $Context)) `
        -RunId $runId -ReceiptKind $ReceiptKind -Context "$Context immutableReceiptPath"
    if ($declaredPath -cne $ArtifactRelative)
    {
        throw "$Context path does not match its immutableReceiptPath"
    }
    $generatedAtUtc = ConvertTo-CanonicalUtcTimestamp `
        (Get-RequiredJsonProperty $Receipt "generatedAtUtc" $Context) "$Context generatedAtUtc"
    $recordedAtUtc = ConvertTo-CanonicalUtcTimestamp `
        (Get-RequiredJsonProperty $Record "recordedAt" "$Context evidence record") `
        "$Context evidence recordedAt"
    if ($generatedAtUtc -cne $recordedAtUtc)
    {
        throw "$Context generatedAtUtc does not match the evidence record"
    }

    $source = Get-RequiredJsonProperty $Receipt "source" $Context
    $sourceCommit = [string](Get-RequiredJsonProperty $source "commit" "$Context source")
    $fingerprintBefore = Assert-Fingerprint `
        (Get-RequiredJsonProperty $source "fingerprintBefore" "$Context source") `
        "$Context source fingerprintBefore"
    $fingerprintAfter = Assert-Fingerprint `
        (Get-RequiredJsonProperty $source "fingerprintAfter" "$Context source") `
        "$Context source fingerprintAfter"
    Assert-FingerprintEqual $fingerprintBefore $fingerprintAfter "$Context source"
    if ($sourceCommit -cne [string](Get-OptionalJsonProperty $Record "sourceCommit") -or
        $fingerprintAfter.digest -cne [string](Get-OptionalJsonProperty $Record "sourceDigest"))
    {
        throw "$Context source identity does not match the evidence record"
    }

    return [pscustomobject]@{
        runId = $runId
        schemaVersion = $schemaVersion
        sourceCommit = $sourceCommit
        sourceDigest = $fingerprintAfter.digest
    }
}

function Assert-BuildTruthEvidenceReceipt
{
    param(
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][string]$ArtifactRelative,
        [Parameter(Mandatory)][object]$Receipt,
        [Parameter(Mandatory)][object]$Record,
        [Parameter(Mandatory)][object]$Gate
    )

    $receipt = $Receipt
    $context = "Evidence receipt '$ArtifactRelative'"
    $receiptIdentity = Assert-RunEvidenceReceiptIdentity -Root $Root -ArtifactRelative $ArtifactRelative `
        -Receipt $receipt -Record $Record -ReceiptKind BuildTruth `
        -ExpectedSchema "RVX.AgentHarness.BuildTruthReceipt" -Context $context

    $request = Get-RequiredJsonProperty $receipt "request" $context
    $expectation = Get-ReceiptExpectation -Gate $Gate -Kind BuildTruth -Context $context
    foreach ($field in @("baseRef", "configurePreset", "buildPreset", "buildDir", "configuration"))
    {
        if ([string]::IsNullOrWhiteSpace([string](Get-RequiredJsonProperty $request $field "$context request")))
        {
            throw "$context request '$field' is empty"
        }
    }
    foreach ($field in @("baseRef", "configurePreset", "buildPreset", "configuration", "requiredNativeBackend", "softwareAdapterAllowed", "fresh"))
    {
        Assert-ReceiptExpectedScalar -Request $request -Expectation $expectation `
            -Field $field -Context $context
    }
    $buildDirRelative = Assert-ReceiptExpectedBuildDirectory -Root $Root `
        -Request $request -Expectation $expectation -Context $context
    Assert-ReceiptRunnerDigest -Root $Root `
        -RunnerDigest (Get-RequiredJsonProperty $request "runnerDigest" "$context request") `
        -ExpectedPaths (Get-BuildTruthReceiptRunnerPaths -ReceiptKind BuildTruth -SchemaVersion $receiptIdentity.schemaVersion) `
        -Context "$context runner digest"
    $laneTestPresets = @(Get-RequiredJsonArrayProperty $request "laneTestPresets" "$context request")
    if ($laneTestPresets.Count -le 0) { throw "$context request has no lane test presets" }
    Assert-ReceiptExpectedStringSequence -Actual $laneTestPresets `
        -Expected @(Get-RequiredJsonArrayProperty $expectation "laneTestPresets" "$context expectation") `
        -Context "$context lane test presets"
    $unmatchedLanePresets = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($laneTestPreset in $laneTestPresets)
    {
        if ([string]::IsNullOrWhiteSpace([string]$laneTestPreset) -or
            -not $unmatchedLanePresets.Add([string]$laneTestPreset))
        {
            throw "$context request has an empty or duplicate lane test preset"
        }
    }
    $validatedReport = Assert-ReceiptFileIdentity -Root $Root `
        -Identity (Get-RequiredJsonProperty $receipt "report" $context) `
        -BuildDirRelative $buildDirRelative -Context "$context report"
    if ($validatedReport.path -cne "$buildDirRelative/BuildTruth/BuildTruth.json")
    {
        throw "$context report path does not match the Build Truth contract"
    }

    $lanes = @(Get-RequiredJsonArrayProperty $receipt "lanes" $context)
    $junit = Get-RequiredJsonProperty $receipt "junit" $context
    $junitLanes = @(Get-RequiredJsonArrayProperty $junit "lanes" "$context JUnit")
    if ($lanes.Count -le 0 -or $lanes.Count -ne $junitLanes.Count -or
        $lanes.Count -ne $laneTestPresets.Count)
    {
        throw "$context lane inventory is incomplete"
    }
    $seenLanes = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($lane in $lanes)
    {
        $laneName = [string](Get-RequiredJsonProperty $lane "lane" "$context lane")
        $testPreset = [string](Get-RequiredJsonProperty $lane "testPreset" "$context lane '$laneName'")
        $selectedCount = [int](Get-RequiredJsonProperty $lane "selectedTestCount" "$context lane '$laneName'")
        if ([string]::IsNullOrWhiteSpace($laneName) -or [string]::IsNullOrWhiteSpace($testPreset) -or
            -not $seenLanes.Add($laneName))
        {
            throw "$context has an invalid or duplicate lane"
        }
        if ($laneName -notin @("architecture", "unit", "integration", "native", "qualification", "shipping") -or
            -not $unmatchedLanePresets.Remove($testPreset))
        {
            throw "$context lane '$laneName' does not match the requested lane preset inventory"
        }
        $validatedInventory = Assert-ReceiptFileIdentity -Root $Root `
            -Identity (Get-RequiredJsonProperty $lane "inventory" "$context lane '$laneName'") `
            -BuildDirRelative $buildDirRelative -Context "$context lane '$laneName' inventory"
        if ($validatedInventory.path -cne "$buildDirRelative/BuildTruth/CTestInventory.$laneName.json")
        {
            throw "$context lane '$laneName' inventory path is not canonical"
        }

        $matchingJUnit = @($junitLanes | Where-Object {
            [string]$_.lane -ceq $laneName -and [string]$_.testPreset -ceq $testPreset
        })
        if ($matchingJUnit.Count -ne 1)
        {
            throw "$context lane '$laneName' does not have exactly one JUnit identity"
        }
        $validatedJUnit = Assert-ReceiptFileIdentity -Root $Root -Identity $matchingJUnit[0] `
            -BuildDirRelative $buildDirRelative -Context "$context JUnit lane '$laneName'"
        if ($validatedJUnit.path -cne "$buildDirRelative/BuildTruth/JUnit.$laneName.xml" -or
            [int](Get-RequiredJsonProperty $matchingJUnit[0] "selectedTestCount" "$context JUnit lane '$laneName'") -ne $selectedCount)
        {
            throw "$context lane '$laneName' JUnit identity is inconsistent"
        }
        Assert-PassingReceiptJUnitOutcome `
            -Outcome (Get-RequiredJsonProperty $matchingJUnit[0] "outcome" "$context JUnit lane '$laneName'") `
            -ExpectedTestCount $selectedCount -Context "$context JUnit lane '$laneName' outcome"
    }
    if ($unmatchedLanePresets.Count -ne 0)
    {
        throw "$context does not contain evidence for every requested lane preset"
    }

    $requiredNativeBackend = [string](Get-OptionalJsonProperty $request "requiredNativeBackend")
    $nativeEvidence = Get-OptionalJsonProperty $receipt "nativeEvidence"
    if (-not [string]::IsNullOrWhiteSpace($requiredNativeBackend))
    {
        if ($null -eq $nativeEvidence) { throw "$context is missing required native evidence" }
        if ([string](Get-RequiredJsonProperty $nativeEvidence "requestedBackend" "$context native evidence") -cne $requiredNativeBackend -or
            [string](Get-RequiredJsonProperty $nativeEvidence "actualBackend" "$context native evidence") -cne $requiredNativeBackend)
        {
            throw "$context native backend identity is inconsistent"
        }
        foreach ($identityField in @("raw", "normalized"))
        {
            $validatedNativeIdentity = Assert-ReceiptFileIdentity -Root $Root `
                -Identity (Get-RequiredJsonProperty $nativeEvidence $identityField "$context native evidence") `
                -BuildDirRelative $buildDirRelative -Context "$context native evidence $identityField"
            $expectedNativeFile = if ($identityField -eq "raw") {
                "NativeRenderLifecycle.raw.json"
            } else {
                "NativeEvidence.json"
            }
            if ($validatedNativeIdentity.path -cne "$buildDirRelative/BuildTruth/$expectedNativeFile")
            {
                throw "$context native evidence '$identityField' path is not canonical"
            }
        }
        $fallback = Get-RequiredJsonProperty $nativeEvidence "fallback" "$context native evidence"
        foreach ($field in @("backendFallback", "softwareAdapter"))
        {
            $fallbackValue = Get-RequiredJsonProperty $fallback $field "$context native fallback"
            if ($fallbackValue -isnot [bool])
            {
                throw "$context native fallback '$field' is not a Boolean"
            }
            if ($fallbackValue)
            {
                throw "$context native fallback '$field' is enabled"
            }
        }
        $softwareAdapterAllowed = Get-RequiredJsonProperty `
            $fallback "softwareAdapterAllowed" "$context native fallback"
        $requestedSoftwareAdapterAllowed = Get-RequiredJsonProperty `
            $request "softwareAdapterAllowed" "$context request"
        if ($softwareAdapterAllowed -isnot [bool] -or
            $requestedSoftwareAdapterAllowed -isnot [bool] -or
            $softwareAdapterAllowed -ne $requestedSoftwareAdapterAllowed)
        {
            throw "$context native software-adapter policy is inconsistent"
        }
    }
    elseif ($null -ne $nativeEvidence)
    {
        throw "$context has unexpected native evidence"
    }
}

function Assert-RequiredSuiteEvidenceReceipt
{
    param(
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][string]$ArtifactRelative,
        [Parameter(Mandatory)][object]$Receipt,
        [Parameter(Mandatory)][object]$Record,
        [Parameter(Mandatory)][object]$Gate
    )

    $receipt = $Receipt
    $context = "Evidence receipt '$ArtifactRelative'"
    $receiptIdentity = Assert-RunEvidenceReceiptIdentity -Root $Root -ArtifactRelative $ArtifactRelative `
        -Receipt $receipt -Record $Record -ReceiptKind RequiredSuite `
        -ExpectedSchema "RVX.AgentHarness.RequiredSuiteReceipt" -Context $context

    $request = Get-RequiredJsonProperty $receipt "request" $context
    $expectation = Get-ReceiptExpectation -Gate $Gate -Kind RequiredSuite -Context $context
    foreach ($field in @("baseRef", "configurePreset", "buildPreset", "testPreset", "buildDir", "configuration", "requiredLabel"))
    {
        if ([string]::IsNullOrWhiteSpace([string](Get-RequiredJsonProperty $request $field "$context request")))
        {
            throw "$context request '$field' is empty"
        }
    }
    foreach ($field in @("baseRef", "configurePreset", "buildPreset", "testPreset", "configuration", "requiredLabel", "expectedTestCount", "fresh", "skipConfigure"))
    {
        Assert-ReceiptExpectedScalar -Request $request -Expectation $expectation `
            -Field $field -Context $context
    }
    $buildDirRelative = Assert-ReceiptExpectedBuildDirectory -Root $Root `
        -Request $request -Expectation $expectation -Context $context
    Assert-ReceiptRunnerDigest -Root $Root `
        -RunnerDigest (Get-RequiredJsonProperty $request "runnerDigest" "$context request") `
        -ExpectedPaths (Get-BuildTruthReceiptRunnerPaths -ReceiptKind RequiredSuite -SchemaVersion $receiptIdentity.schemaVersion) `
        -Context "$context runner digest"
    $buildTargets = @(Get-RequiredJsonArrayProperty $request "buildTargets" "$context request")
    if ($buildTargets.Count -le 0) { throw "$context request has no build targets" }
    Assert-ReceiptExpectedStringSequence -Actual $buildTargets `
        -Expected @(Get-RequiredJsonArrayProperty $expectation "buildTargets" "$context expectation") `
        -Context "$context build targets"
    $requiredLabel = [string](Get-RequiredJsonProperty $receipt "requiredLabel" $context)
    if ($requiredLabel -cne [string](Get-RequiredJsonProperty $request "requiredLabel" "$context request"))
    {
        throw "$context required label is inconsistent"
    }
    $requiredSuiteToken = ($requiredLabel -replace "[^A-Za-z0-9_.-]", "-").Trim("-", ".", "_")
    if ([string]::IsNullOrWhiteSpace($requiredSuiteToken))
    {
        throw "$context required label cannot produce a canonical artifact token"
    }
    $validatedReport = Assert-ReceiptFileIdentity -Root $Root `
        -Identity (Get-RequiredJsonProperty $receipt "report" $context) `
        -BuildDirRelative $buildDirRelative -Context "$context report"
    if ($validatedReport.path -cne "$buildDirRelative/BuildTruth/RequiredSuite.$requiredSuiteToken.json")
    {
        throw "$context report path does not match the Required Suite contract"
    }
    $selectedTests = @(Get-RequiredJsonArrayProperty $receipt "selectedTests" $context)
    $uniqueSelectedTests = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($selectedTest in $selectedTests)
    {
        if ([string]::IsNullOrWhiteSpace([string]$selectedTest) -or
            -not $uniqueSelectedTests.Add([string]$selectedTest))
        {
            throw "$context selected tests are empty or duplicated"
        }
    }
    if ($selectedTests.Count -le 0)
    {
        throw "$context selected tests are empty or duplicated"
    }
    $expectedCount = Get-OptionalJsonProperty $request "expectedTestCount"
    if ($null -ne $expectedCount -and [int]$expectedCount -gt 0 -and
        [int]$expectedCount -ne $selectedTests.Count)
    {
        throw "$context expected test count is inconsistent"
    }
    $junit = Get-RequiredJsonProperty $receipt "junit" $context
    $validatedJUnit = Assert-ReceiptFileIdentity -Root $Root -Identity $junit `
        -BuildDirRelative $buildDirRelative -Context "$context JUnit"
    if ($validatedJUnit.path -cne "$buildDirRelative/BuildTruth/JUnit.$requiredSuiteToken.xml")
    {
        throw "$context JUnit path does not match the Required Suite contract"
    }
    Assert-PassingReceiptJUnitOutcome `
        -Outcome (Get-RequiredJsonProperty $junit "outcome" "$context JUnit") `
        -ExpectedTestCount $selectedTests.Count -Context "$context JUnit outcome"

    $seenArtifacts = [System.Collections.Generic.HashSet[string]]::new((Get-PathStringComparer))
    foreach ($artifact in @(Get-RequiredJsonArrayProperty $receipt "semanticArtifacts" $context))
    {
        $validatedArtifact = Assert-ReceiptFileIdentity -Root $Root -Identity $artifact `
            -BuildDirRelative $buildDirRelative -Context "$context semantic artifact"
        if (-not $seenArtifacts.Add($validatedArtifact.path))
        {
            throw "$context contains duplicate semantic artifact paths"
        }
    }
    $expectedArtifacts = [System.Collections.Generic.HashSet[string]]::new((Get-PathStringComparer))
    foreach ($expectedArtifact in @(Get-RequiredJsonArrayProperty $expectation "requiredSemanticArtifacts" "$context expectation"))
    {
        $normalizedExpected = Get-NormalizedRelativePath $Root ([string]$expectedArtifact) `
            "$context expected semantic artifact"
        if (-not $expectedArtifacts.Add($normalizedExpected))
        {
            throw "$context gate expectation contains duplicate semantic artifact paths"
        }
    }
    if ($seenArtifacts.Count -ne $expectedArtifacts.Count)
    {
        throw "$context semantic artifact set does not match its gate expectation"
    }
    foreach ($expectedArtifact in $expectedArtifacts)
    {
        if (-not $seenArtifacts.Contains($expectedArtifact))
        {
            throw "$context semantic artifact set does not match its gate expectation"
        }
    }
}

function Assert-EvidenceSourceIdentity
{
    param(
        [Parameter(Mandatory)][object]$Record,
        [Parameter(Mandatory)][string]$Context
    )

    $sourceCommit = [string](Get-OptionalJsonProperty $Record "sourceCommit")
    $sourceDigest = [string](Get-OptionalJsonProperty $Record "sourceDigest")
    if ($sourceCommit -notmatch "^[a-f0-9]{40}$" -or
        $sourceDigest -notmatch "^[a-f0-9]{64}$")
    {
        throw "$Context requires a valid sourceCommit and sourceDigest"
    }
}

function Assert-PassingEvidence
{
    param(
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][object]$Features
    )

    foreach ($feature in @($Features.Passing))
    {
        foreach ($gate in @($feature.verification | Where-Object { [bool]$_.required }))
        {
            $matches = @($feature.evidence | Where-Object {
                $_.verificationId -eq $gate.id -and $_.command -ceq $gate.command -and
                $_.result -eq "passed" -and $null -ne $_.exitCode -and [int]$_.exitCode -eq 0
            })
            if ($matches.Count -ne 1)
            {
                throw "Passing feature '$($feature.id)' requires exactly one passed evidence record for '$($gate.id)'"
            }
            $record = $matches[0]
            Assert-EvidenceSourceIdentity -Record $record -Context "Passing feature '$($feature.id)' evidence '$($gate.id)'"
            $expectedReceipt = Get-OptionalJsonProperty $gate "expectedReceipt"
            if ($null -eq $gate.expectedArtifact)
            {
                if ($null -ne $expectedReceipt)
                {
                    throw "Passing feature '$($feature.id)' verification '$($gate.id)' declares a receipt without an artifact"
                }
                if ($null -ne $record.artifact)
                {
                    throw "Passing feature '$($feature.id)' evidence '$($gate.id)' has an unexpected artifact"
                }
                foreach ($identityField in @("artifactSha256", "runId"))
                {
                    if ($null -ne (Get-OptionalJsonProperty $record $identityField))
                    {
                        throw "Passing feature '$($feature.id)' evidence '$($gate.id)' has unexpected '$identityField' identity"
                    }
                }
                continue
            }

            $recordRunId = [string](Get-OptionalJsonProperty $record "runId")
            $expectedArtifact = [string]$gate.expectedArtifact
            $usesRunId = $expectedArtifact.Contains("{runId}")
            if ($usesRunId -and $null -eq $expectedReceipt)
            {
                throw "Passing feature '$($feature.id)' verification '$($gate.id)' must declare expectedReceipt"
            }
            if (-not $usesRunId -and $null -ne $expectedReceipt)
            {
                throw "Passing feature '$($feature.id)' verification '$($gate.id)' declares expectedReceipt for a mutable artifact"
            }
            if ($gate.layer -eq "architecture" -and -not $usesRunId)
            {
                throw "Architecture verification '$($gate.id)' must include '{runId}' in expectedArtifact"
            }
            if ($usesRunId -and $recordRunId -notmatch "^[a-f0-9]{32}$")
            {
                throw "Passing feature '$($feature.id)' evidence '$($gate.id)' requires a valid runId"
            }
            $expectedArtifact = $expectedArtifact.Replace("{runId}", $recordRunId)
            if ($expectedArtifact -match "\{[^}]+\}")
            {
                throw "Verification '$($gate.id)' uses an unsupported expectedArtifact template"
            }
            $expectedRelative = Get-NormalizedRelativePath $Root $expectedArtifact "Verification '$($gate.id)' expectedArtifact"
            $artifactRelative = Get-NormalizedRelativePath $Root ([string]$record.artifact) "Evidence '$($gate.id)' artifact"
            if ($expectedRelative -cne $artifactRelative)
            {
                throw "Passing feature '$($feature.id)' evidence '$($gate.id)' does not bind its expected artifact"
            }
            $artifactPath = Join-Path $Root $artifactRelative
            if (-not (Test-Path -LiteralPath $artifactPath -PathType Leaf))
            {
                throw "Passing feature '$($feature.id)' evidence '$($gate.id)' artifact is missing"
            }
            $safeArtifactPath = Resolve-BuildTruthContainedPath `
                -Root $Root -Path $artifactPath
            if (-not [string]::Equals(
                    [IO.Path]::GetFullPath($artifactPath),
                    [IO.Path]::GetFullPath($safeArtifactPath),
                    (Get-PathStringComparison)))
            {
                throw "Passing feature '$($feature.id)' evidence '$($gate.id)' artifact path is unsafe"
            }
            $artifactSha256 = [string](Get-OptionalJsonProperty $record "artifactSha256")
            $artifactBytes = [IO.File]::ReadAllBytes($safeArtifactPath)
            $artifactHasher = [Security.Cryptography.SHA256]::Create()
            try
            {
                $actualArtifactSha256 = ([BitConverter]::ToString(
                    $artifactHasher.ComputeHash($artifactBytes))).Replace("-", "").ToLowerInvariant()
            }
            finally
            {
                $artifactHasher.Dispose()
            }
            if ($artifactSha256 -notmatch "^[a-f0-9]{64}$" -or $artifactSha256 -cne $actualArtifactSha256)
            {
                throw "Passing feature '$($feature.id)' evidence '$($gate.id)' artifact hash does not match"
            }

            if ($usesRunId)
            {
                try
                {
                    $receiptText = [Text.UTF8Encoding]::new($false, $true).GetString($artifactBytes)
                    $receiptValue = $receiptText | ConvertFrom-Json
                }
                catch
                {
                    throw "Evidence receipt '$artifactRelative' is malformed: $($_.Exception.Message)"
                }
                if ($gate.layer -eq "architecture")
                {
                    Assert-ArchitectureEvidenceReceipt -Root $Root `
                        -ArtifactRelative $artifactRelative -Receipt $receiptValue `
                        -Record $record -Gate $gate
                }
                elseif ($expectedRelative -match "^\.agent-harness/receipts/BuildTruth\.")
                {
                    Assert-BuildTruthEvidenceReceipt -Root $Root `
                        -ArtifactRelative $artifactRelative -Receipt $receiptValue `
                        -Record $record -Gate $gate
                }
                elseif ($expectedRelative -match "^\.agent-harness/receipts/RequiredSuite\.")
                {
                    Assert-RequiredSuiteEvidenceReceipt -Root $Root `
                        -ArtifactRelative $artifactRelative -Receipt $receiptValue `
                        -Record $record -Gate $gate
                }
                else
                {
                    throw "Verification '$($gate.id)' uses an unsupported run-id receipt kind"
                }
            }
            elseif ($null -ne (Get-OptionalJsonProperty $record "runId"))
            {
                throw "Passing feature '$($feature.id)' evidence '$($gate.id)' has an unbound runId"
            }
        }
    }
}

function Write-HarnessReport
{
    param([object]$Report, [string]$Format)

    if ($Format -eq "Json") { $Report | ConvertTo-Json -Depth 12; return }
    Write-Host "RenderVerseX agent harness"
    Write-Host "Repository: $($Report.repositoryRoot)"
    Write-Host "Mode: $($Report.mode)"
    Write-Host "Harness: $($Report.harness.status)"
    Write-Host "Active feature: $(if ($null -eq $Report.harness.activeFeature) { 'none' } else { $Report.harness.activeFeature })"
    Write-Host "Handoff: $(if ($null -eq $Report.harness.handoff) { 'none' } else { $Report.harness.handoff })"
    Write-Host "Preset: $($Report.build.preset)"
    Write-Host "Configured tree: $($Report.build.configured)"
    Write-Host "Verification: $($Report.verification.state)"
    if ($null -ne $Report.verification.receiptPath) { Write-Host "Receipt: $($Report.verification.receiptPath)" }
    if ($null -ne $Report.verification.reason) { Write-Host "Verification note: $($Report.verification.reason)" }
    Write-Host "Readiness: $($Report.readiness)"
}

$repoRoot = $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($Preset)) { $Preset = Get-DefaultPreset }
if ([string]::IsNullOrWhiteSpace($TestPreset)) { $TestPreset = "${Preset}_architecture" }
if ([string]::IsNullOrWhiteSpace($BuildDir)) { $BuildDir = "build/$Preset" }
if ($Mode -eq "Verify" -and $OutputFormat -eq "Json")
{
    throw "Verify streams delegated output; use Text output"
}

$locationPushed = $false
$verifyLock = $null
try
{
    Push-Location $repoRoot
    $locationPushed = $true
    $requiredFiles = @(
        "AGENTS.md", "CLAUDE.md", "README.md", "Docs/README.md",
        "Docs/agent-harness.md", "Docs/build-truth.md",
        "Docs/engine-capabilities.json", "feature_list.json",
        "feature_list.schema.json", "progress.md", "session-handoff.md",
        "init.ps1", "init.sh"
    )
    foreach ($relativePath in $requiredFiles)
    {
        if (-not (Test-Path -LiteralPath (Join-Path $repoRoot $relativePath) -PathType Leaf))
        {
            throw "Required harness file is missing: $relativePath"
        }
    }

    $features = Read-HarnessState `
        -StatePath (Join-Path $repoRoot "feature_list.json") `
        -SchemaPath (Join-Path $repoRoot "feature_list.schema.json")
    $handoff = Read-HandoffTarget `
        -Path (Join-Path $repoRoot "session-handoff.md") `
        -Known $features.Known

    foreach ($tool in @("cmake", "ctest", "git"))
    {
        if (-not (Get-Command $tool -ErrorAction SilentlyContinue))
        {
            throw "Required tool '$tool' is not available on PATH"
        }
    }
    $cmakeLine = [string]((Invoke-NativeChecked "CMake version" "cmake" @("--version")) |
        Select-Object -First 1)
    if ($cmakeLine -notmatch "cmake version\s+(\d+\.\d+\.\d+)" -or
        [version]$Matches[1] -lt [version]"3.25.0")
    {
        throw "CMake 3.25 or newer is required; found '$cmakeLine'"
    }
    $cmakeVersion = $Matches[1]
    Assert-PresetExists -Preset $Preset -TestPreset $TestPreset

    $buildRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot "build"))
    $resolvedBuildDir = [IO.Path]::GetFullPath((Join-Path $repoRoot $BuildDir))
    $prefix = $buildRoot.TrimEnd([IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if (-not $resolvedBuildDir.StartsWith($prefix, (Get-PathStringComparison)))
    {
        throw "BuildDir must resolve below '$buildRoot'"
    }
    $configured = Test-Path -LiteralPath (Join-Path $resolvedBuildDir "CMakeCache.txt") -PathType Leaf

    $gitSafe = $repoRoot.Replace([IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar)
    $gitArgs = @("-c", "safe.directory=$gitSafe", "-c", "core.excludesFile=", "-C", $repoRoot)
    $commit = [string]((Invoke-NativeChecked "Git HEAD" "git" ($gitArgs + @("rev-parse", "HEAD"))) |
        Select-Object -First 1)
    $changes = @(Invoke-NativeChecked "Git status" "git" (
        $gitArgs + @("status", "--short", "--untracked-files=all")))
    Invoke-NativeChecked "Git diff hygiene" "git" ($gitArgs + @("diff", "--check")) | Out-Null
    Invoke-NativeChecked "Git staged diff hygiene" "git" ($gitArgs + @("diff", "--cached", "--check")) | Out-Null

    Import-Module (Join-Path $repoRoot "Scripts\BuildTruth\BuildTruthV3.psm1") `
        -Force -WarningAction SilentlyContinue | Out-Null
    $productFingerprint = Get-ProductSourceFingerprint -Root $repoRoot -GitSafeDirectory $gitSafe
    $verification = Read-VerificationState `
        -Root $repoRoot -ResolvedBuildDir $resolvedBuildDir -Preset $Preset `
        -TestPreset $TestPreset -Configuration $Configuration -BaseRef $BaseRef `
        -AllowConfigure ([bool]$AllowConfigure) `
        -CurrentCommit $commit.Trim() -CurrentProductFingerprint $productFingerprint

    if ($Mode -eq "Verify")
    {
        if (-not $configured -and -not $AllowConfigure)
        {
            throw "No configured tree exists at '$BuildDir'; re-run with -AllowConfigure after authorization"
        }
        $lockDirectory = Join-Path $resolvedBuildDir "BuildTruth\AgentHarness"
        New-Item -ItemType Directory -Force -Path $lockDirectory | Out-Null
        $lockPath = Join-Path $lockDirectory "Architecture.verify.lock"
        try
        {
            $verifyLock = [IO.File]::Open(
                $lockPath, [IO.FileMode]::OpenOrCreate,
                [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        }
        catch
        {
            throw "Another Architecture Verify is already using '$BuildDir'"
        }
        $productFingerprintBefore = $productFingerprint
        $runnerArgs = @(
            "-NoProfile", "-File", (Join-Path $repoRoot "Scripts\run_required_suite.ps1"),
            "-ConfigurePreset", $Preset, "-BuildPreset", $Preset,
            "-TestPreset", $TestPreset, "-BuildDir", $BuildDir,
            "-Configuration", $Configuration,
            "-BuildTarget", "RVXValidationArchitecture",
            "-RequiredLabel", "lane.architecture", "-BaseRef", $BaseRef
        )
        if (-not $AllowConfigure) { $runnerArgs += "-SkipConfigure" }
        Write-Host "Delegating verified bootstrap to Scripts/run_required_suite.ps1"
        $pwshPath = Join-Path $PSHOME $(if ($env:OS -eq "Windows_NT") { "pwsh.exe" } else { "pwsh" })
        & $pwshPath @runnerArgs
        if ($LASTEXITCODE -ne 0)
        {
            throw "Architecture required suite failed with exit code $LASTEXITCODE"
        }

        $commitAfter = [string]((Invoke-NativeChecked "Git HEAD after Architecture suite" "git" ($gitArgs + @("rev-parse", "HEAD"))) |
            Select-Object -First 1)
        if ($commitAfter.Trim() -cne $commit.Trim())
        {
            throw "HEAD changed while the Architecture required suite was running"
        }
        $productFingerprint = Get-ProductSourceFingerprint -Root $repoRoot -GitSafeDirectory $gitSafe
        Assert-FingerprintEqual $productFingerprintBefore $productFingerprint "Product source before and after Architecture suite"
        $reportInfo = Assert-RequiredSuiteReport `
            -ReportPath (Join-Path $resolvedBuildDir "BuildTruth\RequiredSuite.lane.architecture.json") `
            -ResolvedBuildDir $resolvedBuildDir -Preset $Preset -TestPreset $TestPreset `
            -Configuration $Configuration -BaseRef $BaseRef -CurrentCommit $commit.Trim()
        New-ArchitectureReceipt -Root $repoRoot -ResolvedBuildDir $resolvedBuildDir `
            -ReportInfo $reportInfo -ProductFingerprint $productFingerprint `
            -Preset $Preset -TestPreset $TestPreset -Configuration $Configuration `
            -BaseRef $BaseRef -AllowConfigure ([bool]$AllowConfigure) `
            -CurrentCommit $commit.Trim() | Out-Null
        $verification = Read-VerificationState `
            -Root $repoRoot -ResolvedBuildDir $resolvedBuildDir -Preset $Preset `
            -TestPreset $TestPreset -Configuration $Configuration -BaseRef $BaseRef `
            -AllowConfigure ([bool]$AllowConfigure) `
            -CurrentCommit $commit.Trim() -CurrentProductFingerprint $productFingerprint
        if ($verification.state -ne "verified")
        {
            throw "Architecture runner succeeded but the generated receipt is '$($verification.state)': $($verification.reason)"
        }

        # Verify proves and publishes only this invocation. Historical passing
        # records remain fail-closed in Inspect and Close, which provides a
        # non-circular migration path from older immutable receipt schemas.
    }

    $readiness = "ready_unverified"
    if ($Mode -eq "Close")
    {
        Assert-PassingEvidence -Root $repoRoot -Features $features
        if ($features.Wip.Count -eq 1)
        {
            $wipFeature = $features.Wip[0]
            if ($null -eq $handoff -or $handoff -ne [string]$wipFeature.id)
            {
                throw "Close rejected: active or blocked work requires an exact matching handoff"
            }
            $readiness = if ($wipFeature.status -eq "active") {
                "handed_off_active"
            } else {
                "handed_off_blocked"
            }
        }
        else
        {
            if ($null -ne $handoff)
            {
                throw "Close rejected: a handoff exists but no active or blocked feature remains"
            }
            if ($verification.state -ne "verified")
            {
                throw "Close rejected: current product verification is '$($verification.state)'"
            }
            $readiness = "closed"
        }
    }
    elseif ($Mode -eq "Inspect")
    {
        Assert-PassingEvidence -Root $repoRoot -Features $features
        if ($verification.state -eq "verified") { $readiness = "verified" }
    }
    elseif ($verification.state -eq "verified")
    {
        $readiness = "verified"
    }

    $report = [ordered]@{
        schema = "RVX.AgentHarness.Preflight"
        schemaVersion = 2
        generatedAtUtc = (Get-Date).ToUniversalTime().ToString("o")
        mode = $Mode
        repositoryRoot = $repoRoot
        harness = [ordered]@{
            status = "valid"; activeFeature = $features.ActiveId
            activeCount = $features.Active.Count; blockedCount = $features.Blocked.Count
            passingCount = $features.Passing.Count; handoff = $handoff
        }
        tools = [ordered]@{
            powerShell = $PSVersionTable.PSVersion.ToString()
            cmake = $cmakeVersion; ctest = "available"; git = "available"
        }
        build = [ordered]@{
            preset = $Preset; testPreset = $TestPreset
            buildDir = Get-RepositoryRelativePath -Root $repoRoot -Path $resolvedBuildDir -Context "Architecture verification build directory"
            configuration = $Configuration; configured = [bool]$configured
        }
        worktree = [ordered]@{
            head = $commit.Trim()
            changedPathCount = @($changes | Where-Object {
                -not [string]::IsNullOrWhiteSpace([string]$_)
            }).Count
            productFingerprint = $productFingerprint
            diffHygiene = "passed"; stagedDiffHygiene = "passed"
        }
        verification = $verification
        readiness = $readiness
    }
    Write-HarnessReport $report $OutputFormat
}
catch
{
    $failure = $_.Exception.Message
    if ($OutputFormat -eq "Json")
    {
        [ordered]@{
            schema = "RVX.AgentHarness.Preflight"
            schemaVersion = 2
            generatedAtUtc = (Get-Date).ToUniversalTime().ToString("o")
            mode = $Mode
            status = "failed"
            error = $failure
        } | ConvertTo-Json -Depth 8
    }
    else
    {
        Write-Error "RenderVerseX agent harness failed: $failure"
    }
    exit 1
}
finally
{
    if ($null -ne $verifyLock) { $verifyLock.Dispose() }
    if ($locationPushed) { Pop-Location }
}
